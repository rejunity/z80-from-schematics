#!/usr/bin/env python3
"""sigrok_z80_decode.py - parse a sigrok .sr Z80 capture (specifically the
kc85 capture format from sigrokproject/sigrok-dumps z80/kc85/, channel layout
documented in that README) and emit a per-cycle CSV-ish trace and a memory
snapshot reconstructed by following the M1 fetches + MRD operand reads.

Usage:
    sigrok_z80_decode.py <capture.sr> [--trace out.trace] [--mem out.hex] [--samples N]

The kc85 channel order (from the upstream README):
    CH1=CLK  CH2=/M1  CH3=/INT  CH4=MEI  CH5=/WAIT  CH6=IEI
    CH7..CH22 = A0..A15
    CH23=/IORQ  CH24=/MREQ  CH25=/RD  CH26=/WR
    CH27..CH34 = D0..D7

For the cpu-clock synchronous variant, each 5-byte (40-bit) sample is one
sigrok sample (= one Z80 clock falling edge).
"""
import sys, zipfile, configparser, struct, argparse

CHANNEL_NAMES = [
    "CLK","M1n","INTn","MEI","WAITn","IEI",
    "A0","A1","A2","A3","A4","A5","A6","A7",
    "A8","A9","A10","A11","A12","A13","A14","A15",
    "IORQn","MREQn","RDn","WRn",
    "D0","D1","D2","D3","D4","D5","D6","D7",
]

def get_bit(buf, idx):
    return (buf[idx >> 3] >> (idx & 7)) & 1

def decode_sample(buf):
    """Return dict of all signals + computed addr/data for a 5-byte sample."""
    out = {}
    for i, name in enumerate(CHANNEL_NAMES):
        out[name] = get_bit(buf, i)
    addr = 0
    for i in range(16):
        addr |= out[f"A{i}"] << i
    data = 0
    for i in range(8):
        data |= out[f"D{i}"] << i
    out["addr"] = addr
    out["data"] = data
    return out

def parse_sr(path):
    """Return (metadata-dict, list-of-sample-bytes)."""
    with zipfile.ZipFile(path) as z:
        meta = {}
        cfg = configparser.ConfigParser()
        cfg.read_string(z.read("metadata").decode())
        sec = "device 1"
        meta["probes"]    = [cfg[sec][f"probe{i+1}"] for i in range(int(cfg[sec]["total probes"]))]
        meta["samplerate"] = cfg[sec]["samplerate"]
        meta["unitsize"]   = int(cfg[sec]["unitsize"])
        # logic file: usually "logic-1-1" (capture-file + group + segment)
        names = [n for n in z.namelist() if n.startswith("logic-")]
        samples = b"".join(z.read(n) for n in sorted(names))
    return meta, samples

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sr")
    ap.add_argument("--trace", default=None, help="emit per-cycle CSV trace")
    ap.add_argument("--mem",   default=None, help="emit reconstructed memory as Intel-style hex")
    ap.add_argument("--samples", type=int, default=0, help="limit decoded samples (0 = all)")
    ap.add_argument("--bus-only", action="store_true",
                    help="emit only cycles where MREQ or IORQ is asserted (skip idle)")
    args = ap.parse_args()

    meta, raw = parse_sr(args.sr)
    unit = meta["unitsize"]
    n = len(raw) // unit
    if args.samples and args.samples < n: n = args.samples

    print(f"# {args.sr}  samplerate={meta['samplerate']}  unitsize={unit}  samples={n}",
          file=sys.stderr)

    mem = {}
    trace_out = open(args.trace, "w") if args.trace else None
    if trace_out:
        trace_out.write("# i M1 MREQ IORQ RD WR addr data INT WAIT\n")

    bus_cycles = 0
    m1_count   = 0
    for i in range(n):
        s = decode_sample(raw[i*unit:(i+1)*unit])
        bus_active = (not s["MREQn"]) or (not s["IORQn"])
        if trace_out and (not args.bus_only or bus_active):
            trace_out.write(f"{i} {1-s['M1n']} {1-s['MREQn']} {1-s['IORQn']} "
                            f"{1-s['RDn']} {1-s['WRn']} {s['addr']:04x} {s['data']:02x} "
                            f"{1-s['INTn']} {1-s['WAITn']}\n")
        if not s["MREQn"]:
            bus_cycles += 1
            if not s["M1n"]:
                m1_count += 1
            # memory read: snapshot what the CPU saw
            if not s["RDn"]:
                mem[s["addr"]] = s["data"]
    if trace_out: trace_out.close()

    print(f"# bus cycles (MREQ asserted): {bus_cycles}", file=sys.stderr)
    print(f"# M1 fetches:                {m1_count}",   file=sys.stderr)
    print(f"# unique memory addresses observed: {len(mem)}", file=sys.stderr)

    if args.mem:
        with open(args.mem, "w") as f:
            for a in sorted(mem):
                f.write(f"@{a:04x}\n{mem[a]:02x}\n")
        print(f"# wrote {args.mem}", file=sys.stderr)

if __name__ == "__main__":
    main()
