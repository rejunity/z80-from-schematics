#!/usr/bin/env python3
# ===========================================================================
# compare_traces.py - run a program through the C model and the Verilog RTL
# (Icarus Verilog) and diff the shared bus-cycle traces phase-by-phase.
#
#   compare_traces.py [prog.hex] [phases]
#
# Exit 0 if the traces match on every compared phase; nonzero on first diff.
# Compared columns (all CPU-driven / deterministic):
#   t phi m addr data_out data_in mreq iorq rd wr m1 rfsh halt busack
# ===========================================================================
import os, sys, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACEGEN  = os.path.join(ROOT, "build", "bin", "tracegen")
VVP       = os.path.join(ROOT, "build", "tb_z80.vvp")
VERILATOR = os.path.join(ROOT, "build", "obj_dir", "sim_z80")

COLS = ["t", "phi", "m", "addr", "data_o", "data_i",
        "mreq", "iorq", "rd", "wr", "m1", "rfsh", "halt", "busack"]

def is_data_line(line):
    parts = line.split()
    return len(parts) == len(COLS) and parts[0].lstrip("-").isdigit()

def trace_lines(text):
    return [ln.strip() for ln in text.splitlines() if is_data_line(ln.strip())]

def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        raise SystemExit(f"command failed: {' '.join(cmd)}")
    return p.stdout

def compare_one(prog, phases, oracles):
    # programs named *nmi* get an NMI pulse at phase 30 in every harness
    nmi = ["30"] if "nmi" in os.path.basename(prog) else []
    traces = {}
    for name, runner in oracles.items():
        traces[name] = trace_lines(run(runner(prog, phases, nmi)))
    name = os.path.basename(prog)
    # pairwise compare against the C model
    ref = "C"
    cref = traces[ref]
    if not cref:
        print(f"  {name}: FAIL (no trace lines from {ref})")
        return False
    ok_all = True
    for other in [k for k in oracles if k != ref]:
        co = traces[other]
        n = min(len(cref), len(co))
        mism = 0
        for i in range(n):
            if cref[i] != co[i]:
                mism += 1
                if mism <= 3:
                    print(f"  {name} {ref} vs {other} MISMATCH phase {i}:\n    {ref:8s}: {cref[i]}\n    {other:8s}: {co[i]}")
        if mism == 0 and len(cref) == len(co):
            print(f"  {name} {ref} vs {other}: PASS ({n} phases identical)")
        else:
            print(f"  {name} {ref} vs {other}: FAIL ({mism} mismatches; {ref}={len(cref)} {other}={len(co)} lines)")
            ok_all = False
    return ok_all

def main():
    if not os.path.exists(TRACEGEN):
        raise SystemExit(f"missing {TRACEGEN} (run: make tracegen)")
    if not os.path.exists(VVP):
        raise SystemExit(f"missing {VVP} (run: make iverilog)")

    # always C + iverilog; add Verilator if its binary is present
    oracles = {
        "C":        lambda p, ph, nmi: [TRACEGEN, p, ph] + nmi,
        "iverilog": lambda p, ph, nmi: [VVP, f"+prog={p}", f"+phases={ph}"] + ([f"+nmi={nmi[0]}"] if nmi else []),
    }
    if os.path.exists(VERILATOR):
        oracles["Verilator"] = lambda p, ph, nmi: [VERILATOR, p, ph] + nmi
        print(f"C vs iverilog vs Verilator trace comparison (cols: {' '.join(COLS)})")
    else:
        print(f"C vs iverilog trace comparison (Verilator not built; cols: {' '.join(COLS)})")

    if len(sys.argv) > 1:
        progs = [sys.argv[1]]
        phases = sys.argv[2] if len(sys.argv) > 2 else "400"
    else:
        import glob
        progs = sorted(glob.glob(os.path.join(ROOT, "tests/traces/*.hex")))
        phases = "400"

    ok = True
    for p in progs:
        if not compare_one(p, phases, oracles):
            ok = False
    if ok:
        names = ", ".join(oracles.keys())
        print(f"PASS: all programs — {names} traces identical phase-by-phase")
        return 0
    print("FAIL: trace mismatches found")
    return 1

if __name__ == "__main__":
    sys.exit(main())
