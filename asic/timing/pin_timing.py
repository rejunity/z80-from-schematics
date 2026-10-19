#!/usr/bin/env python3
"""pin_timing.py - check a z80_chip layout against the NMOS Z80's AC
characteristics: Zilog PS0178, Z8400 (NMOS) table, transcribed with its
formulas in z8400_nmos.json.

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
at every process corner. The datasheet's waveforms put each pin event on
a clock edge (address at T1 rise, MREQ at T1 fall, ...); this measures the
analog timing of those edges. That the replica's events fall on those
edges is the cycle-level tests' job (perfectz80 pin comparisons).

usage: pin_timing.py RUN [--grade 4|6|8] [--load PF] [--edge NS]
                         [--corners tt,ss,ff] [--sta PATH] [--keep DIR]
       pin_timing.py --write-sdc FILE [--grade ...] [--load ...] x
         (the same contract as the chip flow's SDC)
exit: 0 every check passes at every corner, 1 a check fails, 2 no run.
"""
import argparse, fnmatch, glob, json, os, re, shlex, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = os.path.join(HERE, "z8400_nmos.json")

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
def corner_files(run, cfg, corner):
    """Liberty files and the parasitics of one STA corner (nom_tt_025C_5v00)."""
    libs = []
    for key in ("LIB", "CELL_LIBS", "PAD_LIBS"):
        for pat, files in (cfg.get(key) or {}).items():
            if fnmatch.fnmatch(corner, pat):
                libs += files
    for m in (cfg.get("MACROS") or {}).values():
        for pat, files in (m.get("lib") or {}).items():
            if fnmatch.fnmatch(corner, pat):
                libs += files
    rc = corner.split("_")[0]                       # nom / min / max
    spef = glob.glob(os.path.join(run, "final", "spef", rc, "*.spef"))
    return libs, (spef[0] if spef else None)


# pad pins of the PDKs' output pads: (data in, output enable, pad)
PAD_ARCS = [("A", "OE", "PAD"),                 # gf180mcu_fd_io bi_t / bi_24t
            ("c2p", "c2p_en", "pad")]           # sg13g2_io


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
    # pass 1: output data only (the pads' enable arcs off); pass 2: the
    # output enables only (float and turn-on)
    for kind, off in (("out", "enable"), ("oe", "data")):
        L.append(f"pad_arcs set_disable_timing {off}")
        for p in OUTS:
            for e in ("rise", "fall"):
                for d in (("rise", "fall") if kind == "out" else ("both",)):
                    to = f"-{d}_to" if d != "both" else "-to"
                    for m in ("max", "min"):
                        key = f"{kind} {p} {e} {d} {m}" if kind == "out" else f"oe {p} {e} {m}"
                        L.append(f"q {{{key}}} {m} arr -{e}_from [get_clocks CLK] {to} [get_ports {{{p}}}]")
        L.append(f"pad_arcs unset_disable_timing {off}")
    # inputs, once against each CLK edge: input delay 0 at that PIN edge,
    # paths captured on the same CLK edge a period later (setup) or at the
    # same edge (hold)
    for e in ("rise", "fall"):
        cf = " -clock_fall" if e == "fall" else ""
        L.append(f"set_input_delay 0 -clock PIN{cf} [get_ports {q(INS)}]")
        for p in INS:
            L.append(f"q {{in {p} {e} setup}} max slack -from [get_ports {{{p}}}] -{e}_to [get_clocks CLK]")
            L.append(f"q {{in {p} {e} hold}} min slack -from [get_ports {{{p}}}] -{e}_to [get_clocks CLK]")
    return "\n".join(L) + "\n"


def parse(text, clk):
    """{('out', port, launch, trans, max|min): delay from the CLK pin edge,
        ('oe', port, launch, max|min): same, through the pad's enable,
        ('in', port, edge, setup|hold): requirement at the pin, ns}"""
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
        if key[0] == "out" or key[0] == "oe":
            res[key] = v - (TH if key[2] == "fall" else 0.0)
        elif key[0] == "in":
            res[key] = (T - v) if key[3] == "setup" else -v
    return res


# ------------------------------------------------------------ the checks
class Contract:
    def __init__(self, grade, load):
        spec = json.load(open(SPEC))
        self.g, self.load = str(grade), load
        self.req = {r["id"]: r for r in spec["requirements"]}
        T = float(self.req[1]["table_ns"][self.g])        # TcC min
        tr = (T - 2 * float(self.req[2]["table_ns"][self.g])) / 2   # edges that leave TwCh = TwCl = min
        tr = min(tr, float(self.req[5]["table_ns"][self.g]))
        twch = (T - 2 * tr) / 2
        self.clk = {"period": T, "tr": tr, "tf": tr, "twch": twch, "twcl": T - 2 * tr - twch,
                    "th": twch + tr, "tl": T - twch - tr}

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
        if pid in (10, 11):        # "for loading >= 50 pF, decrease width by 10 ns per 50 pF"
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

        def oe(x, launch, mm):
            vals = [m.get(("oe", p, launch, mm)) for p in ports(x)]
            vals = [v for v in vals if v is not None]
            return (max if mm == "max" else min)(vals) if vals else None

        def need(x, edge, kind):
            vals = [m.get(("in", p, edge, kind)) for p in ports(x)]
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
        dvalid = mx(dmax("D", "fall", "both"), oe("D", "fall", "max"))
        check(53, dvalid)
        # float delays: the pads' output enables released from CLK rise
        check(42, oe("D", "rise", "max"), "data float")
        check(43, mx(*[oe(p, "rise", "max") for p in ("MREQ_n", "IORQ_n", "RD_n", "WR_n")]), "control float")
        check(44, oe("A", "rise", "max"), "address float")

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
        d_chg = mn(dmin("D", "rise", "both"), oe("D", "rise", "min"))
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


def run_corner(sta, cfg, run, corner, contract, edge, keep):
    libs, spef = corner_files(run, cfg, corner)
    script = sta_script(cfg, run, libs, spef, contract.clk, contract.load, edge)
    with tempfile.TemporaryDirectory() as tmp:
        d = keep or tmp
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, f"pin_timing_{corner}.tcl")
        open(path, "w").write(script)
        p = subprocess.run(shlex.split(sta) + ["-no_init", "-exit", path], capture_output=True, text=True)
        if keep:
            open(os.path.join(d, f"pin_timing_{corner}.log"), "w").write(p.stdout + p.stderr)
    if p.returncode != 0 or "## " not in p.stdout:
        sys.exit(f"pin_timing: OpenSTA failed for {corner}:\n" + (p.stderr or p.stdout)[-2000:])
    return parse(p.stdout, contract.clk)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("run")
    ap.add_argument("--grade", default="4", choices=["4", "6", "8"], help="NMOS speed grade, MHz (default 4: Z80A)")
    ap.add_argument("--load", type=float, default=100.0, help="output load, pF (datasheet test load: 100)")
    ap.add_argument("--edge", type=float, default=None, help="clock and input edge, ns (default: what the grade's minimum period leaves)")
    ap.add_argument("--corners", default="tt,ss,ff")
    ap.add_argument("--sta", default=os.environ.get("STA", "sta"))
    ap.add_argument("--keep", help="keep the OpenSTA scripts and logs in this directory")
    ap.add_argument("--write-sdc", metavar="FILE", help="write the flow's SDC for this grade instead (RUN is ignored)")
    a = ap.parse_args()
    if a.write_sdc:
        con = Contract(a.grade, a.load)
        write_sdc(con, a.write_sdc, a.load, a.edge if a.edge is not None else con.clk["tr"])
        return 0
    if not os.path.isfile(os.path.join(a.run, "resolved.json")):
        print(f"pin_timing: {a.run} is not a LibreLane run"); return 2
    cfg = json.load(open(os.path.join(a.run, "resolved.json")))
    con = Contract(a.grade, a.load)
    edge = a.edge if a.edge is not None else con.clk["tr"]
    corners = [c for c in cfg["STA_CORNERS"] if c.startswith("nom_") and
               any(f"_{k}_" in c for k in a.corners.split(","))]
    c = con.clk
    print(f"{cfg['DESIGN_NAME']} ({os.path.basename(os.path.normpath(a.run))}) vs Zilog Z8400 NMOS, "
          f"{a.grade} MHz: clock {c['period']:g} ns, {c['twch']:g}/{c['twcl']:g} ns high/low, "
          f"{c['tr']:g} ns edges; {a.load:g} pF loads")
    results = {k: run_corner(a.sta, cfg, a.run, k, con, edge, a.keep) for k in corners}
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
    print(f"\n  pin timing vs NMOS {a.grade} MHz: {'PASS' if ok_all else 'FAIL'}")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
