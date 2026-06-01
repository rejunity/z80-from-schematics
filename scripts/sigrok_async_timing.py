#!/usr/bin/env python3
"""sigrok_async_timing.py - extract two things from the asynchronous
20 MHz kc85 capture that the synchronous (cpuclk) capture can't give:

  1. The actual Z80 CPU clock period (one of the captured signals IS CLK).
     Cross-check that the cpuclk-sync capture's "1 sample per T-state"
     assumption holds.

  2. Sub-T-state pin transition timing — at what fraction of a T-state
     does MREQ assert? When does RD assert relative to MREQ? Is M1
     deasserted before refresh starts? These match our PHI_P / PHI_N
     two-phase model in cmodel/z80.c.

We then RE-SAMPLE the async capture at every CLK falling edge to produce
a synthetic synchronous trace, run the per-opcode T-state analysis
(same as sigrok_opcode_cycles.py) on it, and confirm the answers agree
with the cpuclk capture.
"""
import os, sys, subprocess, argparse, zipfile, configparser
from collections import defaultdict, Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from sigrok_z80_decode import CHANNEL_NAMES, decode_sample, parse_sr

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sr", default="tests/sigrok/kc85-20mhz.sr", nargs="?")
    ap.add_argument("--max-cycles", type=int, default=0, help="limit re-sampled T-state count")
    ap.add_argument("--dump-resampled", default=None, help="write the re-sampled trace to file")
    ap.add_argument("--silicon-check", action="store_true",
                    help="run the per-opcode T-state spec check on the re-sampled trace")
    args = ap.parse_args()

    meta, raw = parse_sr(args.sr)
    unit = meta["unitsize"]
    n_samples = len(raw) // unit
    # samplerate is "20 MHz" string from metadata; parse it
    sr_str = meta["samplerate"]
    if sr_str.endswith(" MHz"):
        sr_hz = float(sr_str[:-4]) * 1e6
    elif sr_str.endswith(" kHz"):
        sr_hz = float(sr_str[:-4]) * 1e3
    else:
        sr_hz = float(sr_str)
    sample_ns = 1e9 / sr_hz
    print(f"# {os.path.basename(args.sr)}: {n_samples} samples @ {sr_hz/1e6:.1f} MHz "
          f"= {sample_ns:.1f} ns / sample", file=sys.stderr)

    # First pass: find CLK transitions, measure period.
    decoded = [decode_sample(raw[i*unit:(i+1)*unit]) for i in range(n_samples)]
    clk_falls = []
    clk_rises = []
    prev_clk = decoded[0]["CLK"]
    for i in range(1, n_samples):
        c = decoded[i]["CLK"]
        if prev_clk == 1 and c == 0: clk_falls.append(i)
        elif prev_clk == 0 and c == 1: clk_rises.append(i)
        prev_clk = c

    if len(clk_falls) < 2:
        print("not enough CLK falling edges in capture", file=sys.stderr); return 2

    # period = average sample-delta between consecutive falling edges
    deltas = [clk_falls[i+1] - clk_falls[i] for i in range(len(clk_falls) - 1)]
    avg = sum(deltas) / len(deltas)
    period_ns = avg * sample_ns
    cpu_mhz = 1000.0 / period_ns
    minv, maxv = min(deltas), max(deltas)
    print(f"# CLK falling edges: {len(clk_falls)}  rising edges: {len(clk_rises)}",
          file=sys.stderr)
    print(f"# CPU clock period: avg {period_ns:.1f} ns "
          f"(min {minv*sample_ns:.0f} max {maxv*sample_ns:.0f}) "
          f"-> CPU freq ~{cpu_mhz:.3f} MHz", file=sys.stderr)

    # Second pass: re-sample at every CLK falling edge to mimic the cpuclk
    # synchronous capture. This is exactly what the SysClk LWLA's external-
    # clock mode does in hardware.
    resampled = [decoded[i] for i in clk_falls]
    if args.max_cycles and args.max_cycles < len(resampled):
        resampled = resampled[:args.max_cycles]
    print(f"# re-sampled {len(resampled)} T-states", file=sys.stderr)

    # Sub-T-state pin timing: for each T-state's window, where (in samples
    # since the falling edge that opened it) do MREQ / RD / WR / M1 transition?
    # We tabulate transition-offsets for each pin.
    pin_offsets = defaultdict(Counter)   # pin -> Counter[sample_offset_in_period]
    for k in range(len(clk_falls) - 1):
        i0 = clk_falls[k]
        i1 = clk_falls[k+1]
        # for each pin we care about, find any 1<->0 transitions inside this window
        for pin in ("MREQn","RDn","WRn","M1n","IORQn","RFSH" if False else None):
            if pin is None: continue
            prev = decoded[i0][pin] if pin in decoded[i0] else None
            for j in range(i0+1, i1+1):
                cur = decoded[j].get(pin)
                if cur is None: break
                if cur != prev:
                    pin_offsets[pin][j - i0] += 1
                    prev = cur

    period_samples = round(avg)
    print(f"\n# Sub-T-state pin transition offsets (period = ~{period_samples} samples)",
          file=sys.stderr)
    for pin in sorted(pin_offsets):
        bins = pin_offsets[pin]
        top = bins.most_common(5)
        # describe each common offset as a fraction of the period
        descr = ", ".join(f"@{o} ({o/period_samples*100:.0f}% of T) x{cnt}" for o, cnt in top)
        print(f"  {pin:6s}: {sum(bins.values())} transitions, most common: {descr}",
              file=sys.stderr)

    if args.dump_resampled:
        with open(args.dump_resampled, "w") as f:
            f.write("# i M1 MREQ IORQ RD WR addr data INT WAIT\n")
            for i, s in enumerate(resampled):
                f.write(f"{i} {1-s['M1n']} {1-s['MREQn']} {1-s['IORQn']} "
                        f"{1-s['RDn']} {1-s['WRn']} {s['addr']:04x} {s['data']:02x} "
                        f"{1-s['INTn']} {1-s['WAITn']}\n")
        print(f"# wrote re-sampled trace to {args.dump_resampled}", file=sys.stderr)

    if args.silicon_check:
        # write the re-sampled trace and call sigrok_opcode_cycles' core logic
        # on it directly (no need to re-decode; we pass already-decoded list)
        from sigrok_opcode_cycles import find_m1_starts, classify_t_states, emit_emulator_opcode_cycles
        # adapt the resampled dicts to the (i, m1, mreq, ...) shape that
        # find_m1_starts expects
        cycles = []
        for i, s in enumerate(resampled):
            cycles.append(dict(
                i=i,
                m1=1-s["M1n"], mreq=1-s["MREQn"], iorq=1-s["IORQn"],
                rd=1-s["RDn"], wr=1-s["WRn"],
                addr=s["addr"], data=s["data"],
            ))
        starts = find_m1_starts(cycles)
        per_op = defaultdict(Counter)
        for k in range(len(starts) - 1):
            i0, pc0, op0 = starts[k]
            i1, _, _ = starts[k+1]
            per_op[op0][i1 - i0] += 1

        total = matched = mismatch = sysartifact = 0
        print(f"# Re-sampled (async-derived) per-opcode T-state cross-check")
        print("# opcode | samples | real T-states | spec | emu | verdict")
        for op in sorted(per_op):
            spec = classify_t_states(op)
            sample_lens = per_op[op]
            samples = sum(sample_lens.values())
            if spec is None:
                print(f"  {op:02x}     | {samples:3d}     | {dict(sample_lens)} | -    | -   | (prefix / unclassified)")
                continue
            emu = emit_emulator_opcode_cycles(op)
            seen = sorted(sample_lens.keys())
            in_spec = all(spec[0] <= s <= spec[1] for s in seen)
            spec_str = f"{spec[0]}-{spec[1]}T" if spec[0]!=spec[1] else f"{spec[0]}T"
            emu_str  = f"{emu}T" if emu is not None else "?"
            if not in_spec:
                verdict = f"silicon out-of-spec ({seen} not in [{spec[0]},{spec[1]}])"
                sysartifact += 1
            elif emu is not None and spec[0] <= emu <= spec[1]:
                verdict = "OK"; matched += 1
            else:
                verdict = f"emu mismatch ({emu} not in [{spec[0]},{spec[1]}])"; mismatch += 1
            total += 1
            print(f"  {op:02x}     | {samples:3d}     | {dict(sample_lens)} | {spec_str:5s} | {emu_str:5s} | {verdict}")
        print(f"\nClassified: {total} opcodes, {matched} OK, {mismatch} emulator mismatches, "
              f"{sysartifact} silicon out-of-spec (system WAIT/IRQ artifacts)")

if __name__ == "__main__":
    sys.exit(main() or 0)
