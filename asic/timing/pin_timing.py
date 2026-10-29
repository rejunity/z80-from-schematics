#!/usr/bin/env python3
"""pin_timing.py - check a z80_chip layout against the NMOS Z80's AC
characteristics: Zilog PS0178, Z8400 (NMOS) table, transcribed with its
formulas in z8400_nmos.json; the original Z80 CPU's 2.5 MHz column from
Zilog's Z8400 product specification of September 1983 in
z8400_nmos_1983.json.

OpenSTA reads a LibreLane chip run -- netlist, parasitics, and the cell,
pad and z80_top macro libraries of each process corner -- and measures at
the package pins, into the datasheet's 100 pF test load:

  outputs  the delay from the CLK pin's rising and falling edge to every
           rising and falling transition of every output pin (max and
           min), and through the pads' output enables (float / drive);
  inputs   the setup and hold the replica needs around the CLK pin's
           rising and falling edge, for every input pin.

It then checks every datasheet parameter that the replica must meet --
output and float delays, the input setup / hold windows the replica must
accept, pulse widths and pin-to-pin separations -- for one speed grade,
or several, at every process corner. Besides the grades (2.5, 4, 6, 8
MHz) it checks the clocks of machines built around the Z80 against the
grade of the parts they used, the period-dependent limits (the starred
minimums) at the machine's own period: trs80 (TRS-80 Model I, 1.774 MHz,
2.5 MHz table), zx81 (ZX80 / ZX81, 3.25 MHz), spectrum (ZX Spectrum
16K / 48K, 3.5 MHz) and spectrum128 (3.5469 MHz), the last three on the
4 MHz table. The measurements depend on the clock's and the inputs' edge
only, not on the period: OpenSTA measures once for every edge the
points share and each point is checked from that. The datasheet's waveforms put each pin event on
a clock edge (address at T1 rise, MREQ at T1 fall, ...); this measures the
analog timing of those edges. That the replica's events fall on those
edges is the cycle-level tests' job (perfectz80 pin comparisons).

The pads: the PDKs' pad libraries stop at 20-33 pF and 1-5 ns input
edges, where STA extrapolates. Where ../spice/pads/<pdk>/ holds the pads
simulated at transistor level (../spice/pads.py), it reads those
libraries instead and measures at the datasheet's levels (PS0178's AC
test conditions): outputs where they cross 1.5 V (VOH = VOL; the pads'
own offset from the libraries' 50 % at this load, from the same
simulations), inputs where they cross 2.0 V rising and 0.8 V falling
(VIH / VIL) on the given edge. The CLK pin stays referenced at its 50 %.

usage: pin_timing.py RUN [--grade POINT[,POINT...] | --all] [--load PF]
                         [--edge NS] [--corners tt,ss,...] [--sta PATH]
                         [--keep DIR] [--pads auto|none|DIR] [--jobs N]
         POINT: 2.5 4 6 8 (grades, MHz) trs80 zx81 spectrum spectrum128
         (default: 4; every process corner of the run, nominal parasitics)
       pin_timing.py --write-sdc FILE [--grade 2.5|4|6|8] [--load ...]
         (the same contract as the chip flows' SDC)
exit: 0 every check passes at every corner, 1 a check fails, 2 no run.
"""
import argparse, bisect, fnmatch, glob, json, os, re, shlex, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = os.path.join(HERE, "z8400_nmos.json")
# the tables by grade: PS0178's 4 / 6 / 8 MHz, the September 1983
# specification's 2.5 MHz (the Z80 CPU; PS0178 starts at the Z80A)
SPECS = {"2.5": os.path.join(HERE, "z8400_nmos_1983.json")}
# the points checked: a grade at its minimum period, or a machine's clock on
# the grade of the Z80 it used (MHz: its crystal divided down)
POINTS = {  # name: (grade, MHz or None for the grade's own, label)
    "2.5": ("2.5", None, None), "4": ("4", None, None), "6": ("6", None, None), "8": ("8", None, None),
    "trs80": ("2.5", 10.6445 / 6, "TRS-80 Model I"),
    "zx81": ("4", 6.5 / 2, "ZX80 / ZX81"),
    "spectrum": ("4", 14.0 / 4, "ZX Spectrum 16K / 48K"),
    "spectrum128": ("4", 17.7345 / 5, "ZX Spectrum 128K"),
}
ALL_POINTS = ["2.5", "4", "6", "8", "trs80", "zx81", "spectrum", "spectrum128"]
SPICE_PADS = os.path.join(HERE, "..", "spice", "pads")

ADDR = [f"A[{i}]" for i in range(16)]
DATA = [f"D[{i}]" for i in range(8)]
CTRL = ["M1_n", "MREQ_n", "IORQ_n", "RD_n", "WR_n", "RFSH_n", "HALT_n", "BUSAK_n"]
OUTS = ADDR + DATA + CTRL
INS = DATA + ["WAIT_n", "INT_n", "NMI_n", "BUSRQ_n", "RESET_n"]
GROUP = {"A": ADDR, "D": DATA}
# pads with an output enable: pad instance names follow z80_chip.v
OE_PADS = {p: "pad_" + p.replace("[", "").replace("]", "") for p in ADDR + DATA}
OE_PADS.update({p: "pad_" + p for p in ["MREQ_n", "IORQ_n", "RD_n", "WR_n"]})


# --------------------------------------------------------------- the run
def corner_files(run, cfg, corner, pads=None):
    """Liberty files and the parasitics of one STA corner (nom_tt_025C_5v00);
    the pad libraries from `pads` where it has them."""
    libs = []
    for key in ("LIB", "CELL_LIBS", "PAD_LIBS"):
        for pat, files in (cfg.get(key) or {}).items():
            if fnmatch.fnmatch(corner, pat):
                for f in files:
                    sub = os.path.join(pads, os.path.basename(f)) if pads and key == "PAD_LIBS" else None
                    libs.append(sub if sub and os.path.isfile(sub) else f)
    for m in (cfg.get("MACROS") or {}).values():
        for pat, files in (m.get("lib") or {}).items():
            if fnmatch.fnmatch(corner, pat):
                libs += files
    rc = corner.split("_")[0]                       # nom / min / max
    spef = glob.glob(os.path.join(run, "final", "spef", rc, "*.spef"))
    return libs, (spef[0] if spef else None)


# pad pins of the PDKs' output pads: (data in, output enable, pad); both
# enables are active high
PAD_ARCS = [("A", "OE", "PAD"),                 # gf180mcu_fd_io bi_t / bi_24t
            ("c2p", "c2p_en", "pad")]           # sg13g2_io
# process corners, in the order they are printed: typical, slow, fast
CORNER_ORDER = ["tt", "typ", "ss", "slow", "ff", "fast"]


def sta_script(cfg, run, libs, spef, clk, load, edge):
    T, TH = clk["period"], clk["th"]
    q = lambda names: "{" + " ".join(names) + "}"
    L = [f"read_liberty {f}" for f in libs]
    L += [f"read_verilog {os.path.join(run, 'final', 'nl', cfg['DESIGN_NAME'] + '.nl.v')}",
          f"link_design {cfg['DESIGN_NAME']}"]
    if spef:
        L.append(f"read_spef {spef}")
    L += ["set_cmd_units -time ns -capacitance pF",
          "sta::set_recovery_removal_checks_enabled 1",
          # the CLK pin's edges are the datasheet's reference; the pad and
          # the clock tree are part of every path
          f"create_clock -name CLK -period {T} -waveform {{0 {TH}}} [get_ports CLK]",
          "set_propagated_clock [get_clocks CLK]",
          f"set_input_transition {edge} [get_ports CLK]",
          f"create_clock -name PIN -period {T} -waveform {{0 {TH}}}",
          f"set_load {load} [get_ports {q(OUTS)}]",
          f"set_input_transition {edge} [get_ports {q(INS)}]",
          f"set_output_delay 0 -clock PIN [get_ports {q(OUTS)}]",
          f"set_output_delay 0 -clock PIN -clock_fall -add_delay [get_ports {q(OUTS)}]",
          # the extreme arrival (out) or the worst slack (in) over all paths
          "proc q {key mm what args} {",
          "  set pes [find_timing_paths -path_delay $mm {*}$args -group_path_count 100 -endpoint_path_count 100]",
          "  set best {}",
          "  foreach pe $pes {",
          "    if {$what eq {arr}} { set v [sta::time_sta_ui [$pe data_arrival_time]] } else { set v [get_property $pe slack] }",
          "    if {$best eq {} || ($what eq {arr} && $mm eq {max} && $v > $best) || ($what eq {arr} && $mm eq {min} && $v < $best) || ($what eq {slack} && $v < $best)} { set best $v }",
          "  }",
          "  puts \"## $key $best\"",
          "}",
          "proc pad_arcs {cmd from_pin} {",
          "  foreach cell [get_lib_cells */*] {",
          "    foreach a {" + " ".join("{" + " ".join(t) + "}" for t in PAD_ARCS) + "} {",
          "      lassign $a d e p",
          "      set f [expr {$from_pin eq {data} ? $d : $e}]",
          "      catch { $cmd $cell -from $f -to $p }",
          "    }",
          "  }",
          "}"]
    # pass 1: output data only (the pads' enable arcs off)
    L.append("pad_arcs set_disable_timing enable")
    for p in OUTS:
        for e in ("rise", "fall"):
            for d in ("rise", "fall"):
                for m in ("max", "min"):
                    L.append(f"q {{out {p} {e} {d} {m}}} {m} arr -{e}_from [get_clocks CLK] -{d}_to [get_ports {{{p}}}]")
    L.append("pad_arcs unset_disable_timing enable")
    # pass 2: the output enables only -- the pad floating (its enable
    # falls) and turning on (rises), apart
    L.append("pad_arcs set_disable_timing data")
    for p in OUTS:
        if p not in OE_PADS:
            continue
        en = " ".join(f"{OE_PADS[p]}/{e}" for _, e, _ in PAD_ARCS)
        for e in ("rise", "fall"):
            for m in ("max", "min"):
                L.append(f"q {{off {p} {e} {m}}} {m} arr -{e}_from [get_clocks CLK] "
                         f"-fall_through [get_pins -quiet {{{en}}}] -to [get_ports {{{p}}}]")
                for d in ("rise", "fall"):
                    L.append(f"q {{on {p} {e} {d} {m}}} {m} arr -{e}_from [get_clocks CLK] "
                             f"-rise_through [get_pins -quiet {{{en}}}] -{d}_to [get_ports {{{p}}}]")
    L.append("pad_arcs unset_disable_timing data")
    # inputs, once against each CLK edge: input delay 0 at that PIN edge,
    # paths captured on the same CLK edge a period later (setup) or at the
    # same edge (hold)
    for e in ("rise", "fall"):
        cf = " -clock_fall" if e == "fall" else ""
        L.append(f"set_input_delay 0 -clock PIN{cf} [get_ports {q(INS)}]")
        for p in INS:
            for d in ("rise", "fall"):
                L.append(f"q {{in {p} {e} {d} setup}} max slack -{d}_from [get_ports {{{p}}}] -{e}_to [get_clocks CLK]")
                L.append(f"q {{in {p} {e} {d} hold}} min slack -{d}_from [get_ports {{{p}}}] -{e}_to [get_clocks CLK]")
    return "\n".join(L) + "\n"


def parse(text, clk):
    """{('out', port, launch, trans, max|min): delay from the CLK pin edge,
        ('off', port, launch, max|min): same, the pad floating (its enable),
        ('on', port, launch, trans, max|min): the pad turning on,
        ('in', port, edge, trans, setup|hold): requirement at the pin, ns}"""
    T, TH = clk["period"], clk["th"]
    res = {}
    for line in text.splitlines():
        if not line.startswith("## "):
            continue
        f = line[3:].split()
        if len(f) < 2 or f[-1] in ("", "{}"):
            continue
        try:
            v = float(f[-1])
        except ValueError:
            continue
        key = tuple(f[:-1])
        if key[0] in ("out", "off", "on"):
            res[key] = v - (TH if key[2] == "fall" else 0.0)
        elif key[0] == "in":
            res[key] = (T - v) if key[-1] == "setup" else -v
    return res


# ------------------------------------------------------------- the pads
def pad_dir(cfg, want):
    """The SPICE-characterised pad libraries for the run's PDK (pads.py),
    or None: the PDK's own."""
    if want == "none":
        return None
    if want != "auto":
        return want
    pdk = cfg.get("PDK", "")
    d = os.path.join(SPICE_PADS, "gf180mcu" if pdk.startswith("gf180mcu") else pdk)
    return d if os.path.isfile(os.path.join(d, "pads.json")) else None


class Levels:
    """The datasheet's measuring levels against STA's 50 %, at one corner:
    outputs at 1.5 V, from the pads' simulated waveforms at this load;
    inputs at 2.0 V rising / 0.8 V falling, on a linear edge of `edge` ns
    (10-90 %) at the pads' supply. `out_v` (VOH, VOL) moves the outputs'
    level: the September 1983 specification times them at 2.0 V rising
    and 0.8 V falling; the move from 1.5 V is read off the pad's simulated
    data waveform at the test load (both the data and the enable arcs)."""

    def __init__(self, pads, run, cfg, corner, load, edge, out_v=None):
        doc = json.load(open(os.path.join(pads, "pads.json")))
        self.c, cond = doc["corners"][corner], doc["conditions"]
        self.vil, self.vih, self.load, self.loads = cond["vil_V"], cond["vih_V"], load, cond["loads_pF"]
        self.slope = 0.8 * self.c["vio"] / edge
        self.out_v, self.out_level = out_v, cond["out_level_V"]
        nl = open(os.path.join(run, "final", "nl", cfg["DESIGN_NAME"] + ".nl.v")).read()
        self.cell = {inst: cell for cell, inst in re.findall(r"^\s*(\w+)\s+(pad_\w+)\s*\(", nl, re.M)}

    def pad_cell(self, port):
        """the pad cell of a port (its instance named as in z80_chip.v)"""
        return self.cell.get("pad_" + port.replace("[", "").replace("]", ""))

    def shift(self, port, trans):
        """ns from the pin's 1.5 V crossing to its out_v crossing (0 without
        out_v), from the data waveform: rising to VOH, falling to VOL"""
        if not self.out_v:
            return 0.0
        cell = self.pad_cell(port)
        W = self.c["cells"].get(cell, {}).get("data", {}).get("wave")
        if not W:
            return 0.0
        rising = trans == "rise"
        w = W[0 if rising else 1]

        def cross(v):
            for (t0, v0), (t1, v1) in zip(w, w[1:]):
                if (v0 < v <= v1) if rising else (v0 > v >= v1):
                    return t0 + (v - v0) * (t1 - t0) / (v1 - v0)
            raise SystemExit(f"pin_timing: {cell}'s simulated edge never crosses {v} V")
        return cross(self.out_v[0] if rising else self.out_v[1]) - cross(self.out_level)

    def out(self, port, arc, trans):
        """ns from the pin's 50 % to its 1.5 V crossing (data / enable arc)"""
        cell = self.pad_cell(port)
        D = self.c["cells"].get(cell, {}).get(arc)
        if D is None:
            return 0.0
        a = min(range(len(D["slews"])), key=lambda i: abs(D["slews"][i] - 0.5))
        row = D[trans][a] if arc == "data" else D["1" if trans == "rise" else "0"][a]
        off = [r["dlev"] - r["d50"] for r in row]
        k = min(max(bisect.bisect_left(self.loads, self.load), 1), len(self.loads) - 1)
        f = (self.load - self.loads[k - 1]) / (self.loads[k] - self.loads[k - 1])
        return off[k - 1] + f * (off[k] - off[k - 1])

    def need(self, trans, kind):
        """ns to add to a setup / hold requirement at the pin's 50 % to have
        it at VIH / VIL: rising data is valid from VIH, falling from VIL;
        old data stops being valid when it leaves VIH (falling) / VIL
        (rising)"""
        half = self.c["vio"] / 2
        if kind == "setup":
            return -(self.vih - half) / self.slope if trans == "rise" else -(half - self.vil) / self.slope
        return -(half - self.vil) / self.slope if trans == "rise" else (half - self.vih) / self.slope

    def apply(self, res):
        out = {}
        for key, v in res.items():
            if key[0] == "out":
                v += self.out(key[1], "data", key[3]) + self.shift(key[1], key[3])
            elif key[0] == "on":
                v += self.out(key[1], "enable", key[3]) + self.shift(key[1], key[3])
            elif key[0] == "in":
                v += self.need(key[3], key[4])
            out[key] = v
        return out


# ------------------------------------------------------------ the checks
class Contract:
    def __init__(self, grade, load, mhz=None):
        """grade's table; the clock its minimum period leaves, or `mhz` with
        the grade's edges and the rest split evenly between high and low"""
        self.spec = json.load(open(SPECS.get(str(grade), SPEC)))
        self.g, self.load, self.mhz = str(grade), load, mhz
        self.req = {r["id"]: r for r in self.spec["requirements"]}
        T = float(self.req[1]["table_ns"][self.g])        # TcC min
        tr = (T - 2 * float(self.req[2]["table_ns"][self.g])) / 2   # edges that leave TwCh = TwCl = min
        tr = min(tr, float(self.req[5]["table_ns"][self.g]))
        if mhz:
            T = 1000.0 / mhz
        twch = (T - 2 * tr) / 2
        self.clk = {"period": T, "tr": tr, "tf": tr, "twch": twch, "twcl": T - 2 * tr - twch,
                    "th": twch + tr, "tl": T - twch - tr}
        cond = self.spec.get("ac_test_conditions", {})
        self.out_v = (cond["VOH_V"], cond["VOL_V"]) if "VOH_V" in cond else None
        # PS0178: "for loading >= 50 pF, decrease width by 10 ns per 50 pF"
        # (parameters 10 and 11); the 1983 specification has no such rule
        self.derate_widths = "ac_test_conditions" not in self.spec

    def clock_limits(self, stretch_t=0):
        """the clock against the grade's own limits -- the system's, which
        RZ80, static, has no lower bound for: [(what, ok)]; stretch_t: the
        longest high phase, in extra T-states (CLK held high)"""
        c, r = self.clk, self.req
        hi = c["twch"] + stretch_t * c["period"]
        lim = [("TcC %.1f >= %g" % (c["period"], r[1]["table_ns"][self.g]), c["period"] >= r[1]["table_ns"][self.g]),
               ("TwCh %.1f >= %g" % (c["twch"], r[2]["table_ns"][self.g]), c["twch"] >= r[2]["table_ns"][self.g]),
               ("TwCl %.1f >= %g" % (c["twcl"], r[3]["table_ns"][self.g]), c["twcl"] >= r[3]["table_ns"][self.g]),
               ("TwCl %.1f <= %g" % (c["twcl"], r[3]["maximum_ns"]), c["twcl"] <= r[3]["maximum_ns"])]
        if "maximum_ns" in r[2]:
            lim.append(("TwCh %.1f <= %g" % (hi, r[2]["maximum_ns"]), hi <= r[2]["maximum_ns"]))
        return lim

    def limit(self, pid):
        """Datasheet limit; for the starred minimums the greater of the
        printed value and the footnote formula at this clock."""
        r = self.req[pid]
        v = float(r["table_ns"][self.g])
        f = r.get("formula")
        if f and r["bound"] == "min":
            c = self.clk
            env = {"period_ns": c["period"], "high_ns": c["twch"], "low_ns": c["twcl"],
                   "rise_ns": c["tr"], "fall_ns": c["tf"]}
            v = max(v, eval(f["base"], {"__builtins__": {}}, env) + float(f["offset_ns"][self.g]))
        if pid in (10, 11) and self.derate_widths:   # "for loading >= 50 pF, decrease width by 10 ns per 50 pF"
            v -= 10.0 * max(0.0, (self.load - 50.0) / 50.0)
        return v

    def checks(self, m):
        c, T, TH, TL = self.clk, self.clk["period"], self.clk["th"], self.clk["tl"]

        def ports(x):
            return GROUP.get(x, [x])

        def d(fn, x, launch, trans, mm):
            vals = [m.get(("out", p, launch, t, mm)) for p in ports(x)
                    for t in (("rise", "fall") if trans == "both" else (trans,))]
            vals = [v for v in vals if v is not None]
            return (max if mm == "max" else min)(vals) if vals else None

        dmax = lambda x, l, t: d(max, x, l, t, "max")
        dmin = lambda x, l, t: d(min, x, l, t, "min")

        def oe(kind, x, launch, mm):          # kind: off (float) / on
            vals = [m.get((kind, p, launch, mm) if kind == "off" else (kind, p, launch, t, mm))
                    for p in ports(x) for t in ((None,) if kind == "off" else ("rise", "fall"))]
            vals = [v for v in vals if v is not None]
            return (max if mm == "max" else min)(vals) if vals else None

        def need(x, edge, kind):
            vals = [m.get(("in", p, edge, t, kind)) for p in ports(x) for t in ("rise", "fall")]
            vals = [v for v in vals if v is not None]
            return max(vals) if vals else None

        def mx(*v):
            v = [x for x in v if x is not None]
            return max(v) if v else None

        def mn(*v):
            v = [x for x in v if x is not None]
            return min(v) if v else None

        def plus(*v):
            return None if any(x is None for x in v) else sum(v)

        out = []

        def check(pid, value, note=""):
            r, lim = self.req[pid], self.limit(pid)
            if r["bound"] == "max" or r["kind"] in ("input_setup", "input_hold"):
                ok = value is not None and value <= lim
                slack = None if value is None else lim - value
            else:
                ok = value is not None and value >= lim
                slack = None if value is None else value - lim
            out.append((pid, r["symbol"], value, lim, r["bound"], slack, ok, note))

        # ---- output delays from the CLK pin's edges ------------------
        for pid, x, l, t in [(6, "A", "rise", "both"), (8, "MREQ_n", "fall", "fall"),
                             (9, "MREQ_n", "rise", "rise"), (12, "MREQ_n", "fall", "rise"),
                             (13, "RD_n", "fall", "fall"), (14, "RD_n", "rise", "rise"),
                             (19, "M1_n", "rise", "fall"), (20, "M1_n", "rise", "rise"),
                             (21, "RFSH_n", "rise", "fall"), (22, "RFSH_n", "rise", "rise"),
                             (23, "RD_n", "fall", "rise"), (24, "RD_n", "rise", "fall"),
                             (27, "IORQ_n", "rise", "fall"), (28, "IORQ_n", "fall", "rise"),
                             (30, "WR_n", "fall", "fall"), (32, "WR_n", "fall", "rise"),
                             (34, "WR_n", "rise", "fall"), (36, "HALT_n", "fall", "both"),
                             (40, "BUSAK_n", "rise", "fall"), (41, "BUSAK_n", "fall", "rise"),
                             (51, "IORQ_n", "fall", "fall"), (52, "IORQ_n", "rise", "rise")]:
            check(pid, dmax(x, l, t))
        # data valid from CLK fall: driven value or the pad turning on
        dvalid = mx(dmax("D", "fall", "both"), oe("on", "D", "fall", "max"))
        check(53, dvalid)
        # float delays: the pads' output enables released from CLK rise
        check(42, oe("off", "D", "rise", "max"), "data float")
        check(43, mx(*[oe("off", p, "rise", "max") for p in ("MREQ_n", "IORQ_n", "RD_n", "WR_n")]), "control float")
        check(44, oe("off", "A", "rise", "max"), "address float")

        # ---- inputs: what the replica needs vs what the system gives --
        check(15, need("D", "rise", "setup"), "M1 / INTA data")
        check(25, need("D", "fall", "setup"), "M2-M5 read data")
        # read data is held until RD (IORQ in INTA) rises, which happens on
        # the sampling edge itself
        hold_r = need("D", "rise", "hold")
        hold_f = need("D", "fall", "hold")
        rd_up = mn(dmin("RD_n", "rise", "rise"), dmin("IORQ_n", "rise", "rise"))
        v16 = mx(plus(hold_r, None if rd_up is None else -rd_up),
                 plus(hold_f, None if dmin("RD_n", "fall", "rise") is None else -dmin("RD_n", "fall", "rise")))
        check(16, v16, "hold past RD rise")
        check(17, need("WAIT_n", "fall", "setup")); check(18, need("WAIT_n", "fall", "hold"))
        check(38, need("BUSRQ_n", "rise", "setup")); check(39, need("BUSRQ_n", "rise", "hold"))
        check(46, need("RESET_n", "rise", "setup")); check(47, need("RESET_n", "rise", "hold"))
        check(48, need("INT_n", "rise", "setup"), "sampled on CLK rise?")
        check(49, need("INT_n", "rise", "hold"))

        # ---- pulse widths and pin-to-pin separations -----------------
        strobe_up = mx(*[dmax(p, "fall", "rise") for p in ("MREQ_n", "IORQ_n", "RD_n", "WR_n")])
        a_chg = dmin("A", "rise", "both")
        d_chg = mn(dmin("D", "rise", "both"), oe("off", "D", "rise", "min"), oe("on", "D", "rise", "min"))
        sep = {
            7: plus(TH, dmin("MREQ_n", "fall", "fall"), -(dmax("A", "rise", "both") or 0)) if dmax("A", "rise", "both") is not None else None,
            10: plus(TH, dmin("MREQ_n", "fall", "fall"), -(dmax("MREQ_n", "rise", "rise") or 0)) if dmax("MREQ_n", "rise", "rise") is not None else None,
            11: plus(T, dmin("MREQ_n", "fall", "rise"), -(dmax("MREQ_n", "fall", "fall") or 0)) if dmax("MREQ_n", "fall", "fall") is not None else None,
            26: plus(T, dmin("IORQ_n", "rise", "fall"), -(dmax("A", "rise", "both") or 0)) if dmax("A", "rise", "both") is not None else None,
            29: plus(T, dmin("WR_n", "fall", "fall"), -dvalid) if dvalid is not None else None,
            31: plus(T, dmin("WR_n", "fall", "rise"), -(dmax("WR_n", "fall", "fall") or 0)) if dmax("WR_n", "fall", "fall") is not None else None,
            33: plus(TL, dmin("WR_n", "rise", "fall"), -dvalid) if dvalid is not None else None,
            35: plus(TL, d_chg, -(dmax("WR_n", "fall", "rise") or 0)) if dmax("WR_n", "fall", "rise") is not None else None,
            45: plus(TL, a_chg, -strobe_up) if strobe_up is not None else None,
            50: plus(2 * T + TH, dmin("IORQ_n", "fall", "fall"), -(dmax("M1_n", "rise", "fall") or 0)) if dmax("M1_n", "rise", "fall") is not None else None,
        }
        notes = {7: "A valid T1 rise -> MREQ fall T1 fall", 10: "MREQ high in M1 (T3 rise -> T3 fall)",
                 11: "MREQ low in refresh (T3 fall -> T4 fall)", 26: "A valid T1 rise -> IORQ fall T2 rise",
                 29: "data T1 fall -> WR fall T2 fall (memory)", 31: "WR low T2 fall -> T3 fall (memory)",
                 33: "data T1 fall -> WR fall T2 rise (I/O)", 35: "WR rise T3 fall -> data changes next T1 rise",
                 45: "strobes rise T3 fall -> A changes next T1 rise", 50: "M1 fall T1 rise -> IORQ fall TwA fall"}
        for pid, v in sep.items():
            check(pid, v, notes[pid])
        return sorted(out)


# ------------------------------------------------------------- the SDC
# The flow's constraints for z80_chip: the same contract as LibreLane SDC,
# so placement, CTS and the resizer (hold repair on the inputs) and the
# chip's own sign-off STA work against the datasheet. Each output launched
# on one CLK edge is checked only against the datasheet's limit from that
# edge, each input only against the window around the edge that samples it.
SDC_OUT = [   # (ports, reference edge, transition or None, parameters)
    (["A"], "rise", None, [6, 44]),
    (["D"], "fall", None, [53]), (["D"], "rise", None, [42]),
    (["MREQ_n"], "fall", "fall", [8]), (["MREQ_n"], "fall", "rise", [12]),
    (["MREQ_n"], "rise", "rise", [9, 43]), (["MREQ_n"], "rise", "fall", [43]),
    (["RD_n"], "fall", "fall", [13]), (["RD_n"], "fall", "rise", [23]),
    (["RD_n"], "rise", "rise", [14, 43]), (["RD_n"], "rise", "fall", [24, 43]),
    (["IORQ_n"], "rise", "fall", [27, 43]), (["IORQ_n"], "rise", "rise", [52, 43]),
    (["IORQ_n"], "fall", "rise", [28]), (["IORQ_n"], "fall", "fall", [51]),
    (["WR_n"], "fall", "fall", [30]), (["WR_n"], "fall", "rise", [32]),
    (["WR_n"], "rise", "fall", [34, 43]), (["WR_n"], "rise", "rise", [43]),
    (["M1_n"], "rise", "fall", [19]), (["M1_n"], "rise", "rise", [20]),
    (["RFSH_n"], "rise", "fall", [21]), (["RFSH_n"], "rise", "rise", [22]),
    (["HALT_n"], "fall", None, [36]),
    (["BUSAK_n"], "rise", "fall", [40]), (["BUSAK_n"], "fall", "rise", [41]),
]
SDC_IN = [    # (ports, sampling edge, setup parameter, hold parameter)
    (["D"], "rise", 15, 16), (["D"], "fall", 25, 16),
    (["WAIT_n"], "fall", 17, 18), (["BUSRQ_n"], "rise", 38, 39),
    (["RESET_n"], "rise", 46, 47), (["INT_n"], "rise", 48, 49),
]


def write_sdc(con, path, load, edge):
    c, g = con.clk, con.g
    T, TH = c["period"], c["th"]
    names = lambda ps: "{" + " ".join(n for p in ps for n in GROUP.get(p, [p])) + "}"
    L = [f"# Generated by asic/timing/pin_timing.py --write-sdc (grade {g} MHz); do not edit.",
         "# z80_chip's pins against the NMOS Z80's AC characteristics: Zilog",
         f"# PS0178, Z8400 (NMOS) table, {g} MHz column (z8400_nmos.json). Clock",
         f"# {T:g} ns, {c['twch']:g}/{c['twcl']:g} ns high/low, {edge:g} ns edges; {load:g} pF loads.",
         "current_design $::env(DESIGN_NAME)",
         f"create_clock -name CLK -period {T:g} -waveform {{0 {TH:g}}} [get_ports CLK]",
         f"set_input_transition {edge:g} [get_ports CLK]",
         "# the CLK pin's edges, for the I/O constraints: the CLK pad and the clock",
         "# tree are part of every I/O path, as in the datasheet's measurements",
         f"create_clock -name PIN -period {T:g} -waveform {{0 {TH:g}}}",
         "set_false_path -rise_from [get_clocks CLK] -fall_to [get_clocks PIN]",
         "set_false_path -fall_from [get_clocks CLK] -rise_to [get_clocks PIN]",
         "set_false_path -rise_from [get_clocks PIN] -fall_to [get_clocks CLK]",
         "set_false_path -fall_from [get_clocks PIN] -rise_to [get_clocks CLK]",
         f"set_load {load / 1:g} [get_ports {names(['A', 'D'] + CTRL)}]",
         f"set_input_transition {edge:g} [get_ports {names(['D', 'WAIT_n', 'INT_n', 'NMI_n', 'BUSRQ_n', 'RESET_n'])}]",
         "# NMI is edge-triggered and asynchronous (the pulse catcher in z80_top)",
         "set_false_path -from [get_ports NMI_n]",
         "# RESET floats the address bus directly (z80_chip.v), unclocked",
         "set_false_path -from [get_ports RESET_n] -to [get_ports {A[*]}]",
         "# MREQ IORQ RD WR float while BUSACK is low and come back high: their",
         "# pads, enabled by z80_top's busack_n, never turn on driving low",
         "set_false_path -rise_through [get_pins u_top/busack_n] -fall_to [get_ports {MREQ_n IORQ_n RD_n WR_n}]",
         "# read data is held until RD (IORQ in INTA) rises, not 0 ns past the",
         "# sampling edge (P16 ThD(RDr)): pin_timing.py checks the replica's hold",
         "# against its own RD / IORQ rise; the flow does not repair a hold the",
         "# datasheet does not ask for",
         "set_false_path -hold -from [get_ports {D[*]}]",
         "set_max_fanout $::env(MAX_FANOUT_CONSTRAINT) [current_design]",
         "if { [info exists ::env(MAX_TRANSITION_CONSTRAINT)] } { set_max_transition $::env(MAX_TRANSITION_CONSTRAINT) [current_design] }",
         "set_clock_uncertainty $::env(CLOCK_UNCERTAINTY_CONSTRAINT) [get_clocks CLK]",
         "set_timing_derate -early [expr 1 - $::env(TIME_DERATING_CONSTRAINT) / 100.0]",
         "set_timing_derate -late [expr 1 + $::env(TIME_DERATING_CONSTRAINT) / 100.0]",
         "# outputs: at most D after the CLK pin's edge (max T - D), and not before it"]
    for ports, ref, trans, pids in SDC_OUT:
        d = min(con.limit(i) for i in pids)
        cf = " -clock_fall" if ref == "fall" else ""
        tr = f" -{trans}" if trans else ""
        sym = ", ".join(f"P{i} {con.req[i]['symbol']}" for i in pids)
        L.append(f"set_output_delay -add_delay -clock PIN{cf}{tr} -max {T - d:g} [get_ports {names(ports)}]   ;# {sym} <= {d:g}")
        L.append(f"set_output_delay -add_delay -clock PIN{cf}{tr} -min 0 [get_ports {names(ports)}]")
    L.append("# inputs: valid S before the sampling edge (max T - S), held H after it")
    for ports, ref, sp, hp in SDC_IN:
        cf = " -clock_fall" if ref == "fall" else ""
        S_, H_ = con.limit(sp), con.limit(hp)
        L.append(f"set_input_delay -add_delay -clock PIN{cf} -max {T - S_:g} [get_ports {names(ports)}]   ;# P{sp} {con.req[sp]['symbol']} = {S_:g}")
        L.append(f"set_input_delay -add_delay -clock PIN{cf} -min {H_:g} [get_ports {names(ports)}]   ;# P{hp} {con.req[hp]['symbol']} = {H_:g}")
    open(path, "w").write("\n".join(L) + "\n")


def measure(sta, cfg, run, corner, clk, load, edge, keep, pads=None, tag=""):
    """OpenSTA's measurements at one corner for one clock and input edge:
    parse()'s, from the CLK pin's edges -- independent of the period"""
    libs, spef = corner_files(run, cfg, corner, pads)
    script = sta_script(cfg, run, libs, spef, clk, load, edge)
    with tempfile.TemporaryDirectory() as tmp:
        d = keep or tmp
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, f"pin_timing_{corner}{tag}.tcl")
        open(path, "w").write(script)
        p = subprocess.run(shlex.split(sta) + ["-no_init", "-exit", path], capture_output=True, text=True)
        if keep:
            open(os.path.join(d, f"pin_timing_{corner}{tag}.log"), "w").write(p.stdout + p.stderr)
    if p.returncode != 0 or "## " not in p.stdout:
        sys.exit(f"pin_timing: OpenSTA failed for {corner}:\n" + (p.stderr or p.stdout)[-2000:])
    return parse(p.stdout, clk)


def run_corner(sta, cfg, run, corner, contract, edge, keep, pads=None, levels=False):
    res = measure(sta, cfg, run, corner, contract.clk, contract.load, edge, keep, pads)
    if levels:
        res = Levels(pads, run, cfg, corner.split("_")[1], contract.load, edge, contract.out_v).apply(res)
    return res


# the ZX Spectrum's ULA holds CLK high through contended accesses, up to six
# extra T-states (its 6, 5, 4, 3, 2, 1, 0, 0 pattern)
STRETCH_T = {"spectrum": 6, "spectrum128": 6}


def title(point, con, cfg, run, load):
    """the header line of a point's table"""
    g, mhz, label = POINTS[point]
    c = con.clk
    clock = (f"clock {c['period']:g} ns, {c['twch']:g}/{c['twcl']:g} ns high/low, "
             f"{c['tr']:g} ns edges; {load:g} pF loads")
    head = f"{cfg['DESIGN_NAME']} ({os.path.basename(os.path.normpath(run))}) vs Zilog Z8400 NMOS, "
    if g == "2.5" and not mhz:
        return head + f"2.5 MHz (Z80 CPU, September 1983 specification): " + clock
    if mhz:
        c = {k: round(v, 1) for k, v in c.items()}
        clock = (f"clock {c['period']:g} ns, {c['twch']:g}/{c['twcl']:g} ns high/low, "
                 f"{c['tr']:g} ns edges; {load:g} pF loads")
        return head + f"{g} MHz table at the {label}'s {mhz:.5g} MHz: " + clock
    return head + f"{g} MHz: " + clock


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("run", nargs="?")
    ap.add_argument("--grade", default="4", help="the points to check, comma-separated: NMOS speed grades "
                    "2.5 4 6 8 (MHz) and machines " + " ".join(p for p in ALL_POINTS if POINTS[p][1])
                    + " (default 4: Z80A)")
    ap.add_argument("--all", action="store_true", help="every point: " + ",".join(ALL_POINTS))
    ap.add_argument("--load", type=float, default=100.0, help="output load, pF (datasheet test load: 100)")
    ap.add_argument("--edge", type=float, default=None, help="clock and input edge, ns (default: what the grade's minimum period leaves)")
    ap.add_argument("--corners", default="", help="process corners to check, e.g. tt,ss (default: all of the run's)")
    ap.add_argument("--sta", default=os.environ.get("STA", "sta"))
    ap.add_argument("--keep", help="keep the OpenSTA scripts and logs in this directory")
    ap.add_argument("--pads", default="auto", help="SPICE-characterised pad libraries: auto (../spice/pads/<pdk>/ "
                    "if there), none (the PDK's), or a directory")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="OpenSTA runs at once (default: the CPUs)")
    ap.add_argument("--write-sdc", metavar="FILE", help="write the flow's SDC for this grade instead (RUN is ignored)")
    a = ap.parse_args()
    points = ALL_POINTS if a.all else [g for g in a.grade.split(",") if g]
    bad = [g for g in points if g not in POINTS]
    if bad or not points:
        ap.error(f"--grade: unknown point(s) {', '.join(bad) or '(none)'}; one of {', '.join(ALL_POINTS)}")
    if a.write_sdc:
        if len(points) != 1 or POINTS[points[0]][1]:
            ap.error("--write-sdc takes one grade: 2.5, 4, 6 or 8")
        con = Contract(points[0], a.load)
        write_sdc(con, a.write_sdc, a.load, a.edge if a.edge is not None else con.clk["tr"])
        return 0
    if not a.run or not os.path.isfile(os.path.join(a.run, "resolved.json")):
        print(f"pin_timing: {a.run} is not a LibreLane run"); return 2
    cfg = json.load(open(os.path.join(a.run, "resolved.json")))
    cons = {pt: Contract(POINTS[pt][0], a.load, POINTS[pt][1]) for pt in points}
    edges = {pt: a.edge if a.edge is not None else cons[pt].clk["tr"] for pt in points}
    want = [k for k in a.corners.split(",") if k]
    corners = [c for c in cfg["STA_CORNERS"] if c.startswith("nom_") and (not want or c.split("_")[1] in want)]
    corners.sort(key=lambda c: CORNER_ORDER.index(c.split("_")[1]) if c.split("_")[1] in CORNER_ORDER else len(CORNER_ORDER))
    if not corners:
        print(f"pin_timing: no corner of {a.run} matches {a.corners!r}"); return 2
    pads = pad_dir(cfg, a.pads)
    if pads:
        have = json.load(open(os.path.join(pads, "pads.json")))["corners"]
        missing = [k for k in corners if k.split("_")[1] not in have]
        if missing:
            print(f"pin_timing: {pads} has no corner {', '.join(missing)}"); return 2
    # one OpenSTA run per corner and edge, with the clock of a grade that
    # has that edge (the measurements do not depend on the period)
    ref = {}
    for pt in sorted(points, key=lambda pt: POINTS[pt][1] is not None):
        ref.setdefault(edges[pt], pt)
    tag = (lambda e: "") if len(ref) == 1 else (lambda e: f"_edge{e:g}")
    with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        futs = {(k, e): ex.submit(measure, a.sta, cfg, a.run, k, cons[pt].clk, a.load, e, a.keep, pads, tag(e))
                for k in corners for e, pt in ref.items()}
        raw = {key: f.result() for key, f in futs.items()}
    ok_points = {}
    for n, pt in enumerate(points):
        con = cons[pt]
        results = {}
        for k in corners:
            m = raw[(k, edges[pt])]
            if pads:
                m = Levels(pads, a.run, cfg, k.split("_")[1], a.load, edges[pt], con.out_v).apply(m)
            results[k] = m
        if n:
            print()
        ok_points[pt] = report(pt, con, cfg, a, corners, results, pads)
    if len(points) > 1:
        print("\n  " + ", ".join(f"{pt}: {'PASS' if ok else 'FAIL'}" for pt, ok in ok_points.items()))
    return 0 if all(ok_points.values()) else 1


def report(pt, con, cfg, a, corners, results, pads):
    """print one point's table; True when every check passes"""
    g, mhz, label = POINTS[pt]
    print(title(pt, con, cfg, a.run, a.load))
    lv = "2.0 / 0.8 V" if con.out_v else "1.5 V"
    print("pads: " + (f"simulated at transistor level ({os.path.relpath(pads, os.path.join(HERE, '..', '..'))}); outputs at {lv}, "
                      f"inputs at 2.0 / 0.8 V, CLK at 50 %" if pads else
                      "the PDK's Liberty (extrapolated beyond its range); every pin at 50 %"))
    if mhz or g == "2.5":
        lim = con.clock_limits(STRETCH_T.get(pt, 0))
        print("clock: " + ", ".join(w for w, _ in lim)
              + (f" (CLK held high {STRETCH_T[pt]} T-states more by the ULA)" if pt in STRETCH_T else "")
              + ": " + ("within the NMOS Z80's limits" if all(ok for _, ok in lim) else "OUTSIDE the NMOS Z80's limits")
              + "; RZ80 is static: no upper bound on a clock phase")
    rows = {k: con.checks(m) for k, m in results.items()}
    ok_all = True
    first = rows[corners[0]]
    print(f"\n  {'#':>2} {'parameter':16} {'limit':>9}   " + "".join(f"{k.split('_')[1]:>9}" for k in corners) + "   worst slack")
    for i, (pid, sym, _, lim, bound, _, _, note) in enumerate(first):
        vals = [rows[k][i] for k in corners]
        slacks = [v[5] for v in vals]
        ok = all(v[6] for v in vals)
        ok_all &= ok
        worst = None if any(s is None for s in slacks) else min(slacks)
        cells = "".join(f"{v[2]:9.1f}" if v[2] is not None else f"{'-':>9}" for v in vals)
        flag = "" if ok else "   <-- FAIL"
        lim_s = f"{'<=' if bound == 'max' or pid in (15, 16, 17, 18, 25, 38, 39, 46, 47, 48, 49) else '>='}{lim:6.1f}"
        print(f"  {pid:2d} {sym:16} {lim_s:>9}   {cells}   {'n/a' if worst is None else f'{worst:+.1f}'}{flag}"
              + (f"   ({note})" if note and not ok else ""))
    name = f"{g} MHz" if not mhz else f"{g} MHz at the {label}'s {mhz:.5g} MHz"
    print(f"\n  pin timing vs NMOS {name}: {'PASS' if ok_all else 'FAIL'}")
    return ok_all


if __name__ == "__main__":
    sys.exit(main())
