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
TRACEGEN = os.path.join(ROOT, "build", "bin", "tracegen")
VVP      = os.path.join(ROOT, "build", "tb_z80.vvp")

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

def compare_one(prog, phases):
    c = trace_lines(run([TRACEGEN, prog, phases]))
    v = trace_lines(run([VVP, f"+prog={prog}", f"+phases={phases}"]))
    n = min(len(c), len(v))
    name = os.path.basename(prog)
    if n == 0:
        print(f"  {name}: FAIL (no trace lines)")
        return False
    mism = 0
    for i in range(n):
        if c[i] != v[i]:
            mism += 1
            if mism <= 5:
                print(f"  {name} MISMATCH phase {i}:\n    C  : {c[i]}\n    RTL: {v[i]}")
    if mism == 0 and len(c) == len(v):
        print(f"  {name}: PASS ({n} phases identical)")
        return True
    print(f"  {name}: FAIL ({mism} mismatches; C={len(c)} RTL={len(v)} lines)")
    return False

def main():
    if not os.path.exists(TRACEGEN):
        raise SystemExit(f"missing {TRACEGEN} (run: make tracegen)")
    if not os.path.exists(VVP):
        raise SystemExit(f"missing {VVP} (run: make iverilog)")

    if len(sys.argv) > 1:
        progs = [sys.argv[1]]
        phases = sys.argv[2] if len(sys.argv) > 2 else "400"
    else:
        import glob
        progs = sorted(glob.glob(os.path.join(ROOT, "tests/traces/*.hex")))
        phases = "400"

    print(f"C vs RTL trace comparison  (cols: {' '.join(COLS)})")
    ok = True
    for p in progs:
        if not compare_one(p, phases):
            ok = False
    if ok:
        print("PASS: all programs — C and Verilog RTL traces identical phase-by-phase")
        return 0
    print("FAIL: trace mismatches found")
    return 1

if __name__ == "__main__":
    sys.exit(main())
