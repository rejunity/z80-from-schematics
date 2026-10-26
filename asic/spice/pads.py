#!/usr/bin/env python3
"""pads.py - the chip's I/O pads at transistor level, for its pin timing.

z80_chip's pins drive the NMOS Z80's 100 pF test load and take 15 ns
clock and input edges, but the PDKs' pad libraries stop at 30 pF (GF180
bi_t) or 20 pF (IHP) and at 1 ns (GF180) or 5 ns (IHP) input edges:
beyond that STA extrapolates. This simulates every pad cell the chip uses
-- the PDK's own transistor netlist and SPICE models, in ngspice -- at
the process corners of the chip's STA, configured the way z80_chip.v
ties it, and measures

  outputs  data and output enable to the pin, into 0-250 pF: delay at
           50 % (as the Liberty) and at 1.5 V (the datasheet's VOH =
           VOL), 10-90 % transition; the float delay (the driver letting
           go of the pin) and the time the pin needs to move 0.5 V (the
           datasheet's float level) against a 2 kohm pull
  inputs   pin to core with edges up to 120 ns (past the slowest edge
           an output makes into 250 pF: a bidirectional pad's input sees
           its own output's), and where the input switches on a slow
           ramp (the datasheet's VIL / VIH: 0.8 / 2.0 V)
  DC       the output's current at 0.4 V and 2.4 V: the NMOS part sinks
           2.0 mA at 0.4 V and sources 250 uA at 2.4 V
  energy   in each of those runs the energy the core and the I/O supply
           deliver for each edge, and each state's leakage: the Liberty
           internal and leakage power (the PDKs' tables stop at 30 / 20 pF
           too, and STA extrapolates them to negative energies)

and writes to OUT (default pads/<pdk>/ beside this script):

  <pad library>.lib  per corner, the PDK's pad Liberty with those cells'
                     arcs re-measured (tables to 250 pF and 120 ns input
                     edges; 50 % delays and 10-90 % slews, as the PDK's),
                     that range the pins' max_transition / max_capacitance,
                     their internal and leakage power from the same runs;
                     ../timing/pin_timing.py uses them in place of the PDK's
  pads.json          every measurement
  *.svg              waveforms into 100 pF, delay against load and input
                     edge (SPICE against the PDK library's extrapolation)

usage: pads.py PDK [--out DIR] [--corners tt,ss] [--jobs N] [--keep DIR]
                   [--ngspice PATH] [--openvaf PATH] [--pdk-root DIR]
       PDK: gf180mcu (the 7- and 9-track chips share its pads) | ihp-sg13g2
"""
import argparse, bisect, concurrent.futures, glob, json, os, re, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import svgplot  # noqa: E402

# pF on the pin; from 1 pF, as a pin always carries some (with none at all
# GF180's slow corner makes ngspice's timestep collapse)
LOADS = [1, 10, 20, 50, 100, 150, 200, 250]
# ns, added to the PDK's input-edge index: to past the slowest edge the
# outputs make into LOADS[-1] (out_edges_covered), as a bidirectional pad's
# input sees its own output's edges
IN_EDGES = [10, 15, 20, 30, 60, 120]
TEST_LOAD = 100.0                             # pF, PS0178's AC test load
OUT_LEVEL = 1.5                               # V, PS0178's AC test conditions: VOH = VOL = 1.5 V
VIL, VIH = 0.8, 2.0                           # V, input levels (PS0178)
FLOAT_DV = 0.5                                # V, PS0178's float level
FLOAT_R = 2000.0                              # ohm, pull to the opposite rail while floating
VOL_TEST, VOH_TEST = 0.4, 2.4                 # V; Z8400: 2.0 mA at VOL, -250 uA at VOH
NMOS_IOL, NMOS_IOH = 2.0, 0.25                # mA

# ------------------------------------------------------------- the PDKs
# corners: STA corner name (as pin_timing.py prints it) -> (Liberty corner,
# model corner, temperature, core supply, I/O supply). cells: the pad cells
# z80_chip.v uses, with their pins tied as there; out = (data, enable,
# pad), inp = (pad, core-side output).
PDKS = {
    "gf180mcu": {
        "root": "gf180mcu/versions/54435919abffb937387ec956209f9cf5fd2dfbee/gf180mcuD",
        "netlist": "libs.ref/gf180mcu_fd_io/spice/gf180mcu_fd_io.spice",
        "liberty": "libs.ref/gf180mcu_fd_io/lib/gf180mcu_fd_io__{}.lib",
        "corners": {"tt": ("tt_025C_5v00", "typical", 25, 5.0, 5.0),
                    "ss": ("ss_125C_4v50", "ss", 125, 4.5, 4.5),
                    "ff": ("ff_n40C_5v50", "ff", -40, 5.5, 5.5)},
        "supplies": {"VDD": "vdd", "DVDD": "vio", "VSS": 0, "DVSS": 0},
        "cells": {
            # Z80_PAD_DRIVE: the weakest drive, 4 mA (PDRV1:PDRV0 = 00), slow
            # slew; CMOS (not Schmitt) input; no pull-up / pull-down
            "gf180mcu_fd_io__bi_t": {
                "ports": "A CS DVDD DVSS IE OE PAD PD PDRV0 PDRV1 PU SL VDD VSS Y",
                "ties": {"CS": 0, "PU": 0, "PD": 0, "PDRV0": 0, "PDRV1": 0, "SL": 1},
                "out": ("A", "OE", "PAD"), "out_ties": {"IE": 0},
                "inp": ("PAD", "Y"), "in_ties": {"IE": 1, "OE": 0, "A": 0}},
            "gf180mcu_fd_io__in_c": {"ports": "DVDD DVSS PAD PD PU VDD VSS Y",
                                     "ties": {"PU": 0, "PD": 0}, "inp": ("PAD", "Y")},
            "gf180mcu_fd_io__in_s": {"ports": "DVDD DVSS PAD PD PU VDD VSS Y",
                                     "ties": {"PU": 0, "PD": 0}, "inp": ("PAD", "Y")},
        },
    },
    "ihp-sg13g2": {
        "root": "ihp-sg13g2/versions/7bbb4b5a5b64c446fedc64eacadc33fa65ade276/ihp-sg13g2",
        "netlist": "libs.ref/sg13g2_io/spice/sg13g2_io.spice",
        "liberty": "libs.ref/sg13g2_io/lib/sg13g2_io_{}.lib",
        "corners": {"typ": ("typ_1p2V_3p3V_25C", "tt", 25, 1.2, 3.3),
                    "slow": ("slow_1p08V_3p0V_125C", "ss", 125, 1.08, 3.0),
                    "fast": ("fast_1p32V_3p6V_m40C", "ff", -40, 1.32, 3.6)},
        "supplies": {"vdd": "vdd", "iovdd": "vio", "vss": 0, "iovss": 0},
        "cells": {
            "sg13g2_IOPadInOut16mA": {"ports": "pad c2p p2c c2p_en vdd vss iovdd iovss",
                                      "out": ("c2p", "c2p_en", "pad"),
                                      "inp": ("pad", "p2c"), "in_ties": {"c2p_en": 0, "c2p": 0}},
            "sg13g2_IOPadTriOut16mA": {"ports": "pad c2p c2p_en vdd vss iovdd iovss",
                                       "out": ("c2p", "c2p_en", "pad")},
            "sg13g2_IOPadOut16mA": {"ports": "pad c2p vdd vss iovdd iovss", "out": ("c2p", None, "pad")},
            "sg13g2_IOPadIn": {"ports": "pad p2c vdd vss iovdd iovss", "inp": ("pad", "p2c")},
        },
    },
}


def models(pdk, root, build, mc):
    """The model includes of one corner."""
    if pdk == "gf180mcu":
        t = f"{root}/libs.tech/ngspice/"
        return [f".include {t}design.ngspice", f".lib {t}sm141064.ngspice {mc}"] + \
               [f".lib {t}sm141064.ngspice {k}_{mc}" for k in ("diode", "res", "moscap")]
    m = f"{root}/libs.tech/ngspice/models/"
    res = {"tt": "res_typ", "ss": "res_wcs", "ff": "res_bcs"}[mc]
    # diodes.lib with the antenna diodes' transit time zeroed (setup()); the
    # substrate (sub!) is one node, as in IHP's own netlists
    return [f".lib {m}cornerMOSlv.lib mos_{mc}", f".lib {m}cornerMOShv.lib mos_{mc}",
            f".lib {m}cornerRES.lib {res}", f".include {build}/diodes.lib",
            f".include {m}sg13g2_esd.lib", ".global sub!"]


def setup(pdk, root, build, openvaf):
    """The run directory: .spiceinit (ngspice single-threaded: its OpenMP
    device loading makes GF180's hundreds of binned models ~1000x slower),
    for IHP the compiled Verilog-A models and the diode library."""
    os.makedirs(build, exist_ok=True)
    init = ["set num_threads=1"]
    if pdk == "ihp-sg13g2":
        va, osdi = f"{root}/libs.tech/verilog-a", os.path.join(build, "osdi")
        os.makedirs(osdi, exist_ok=True)
        for m in ("psp103/psp103", "psp103/psp103_nqs", "r3_cmc/r3_cmc", "mosvar/mosvar"):
            out = os.path.join(osdi, os.path.basename(m) + ".osdi")
            if not os.path.exists(out):
                run([openvaf, "-D__NGSPICE__", "-o", out, f"{m}.va"], cwd=va)
            init.append(f"osdi '{out}'")
        # IHP's antenna diodes (darea, dperim) carry a 0.7 us transit time.
        # The clamps' gate diodes stay reverse-biased, but that diffusion
        # charge makes ngspice's timestep collapse ("timestep too small")
        # as the output switches; without it they keep their capacitance and
        # leakage (dropping them changes no delay by 0.01 ns).
        src = open(f"{root}/libs.tech/ngspice/models/diodes.lib").read()
        assert src.count("tt=7.009E-007") == 2, "IHP diodes.lib changed"
        open(os.path.join(build, "diodes.lib"), "w").write(src.replace("tt=7.009E-007", "tt=0"))
    open(os.path.join(build, ".spiceinit"), "w").write("\n".join(init) + "\n")


def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if p.returncode != 0:
        sys.exit(f"pads: {' '.join(cmd)} failed:\n{(p.stderr or p.stdout)[-1500:]}")
    return p


# The simulators, pinned: ngspice and OpenVAF from one nixpkgs, and Magic as
# LibreLane runs it for the chips' sign-off (chip.py, nmi.py). Other versions
# move results by tenths of a ns: Magic 8.3.681 extracts the cells'
# parasitics differently from 8.3.623, which moved nmi.py's clear by 0.3 ns.
NIXPKGS = "github:NixOS/nixpkgs/494ce7fd23ff6a5dff39e1fb11e9b6f2ac74bf25"   # ngspice 45, OpenVAF 24.0.1
LIBRELANE = "github:librelane/librelane/3.0.14"                             # Magic 8.3.623
TOOLS = {"ngspice": f"{NIXPKGS}#ngspice", "openvaf-r": f"{NIXPKGS}#openvaf", "magic": f"{LIBRELANE}#magic"}


def tool(name, given=None, who="pads"):
    """A program: given (its --flag), else the pinned build (nix, built
    once), else whatever PATH has."""
    if given:
        return given
    nix = shutil.which("nix") or os.path.expanduser("~/.nix-profile/bin/nix")
    if os.path.exists(nix):
        p = subprocess.run([nix, "build", TOOLS[name], "--print-out-paths", "--no-link"],
                           capture_output=True, text=True)
        path = os.path.join(p.stdout.strip().splitlines()[-1], "bin", name) if p.returncode == 0 and p.stdout.strip() else ""
        if os.path.exists(path):
            return path
    if shutil.which(name):
        return shutil.which(name)
    sys.exit(f"{who}: {name} not found ({TOOLS[name]} with nix, or PATH)")


# --------------------------------------------------------- the testbenches
class Bench:
    """One ngspice run of one cell at one corner."""

    def __init__(self, ctx, corner, cell, name):
        self.ctx, self.corner, self.cell, self.name = ctx, corner, cell, name
        lib, self.mc, self.temp, self.vdd, self.vio = ctx["pdk"]["corners"][corner]
        self.cfg = ctx["pdk"]["cells"][cell]
        self.lines, self.ties = [], dict(self.cfg.get("ties", {}))
        self.pad_node = None

    def v(self, rail):
        return {"vdd": self.vdd, "vio": self.vio}.get(rail, rail)

    def pwl(self, node, pts, rail="vdd"):
        V = self.vdd if rail == "vdd" else self.vio
        self.lines.append(f"V_{node} {node} 0 PWL(" + " ".join(f"{t:.4f}n {v * V:.4f}" for t, v in pts) + ")")

    def pulse(self, node, t1, t2, slew, rail="vdd", start=0):
        """start -> other level at t1, back at t2; slew is 10-90 %"""
        r, a, b = slew / 0.8, start, 1 - start
        self.pwl(node, [(0, a), (t1, a), (t1 + r, b), (t2, b), (t2 + r, a)], rail)

    def deck(self, vectors, tstop, tmax, analysis=None):
        c = self.ctx
        L = [f"* {self.cell} {self.name} at {self.corner}"]
        L += models(c["pdk_name"], c["root"], c["build"], self.mc)
        L += [f".include {c['root']}/{c['pdk']['netlist']}", f".temp {self.temp}"]
        for pin, rail in c["pdk"]["supplies"].items():
            L.append(f"V_{pin} {pin} 0 {self.v(rail) if rail else 0}")
        for pin, val in self.ties.items():
            L.append(f"V_{pin} {pin} 0 {self.vdd * val}")
        L += self.lines
        ports = self.cfg["ports"].split()
        pad = (self.cfg.get("out") or self.cfg.get("inp"))[-1 if "out" in self.cfg else 0]
        if self.pad_node:          # a 0 V probe between the cell and the pin
            ports = [self.pad_node if p == pad else p for p in ports]
            L.append(f"V_probe {self.pad_node} {pad} 0")
        L.append(f"X1 {' '.join(ports)} {self.cell}")
        an = analysis or f"tran {tmax / 2:.5f}n {tstop:.3f}n 0 {tmax:.5f}n"
        L += [".control", "set wr_singlescale", "set wr_vecnames", "option numdgt=9", an,
              f"wrdata {self.name}.dat {' '.join(vectors)}", "quit", ".endc", ".end"]
        return "\n".join(L) + "\n"


def simulate(ctx, bench, vectors, tstop=None, tmax=None, analysis=None):
    d = os.path.join(ctx["build"], bench.corner)      # with its .spiceinit (characterise)
    sp = os.path.join(d, bench.name + ".sp")
    open(sp, "w").write(bench.deck(vectors, tstop, tmax, analysis))
    p = subprocess.run([ctx["ngspice"], "-b", sp], cwd=d, capture_output=True, text=True)
    open(os.path.join(d, bench.name + ".log"), "w").write(p.stdout + p.stderr)
    dat = os.path.join(d, bench.name + ".dat")
    if p.returncode != 0 or not os.path.exists(dat):
        raise RuntimeError(f"ngspice failed: {sp}\n" + (p.stdout + p.stderr)[-800:])
    if "simulation(s) aborted" in p.stdout + p.stderr:
        raise RuntimeError(f"ngspice aborted: {sp}\n" + re.findall(r"[^\n]*too small[^\n]*|$", p.stdout + p.stderr)[0])
    rows = [l.split() for l in open(dat) if l.strip()]
    head, rows = rows[0], [[float(x) for x in r] for r in rows[1:]]
    return Waves({h.lower(): [r[i] for r in rows] for i, h in enumerate(head)})


class Waves(dict):
    """wrdata's columns by vector name; ngspice lowercases node names"""
    def __getitem__(self, k):
        return dict.__getitem__(self, k.lower())

    def __contains__(self, k):
        return dict.__contains__(self, k.lower())


def cross(t, v, level, rising, after=0.0):
    """First time after `after` at which v passes level upwards (rising) or
    downwards, linearly interpolated; None if it does not."""
    if after is None:
        return None
    i = max(bisect.bisect_left(t, after), 1)
    for k in range(i, len(t)):
        a, b = v[k - 1] - level, v[k] - level
        if (rising and a < 0 <= b) or (not rising and a > 0 >= b):
            return t[k - 1] + (t[k] - t[k - 1]) * (-a) / (b - a) if b != a else t[k]
    return None


def value_at(t, v, x):
    """v at time x, linearly interpolated"""
    i = min(max(bisect.bisect_left(t, x), 1), len(t) - 1)
    f = (x - t[i - 1]) / (t[i] - t[i - 1]) if t[i] != t[i - 1] else 0.0
    return v[i - 1] + f * (v[i] - v[i - 1])


def supply_vectors(b):
    """the current vectors of the cell's supplies, core (vdd) and I/O (vio)"""
    return [f"i(v_{pin})" for pin, rail in b.ctx["pdk"]["supplies"].items() if rail]


def energy(b, t, w, ta, tb, node, cap, rail, what, pull=None):
    """The energy (pJ) the core and the I/O supply each deliver from ta to
    tb (ns), and how far `node` rises from ta (dv, V: what charges its
    load). Summed by the trapezoid over ngspice's own time points: under its
    trapezoidal integration that is the capacitors' charge exactly, and the
    supplies' currents ring in sign from one point to the next (the
    method's ringing), which cancels only over whole steps -- so no point
    between them, nor a 'quiescent' current read at one (leakage, nW here,
    is measured apart). Settled: beyond what charges `cap` (pF) on `node`
    from `rail`, the energy moves by under 1 pJ / 1 % when the window is cut
    to 85 %. pull: ohms from `node` to ground, the bench's (not the cell's),
    whose current the I/O supply delivers while it drives the node high:
    taken out."""
    k0 = max(bisect.bisect_right(t, ta) - 1, 0)
    k1 = min(bisect.bisect_left(t, tb), len(t) - 1)
    k85 = min(bisect.bisect_left(t, ta + 0.85 * (tb - ta)), k1)
    v = w[f"v({node})"]
    peak = lambda k: max(v[k0:k + 1]) - v[k0]
    out, total = {"t_ns": t[k1] - t[k0], "dv": peak(k1)}, [0.0, 0.0]
    for pin, r in b.ctx["pdk"]["supplies"].items():
        if not r:
            continue
        i, V = w[f"i(v_{pin})"], b.v(r)
        acc, e85 = 0.0, None
        for k in range(k0, k1):
            acc += (t[k + 1] - t[k]) * (i[k] + i[k + 1]) / 2
            if k + 1 == k85:
                e85 = acc
        if pull and r == "vio":       # V ns / ohm = A ns
            pv = sum((t[k + 1] - t[k]) * (v[k] + v[k + 1]) / 2 / pull for k in range(k0, k1))
            pv85 = sum((t[k + 1] - t[k]) * (v[k] + v[k + 1]) / 2 / pull for k in range(k0, k85))
            acc, e85 = acc + pv, (e85 if e85 is not None else acc) + pv85
        # into the source's + terminal: a supply that delivers is negative; A ns V -> pJ
        out[f"e_{'core' if r == 'vdd' else 'io'}_pJ"] = -V * 1e3 * acc
        total[0] += -V * 1e3 * acc
        total[1] += -V * 1e3 * (e85 if e85 is not None else acc)
    full, cut = total[0] - b.v(rail) * cap * peak(k1), total[1] - b.v(rail) * cap * peak(k85)
    if abs(full - cut) > max(1.0, 0.01 * abs(full)):
        raise RuntimeError(f"{what}: the supplies' energy has not settled ({cut:.2f} pJ at 85 % of the "
                           f"window, {full:.2f} pJ at its end; see {b.name}.log)")
    return out


def window(load, slew):
    """ns to let one output edge settle: the slow GF180 corner into 250 pF
    takes ~170 ns"""
    return 20 + 0.8 * load + 2 * slew


# Each measurement returns plain numbers in ns / V / mA (None: the edge
# never came).
def m_out(ctx, corner, cell, slew, load):
    b = Bench(ctx, corner, cell, f"out_{cell}_{slew:g}_{load:g}")
    data, en, pad = b.cfg["out"]
    b.ties.update(b.cfg.get("out_ties", {}))
    if en:
        b.ties[en] = 1
    W = window(load, slew)
    t1, t2 = 2.0, 2.0 + W
    b.pulse(data, t1, t2, slew)
    b.lines.append(f"C_load {pad} 0 {load}p")
    w = simulate(ctx, b, [f"v({data})", f"v({pad})"] + supply_vectors(b), t2 + W, max(0.01, min(0.4, W / 600)))
    t, vd, vp = w["time"], w[f"v({data})"], w[f"v({pad})"]
    t = [x * 1e9 for x in t]
    if t[-1] < t2 + W - 1:
        raise RuntimeError(f"ngspice stopped at {t[-1]:.2f} ns (see {b.name}.log)")
    res = {}
    for edge, rising, after, tb in (("rise", True, t1 - 0.5, t2 - 0.5), ("fall", False, t2 - 0.5, t[-1])):
        t0 = cross(t, vd, b.vdd / 2, rising, after)
        res[edge] = edge_out(t, vp, b.vio, rising, t0)
        # the supplies' energy for the edge, from before the data input moves
        # to before the next edge (rising: beyond the load's charge)
        res[edge].update(energy(b, t, w, after, tb, pad, load + ctx["cpin"][cell], "vio", f"{b.name} {edge}"))
    if load == TEST_LOAD and abs(slew - ctx["mid_slew"][cell]) < 1e-9:
        res["wave"] = wave(t, vp, [cross(t, vd, b.vdd / 2, True, t1 - 0.5), cross(t, vd, b.vdd / 2, False, t2 - 0.5)], W)
    return res


def edge_out(t, vp, vio, rising, t0):
    lo, hi = (0.1, 0.9) if rising else (0.9, 0.1)
    c50 = cross(t, vp, vio / 2, rising, t0)
    clev = cross(t, vp, OUT_LEVEL, rising, t0)
    a, z = cross(t, vp, lo * vio, rising, t0), cross(t, vp, hi * vio, rising, t0)
    sub = lambda x, y: None if x is None or y is None else x - y
    return {"d50": sub(c50, t0), "dlev": sub(clev, t0), "slew": sub(z, a)}


def wave(t, v, starts, W, n=240):
    """the pin's waveform after each edge, n points, time from the edge"""
    out = []
    for t0 in starts:
        if t0 is None:
            out.append(None)
            continue
        pts = [(t0 + W * k / n, None) for k in range(n + 1)]
        res = []
        for tk, _ in pts:
            i = min(max(bisect.bisect_left(t, tk), 1), len(t) - 1)
            f = (tk - t[i - 1]) / (t[i] - t[i - 1]) if t[i] != t[i - 1] else 0
            res.append((round(tk - t0, 3), round(v[i - 1] + f * (v[i] - v[i - 1]), 4)))
        out.append(res)
    return out


def sample(t, cols, start, span, n=320):
    """cols ({name: values}) at n + 1 evenly spaced times from start, as
    {"t": ns after start, name: values} for the plots"""
    out = {"t": [round(span * k / n, 3) for k in range(n + 1)]}
    for name, v in cols.items():
        vals = []
        for k in range(n + 1):
            tk = start + span * k / n
            i = min(max(bisect.bisect_left(t, tk), 1), len(t) - 1)
            f = (tk - t[i - 1]) / (t[i] - t[i - 1]) if t[i] != t[i - 1] else 0
            vals.append(round(v[i - 1] + f * (v[i] - v[i - 1]), 4))
        out[name] = vals
    return out


def m_enable(ctx, corner, cell, slew, load, val):
    """Turn-on: the pin floats (held at the other level by 1 Mohm), the
    enable rises with the data at val."""
    b = Bench(ctx, corner, cell, f"en{val}_{cell}_{slew:g}_{load:g}")
    data, en, pad = b.cfg["out"]
    b.ties.update(b.cfg.get("out_ties", {}))
    b.ties[data] = val
    W = window(load, slew)
    t1 = 2.0
    b.pulse(en, t1, t1 + W, slew)
    b.lines += [f"C_load {pad} 0 {load}p", f"R_hold {pad} {'0' if val else 'vio_node'} 1Meg",
                f"V_vio_node vio_node 0 {b.vio}"]
    w = simulate(ctx, b, [f"v({en})", f"v({pad})"] + supply_vectors(b), t1 + W, max(0.01, min(0.4, W / 600)))
    t = [x * 1e9 for x in w["time"]]
    t0 = cross(t, w[f"v({en})"], b.vdd / 2, True, t1 - 0.5)
    res = edge_out(t, w[f"v({pad})"], b.vio, bool(val), t0)
    # the supplies' energy turning on (the 1 Mohm's draw taken out)
    res.update(energy(b, t, w, t1 - 0.5, t[-1], pad, load + ctx["cpin"][cell], "vio", b.name, 1e6 if val else None))
    if load == TEST_LOAD and abs(slew - ctx["mid_slew"][cell]) < 1e-9 and t0 is not None:
        res["wave"] = sample(t, {"en": w[f"v({en})"], "pad": w[f"v({pad})"]}, t0 - 5, W - 5)
    return res


def m_release(ctx, corner, cell, slew, val):
    """Letting go, for the supplies' energy: the pin driven to val, the test
    load and 1 Mohm to the other level on it (as m_enable's), the enable
    falls. The pin floats and does not move -- what it drifts on the
    1 Mohm no supply feeds -- so the load hardly matters (IHP's pads: under
    1 pJ from 1 to 250 pF): this one stands for every load. (Its own run:
    in m_enable's, letting a 1 pF pin go stalls ngspice's timestep at
    GF180's slow corner.)"""
    b = Bench(ctx, corner, cell, f"rel{val}_{cell}_{slew:g}")
    data, en, pad = b.cfg["out"]
    b.ties.update(b.cfg.get("out_ties", {}))
    b.ties[data] = val
    t1, W = 2.0, 40.0
    b.pulse(en, t1, t1 + 10 * W, slew, start=1)
    b.lines += [f"C_load {pad} 0 {TEST_LOAD}p", f"R_hold {pad} {'0' if val else 'vio_node'} 1Meg",
                f"V_vio_node vio_node 0 {b.vio}"]
    w = simulate(ctx, b, [f"v({en})", f"v({pad})"] + supply_vectors(b), t1 + W, 0.05)
    t = [x * 1e9 for x in w["time"]]
    if t[-1] < t1 + W - 1:
        raise RuntimeError(f"ngspice stopped at {t[-1]:.2f} ns (see {b.name}.log)")
    return energy(b, t, w, t1 - 0.5, t[-1], pad, 0.0, "vio", b.name)


def m_disable(ctx, corner, cell, slew, val):
    """Float: the pin driven to val, 100 pF and FLOAT_R to the other rail;
    the enable falls. The driver has let go when the current it delivers
    is under 10 % of what it was; the pin has 'floated' (PS0178) when it
    has moved FLOAT_DV."""
    b = Bench(ctx, corner, cell, f"off{val}_{cell}_{slew:g}")
    data, en, pad = b.cfg["out"]
    b.ties.update(b.cfg.get("out_ties", {}))
    b.ties[data] = val
    t1, W = 5.0, 80.0
    b.pulse(en, t1, t1 + 2 * W, slew, start=1)
    b.pad_node = "pad_cell"
    b.lines += [f"C_load {pad} 0 {TEST_LOAD}p", f"R_float {pad} {'0' if val else 'vio_node'} {FLOAT_R}",
                f"V_vio_node vio_node 0 {b.vio}"]
    w = simulate(ctx, b, [f"v({en})", f"v({pad})", "i(v_probe)"], t1 + W, 0.05)
    t = [x * 1e9 for x in w["time"]]
    i = [x * 1e3 for x in w["i(v_probe)"]]
    t0 = cross(t, w[f"v({en})"], b.vdd / 2, False, t1 - 0.5)
    k0 = bisect.bisect_left(t, t1 - 0.2)
    i0, v0 = i[k0], w[f"v({pad})"][k0]
    toff = cross(t, [abs(x) for x in i], 0.1 * abs(i0), False, t0)
    tfl = cross(t, w[f"v({pad})"], v0 + (FLOAT_DV if not val else -FLOAT_DV), not val, t0)
    sub = lambda x, y: None if x is None or y is None else x - y
    res = {"toff": sub(toff, t0), "tfloat": sub(tfl, t0), "i0_mA": abs(i0), "v0": v0}
    if abs(slew - ctx["mid_slew"][cell]) < 1e-9 and t0 is not None:
        res["wave"] = sample(t, {"en": w[f"v({en})"], "pad": w[f"v({pad})"], "i_mA": i}, t0 - 5, 70)
    return res


def m_input(ctx, corner, cell, slew, load):
    b = Bench(ctx, corner, cell, f"in_{cell}_{slew:g}_{load:g}")
    pad, y = b.cfg["inp"]
    b.ties.update(b.cfg.get("in_ties", {}))
    W = 10 + 1.5 * slew
    t1, t2 = 2.0, 2.0 + W
    b.pulse(pad, t1, t2, slew, rail="vio")
    b.lines.append(f"C_y {y} 0 {load}p")
    w = simulate(ctx, b, [f"v({pad})", f"v({y})"] + supply_vectors(b), t2 + W, max(0.005, min(0.1, W / 800)))
    t = [x * 1e9 for x in w["time"]]
    vp, vy = w[f"v({pad})"], w[f"v({y})"]
    res = {}
    for edge, rising, after, tb in (("rise", True, t1 - 0.5, t2 - 0.5), ("fall", False, t2 - 0.5, t[-1])):
        t0 = cross(t, vp, b.vio / 2, rising, after)
        ty = cross(t, vy, b.vdd / 2, rising, t0 - slew)
        lo, hi = (0.1, 0.9) if rising else (0.9, 0.1)
        a, z = cross(t, vy, lo * b.vdd, rising, t0 - slew), cross(t, vy, hi * b.vdd, rising, t0 - slew)
        # the pin's voltage when the core side switches
        vsw = None
        if ty is not None:
            k = min(max(bisect.bisect_left(t, ty), 1), len(t) - 1)
            vsw = vp[k]
        res[edge] = {"d50": None if ty is None else ty - t0, "slew": None if a is None or z is None else z - a,
                     "vsw": vsw}
        res[edge].update(energy(b, t, w, after, tb, y, load, "vdd", f"{b.name} {edge}"))
        if slew == 15 and load == ctx["min_yload"][cell] and t0 is not None:
            res[edge]["wave"] = sample(t, {"pad": vp, "y": vy}, t0 - 15, 35)
    return res


def m_dc_in(ctx, corner, cell):
    """Where the input switches: a 2 us ramp up and down."""
    b = Bench(ctx, corner, cell, f"dcin_{cell}")
    pad, y = b.cfg["inp"]
    b.ties.update(b.cfg.get("in_ties", {}))
    b.pwl(pad, [(0, 0), (100, 0), (2100, 1), (2300, 1), (4300, 0)], rail="vio")
    b.lines.append(f"C_y {y} 0 0.01p")
    w = simulate(ctx, b, [f"v({pad})", f"v({y})"], 4400, 1.0)
    t = [x * 1e9 for x in w["time"]]
    vp, vy = w[f"v({pad})"], w[f"v({y})"]
    res = {}
    k_up, k_top = bisect.bisect_left(t, 100), bisect.bisect_left(t, 2300)
    step = max(1, (k_top - k_up) // 300)
    res["curve"] = {"up": [[round(vp[k], 4), round(vy[k], 4)] for k in range(k_up, bisect.bisect_left(t, 2100), step)],
                    "down": [[round(vp[k], 4), round(vy[k], 4)] for k in range(k_top, len(t), step)]}
    for name, rising, after in (("vt_rise", True, 100), ("vt_fall", False, 2300)):
        ty = cross(t, vy, b.vdd / 2, rising, after)
        k = min(max(bisect.bisect_left(t, ty), 1), len(t) - 1) if ty is not None else None
        res[name] = None if k is None else vp[k]
    return res


def m_leak(ctx, corner, cell):
    """Leakage: the supplies' DC power (uW) in each state the chip holds the
    cell in -- driving 0 / 1 (out0 / out1), receiving 0 / 1 (in0 / in1)."""
    cfg, res = ctx["pdk"]["cells"][cell], {}
    states = [(f"out{v}", "out", v) for v in (0, 1) if "out" in cfg] + \
             [(f"in{v}", "inp", v) for v in (0, 1) if "inp" in cfg]
    for name, kind, val in states:
        b = Bench(ctx, corner, cell, f"leak_{name}_{cell}")
        if kind == "out":
            data, en, pad = cfg["out"]
            b.ties.update(cfg.get("out_ties", {}))
            b.ties[data] = val
            if en:
                b.ties[en] = 1
        else:
            pad, y = cfg["inp"]
            b.ties.update(cfg.get("in_ties", {}))
            b.lines.append(f"V_force {pad} 0 {b.vio * val}")
        b.lines.append("V_sweep sweep 0 0")
        w = simulate(ctx, b, supply_vectors(b), analysis="dc v_sweep 0 0 1")
        res[name] = {f"{'core' if r == 'vdd' else 'io'}_uW": -b.v(r) * w[f"i(v_{pin})"][0] * 1e6
                     for pin, r in ctx["pdk"]["supplies"].items() if r}
    return res


def m_dc_out(ctx, corner, cell):
    """The pin forced from 0 V to the I/O supply: the current the output
    sinks at VOL_TEST (data 0) and sources at VOH_TEST (data 1)."""
    res = {}
    for val in (0, 1):
        b = Bench(ctx, corner, cell, f"dcout{val}_{cell}")
        data, en, pad = b.cfg["out"]
        b.ties.update(b.cfg.get("out_ties", {}))
        b.ties[data] = val
        if en:
            b.ties[en] = 1
        b.lines.append(f"V_force {pad} 0 0")
        w = simulate(ctx, b, ["i(v_force)"], analysis=f"dc v_force 0 {b.vio:g} 0.01")
        vs = w["v-sweep"] if "v-sweep" in w else w[list(w)[0]]
        cur = w["i(v_force)"]
        at = lambda v0: next((cur[k - 1] + (cur[k] - cur[k - 1]) * (v0 - vs[k - 1]) / (vs[k] - vs[k - 1])
                              for k in range(1, len(vs)) if vs[k - 1] <= v0 <= vs[k]), None)
        # i(v_force) flows into V_force's + terminal, the pin
        if val == 0:
            res["iol_mA"] = -at(VOL_TEST) * 1e3     # sunk by the pin
        else:
            res["ioh_mA"] = at(VOH_TEST) * 1e3      # sourced by the pin
    return res


# --------------------------------------------------------- the Liberty file
def block_end(s, i):
    j, d = s.index("{", i), 0
    for k in range(j, len(s)):
        if s[k] == "{":
            d += 1
        elif s[k] == "}":
            d -= 1
            if d == 0:
                return k + 1
    raise ValueError("unbalanced Liberty group")


def groups(s, kind, start=0, end=None):
    """(name, begin, end) of the `kind (name) { }` groups in s[start:end],
    not looking inside each one found"""
    end = len(s) if end is None else end
    pat = re.compile(r"\b" + kind + r"\s*\(\s*\"?([^\")]*)\"?\s*\)\s*\{")
    out, i = [], start
    while True:
        m = pat.search(s, i, end)
        if not m:
            return out
        e = block_end(s, m.start())
        out.append((m.group(1).strip(), m.start(), e))
        i = e


def attr(s, name):
    m = re.search(r"\b" + name + r"\s*:\s*\"?([^\";]*)\"?\s*;", s)
    return m.group(1).strip() if m else None


def nums(s, key):
    m = re.search(key + r"\s*\(\s*\"([^\"]*)\"", s)
    return [float(x) for x in m.group(1).replace("\\", "").split(",")] if m else None


def table(s):
    """index_1, index_2 and values (rows) of one table group's text"""
    i1, i2 = nums(s, "index_1"), nums(s, "index_2")
    m = re.search(r"values\s*\((.*?)\)\s*;", s, re.S)
    rows = [[float(x) for x in r.split(",")] for r in re.findall(r"\"([^\"]*)\"", m.group(1))] if m else []
    return i1, i2, rows


def lookup(tab, x1, x2):
    """NLDM lookup as STA does it: bilinear, extrapolated linearly from the
    last two points beyond the table"""
    i1, i2, rows = tab

    def seg(idx, x):
        if len(idx) == 1:
            return 0, 0, 0.0
        k = min(max(bisect.bisect_left(idx, x), 1), len(idx) - 1)
        return k - 1, k, (x - idx[k - 1]) / (idx[k] - idx[k - 1])
    a, b, f = seg(i1, x1)
    c, d, g = seg(i2, x2)
    r = lambda i: rows[i][c] + g * (rows[i][d] - rows[i][c])
    return r(a) + f * (r(b) - r(a))


def when_ok(when, known):
    """the arc's `when` agrees with every pin we tie"""
    for tok in (when or "").replace(" ", "").split("&"):
        if not tok:
            continue
        pin, val = tok.lstrip("!"), not tok.startswith("!")
        if pin in known and bool(known[pin]) != val:
            return False
    return True


def literals(when):
    """a Liberty `when` that is one product (literals joined by &, * or a
    space) as {pin: 0 / 1}; None for anything else"""
    if not when:
        return {}
    if re.search(r"[|+^']", when):
        return None
    lits = {}
    for tok in re.split(r"[&*\s]+", re.sub(r"[()]", " ", when).strip()):
        m = re.fullmatch(r"(!?)(\w+)", tok)
        if not m:
            return None
        lits[m.group(2)] = 0 if m.group(1) else 1
    return lits


def contradicts(when, known):
    """the `when` cannot hold with the pins in `known` tied so"""
    lits = literals(when)
    return lits is not None and any(p in known and bool(known[p]) != bool(v) for p, v in lits.items())


def arcs(lib, cell, cfg):
    """the cell's arcs that z80_chip uses: [(kind, timing group span,
    {table name: (begin, end)})], kind: data / enable / disable / input"""
    found = [g for g in groups(lib, "cell") if g[0] == cell]
    assert found, f"{cell} not in the Liberty file"
    _, cb, ce = found[0]
    out = []
    data, en, pad = cfg.get("out") or (None, None, None)
    ipad, y = cfg.get("inp") or (None, None)
    for pin, pb, pe in groups(lib, "pin", cb, ce):
        for _, tb, te in groups(lib, "timing", pb, pe):
            g = lib[tb:te]
            rel, ttype, when = attr(g, "related_pin"), attr(g, "timing_type") or "combinational", attr(g, "when")
            kind = None
            if pin == pad and rel == data and ttype == "combinational":
                kind, known = "data", dict(cfg.get("ties", {}), **cfg.get("out_ties", {}), **({en: 1} if en else {}))
            elif pin == pad and rel == en and ttype.startswith("three_state_enable"):
                kind, known = "enable", dict(cfg.get("ties", {}), **cfg.get("out_ties", {}))
            elif pin == pad and rel == en and ttype.startswith("three_state_disable"):
                kind, known = "disable", dict(cfg.get("ties", {}), **cfg.get("out_ties", {}))
            elif pin == y and rel == ipad and ttype == "combinational":
                kind, known = "input", dict(cfg.get("ties", {}), **cfg.get("in_ties", {}))
            if kind and when_ok(when, known):
                tabs = {name: (tb + b, tb + e) for name, b, e in
                        [(n, b, e) for n in ("cell_rise", "cell_fall", "rise_transition", "fall_transition")
                         for (_, b, e) in groups(g, n)]}
                out.append((kind, (tb, te), tabs))
    return out


def power_groups(lib, cell, cfg):
    """the cell's internal_power groups that z80_chip's ties select, as
    arcs() the timing: [(kind, val, related_pg_pin, {table: (begin, end)})],
    kind data / enable / input; val: the data input's level an enable group
    holds (None: either)"""
    _, cb, ce = [g for g in groups(lib, "cell") if g[0] == cell][0]
    data, en, pad = cfg.get("out") or (None, None, None)
    ipad, y = cfg.get("inp") or (None, None)
    out = []
    for pin, pb, pe in groups(lib, "pin", cb, ce):
        for _, gb, ge in groups(lib, "internal_power", pb, pe):
            g = lib[gb:ge]
            rel, when = attr(g, "related_pin"), attr(g, "when")
            kind, val, known = None, None, {}
            if pad and pin == pad and rel == data:
                kind, known = "data", dict(cfg.get("ties", {}), **cfg.get("out_ties", {}), **({en: 1} if en else {}))
            elif pad and en and pin == pad and rel == en:
                kind, known = "enable", dict(cfg.get("ties", {}), **cfg.get("out_ties", {}))
                toks = (when or "").replace(" ", "").split("&")
                val = 1 if data in toks else 0 if "!" + data in toks else None
            elif y and pin == y and rel == ipad:
                kind, known = "input", dict(cfg.get("ties", {}), **cfg.get("in_ties", {}))
            if kind and when_ok(when, known):
                tabs = {n: (gb + b, gb + e) for n in ("rise_power", "fall_power") for (_, b, e) in groups(g, n)}
                out.append((kind, val, attr(g, "related_pg_pin"), tabs))
    return out


def internal_energy(d, kind, rail, ctx, corner, cpin):
    """Liberty internal energy (pJ), [rise rows, fall rows], from the
    supplies' measured energy. The PDKs' convention (the rise less the
    load's C V^2 the supply charges it with, the fall as measured; leakage
    apart), with the load as STA takes it: the pin's own capacitance too
    (an output pin's, as in the timing tables), half of its C V^2 off each
    edge (STA's switching power counts 0.5 C V^2 an edge; part of the pin's
    capacitance is to the I/O supply and draws its energy on the fall), so
    that STA's sum -- internal plus switching -- is the simulated energy.
    rail: "vio" / "vdd" for one supply's share (IHP's related_pg_pin),
    None for both (GF180's)."""
    vdd, vio = ctx["pdk"]["corners"][corner][3:5]
    take = lambda m, r: m.get(f"e_{r}_pJ", 0.0)
    e = lambda m: (take(m, "io") if rail in (None, "vio") else 0.0) + (take(m, "core") if rail in (None, "vdd") else 0.0)
    def leak(states):
        P = [sum(v for k, v in d["leak"][st].items() if rail is None or k.startswith("io" if rail == "vio" else "core"))
             for st in states]
        return sum(P) / len(P) * 1e-3                       # uW ns -> pJ
    io_side, core_side = rail in (None, "vio"), rail in (None, "vdd")
    if kind == "input":
        t, lk = d["input"], leak(("in0", "in1"))
        rows = []
        for edge in ("rise", "fall"):
            rows.append([[e(m) - (vdd * L * m["dv"] if core_side else 0.0) - lk * m["t_ns"]
                          for L, m in zip(t["loads"], row)] for row in t[edge]["e"]])
        return rows
    lk = leak(("out0", "out1"))
    pin = 0.5 * cpin * vio * vio if io_side else 0.0
    ld = lambda L, m: vio * L * m["dv"] if io_side else 0.0
    if kind == "data":
        D = d["data"]
        return [[[e(m) - ld(L, m) - pin - lk * m["t_ns"] for L, m in zip(LOADS, row)] for row in D[edge]]
                for edge in ("rise", "fall")]
    # enable: Z -> 1 is the pin rising (turn-on, data 1), Z -> 0 falling
    # (turn-on, data 0); letting go the pin does not move (1 -> Z the
    # fall's table, 0 -> Z the rise's, as the PDK's): its energy as measured
    E = d["enable"]
    def ev(val, which, a, c):
        if which == "off":                  # the pin floats: nothing charges it
            m = E[f"release_{val}"][a]
            return e(m) - lk * m["t_ns"]
        m = E[str(val)][a][c]
        return e(m) - ld(LOADS[c], m) - pin - lk * m["t_ns"]
    n1 = len(E["slews"])
    tab = {(1, "rise"): "on", (1, "fall"): "off", (0, "fall"): "on", (0, "rise"): "off"}
    return {val: [[[ev(val, tab[(val, edge)], a, c) for c in range(len(LOADS))] for a in range(n1)]
                  for edge in ("rise", "fall")] for val in (0, 1)}


def pin_cap(lib, cell, pin):
    _, cb, ce = [g for g in groups(lib, "cell") if g[0] == cell][0]
    _, pb, pe = [g for g in groups(lib, "pin", cb, ce) if g[0] == pin][0]
    c = attr(lib[pb:pe], "capacitance")
    return float(c) if c else 0.0         # IHP's output-only pads give none


def set_attr(lib, b, e, key, val):
    """the edit (begin, end, text) that makes `key : val;` the group
    lib[b:e]'s own attribute: replacing it (not a nested group's), else a
    line of its own after the group's opening brace"""
    pat = re.compile(r"\b" + key + r"\s*:\s*[^;]*;")
    o = lib.index("{", b) + 1
    depth, k = 1, o
    while k < e and depth:
        c = lib[k]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        elif depth == 1 and c == key[0] and not (lib[k - 1].isalnum() or lib[k - 1] == "_"):
            m = pat.match(lib, k)
            if m:
                return m.start(), m.end(), f"{key} : {val};"
        k += 1
    at = lib.index("\n", o) + 1
    indent = re.match(r"[ \t]*", lib[at:]).group(0)
    return at, at, f"{indent}{key} : {val};\n"


def fmt_table(name, template, i1, i2, rows, indent):
    pad = indent + "  "
    vals = (", \\\n" + pad + "       ").join('"' + ", ".join(f"{v:.6f}" for v in r) + '"' for r in rows)
    return (f'{name} ("{template}") {{\n{pad}index_1("{", ".join(f"{x:g}" for x in i1)}");\n'
            f'{pad}index_2("{", ".join(f"{x:g}" for x in i2)}");\n{pad}values({vals});\n{indent}}}')


def write_liberty(src, dst, cells, data, corner, ctx):
    """the PDK's pad library with the used cells' arcs from `data`"""
    lib = open(src).read()
    edits, templates, ptemplates = [], {}, {}
    for cell, cfg in cells.items():
        d = data[cell]
        for kind, (tb, te), tabs in arcs(lib, cell, cfg):
            for name, (b, e) in tabs.items():
                rise = name in ("cell_rise", "rise_transition")
                q = "slew" if "transition" in name else ("toff" if kind == "disable" else "d50")
                if kind == "disable" and q == "slew":
                    continue                   # the PDK's (the pin floats)
                if kind == "input":
                    t = d["input"]
                    i1, i2 = t["edges"], t["loads"]
                    rows = [[t["rise" if rise else "fall"][q][a][c] for c in range(len(i2))] for a in range(len(i1))]
                else:
                    t = d["data" if kind == "data" else "enable" if kind == "enable" else "disable"]
                    i1, i2 = t["slews"], [round(t["cpin"] + c, 6) for c in LOADS]
                    if kind == "disable":     # 0 -> Z is cell_rise, 1 -> Z cell_fall
                        v = t["0" if rise else "1"]
                        rows = [[v[a][q]] * len(i2) for a in range(len(i1))]
                    elif kind == "enable":    # Z -> 1 is cell_rise
                        v = t["1" if rise else "0"]
                        rows = [[v[a][c][q] for c in range(len(LOADS))] for a in range(len(i1))]
                    else:
                        v = t["rise" if rise else "fall"]
                        rows = [[v[a][c][q] for c in range(len(LOADS))] for a in range(len(i1))]
                if any(x is None for r in rows for x in r):
                    sys.exit(f"pads: {cell} {kind} {name} at {corner}: an edge did not come")
                tname = f"spice_{len(i1)}x{len(i2)}_{'in' if kind == 'input' else 'out'}"
                templates.setdefault(tname, (i1, i2))
                indent = re.search(r"([ \t]*)$", lib[:b]).group(1)
                edits.append((b, e, fmt_table(name, tname, i1, i2, rows, indent)))
        # the characterised range as the pins' limits, so that STA flags a
        # pin driven past what was simulated: the pin's edges to the last
        # input edge (past every output edge, out_edges_covered), an output's
        # load to the last load, the core side's output load and data /
        # enable edges to their tables' last points
        dpin, epin, pad = cfg.get("out") or (None, None, None)
        ipad, y = cfg.get("inp") or (None, None)
        limits = {p: {"max_transition": float(max(IN_EDGES))} for p in {pad, ipad} - {None}}
        if pad:
            limits[pad]["max_capacitance"] = round(d["data"]["cpin"] + LOADS[-1], 6)
            for p in {dpin, epin} - {None}:
                limits[p] = {"max_transition": max(d["data"]["slews"])}
        if y:
            limits[y] = {"max_capacitance": max(d["input"]["loads"])}
        _, cb, ce = [g for g in groups(lib, "cell") if g[0] == cell][0]
        for pin, pb, pe in groups(lib, "pin", cb, ce):
            for key, val in sorted(limits.pop(pin, {}).items()):
                edits.append(set_attr(lib, pb, pe, key, f"{val:.6f}".rstrip("0").rstrip(".")))
        assert not limits, f"{cell}: no pin {sorted(limits)} in the Liberty file"
        # internal power: the supplies' energy for each arc simulated
        cpin = d["data"]["cpin"] if "data" in d else 0.0
        for kind, val, pg, tabs in power_groups(lib, cell, cfg):
            rail = ctx["pdk"]["supplies"].get(pg) if pg else None
            if pg and not rail:
                continue                                   # a ground pin's
            if kind == "input":
                i1, i2 = d["input"]["edges"], d["input"]["loads"]
                rows = internal_energy(d, kind, rail, ctx, corner, 0.0)
            else:
                i1, i2 = d["data"]["slews"], [round(cpin + c, 6) for c in LOADS]
                if kind == "data":
                    rows = internal_energy(d, kind, rail, ctx, corner, cpin)
                else:
                    E = internal_energy(d, kind, rail, ctx, corner, cpin)
                    rows = E[val] if val is not None else \
                        [[[(x + y) / 2 for x, y in zip(r0, r1)] for r0, r1 in zip(E[0][k], E[1][k])] for k in (0, 1)]
            tname = f"spice_power_{len(i1)}x{len(i2)}_{'in' if kind == 'input' else 'out'}"
            ptemplates.setdefault(tname, (i1, i2))
            for name, (b, e) in tabs.items():
                indent = re.search(r"([ \t]*)$", lib[:b]).group(1)
                edits.append((b, e, fmt_table(name, tname, i1, i2, rows[0 if name == "rise_power" else 1], indent)))
        # leakage: each state the chip holds the cell in, its groups in
        # place of the PDK's for that state; the cell's the mean
        unit = {"1mW": 1e-3, "1uW": 1.0, "1nW": 1e3, "1pW": 1e6}[attr(lib[:lib.index("cell")], "leakage_power_unit")]
        pgs = sorted({g[0] for g in groups(lib, "pg_pin", cb, ce)})
        states = {}
        for st in d["leak"]:
            v = int(st[-1])
            if st.startswith("out"):
                known = dict(cfg.get("ties", {}), **cfg.get("out_ties", {}), **({epin: 1} if epin else {}), **{dpin: v})
            else:
                known = dict(cfg.get("ties", {}), **cfg.get("in_ties", {}), **{ipad: v})
            states[st] = known
        ties = cfg.get("ties", {})
        for _, lb, le in groups(lib, "leakage_power", cb, ce):
            when = attr(lib[lb:le], "when")
            if contradicts(when, ties) or any(not contradicts(when, k) for k in states.values()):
                edits.append((lb, le, ""))
        # the states the chip's tied pins rule out: their internal power goes
        # too, as STA would mix it in (OpenSTA 2.7 weights an output's groups
        # alike whatever their `when`, and takes a pin tied high to have duty
        # 0), and the PDK's carry its extrapolation (GF180's bi_t at SL = 0:
        # -1.25 nJ an edge at 100 pF)
        for pin, pb, pe in groups(lib, "pin", cb, ce):
            for _, gb, ge in groups(lib, "internal_power", pb, pe):
                if contradicts(attr(lib[gb:ge], "when"), ties):
                    edits.append((gb, ge, ""))
        at = lib.index("\n", lib.index("{", cb)) + 1
        indent = re.match(r"[ \t]*", lib[at:]).group(0)
        text = ""
        for st, known in states.items():
            when = "&".join(("" if x else "!") + p for p, x in sorted(known.items()))
            for pg in (pgs and [g for g in pgs if ctx["pdk"]["supplies"].get(g)]) or [None]:
                r = ctx["pdk"]["supplies"].get(pg) if pg else None
                uw = sum(x for k, x in d["leak"][st].items() if r is None or k.startswith("io" if r == "vio" else "core"))
                text += (f"{indent}leakage_power () {{\n{indent}  when : \"{when}\";\n"
                         + (f"{indent}  related_pg_pin : {pg};\n" if pg else "")
                         + f"{indent}  value : {uw * unit:.6g};\n{indent}}}\n")
        edits.append((at, at, text))
        mean = sum(sum(x.values()) for x in d["leak"].values()) / len(d["leak"])
        edits.append(set_attr(lib, cb, ce, "cell_leakage_power", f"{mean * unit:.6g}"))
    for b, e, text in sorted(edits, reverse=True):
        lib = lib[:b] + text + lib[e:]
    # the templates, after the library's own
    tm = list(re.finditer(r"lu_table_template\s*\(", lib))
    at = block_end(lib, tm[-1].start())
    tpl = "".join(f"\n  lu_table_template ({n}) {{\n    variable_1 : input_net_transition;\n"
                  f"    variable_2 : total_output_net_capacitance;\n"
                  f"    index_1(\"{', '.join(f'{x:g}' for x in i1)}\");\n"
                  f"    index_2(\"{', '.join(f'{x:g}' for x in i2)}\");\n  }}"
                  for n, (i1, i2) in sorted(templates.items()))
    lib = lib[:at] + tpl + lib[at:]
    tm = list(re.finditer(r"power_lut_template\s*\(", lib))
    at = block_end(lib, tm[-1].start())
    tpl = "".join(f"\n  power_lut_template ({n}) {{\n    variable_1 : input_transition_time;\n"
                  f"    variable_2 : total_output_net_capacitance;\n"
                  f"    index_1(\"{', '.join(f'{x:g}' for x in i1)}\");\n"
                  f"    index_2(\"{', '.join(f'{x:g}' for x in i2)}\");\n  }}"
                  for n, (i1, i2) in sorted(ptemplates.items()))
    lib = lib[:at] + tpl + lib[at:]
    _, mc, temp, vdd, vio = ctx["pdk"]["corners"][corner]
    # the supplies' voltages by name, where the pins name their supply but
    # the library maps none (IHP's): STA's switching power for a pin is then
    # its own supply's, the pad pin's the I/O supply's (else the library's
    # one operating voltage, the core's)
    names = sorted(set(re.findall(r"voltage_name\s*:\s*\"?(\w+)\"?\s*;", lib)))
    if names and "voltage_map" not in lib:
        sup = ctx["pdk"]["supplies"]
        at = lib.index("\n", lib.index("{", lib.index("library"))) + 1
        lib = lib[:at] + "".join(f"  voltage_map ({n}, {(vio if sup.get(n) == 'vio' else vdd if sup.get(n) == 'vdd' else 0.0):g});\n"
                                 for n in names) + lib[at:]
    head = (f"/* asic/spice/pads.py: the PDK's {os.path.basename(src)}, with the timing of\n"
            + "".join(f"     {c}\n" for c in cells)
            + f"   as z80_chip.v ties them re-measured at transistor level (ngspice; the\n"
            f"   PDK's netlist and {mc} models, {temp} C, {vdd:g} V core / {vio:g} V I/O): outputs\n"
            f"   into {LOADS[0]}-{LOADS[-1]} pF beyond the pin's own capacitance, inputs with edges to\n"
            f"   {max(IN_EDGES)} ns. Delays 50 % to 50 %, slews 10-90 %, as the PDK's; disable: the\n"
            f"   driver's current under 10 %. The pins' max_transition / max_capacitance:\n"
            f"   the ranges simulated (the pin's edges to {max(IN_EDGES)} ns, past every output edge\n"
            f"   into {LOADS[-1]} pF; an output's load to {LOADS[-1]} pF beyond its own; the core side's\n"
            f"   to the tables' last points). Internal power: the supplies' energy in the same\n"
            f"   runs, the PDK's way (a rise less the C V^2 that charges the load), with the load\n"
            f"   as STA takes it -- the pin's own capacitance too, half of its C V^2 off each\n"
            f"   edge -- so that STA's internal plus switching power is the simulated energy;\n"
            f"   leakage: each state's DC power. Generated; do not edit. */\n")
    open(dst, "w").write(head + lib)


# --------------------------------------------------------- the whole run
def characterise(ctx, corners, jobs):
    P = ctx["pdk"]
    jobs_l, res = [], {}
    for k in corners:   # ngspice reads .spiceinit from its working directory
        os.makedirs(os.path.join(ctx["build"], k), exist_ok=True)
        shutil.copy(os.path.join(ctx["build"], ".spiceinit"), os.path.join(ctx["build"], k, ".spiceinit"))
    for k in corners:
        lib_corner = P["corners"][k][0]
        lib = open(os.path.join(ctx["root"], P["liberty"].format(lib_corner))).read()
        res[k] = {"liberty": lib_corner, "temp": P["corners"][k][2], "vdd": P["corners"][k][3],
                  "vio": P["corners"][k][4], "cells": {}}
        for cell, cfg in P["cells"].items():
            C = res[k]["cells"][cell] = {}
            found = arcs(lib, cell, cfg)
            kinds = {a[0]: a for a in found}
            pdk_power = {}
            for kind, val, pg, tabs in power_groups(lib, cell, cfg):
                for n, (b2, e2) in tabs.items():
                    pdk_power.setdefault(kind, {}).setdefault(n + (f"/{pg}" if pg else ""), table(lib[b2:e2]))
            unit = {"1mW": 1e3, "1uW": 1.0, "1nW": 1e-3, "1pW": 1e-6}[attr(lib[:lib.index("cell")], "leakage_power_unit")]
            _, cb, ce = [g for g in groups(lib, "cell") if g[0] == cell][0]
            leak = attr(lib[cb:ce], "cell_leakage_power")
            C["pdk_leak_uW"] = None if leak is None else float(leak) * unit
            if "out" in cfg:
                data_arc = kinds["data"]
                tb, te = data_arc[1]
                b, e = data_arc[2]["cell_rise"]
                slews, _, _ = table(lib[b:e])
                cpin = ctx["cpin"][cell] = pin_cap(lib, cell, cfg["out"][2])
                C["data"] = {"slews": slews, "loads": LOADS, "cpin": cpin,
                             "pdk": {n: table(lib[b2:e2]) for n, (b2, e2) in data_arc[2].items()},
                             "pdk_power": pdk_power.get("data", {})}
                ctx["mid_slew"][cell] = slews[len(slews) // 2]
                for a, s in enumerate(slews):
                    for c, L in enumerate(LOADS):
                        jobs_l.append(((k, cell, "data", a, c), m_out, (ctx, k, cell, s, L)))
                if cfg["out"][1]:
                    C["enable"] = {"slews": slews, "loads": LOADS, "cpin": cpin,
                                   "pdk": {f"{n}": table(lib[b2:e2]) for kk, sp, tabs in found if kk == "enable"
                                           for n, (b2, e2) in tabs.items()}}
                    C["disable"] = {"slews": slews, "loads": LOADS, "cpin": cpin,
                                    "pdk": {f"{n}": table(lib[b2:e2]) for kk, sp, tabs in found if kk == "disable"
                                            for n, (b2, e2) in tabs.items() if n.startswith("cell")}}
                    for val in (0, 1):
                        for a, s in enumerate(slews):
                            jobs_l.append(((k, cell, "disable", val, a), m_disable, (ctx, k, cell, s, val)))
                            jobs_l.append(((k, cell, "release", val, a), m_release, (ctx, k, cell, s, val)))
                            for c, L in enumerate(LOADS):
                                jobs_l.append(((k, cell, "enable", val, a, c), m_enable, (ctx, k, cell, s, L, val)))
                jobs_l.append(((k, cell, "dc_out"), m_dc_out, (ctx, k, cell)))
            jobs_l.append(((k, cell, "leak"), m_leak, (ctx, k, cell)))
            if "inp" in cfg:
                ia = kinds["input"]
                b, e = ia[2]["cell_rise"]
                e1, loads, _ = table(lib[b:e])
                edges = sorted(set(e1) | set(IN_EDGES))
                C["input"] = {"edges": edges, "loads": loads,
                              "pdk": {n: table(lib[b2:e2]) for n, (b2, e2) in ia[2].items()},
                              "pdk_power": pdk_power.get("input", {})}
                ctx["min_yload"][cell] = loads[0]
                for a, s in enumerate(edges):
                    for c, L in enumerate(loads):
                        jobs_l.append(((k, cell, "input", a, c), m_input, (ctx, k, cell, s, L)))
                jobs_l.append(((k, cell, "dc_in"), m_dc_in, (ctx, k, cell)))
    print(f"pads: {len(jobs_l)} simulations on {jobs} processes", flush=True)
    t0, done, failed = time.time(), {}, []
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        futs = {ex.submit(f, *args): key for key, f, args in jobs_l}
        for n, fu in enumerate(concurrent.futures.as_completed(futs), 1):
            try:
                done[futs[fu]] = fu.result()
            except Exception as e:      # noqa: BLE001 -- report them all
                failed.append(f"{futs[fu]}: {e}")
            if n % 100 == 0 or n == len(jobs_l):
                print(f"pads: {n}/{len(jobs_l)} ({time.time() - t0:.0f} s)", flush=True)
    if failed:
        sys.exit("pads: failed simulations:\n" + "\n".join(failed))
    # assemble
    for k in corners:
        for cell, C in res[k]["cells"].items():
            if "data" in C:
                n1 = len(C["data"]["slews"])
                for edge in ("rise", "fall"):
                    C["data"][edge] = [[done[(k, cell, "data", a, c)][edge] for c in range(len(LOADS))] for a in range(n1)]
                C["data"]["wave"] = next(done[(k, cell, "data", a, LOADS.index(TEST_LOAD))]["wave"]
                                         for a in range(n1) if "wave" in done[(k, cell, "data", a, LOADS.index(TEST_LOAD))])
                if "enable" in C:
                    for val in (0, 1):
                        C["enable"][str(val)] = [[done[(k, cell, "enable", val, a, c)] for c in range(len(LOADS))] for a in range(n1)]
                        C["disable"][str(val)] = [done[(k, cell, "disable", val, a)] for a in range(n1)]
                        C["enable"]["release_" + str(val)] = [done[(k, cell, "release", val, a)] for a in range(n1)]
                C["dc"] = done[(k, cell, "dc_out")]
            if "input" in C:
                ne, nl = len(C["input"]["edges"]), len(C["input"]["loads"])
                for edge in ("rise", "fall"):
                    C["input"][edge] = {q: [[done[(k, cell, "input", a, c)][edge][q] for c in range(nl)] for a in range(ne)]
                                        for q in ("d50", "slew", "vsw")}
                    C["input"][edge]["e"] = [[{q: v for q, v in done[(k, cell, "input", a, c)][edge].items()
                                               if q.endswith("_pJ") or q in ("dv", "t_ns")} for c in range(nl)]
                                             for a in range(ne)]
                    C["input"][edge]["wave"] = done[(k, cell, "input", C["input"]["edges"].index(15), 0)][edge].get("wave")
                C.setdefault("dc", {}).update(done[(k, cell, "dc_in")])
            C["leak"] = done[(k, cell, "leak")]
    return res


def out_edges_covered(res):
    """the slowest edge an output makes into the loads, against the input
    edges simulated: a bidirectional pad's input sees its own output's
    edges, and the pin's one max_transition holds both"""
    worst = 0.0
    for r in res.values():
        for C in r["cells"].values():
            tabs = [C["data"][e] for e in ("rise", "fall")] if "data" in C else []
            tabs += [C["enable"][v] for v in ("0", "1")] if "enable" in C else []
            worst = max([worst] + [m["slew"] for t in tabs for row in t for m in row if m.get("slew") is not None])
    if worst > max(IN_EDGES):
        sys.exit(f"pads: an output's edge takes {worst:.1f} ns, past the input edges simulated "
                 f"(IN_EDGES, to {max(IN_EDGES)} ns): extend IN_EDGES")
    return worst


# ------------------------------------------------------------- the report
def pdk_energy(tabs, name, x1, x2):
    """the PDK's table(s) `name` at (x1, x2), summed over its supplies"""
    v = [lookup(t, x1, x2) for n, t in tabs.items() if n.split("/")[0] == name]
    return sum(v) if v else None


def energy_line(C, a, vio):
    """an output's energy into 100 pF a cycle, and the pad's own against
    the PDK library (the PDK's way: the rise less the load's C V^2)"""
    D, s = C["data"], C["data"]["slews"][a]
    tot = lambda m: m["e_io_pJ"] + m["e_core_pJ"]
    own = lambda edge, c: tot(D[edge][a][c]) - (vio * LOADS[c] * D[edge][a][c]["dv"])
    c100, c20 = LOADS.index(TEST_LOAD), LOADS.index(20)
    cyc = tot(D["rise"][a][c100]) + tot(D["fall"][a][c100])
    load = vio * TEST_LOAD * (D["rise"][a][c100]["dv"] + D["fall"][a][c100]["dv"])
    pk = lambda edge, L: pdk_energy(D["pdk_power"], edge + "_power", s, L)
    txt = (f"energy, data {s:g} ns: {cyc:.0f} pJ a cycle into 100 pF (the load's C V^2 {load:.0f}, the pad"
           f" {cyc - load:.1f}); the pad's, rise / fall: 20 pF {own('rise', c20):.1f} / {own('fall', c20):.1f} pJ")
    if pk("rise", 20) is not None:
        txt += f" (PDK lib {pk('rise', 20):.1f} / {pk('fall', 20):.1f})"
    txt += f", 100 pF {own('rise', c100):.1f} / {own('fall', c100):.1f}"
    if pk("rise", 100) is not None:
        txt += f" (PDK lib extrapolated {pk('rise', 100):.1f} / {pk('fall', 100):.1f})"
    return txt


def input_energy_line(C, vdd):
    """an input's energy a cycle (pin rising and falling), the core side's
    lightest load, against the PDK library"""
    D = C["input"]
    cyc = lambda e: sum(D[edge]["e"][D["edges"].index(e)][0]["e_io_pJ"] + D[edge]["e"][D["edges"].index(e)][0]["e_core_pJ"]
                        - (vdd * D["loads"][0] * D[edge]["e"][D["edges"].index(e)][0]["dv"] if edge == "rise" else 0.0)
                        for edge in ("rise", "fall"))
    last = max(x for x in D["edges"] if x <= 5)
    pk = lambda e: (None if not D["pdk_power"] else
                    pdk_energy(D["pdk_power"], "rise_power", e, D["loads"][0]) + pdk_energy(D["pdk_power"], "fall_power", e, D["loads"][0]))
    txt = f"energy a cycle: {last:g} ns edges {cyc(last):.2f} pJ"
    if pk(last) is not None:
        txt += f" (PDK lib {pk(last):.2f})"
    txt += f", 15 ns {cyc(15):.2f}"
    if pk(15) is not None:
        txt += f" (PDK lib extrapolated {pk(15):.2f})"
    return txt + f", 120 ns {cyc(max(D['edges'])):.2f}"


def summary(pdk, res, corners):
    """the datasheet's view, at 100 pF and 15 ns edges"""
    L = []
    for k in corners:
        r = res[k]
        L.append(f"\n{pdk} {k} ({r['liberty']}): {r['temp']} C, core {r['vdd']:g} V, I/O {r['vio']:g} V")
        for cell, C in r["cells"].items():
            short = cell.split("__")[-1] if "__" in cell else cell.replace("sg13g2_IOPad", "")
            if "data" in C:
                a = C["data"]["slews"].index(min(C["data"]["slews"], key=lambda s: abs(s - 0.5)))
                c = LOADS.index(TEST_LOAD)
                for edge in ("rise", "fall"):
                    m = C["data"][edge][a][c]
                    pdk_d = lookup(C["data"]["pdk"]["cell_" + edge], C["data"]["slews"][a], C["data"]["cpin"] + TEST_LOAD)
                    pdk_s = lookup(C["data"]["pdk"][edge + "_transition"], C["data"]["slews"][a], C["data"]["cpin"] + TEST_LOAD)
                    L.append(f"  {short:10} out {edge:4} 100 pF: {m['d50']:6.2f} ns at 50 % (PDK lib {pdk_d:6.2f}),"
                             f" {m['dlev']:6.2f} ns at 1.5 V; slew {m['slew']:6.2f} ns (PDK lib {pdk_s:6.2f})")
                if "enable" in C:
                    on = [C["enable"][v][a][c] for v in ("1", "0")]
                    off = [C["disable"][v][a] for v in ("0", "1")]
                    L.append(f"  {short:10} turn-on 100 pF: Z->1 {on[0]['d50']:.2f} / Z->0 {on[1]['d50']:.2f} ns at 50 %;"
                             f" float: driver off {off[0]['toff']:.2f} / {off[1]['toff']:.2f} ns,"
                             f" pin moved 0.5 V {off[0]['tfloat']:.2f} / {off[1]['tfloat']:.2f} ns (2 kohm)")
                dc = C["dc"]
                L.append(f"  {short:10} DC: sinks {dc['iol_mA']:.1f} mA at 0.4 V (NMOS Z80: 2.0), sources"
                         f" {dc['ioh_mA']:.1f} mA at 2.4 V (0.25)")
                L.append(f"  {short:10} " + energy_line(C, a, r["vio"]))
                if "enable" in C:
                    m = [C["enable"][v][a][c] for v in ("1", "0")]
                    rel = [C["enable"]["release_" + v][a] for v in ("1", "0")]
                    ins = lambda x, mv: x["e_io_pJ"] + x["e_core_pJ"] - (r["vio"] * TEST_LOAD * x["dv"] if mv else 0.0)
                    L.append(f"  {short:10} energy, the enable into 100 pF: turning on Z->1 {ins(m[0], 1):.1f}"
                             f" / Z->0 {ins(m[1], 0):.1f} pJ beyond the load's C V^2, letting go"
                             f" {ins(rel[0], 0):.1f} / {ins(rel[1], 0):.1f} pJ")
            if "input" in C:
                e = C["input"]["edges"].index(15)
                c = 0
                for edge in ("rise", "fall"):
                    m = C["input"][edge]
                    pdk_d = lookup(C["input"]["pdk"]["cell_" + edge], 15, C["input"]["loads"][c])
                    L.append(f"  {short:10} in  {edge:4} 15 ns edge: {m['d50'][e][c]:6.2f} ns pin 50 % -> core 50 %"
                             f" (PDK lib extrapolated {pdk_d:6.2f}); switches at {m['vsw'][e][c]:.2f} V")
                dc = C["dc"]
                L.append(f"  {short:10} DC input: switches up at {dc['vt_rise']:.2f} V, down at {dc['vt_fall']:.2f} V"
                         f" (TTL: high from {VIH} V, low to {VIL} V)")
                L.append(f"  {short:10} " + input_energy_line(C, r["vdd"]))
            lk = C["leak"]
            nw = lambda st: sum(lk[st].values()) * 1e3
            parts = ([f"driving 0 / 1 {nw('out0'):.2f} / {nw('out1'):.2f} nW"] if "out0" in lk else []) + \
                    ([f"receiving 0 / 1 {nw('in0'):.2f} / {nw('in1'):.2f} nW"] if "in0" in lk else [])
            pl = C.get("pdk_leak_uW")
            L.append(f"  {short:10} leakage: " + ", ".join(parts)
                     + (f" (PDK lib {pl * 1e3:.2f} nW)" if pl is not None else ""))
    return "\n".join(L)


def plots(pdk, res, corners, out):
    names = list(corners)
    slow = names[1] if len(names) > 1 else names[0]          # ss / slow
    cells = res[names[0]]["cells"]
    short = lambda c: c.split("__")[-1] if "__" in c else c.replace("sg13g2_IOPad", "")
    colour = {k: svgplot.PALETTE[i] for i, k in enumerate(names)}
    # one panel per output driver (IHP's TriOut16mA is InOut16mA's)
    outs = [c for c, C in cells.items() if "data" in C and not (pdk == "ihp-sg13g2" and c.endswith("TriOut16mA"))]
    charts = []
    for cell in outs:
        ch = svgplot.Chart(f"{short(cell)}: data input -> pin, 100 pF", "ns after the data input's edge", "V at the pin")
        for k in names:
            for i, edge in enumerate(res[k]["cells"][cell]["data"]["wave"]):
                if edge:
                    ch.line([p[0] for p in edge], [p[1] for p in edge], k if i == 0 else None, colour[k])
        ch.hline(OUT_LEVEL, "1.5 V: the datasheet's VOH = VOL")
        charts.append(ch)
    if charts:
        svgplot.write(os.path.join(out, "waveforms.svg"), charts, cols=min(2, len(charts)), w=620,
                      title=f"{pdk}: output pads into the NMOS Z80's 100 pF test load (ngspice)")
    charts = []
    for cell in outs:
        for edge in ("rise", "fall"):
            ch = svgplot.Chart(f"{short(cell)}: data -> pin, {edge}", "pF on the pin", "ns, 50 % to 50 %",
                               xlim=(0, LOADS[-1]))
            for k in names:
                D = res[k]["cells"][cell]["data"]
                a = D["slews"].index(min(D["slews"], key=lambda s: abs(s - 0.5)))
                ch.line(LOADS, [D[edge][a][c]["d50"] for c in range(len(LOADS))], f"{k}", colour[k])
                xs = list(range(0, LOADS[-1] + 1, 5))
                ch.line(xs, [lookup(D["pdk"]["cell_" + edge], D["slews"][a], D["cpin"] + x) for x in xs],
                        None, colour[k], dash="5 4", width=1.1)
                end = D["pdk"]["cell_" + edge][1][-1] - D["cpin"]
            ch.vline(end, "the PDK library's last point")
            ch.vline(TEST_LOAD, "100 pF", top=True)
            ch.note("solid: SPICE; dashed: the PDK library as STA extrapolates it")
            charts.append(ch)
    if charts:
        svgplot.write(os.path.join(out, "delay_vs_load.svg"), charts, cols=2,
                      title=f"{pdk}: output delay against load, SPICE against the PDK's Liberty")
    charts = []
    for cell in outs:
        for edge in ("rise", "fall"):
            ch = svgplot.Chart(f"{short(cell)}: the pad's own energy, data {edge}", "pF on the pin",
                               "pJ an edge" + (", beyond the load's C V^2" if edge == "rise" else ""),
                               xlim=(0, LOADS[-1]))
            end = None
            for k in names:
                D, vio = res[k]["cells"][cell]["data"], res[k]["vio"]
                a = D["slews"].index(min(D["slews"], key=lambda s: abs(s - 0.5)))
                ch.line(LOADS, [D[edge][a][c]["e_io_pJ"] + D[edge][a][c]["e_core_pJ"] - vio * L * D[edge][a][c]["dv"]
                                for c, L in enumerate(LOADS)], f"{k}", colour[k])
                if pdk_energy(D["pdk_power"], edge + "_power", D["slews"][a], 0) is not None:
                    xs = list(range(0, LOADS[-1] + 1, 5))
                    ch.line(xs, [pdk_energy(D["pdk_power"], edge + "_power", D["slews"][a], x) for x in xs],
                            None, colour[k], dash="5 4", width=1.1)
                    end = max(t[1][-1] for n, t in D["pdk_power"].items() if n.startswith(edge))
            if end is not None:
                ch.vline(end, "the PDK library's last point")
            ch.vline(TEST_LOAD, "100 pF", top=True)
            ch.note("solid: SPICE; dashed: the PDK library as STA extrapolates it")
            charts.append(ch)
    if charts:
        svgplot.write(os.path.join(out, "energy_vs_load.svg"), charts, cols=2,
                      title=f"{pdk}: an output pad's own energy an edge against load, SPICE against the PDK's Liberty")
    # inputs; cells with the same input circuit share a panel
    charts, seen = [], {}
    for cell, C in cells.items():
        if "input" not in C:
            continue
        key = json.dumps([[round(x, 2) for x in r] for r in C["input"]["rise"]["d50"]])
        if key in seen:
            seen[key].title = seen[key].title.replace(":", f" (and {short(cell)}):", 1)
            continue
        ch = svgplot.Chart(f"{short(cell)}: pin -> core", "ns input edge (10-90 %)", "ns, pin 50 % to core 50 %",
                           xlim=(0, max(IN_EDGES)))
        for k in names:
            D = res[k]["cells"][cell]["input"]
            for edge, dash in (("rise", None), ("fall", "2 3")):
                ch.line(D["edges"], [D[edge]["d50"][a][0] for a in range(len(D["edges"]))], f"{k} {edge}", colour[k], dash)
        D = res[slow]["cells"][cell]["input"]
        xs = [x / 2 for x in range(0, 2 * max(IN_EDGES) + 1)]
        for edge in ("rise", "fall"):
            ch.line(xs, [lookup(D["pdk"]["cell_" + edge], x, D["loads"][0]) for x in xs],
                    f"{slow}, the PDK library" if edge == "rise" else None, "#999", "6 4", 1.1)
        ch.vline(D["pdk"]["cell_rise"][0][-1], "the PDK library's last point")
        ch.vline(15, "15 ns")
        seen[key] = ch
        charts.append(ch)
    if charts:
        svgplot.write(os.path.join(out, "inputs.svg"), charts, cols=min(2, len(charts)), w=620,
                      title=f"{pdk}: input pads, pin to core against the input edge (ngspice)")


def pin_plots(pdk, res, corners, out):
    """The pads one by one, as their pins behave: out.svg (driving 100 pF),
    tristate.svg (turning on, letting go), in.svg (a 15 ns edge to the
    core, and where the input switches) and, where CLK and RESET have their
    own (Schmitt) pad, clk_reset.svg."""
    names = list(corners)
    cells = res[names[0]]["cells"]
    short = lambda c: c.split("__")[-1] if "__" in c else c.replace("sg13g2_IOPad", "")
    colour = {k: svgplot.PALETTE[i] for i, k in enumerate(names)}
    sup = lambda k: f"{k}: {res[k]['vio']:g} V, {res[k]['temp']} C"
    # outputs
    charts = []
    for cell, C in cells.items():
        if "data" not in C or (pdk == "ihp-sg13g2" and cell.endswith("TriOut16mA")):
            continue
        for i, edge in enumerate(("rising", "falling")):
            ch = svgplot.Chart(f"{short(cell)}: data input {edge}, pin into 100 pF", "ns after the data input",
                               "V at the pin")
            for k in names:
                w = res[k]["cells"][cell]["data"]["wave"][i]
                if w:
                    ch.line([p[0] for p in w], [p[1] for p in w], sup(k), colour[k])
            ch.hline(OUT_LEVEL, "1.5 V: the datasheet's output level")
            charts.append(ch)
    if charts:
        svgplot.write(os.path.join(out, "out.svg"), charts, cols=2,
                      title=f"{pdk}: output pads into the NMOS Z80's 100 pF test load (ngspice)")
    # tri-state: on, off, and the driver's current letting go
    charts = []
    for cell, C in cells.items():
        if "enable" not in C or (pdk == "ihp-sg13g2" and cell.endswith("InOut16mA")):
            continue
        for val in ("1", "0"):
            ch = svgplot.Chart(f"{short(cell)}: output enable rising, pin from float to {val}", "ns after the enable",
                               "V at the pin")
            for k in names:
                a = len(C["enable"]["slews"]) // 2          # the slew whose waveform was kept
                w = res[k]["cells"][cell]["enable"][val][a][LOADS.index(TEST_LOAD)].get("wave")
                if w:                                   # sampled from 5 ns before the enable
                    ch.line([x - 5 for x in w["t"]], w["pad"], sup(k), colour[k])
            ch.hline(OUT_LEVEL)
            charts.append(ch)
        for val in ("1", "0"):
            ch = svgplot.Chart(f"{short(cell)}: output enable falling, the pin at {val} lets go",
                               "ns after the enable", "V at the pin (100 pF, 2 kohm to the other rail)")
            for k in names:
                a = len(C["disable"]["slews"]) // 2
                w = res[k]["cells"][cell]["disable"][val][a].get("wave")
                if w:
                    ch.line([x - 5 for x in w["t"]], w["pad"], sup(k), colour[k])
            charts.append(ch)
        for val in ("1", "0"):
            ch = svgplot.Chart(f"{short(cell)}: the driver's current as it lets go of {val}",
                               "ns after the enable", "mA out of the pin", xlim=(-2, 10))
            for k in names:
                a = len(C["disable"]["slews"]) // 2
                w = res[k]["cells"][cell]["disable"][val][a].get("wave")
                if w:
                    ch.line([x - 5 for x in w["t"]], w["i_mA"], sup(k), colour[k])
            charts.append(ch)
    if charts:
        svgplot.write(os.path.join(out, "tristate.svg"), charts, cols=2,
                      title=f"{pdk}: tri-state pads -- turning on into 100 pF, floating (ngspice)")
    # inputs: same circuit, one figure; GF180's Schmitt pad (CLK, RESET) its own
    groups = {}
    for cell, C in cells.items():
        if "input" in C:
            key = json.dumps([[round(x, 2) for x in r] for r in C["input"]["rise"]["d50"]])
            groups.setdefault(key, []).append(cell)
    for cells_g in groups.values():
        cell = cells_g[-1] if pdk == "gf180mcu" else cells_g[0]
        schmitt = cell.endswith("in_s")
        title = " / ".join(short(c) for c in cells_g)
        charts = []
        for edge in ("rise", "fall"):
            ch = svgplot.Chart(f"{title}: pin {'rising' if edge == 'rise' else 'falling'} in 15 ns, core side",
                               "ns after the pin's 50 %", "V (dashed: the pin)")
            for k in names:
                w = res[k]["cells"][cell]["input"][edge].get("wave")
                if w:
                    tt = [x - 15 for x in w["t"]]
                    ch.line(tt, w["pad"], None, colour[k], dash="4 3", width=1.1)
                    ch.line(tt, w["y"], sup(k), colour[k])
            ch.hline(VIH, "2.0 V (VIH)")
            ch.hline(VIL, "0.8 V (VIL)")
            charts.append(ch)
        vmax = max(res[k]["vio"] for k in names)
        ch = svgplot.Chart(f"{title}: where it switches (2 us ramps)", "V at the pin", "V at the core side",
                           xlim=(0, vmax + 0.2))
        for k in names:
            cv = res[k]["cells"][cell]["dc"].get("curve")
            if cv:
                ch.line([p[0] for p in cv["up"]], [p[1] for p in cv["up"]], sup(k), colour[k])
                ch.line([p[0] for p in cv["down"]], [p[1] for p in cv["down"]], None, colour[k], dash="4 3")
        ch.vline(VIL, "VIL 0.8 V")
        ch.vline(VIH, "VIH 2.0 V")
        ch.note("solid: the pin rising, dashed: falling")
        charts.append(ch)
        name = "clk_reset.svg" if schmitt else "in.svg"
        what = ("the Schmitt input pad of CLK and RESET" if schmitt else
                "the input pads (all inputs, CLK and RESET too)" if pdk == "ihp-sg13g2" else
                "the input pads (WAIT INT NMI BUSRQ, and the data bus's input buffer)")
        svgplot.write(os.path.join(out, name), charts, cols=2, title=f"{pdk}: {what} (ngspice)")


def write_json(doc, path):
    """indented JSON, every list of plain values on one line"""
    text = json.dumps(doc, indent=1)
    text = re.sub(r"\[([^\[\]{}\"]*)\]", lambda m: "[" + " ".join(m.group(1).split()) + "]", text)
    open(path, "w").write(text + "\n")


def compare(new, old, tol_ns=0.1, tol_rel=0.03, tol_v=0.03, tol_ma_rel=0.03, tol_pj=0.5, tol_e_rel=0.01,
            tol_uw=1e-5, tol_uw_rel=0.05):
    """differences between two characterisations beyond the tolerances:
    delays and slews within tol_ns or tol_rel, switching levels within
    tol_v, drive currents within tol_ma_rel, the supplies' energy within
    tol_pj or tol_e_rel, leakage within tol_uw or tol_uw_rel -- ngspice and
    the PDK may drift a little between versions"""
    bad = []

    def num(a, b, where, tol_abs, rel):
        if a is None or b is None:
            if (a is None) != (b is None):
                bad.append(f"{where}: {a} against {b}")
            return
        if abs(a - b) > max(tol_abs, rel * abs(b)):
            bad.append(f"{where}: {a:.3f} against {b:.3f}")

    def walk(a, b, where):
        if isinstance(b, dict):
            for k, v in b.items():
                if k in ("wave", "curve", "pdk", "pdk_power", "slews", "loads", "edges"):
                    continue
                if k in a:
                    walk(a[k], v, f"{where}/{k}")
        elif isinstance(b, list):
            for i, (x, y) in enumerate(zip(a, b)):
                walk(x, y, f"{where}[{i}]")
        elif isinstance(b, (int, float)) and not isinstance(b, bool):
            key = where.rsplit("/", 1)[-1]
            if key.startswith("vt") or key in ("vsw", "v0"):
                num(a, b, where, tol_v, 0)
            elif key.endswith("mA"):
                num(a, b, where, 0, tol_ma_rel)
            elif key.endswith("_pJ"):
                num(a, b, where, tol_pj, tol_e_rel)
            elif key.endswith("_uW"):
                num(a, b, where, tol_uw, tol_uw_rel)
            elif key in ("temp", "vdd", "vio", "cpin"):
                num(a, b, where, 1e-9, 0)
            else:
                num(a, b, where, tol_ns, tol_rel)
    walk(new, old, "")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("pdk", choices=sorted(PDKS))
    ap.add_argument("--out", help="output directory (default: pads/<pdk>/ beside this script)")
    ap.add_argument("--corners", default="", help="e.g. tt,ss (default: all three)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--keep", help="keep the decks, waveforms and logs here")
    ap.add_argument("--reuse", action="store_true", help="no simulations: the libraries, plots and summary "
                    "again from OUT/pads.json")
    ap.add_argument("--check", metavar="JSON", help="fail if the results differ from this pads.json (the committed "
                    "one) beyond small tolerances")
    ap.add_argument("--ngspice")
    ap.add_argument("--openvaf", help="openvaf-r, for IHP's Verilog-A transistor models")
    ap.add_argument("--pdk-root", default=os.path.join(os.environ.get("CIEL_HOME", os.path.expanduser("~/.ciel")), "ciel"))
    a = ap.parse_args()
    P = PDKS[a.pdk]
    root = os.path.join(a.pdk_root, P["root"])
    if not os.path.isdir(root):
        sys.exit(f"pads: no PDK at {root} (fetch it with the asic-<pdk> branch's make, or --pdk-root)")
    corners = [k for k in P["corners"] if not a.corners or k in a.corners.split(",")]
    out = a.out or os.path.join(HERE, "pads", a.pdk)
    os.makedirs(out, exist_ok=True)
    ctx = {"pdk": P, "pdk_name": a.pdk, "root": root, "mid_slew": {}, "min_yload": {}, "cpin": {}}
    if a.reuse:
        old = json.load(open(os.path.join(out, "pads.json")))
        res, ver = old["corners"], old["simulator"]
        corners = [k for k in corners if k in res]
    else:
        ngspice = tool("ngspice", a.ngspice)
        openvaf = tool("openvaf-r", a.openvaf) if a.pdk == "ihp-sg13g2" else None
        with tempfile.TemporaryDirectory() as tmp:
            ctx.update(build=os.path.abspath(a.keep or tmp), ngspice=ngspice)
            setup(a.pdk, root, ctx["build"], openvaf)
            res = characterise(ctx, corners, a.jobs)
        ver = subprocess.run([ngspice, "-v"], capture_output=True, text=True).stdout
        ver = (re.search(r"ngspice-\S+", ver) or re.search(r"\S+", "ngspice")).group(0)
    worst = out_edges_covered({k: res[k] for k in corners})
    print(f"pads: the slowest output edge, {worst:.1f} ns, is inside the input edges simulated (to {max(IN_EDGES)} ns)")
    for k in corners:
        src = os.path.join(root, P["liberty"].format(res[k]["liberty"]))
        write_liberty(src, os.path.join(out, os.path.basename(src)), P["cells"], res[k]["cells"], k, ctx)
    doc = {"pdk": a.pdk, "pdk_version": P["root"].split("/")[2], "simulator": ver,
           "conditions": {"loads_pF": LOADS, "test_load_pF": TEST_LOAD, "out_level_V": OUT_LEVEL,
                          "vil_V": VIL, "vih_V": VIH, "float_dV": FLOAT_DV, "float_pull_ohm": FLOAT_R},
           "corners": res}
    write_json(doc, os.path.join(out, "pads.json"))
    plots(a.pdk, res, corners, out)
    pin_plots(a.pdk, res, corners, out)
    text = summary(a.pdk, res, corners)
    open(os.path.join(out, "summary.txt"), "w").write(text.lstrip("\n") + "\n")
    print(text)
    if a.check:
        bad = compare(res, json.load(open(a.check))["corners"])
        if bad:
            print(f"\npads: {len(bad)} results differ from {a.check}:\n  " + "\n  ".join(bad[:40]))
            return 1
        print(f"\npads: every result matches {a.check}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
