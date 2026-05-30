#!/usr/bin/env python3
# compare_signal_timing.py - diff per-half-cycle pin signals between our C
# model (build/bin/tracegen) and the perfectz80 gate-level netlist simulator
# (build/bin/perfectz80_runner). Compares addr/data_o/data_i/mreq/iorq/rd/wr/
# m1/rfsh/halt columns — drops metadata prefix differences between the two
# trace formats. perfectz80 is gate-level slow, so default phases=200.
import os, sys, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACEGEN = os.path.join(ROOT, "build", "bin", "tracegen")
PZ80     = os.path.join(ROOT, "build", "bin", "perfectz80_runner")

# tracegen cols: t phi m addr data_o data_i mreq iorq rd wr m1 rfsh halt busack
# pz80 cols:    phase addr data_o data_i mreq iorq rd wr m1 rfsh halt
# Shared (compared): addr data_o data_i mreq iorq rd wr m1 rfsh halt
COMPARED = ["addr","data_o","data_i","mreq","iorq","rd","wr","m1","rfsh","halt"]

def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write(p.stderr); raise SystemExit(f"failed: {' '.join(cmd)}")
    return p.stdout

def parse_tracegen(text):
    rows = []
    for ln in text.splitlines():
        p = ln.split()
        if len(p) < 14: continue
        if not p[0].lstrip('-').isdigit(): continue
        rows.append(dict(zip(["t","phi","m","addr","data_o","data_i","mreq","iorq","rd","wr","m1","rfsh","halt","busack"], p)))
    return rows

def parse_pz80(text):
    rows = []
    for ln in text.splitlines():
        p = ln.split()
        if len(p) < 11: continue
        if not p[0].isdigit(): continue
        rows.append(dict(zip(["phase","addr","data_o","data_i","mreq","iorq","rd","wr","m1","rfsh","halt"], p)))
    return rows

def compare(prog, phases):
    c = parse_tracegen(run([TRACEGEN, prog, str(phases)]))
    g = parse_pz80(run([PZ80, prog, str(phases)]))
    n = min(len(c), len(g))
    name = os.path.basename(prog)
    if n == 0:
        print(f"  {name}: FAIL (no rows)"); return False
    mism = 0
    for i in range(n):
        diff = [(col, c[i][col], g[i][col]) for col in COMPARED if c[i][col] != g[i][col]]
        if diff:
            mism += 1
            if mism <= 5:
                d = " ".join(f"{c1}: C={v1} pz80={v2}" for c1,v1,v2 in diff)
                print(f"  {name} phase {i} differ:  {d}")
    if mism == 0:
        print(f"  {name}: PASS ({n} phases identical on {len(COMPARED)} pin columns)")
        return True
    print(f"  {name}: {mism}/{n} phases differ")
    return False

def main():
    if not os.path.exists(TRACEGEN): raise SystemExit("missing tracegen (make tracegen)")
    if not os.path.exists(PZ80):     raise SystemExit("missing perfectz80_runner")
    phases = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    progs = sys.argv[2:] if len(sys.argv) > 2 else \
            sorted([p for p in
                    [os.path.join(ROOT, f"tests/traces/{n}.hex") for n in ("prog1","prog2","prog3_cb")]
                    if os.path.exists(p)])
    print(f"C model vs perfectz80 gate-level signal-timing comparison "
          f"({phases} phases per program, {len(COMPARED)} pin columns)")
    ok = True
    for p in progs:
        if not compare(p, phases): ok = False
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
