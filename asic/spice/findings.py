#!/usr/bin/env python3
"""findings.py - what the pad simulations found, as plots (findings/*.svg):

  outputs.svg  into the 100 pF test load: the PDK library as STA
               extrapolates it, the simulated pad at 50 %, and the
               simulated pad where the datasheet measures (1.5 V)
  inputs.svg   pin to core on the datasheet's 15 ns edges: the PDK
               library's extrapolation against the simulation
  ttl.svg      where every input pad switches, against the TTL window
               (low below 0.8 V, high above 2.0 V) and the half-supply
               point STA measures inputs from
  timing.svg   the chips' tightest pin-timing margins at 4 MHz: the PDK
               libraries at 50 % against the simulated pads at the
               datasheet's levels

Reads pads/<pdk>/pads.json (pads.py) and, for the timing, ../timing/
pin_timing.py's output on each chip's run with the simulated pads and with
--pads none, as rz80-<chip>-chip-pin_timing.txt / -pin_timing-pdk.txt
(findings/pin_timing/ holds a set).

usage: findings.py [--pads DIR] [--timing DIR] [--out DIR]
"""
import argparse, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import svgplot  # noqa: E402
from pads import lookup, TEST_LOAD, LOADS, VIL, VIH  # noqa: E402

OUT = os.path.join(HERE, "findings")
PADS_DIR = os.path.join(HERE, "pads")
TIMING_DIR = os.path.join(HERE, "findings", "pin_timing")
PADS = {"gf180mcu": "GF180", "ihp-sg13g2": "IHP"}
CHIPS = {"gf180mcu": "GF180 7-track", "gf180mcu-9t": "GF180 9-track", "ihp-sg13g2": "IHP"}
GREY, BLUE, RED, GREEN = "#9a9a9a", "#1f6fb4", "#d1352b", "#2b9a3e"
short = lambda c: c.split("__")[-1] if "__" in c else c.replace("sg13g2_IOPad", "")


def load(pdk):
    return json.load(open(os.path.join(PADS_DIR, pdk, "pads.json")))["corners"]


def outputs():
    panels = []
    for pdk, name in PADS.items():
        res = load(pdk)
        cells = [c for c, C in next(iter(res.values()))["cells"].items() if "data" in C and not c.endswith("TriOut16mA")]
        rows = [f"{short(c)} {k} {e}" for c in cells for k in res for e in ("rise", "fall")]
        series = [("PDK library, 50 % (as STA extrapolates it)", GREY, "d"), ("simulated, 50 %", BLUE, "o"),
                  ("simulated, 1.5 V (where PS0178 measures)", RED, "s")]
        p = svgplot.Dumbbell(f"{name}: data input to pin, into 100 pF", "ns", rows, series)
        for c in cells:
            for k, r in res.items():
                D = r["cells"][c]["data"]
                a = D["slews"].index(min(D["slews"], key=lambda s: abs(s - 0.5)))
                col = LOADS.index(TEST_LOAD)
                for e in ("rise", "fall"):
                    row = f"{short(c)} {k} {e}"
                    p.set(row, series[0][0], lookup(D["pdk"]["cell_" + e], D["slews"][a], D["cpin"] + TEST_LOAD))
                    p.set(row, series[1][0], D[e][a][col]["d50"])
                    p.set(row, series[2][0], D[e][a][col]["dlev"])
        panels.append(p)
    svgplot.write_panels(os.path.join(OUT, "outputs.svg"), panels,
                         title="Outputs: the libraries extrapolate fine; the datasheet's 1.5 V level moves the edges")


def inputs():
    panels = []
    for pdk, name in PADS.items():
        res = load(pdk)
        first = next(iter(res.values()))["cells"]
        cells = [c for c, C in first.items() if "input" in C and not c.endswith("bi_t") and not c.endswith("InOut16mA")]
        rows = [f"{short(c)} {k} {e}" for c in cells for k in res for e in ("rise", "fall")]
        series = [("PDK library (characterised to 1-5 ns edges), extrapolated", GREY, "d"), ("simulated", BLUE, "o")]
        p = svgplot.Dumbbell(f"{name}: pin to core on a 15 ns edge", "ns, pin 50 % to core 50 %", rows, series)
        for c in cells:
            for k, r in res.items():
                D = r["cells"][c]["input"]
                a = D["edges"].index(15)
                for e in ("rise", "fall"):
                    row = f"{short(c)} {k} {e}"
                    p.set(row, series[0][0], lookup(D["pdk"]["cell_" + e], 15, D["loads"][0]))
                    p.set(row, series[1][0], D[e]["d50"][a][0])
        p.vline(0, "the pin's 50 %")
        panels.append(p)
    svgplot.write_panels(os.path.join(OUT, "inputs.svg"), panels,
                         title="Inputs: on 15 ns edges the libraries were up to 4 ns off, both ways")


def ttl():
    rows, vals = [], []
    for pdk, name in PADS.items():
        res = load(pdk)
        first = next(iter(res.values()))["cells"]
        cells = [c for c, C in first.items() if "input" in C and not c.endswith("bi_t") and not c.endswith("InOut16mA")]
        for c in cells:
            for k, r in res.items():
                dc = r["cells"][c]["dc"]
                row = f"{name} {short(c)} {k} ({r['vio']:g} V)"
                rows.append(row)
                vals.append((row, dc["vt_fall"], dc["vt_rise"], r["vio"] / 2))
    series = [("switches low (falling input)", BLUE, "v"), ("switches high (rising input)", RED, "^"),
              ("half the supply: where STA measures from", GREY, "d")]
    p = svgplot.Dumbbell("Every input pad, every corner: where it switches (2 us ramps)", "V at the pin", rows, series,
                         xlim=(0, 3.0), band=(VIL, VIH, "TTL: low below 0.8 V, high above 2.0 V"))
    for row, lo, hi, half in vals:
        p.set(row, series[0][0], lo)
        p.set(row, series[1][0], hi)
        p.set(row, series[2][0], half)
    svgplot.write_panels(os.path.join(OUT, "ttl.svg"), [p],
                         title="TTL levels: every input switches inside 0.8-2.0 V, as the NMOS Z80's must")


def table(path):
    """{parameter: (symbol, worst slack)} from pin_timing.py's output"""
    out = {}
    for l in open(path):
        f = l.split()
        if f and f[0].isdigit() and len(f) >= 8:
            try:
                out[int(f[0])] = (f[1], float(f[-1].replace("+", "")) if f[-1] not in ("n/a",) else None)
            except ValueError:
                pass
    return out


def timing():
    panels = []
    series = [("PDK pad libraries, every pin at 50 %", GREY, "d"),
              ("simulated pads, at the datasheet's levels", GREEN, "o")]
    for chip, name in CHIPS.items():
        fa = os.path.join(TIMING_DIR, f"rz80-{chip}-chip-pin_timing-pdk.txt")
        fb = os.path.join(TIMING_DIR, f"rz80-{chip}-chip-pin_timing.txt")
        if not (os.path.exists(fa) and os.path.exists(fb)):
            continue
        a, b = table(fa), table(fb)
        worst = {pid: min(x for x in (a[pid][1], b.get(pid, a[pid])[1]) if x is not None)
                 for pid in a if a[pid][1] is not None or b.get(pid, (0, None))[1] is not None}
        if not worst:
            continue
        tight = sorted(worst, key=worst.get)[:12]
        rows = [f"{pid} {a[pid][0]}" for pid in sorted(tight)]
        p = svgplot.Dumbbell(f"{name} chip: its 12 tightest margins at 4 MHz (worst corner)", "ns of margin", rows,
                             series)
        for pid in tight:
            row = f"{pid} {a[pid][0]}"
            p.set(row, series[0][0], a[pid][1])
            if pid in b:
                p.set(row, series[1][0], b[pid][1])
        p.vline(0, "the limit")
        panels.append(p)
    if panels:
        svgplot.write_panels(os.path.join(OUT, "timing.svg"), panels,
                             title="The pin-timing checks: all three chips pass either way; the input holds were the libraries'")


def main():
    global OUT, PADS_DIR, TIMING_DIR
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pads", default=PADS_DIR, help="where <pdk>/pads.json are")
    ap.add_argument("--timing", default=TIMING_DIR, help="where the chips' pin_timing outputs are")
    ap.add_argument("--out", default=OUT)
    a = ap.parse_args()
    OUT, PADS_DIR, TIMING_DIR = a.out, a.pads, a.timing
    os.makedirs(OUT, exist_ok=True)
    outputs()
    inputs()
    ttl()
    timing()
    made = [f for f in ("outputs.svg", "inputs.svg", "ttl.svg", "timing.svg") if os.path.exists(os.path.join(OUT, f))]
    print(f"findings: {OUT}/ {' '.join(made)}")


if __name__ == "__main__":
    main()
