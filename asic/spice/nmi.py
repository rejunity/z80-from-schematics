#!/usr/bin/env python3
"""nmi.py - the NMI pulse catcher at transistor level, cut out of the
extracted GF180 chip (chip.py's `extract`) from the NMI and CLK pins to the
first flip-flops and simulated on its own in ngspice.

The NMOS Z80 takes an NMI pulse of any width (TwNMI, 80 ns at 4 MHz, is
under half a clock): its NMI flip-flop is set by the pin's falling edge.
z80_top does the same (rtl/z80_top.v):

  NMI_n pin -> in_c pad -> input11 (port buffer) -> _249_, nmi_hold: a
      flip-flop clocked by the pin's falling edge, D tied high
  _170_ (nor2): nmi_n_held = nmi_n & ~nmi_hold -- what the core samples
  _248_, nmi_hold_clr: on the next rising CLK edge, clears nmi_hold
      through _070_ (inverter) once that edge has seen it
  _246_, nmi_n_neg: the falling-edge sampler of nmi_n_held
  CLK pin -> in_s pad -> clkbuf_0 -> clkbuf_2_1 -> clkbuf_leaf_13 (the
      leaf that clocks _248_ and _246_)

The cut is found in the chip's gate-level netlists by the RTL's net names
(nmi_hold, nmi_hold_clr, nmi_n_held, nmi_n_neg) and followed back through
buffers and inverters to the pins. Its transistors and wires come either
from the cells' layouts and the routed layout's own extraction (SPEF;
the default, a minute -- what CI runs) or, with --magic, from Magic's
extraction of the whole chip (chip.py extract). Each net carries its wire
capacitance plus the input capacitance (Liberty) of the cells on it that
the cut leaves out: the other flip-flops on the clock leaf, the core gates
that read nmi_n_held and nmi_n_neg, the clock tree's other branches. (The
default extracts the cut's standard cells with Magic from the PDK's cell
layouts; the PDK's own cell netlists carry no junction areas.)

Six cases at 4 MHz, 15 ns edges on both pins:
  A  an 80 ns pulse (TwNMI) inside the low half of the clock
  B  a 20 ns pulse, a quarter of TwNMI
  C  NMI falling 5 ns before a rising CLK edge
  D  NMI falling 5 ns after a rising CLK edge
  E  two 25 ns pulses before one rising edge: one NMI
  F  a second pulse while the first is being cleared
A, B, E and F sit in the middle of a clock phase, clear of CLK's edges.
Each corner's figures and numbers go to --out (nmi_<corner>.svg, the
cases' nmi_<case>_<corner>.svg, nmi_<corner>.json). Exits 1 when a case's
pulse is not caught, held for the core, and cleared at the next edge.

usage: nmi.py CHIPRUN [--core RUN] [--magic WORK] [--corners tt,ss,ff] [--out DIR]
  CHIPRUN  the chip's LibreLane run (asic-gf180mcu chip/runs/<tag>)
"""
import argparse, bisect, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import svgplot  # noqa: E402
import chip  # noqa: E402

OUTS = {"Z", "ZN", "Q", "Y"}
SUP = {"VDD", "VSS", "VNW", "VPW", "DVDD", "DVSS"}
# what the plots show, by (role, pin): the roles are find_cut()'s
SIGNALS = [("NMI pin", "pad_nmi", "PAD"), ("NMI in the core", "hold", "CLK*"), ("CLK pin", "pad_clk", "PAD"),
           ("CLK at the flip-flops", "clr", "CLK"), ("nmi_hold", "hold", "Q"), ("nmi_hold_clr", "clr", "Q"),
           ("nmi_n_held", "held", "OUT"), ("nmi_n_neg", "neg", "Q")]
PERIOD, EDGE = 250.0, 15.0
T_RESET = 600.0              # reset released, after the first clocks


def parse(path):
    """a SPICE file's subcircuits: {name: ports}, {name: body lines}"""
    ports, body, cur = {}, {}, None
    for line in open(path).read().replace("\n+", " ").splitlines():
        t = line.split()
        if not t or t[0].startswith("*"):
            continue
        if t[0].lower() == ".subckt":
            cur = t[1]
            ports[cur] = [x for x in t[2:] if "=" not in x]
            body[cur] = []
            continue
        if t[0].lower() == ".ends":
            cur = None
            continue
        if cur:
            body[cur].append(line)
    return ports, body


def verilog(path):
    """a gate-level netlist: {instance: (cell, {pin: net})}"""
    text = open(path).read()
    out = {}
    for m in re.finditer(r"^\s*(\w+)\s+(\\\S+\s|\w+)\s*\((.*?)\);", text, re.S | re.M):
        cell, name, body = m.group(1), m.group(2).strip(), m.group(3)
        if cell in ("module", "input", "output", "inout", "wire", "assign"):
            continue
        pins = {p: n.strip() for p, n in re.findall(r"\.(\w+)\s*\(\s*(\\\S+\s|[^()\s]*)\s*\)", body)}
        out[name] = (cell, pins)
    return out


def drivers(insts):
    return {n: (name, p) for name, (cell, pins) in insts.items() for p, n in pins.items() if p in OUTS}


def clock_pin(pins):
    return next(p for p in pins if p.startswith("CLK"))


def find_cut(core, chip):
    """The instances from the NMI and CLK pins to the catcher's flip-flops,
    found by the RTL's nets (rtl/z80_top.v): {role: instance}, the core's
    cut, the chip's cut"""
    drv = drivers(core)
    role = {"hold": drv["nmi_hold"][0], "clr": drv["nmi_hold_clr"][0], "neg": drv["nmi_n_neg"][0],
            "held": drv["nmi_n_held"][0]}
    cut = set(role.values())

    def back(net):      # single-input cells (buffers, inverters) back to a port
        out = []
        while net in drv:
            name = drv[net][0]
            ins = [n for p, n in core[name][1].items() if p not in OUTS and p not in SUP]
            if len(ins) != 1:
                break
            out.append(name)
            net = ins[0]
        return out
    hold = core[role["hold"]][1]
    cut |= set(back(hold[clock_pin(hold)]))                          # the NMI port buffer
    for p in ("RN", "SETN"):
        if p in hold:
            cut |= set(back(hold[p]))                                # nmi_hold_clr's inverter
    if hold.get("D") in drv:
        cut.add(drv[hold["D"]][0])                                   # the tie cell
    for p, n in core[role["held"]][1].items():
        if p not in OUTS and p not in SUP and n != "nmi_hold":
            cut |= set(back(n))                                      # the NMI's inverter
    for r in ("clr", "neg"):
        pins = core[role[r]][1]
        cut |= set(back(pins[clock_pin(pins)]))                      # the clock tree to the port
    u_top = next(name for name, (cell, pins) in chip.items() if cell == "z80_top")
    cdrv = drivers(chip)
    role["u_top"] = u_top
    role["pad_nmi"] = cdrv[chip[u_top][1]["nmi_n"]][0]
    role["pad_clk"] = cdrv[chip[u_top][1]["clk"]][0]
    return role, cut, {role["pad_nmi"], role["pad_clk"]}


def spef_caps(path):
    """{net: pF}: each net's total in a SPEF (wire only: PIN_CAP NONE)"""
    names, out = {}, {}
    text = open(path).read()
    for m in re.finditer(r"^\*(\d+)\s+(\S+)", text.split("*PORTS")[0].split("*NAME_MAP")[-1], re.M):
        names["*" + m.group(1)] = m.group(2).replace("\\", "")
    for m in re.finditer(r"^\*D_NET\s+(\S+)\s+([0-9.eE+-]+)", text, re.M):
        out[names.get(m.group(1), m.group(1)).replace("\\", "")] = float(m.group(2))
    return out


def pin_caps(lib):
    """{(cell, pin): pF} of the standard cells' inputs (Liberty)"""
    s = open(lib).read()
    out = {}
    for m in re.finditer(r'cell\s*\(\s*"?(\w+)"?\s*\)\s*\{', s):
        end = s.find('cell (', m.end())
        body = s[m.end(): end if end > 0 else len(s)]
        for p in re.finditer(r'pin\s*\(\s*"?(\w+)"?\s*\)\s*\{([^{}]*)', body):
            c = re.search(r'\bcapacitance\s*:\s*([0-9.]+)', p.group(2))
            if c and "input" in p.group(2):
                out[(m.group(1), p.group(1))] = float(c.group(1))
    return out


def subckts_needed(ports, body, models):
    """the definitions of models and of everything inside them"""
    need, todo = [], list(models)
    while todo:
        m = todo.pop()
        if m in need or m not in body:
            continue
        need.append(m)
        for line in body[m]:
            t = line.split()
            if t[0][0] in "Xx":
                todo.append([x for x in t[1:] if "=" not in x][-1])
    return need


class Cut:
    """The cut as a netlist: subcircuit definitions, instances on named
    nodes, each node's capacitance, and the nodes of (instance, pin)."""

    def __init__(self, core, chip, role, cut_core, cut_chip):
        self.core, self.chip, self.role = core, chip, role
        self.cut = {"t": cut_core, "c": cut_chip}
        self.port = chip[role["u_top"]][1]          # z80_top's ports -> the chip's nets

    def node(self, side, net):
        if net in SUP or net in ("VDD", "DVDD"):
            return "vdd" if net in ("VDD", "DVDD", "VNW") else "0"
        if side == "t" and net in self.port:
            side, net = "c", self.port[net]
        return f"{side}_" + re.sub(r"[^A-Za-z0-9_]", "_", net)

    def pin_node(self, inst, pin):
        side = "c" if inst in self.cut["c"] else "t"
        pins = (self.chip if side == "c" else self.core)[inst][1]
        if pin == "CLK*":
            pin = clock_pin(pins)
        if pin == "OUT":
            pin = next(p for p in pins if p in OUTS)
        return self.node(side, pins[pin])

    def loads(self, wire, pcap):
        """each node of the cut: its wire capacitance and the Liberty input
        capacitance of the cells on it that the cut leaves out (pF)"""
        members = {}
        for side, insts in (("t", self.core), ("c", self.chip)):
            for name in self.cut[side]:
                for p, n in insts[name][1].items():
                    if p not in SUP:
                        members.setdefault(self.node(side, n), set()).add((side, n))
                        if side == "t" and n in self.port:
                            members[self.node(side, n)].add(("c", self.port[n]))
        out = {}
        for nd, mem in members.items():
            if nd in ("vdd", "0"):
                continue
            c = 0.0
            for side, n in mem:
                c += wire[side].get(n, 0.0)
                for name, (cell, pins) in (self.core if side == "t" else self.chip).items():
                    if name in self.cut[side] or (side == "c" and cell == "z80_top"):
                        continue
                    for p, m in pins.items():
                        if m == n and p not in OUTS and p not in SUP:
                            c += pcap.get((cell.replace("C1_", ""), p), 0.0)
            out[nd] = c
        return out


MAGIC_CELL = """load {cell}
select top cell
extract path {ext}
extract do local
extract all
ext2spice lvs
ext2spice cthresh 0
ext2spice -o {out}
"""


def cell_netlists(cells, build):
    """the standard cells' transistors as Magic extracts them from the PDK's
    layouts (the PDK's own cell netlists give no drain / source areas: no
    junction capacitance, ~10 % fast); seconds, cached in build"""
    mag = os.path.join(chip.PDK_ROOT, "libs.ref", "gf180mcu_fd_sc_mcu7t5v0", "mag")
    d = os.path.join(build, "cells")
    os.makedirs(os.path.join(d, "ext"), exist_ok=True)
    todo = [c for c in cells if not os.path.exists(os.path.join(d, c + ".spice"))]
    if todo:
        tcl = os.path.join(d, "extract.tcl")
        open(tcl, "w").write("crashbackups stop\ndrc off\nlocking disable\n" + f"addpath {mag}\n" +
                             "".join(MAGIC_CELL.format(cell=c, ext=os.path.join(d, "ext"), out=os.path.join(d, c + ".spice"))
                                     for c in todo) + "quit -noprompt\n")
        env = dict(os.environ, PDK_ROOT=os.path.dirname(chip.PDK_ROOT))
        rc = os.path.join(chip.PDK_ROOT, "libs.tech", "magic", "gf180mcuD.magicrc")
        p = subprocess.run([chip.tool("magic"), "-dnull", "-noconsole", "-rcfile", rc, tcl],
                           capture_output=True, text=True, env=env, cwd=d)
        open(os.path.join(d, "extract.log"), "w").write(p.stdout + p.stderr)
    ports, body = {}, {}
    for c in cells:
        f = os.path.join(d, c + ".spice")
        if not os.path.exists(f):
            sys.exit(f"nmi: Magic did not extract {c} ({d}/extract.log)")
        p2, b2 = parse(f)
        ports.update(p2)
        body.update(b2)
    return ports, body


def netlist_spef(cut, core_run, chip_run, build):
    """the cut from the cells' extracted transistors and the routed layout's
    SPEF: the standard cells as Magic extracts them from the PDK's layouts,
    the pads from the PDK's netlists (extracted, with their junctions)"""
    io = os.path.join(chip.PDK_ROOT, "libs.ref", "gf180mcu_fd_io", "spice", "gf180mcu_fd_io.spice")
    ports, body = parse(io)
    std = sorted({cut.core[n][0] for n in cut.cut["t"]} | {cut.chip[n][0] for n in cut.cut["c"]} - set(ports))
    p2, b2 = cell_netlists(std, build)
    ports.update(p2)
    body.update(b2)
    lines, models = [], []
    for side, insts in (("t", cut.core), ("c", cut.chip)):
        for name in sorted(cut.cut[side]):
            cell, pins = insts[name]
            nodes = [cut.node(side, pins.get(p, "VSS" if p in ("VSS", "VPW", "DVSS") else "VDD")) for p in ports[cell]]
            lines.append(f"X{side}_{re.sub(r'[^A-Za-z0-9_]', '_', name)} {' '.join(nodes)} {cell}")
            models.append(cell)
    defs = []
    for m in subckts_needed(ports, body, models):
        defs += [f".subckt {m} {' '.join(ports[m])}"] + body[m] + [".ends"]
    wire = {"t": spef_caps(glob1(os.path.join(core_run, "final", "spef", "nom", "*.spef"))),
            "c": spef_caps(glob1(os.path.join(chip_run, "final", "spef", "nom", "*.spef")))}
    return defs, lines, wire


def netlist_magic(cut, work):
    """the cut from Magic's extraction of the whole chip (chip.py extract):
    the same instances, with their extracted transistors and wires"""
    ports, body = parse(os.path.join(work, "z80_chip.spice"))
    lines, models = [], []
    ext = {}
    for side, sub in (("t", "z80_top"), ("c", "z80_chip")):
        for line in body[sub]:
            t = line.split()
            if t[0][0] in "Xx":
                nets = [x for x in t[1:] if "=" not in x]
                ext[(side, t[0][1:])] = (nets[-1], dict(zip(ports.get(nets[-1], []), nets[:-1])))
    # the extraction names a net after an instance's pin: rename every pin of the
    # cut to the gate-level net it is on, so both netlists share node names
    rename = {}
    for side, insts in (("t", cut.core), ("c", cut.chip)):
        for name in cut.cut[side]:
            model, pins = ext[(side, name)]
            for p, n in pins.items():
                if p in insts[name][1]:
                    rename[(side, n)] = insts[name][1][p]
    for side, insts in (("t", cut.core), ("c", cut.chip)):
        for name in sorted(cut.cut[side]):
            model, pins = ext[(side, name)]
            nodes = [cut.node(side, rename.get((side, pins[p]), pins[p])) if p not in SUP else cut.node(side, p)
                     for p in ports[model]]
            lines.append(f"X{side}_{re.sub(r'[^A-Za-z0-9_]', '_', name)} {' '.join(nodes)} {model}")
            models.append(model)
    defs = []
    for m in subckts_needed(ports, body, models):
        defs += [f".subckt {m} {' '.join(ports[m])}"] + body[m] + [".ends"]
    wire = {}
    for side, sub in (("t", "z80_top"), ("c", "z80_chip")):
        w = {}
        for line in body[sub]:
            t = line.split()
            if t[0][0] in "Cc":
                for n in t[1:3]:
                    k = rename.get((side, n), n)
                    w[k] = w.get(k, 0.0) + chip.cval(t[3]) * 1e12
        wire[side] = w
    return defs, lines, wire


def glob1(pattern):
    import glob
    found = sorted(glob.glob(pattern))
    if not found:
        sys.exit(f"nmi: nothing matches {pattern}")
    return found[0]


def deck(source, corner, out_dir):
    """source: ("magic", work) or ("spef", core_run, chip_run)"""
    mc, temp, vdd = chip.CORNERS[corner]
    core_run, chip_run = (source[1], source[2]) if source[0] == "spef" else (source[2], source[3])
    core = verilog(glob1(os.path.join(core_run, "final", "nl", "z80_top.nl.v")))
    chp = verilog(glob1(os.path.join(chip_run, "final", "nl", "z80_chip.nl.v")))
    role, cut_core, cut_chip = find_cut(core, chp)
    cut = Cut(core, chp, role, cut_core, cut_chip)
    defs, lines, wire = (netlist_spef(cut, core_run, chip_run, out_dir) if source[0] == "spef" else
                         netlist_magic(cut, source[1]))
    lib = os.path.join(chip.PDK_ROOT, "libs.ref", "gf180mcu_fd_sc_mcu7t5v0", "lib",
                       "gf180mcu_fd_sc_mcu7t5v0__tt_025C_5v00.lib")
    loads = cut.loads(wire, pin_caps(lib))
    L = [f"* the NMI pulse catcher of z80_chip (GF180), cut out ({source[0]}): {corner}",
         f".include {chip.PDK_ROOT}/libs.tech/ngspice/design.ngspice",
         f".lib {chip.PDK_ROOT}/libs.tech/ngspice/sm141064.ngspice {mc}"]
    L += [f".lib {chip.PDK_ROOT}/libs.tech/ngspice/sm141064.ngspice {k}_{mc}" for k in ("diode", "res", "moscap")]
    L += [f".temp {temp}", ".options klu rshunt=1e12"] + defs + lines
    L += [f"C_{nd} {nd} 0 {c:.5f}p" for nd, c in sorted(loads.items()) if c > 0]
    t_end, nmi = scenarios()
    clk = [(0, 0)]
    t = PERIOD / 2
    while t < t_end:
        clk += [(t - EDGE / 1.6, 0), (t + EDGE / 1.6, vdd), (t + PERIOD / 2 - EDGE / 1.6, vdd), (t + PERIOD / 2 + EDGE / 1.6, 0)]
        t += PERIOD
    pwl = lambda pts: "PWL(" + " ".join(f"{a:.3f}n {b:g}" for a, b in pts if a >= 0) + ")"
    L += [f"V_vdd vdd 0 PWL(0 0 20n {vdd})", f"V_clk {cut.pin_node(role['pad_clk'], 'PAD')} 0 {pwl(clk)}"]
    pts = [(0, 0), (20, vdd)]
    for a, b in nmi:
        pts += [(a - EDGE / 1.6, vdd), (a + EDGE / 1.6, 0), (b - EDGE / 1.6, 0), (b + EDGE / 1.6, vdd)]
    L.append(f"V_nmi {cut.pin_node(role['pad_nmi'], 'PAD')} 0 {pwl(pts)}")
    # the flip-flops' set / reset from outside the cut: RESET, released after the first clocks
    seen = set()
    for r in ("clr", "neg", "hold"):
        for p in ("SETN", "RN"):
            pins = core[role[r]][1]
            if p in pins and pins[p] not in drivers({k: core[k] for k in cut_core}) and pins[p] not in seen:
                seen.add(pins[p])
                L.append(f"V_rst_{len(seen)} {cut.node('t', pins[p])} 0 PWL(0 0 {T_RESET}n 0 {T_RESET + 1}n {vdd})")
    for name in (role["pad_nmi"], role["pad_clk"]):
        for p in ("PU", "PD"):
            if p in chp[name][1]:
                L.append(f"V_{name}_{p} {cut.node('c', chp[name][1][p])} 0 0")
    nodes = {label: cut.pin_node(role[r], pin) for label, r, pin in SIGNALS}
    save = " ".join(f"v({n})" for n in nodes.values())
    d = os.path.join(out_dir, f"{source[0]}_{corner}")
    os.makedirs(d, exist_ok=True)
    dat = os.path.join(d, "nmi.dat")
    L += [".control", "set num_threads=1", "set wr_singlescale", "set wr_vecnames", "option numdgt=7",
          f"save {save}", f"tran 0.02n {t_end:.0f}n 0 0.5n uic", f"wrdata {dat} {save}", "quit", ".endc", ".end"]
    sp = os.path.join(d, "nmi.sp")
    open(sp, "w").write("\n".join(L) + "\n")
    info = {"source": source[0], "cut": {"core": sorted(cut_core), "chip": sorted(cut_chip)},
            "roles": role, "loads_pF": {k: round(v, 5) for k, v in sorted(loads.items())}}
    json.dump(info, open(os.path.join(d, "cut.json"), "w"), indent=1)
    return sp, dat, nmi, nodes


def scenarios():
    """NMI pulses (fall, rise) at the pin's 50 %: (end time, [(a, b)]).
    Except C and D, which are about the edge, each pulse sits in the
    middle of a clock phase (62.5 ns after its edge), clear of CLK's
    18.75 ns ramps (EDGE is 10-90 %)."""
    f = lambda k: PERIOD * k            # falling CLK edges at k * 250 ns
    r = lambda k: PERIOD * k + PERIOD / 2
    mid = lambda e, w: (e + PERIOD / 4 - w / 2, e + PERIOD / 4 + w / 2)    # w ns, mid-phase after edge e
    cases = [mid(f(4), 80),                              # A: 80 ns, the low half
             mid(f(6), 20),                              # B: 20 ns
             (r(8) - 5, r(8) + 195),                     # C: falls 5 ns before CLK rises
             (r(11) + 5, r(11) + 205),                   # D: falls 5 ns after
             (f(14) + 22.5, f(14) + 47.5), (f(14) + 77.5, f(14) + 102.5),   # E: two pulses, one edge
             mid(f(17), 40), mid(r(17), 40)]             # F: the second while clearing
    return f(20), cases


CASES = [("A", "an 80 ns NMI pulse (TwNMI) in the low half of the clock", 0),
         ("B", "a 20 ns pulse, a quarter of TwNMI", 1),
         ("C", "NMI falling 5 ns before a rising CLK edge", 2),
         ("D", "NMI falling 5 ns after a rising CLK edge", 3),
         ("E", "two short pulses before one rising edge", 4),
         ("F", "a second pulse while the first is being cleared", 6)]


def figures(dat, nmi, nodes, corner, out_dir, source):
    vdd = chip.CORNERS[corner][2]
    rows_t = [l.split() for l in open(dat) if l.strip()]
    head = [h.lower() for h in rows_t[0]]
    cols = list(zip(*[[float(x) for x in r] for r in rows_t[1:]]))
    w = dict(zip(head, cols))
    t = [x * 1e9 for x in w["time"]]
    v = {label: w[f"v({nd})".lower()] for label, nd in nodes.items()}
    colours = {"NMI pin": "#c0392b", "NMI in the core": "#d35400", "CLK pin": "#111", "CLK at the flip-flops": "#555",
               "nmi_hold": "#1f6fb4", "nmi_hold_clr": "#8b56b8", "nmi_n_held": "#2b9a3e", "nmi_n_neg": "#0f766e"}
    rows = [(label, t, v[label], vdd, colours[label]) + ((2.6,) if label == "NMI pin" else ())
            for label, _, _ in SIGNALS]
    cross = lambda vv, lvl, rising, after: chip.crossing(t, vv, lvl, rising, after, within=400)
    _, temp, volts = chip.CORNERS[corner]
    how = ("cut out of Magic's extraction of the whole chip" if source == "magic" else
           "Magic-extracted from the PDK's layouts, on the routed wiring (SPEF)")
    title = f"z80_chip's NMI pulse catcher at transistor level (GF180 {corner}: {volts:g} V, {temp} C)"
    sub = f"The NMI and CLK pads, the clock tree, the catcher's cells -- {how}; 4 MHz, 15 ns edges."
    figs = [f"nmi_{corner}.svg"]
    svgplot.stacked(os.path.join(out_dir, figs[0]), title, sub, rows, 0, t[-1],
                    vlines=[(nmi[i][0], c) for c, _, i in CASES])
    meas = {}
    row_of = {label: k for k, (label, _, _) in enumerate(SIGNALS)}
    for c, text, i in CASES:
        a, b = nmi[i]
        t0, t1 = a - 60, a + 420
        marks = []
        pin_fall = cross(v["NMI pin"], vdd / 2, False, a - 20)
        hold_up = cross(v["nmi_hold"], vdd / 2, True, a - 20)
        held_dn = cross(v["nmi_n_held"], vdd / 2, False, a - 20)
        if pin_fall and hold_up:
            marks.append((pin_fall, hold_up, row_of["nmi_hold"], f"NMI pin falls -> nmi_hold set: {hold_up - pin_fall:.1f} ns"))
        if pin_fall and held_dn:
            marks.append((pin_fall, held_dn, row_of["nmi_n_held"], f"NMI pin falls -> nmi_n_held low: {held_dn - pin_fall:.1f} ns"))
        clr_up = cross(v["nmi_hold_clr"], vdd / 2, True, a - 20)
        held_up = None
        if clr_up:
            # the CLK edge that cleared it: the nearest -- at ff the pad switches early enough on the
            # 15 ns edge for the clear to come before the pin's 50 %
            k = min((PERIOD * n + PERIOD / 2 for n in range(40)), key=lambda e: abs(e - clr_up))
            marks.append((k, clr_up, row_of["nmi_hold_clr"], f"CLK pin rises -> clear: {clr_up - k:.1f} ns"))
            held_up = cross(v["nmi_n_held"], vdd / 2, True, clr_up - 1)
            if held_up:
                marks.append((k, held_up, row_of["nmi_n_held"], f"CLK pin rises -> nmi_n_held high again: {held_up - k:.1f} ns"))
        meas[c] = {"pulse_ns": b - a, "set_ns": None if not (pin_fall and hold_up) else round(hold_up - pin_fall, 3),
                   "held_low_ns": None if not (held_dn and held_up) else round(held_up - held_dn, 3),
                   "clear_after_clk_ns": None if not clr_up else round(clr_up - k, 3)}
        name = f"nmi_{c}_{corner}.svg"
        svgplot.stacked(os.path.join(out_dir, name), f"NMI case {c}: {text}", title + ".", rows, t0, t1, marks=marks,
                        vlines=[(PERIOD * n + PERIOD / 2, "CLK rises") for n in range(40) if t0 < PERIOD * n + PERIOD / 2 < t1])
        figs.append(name)
    json.dump({"source": source, "corner": corner, "volts": volts, "temp_c": temp, "cases": meas,
               "about": {c: text for c, text, _ in CASES}}, open(os.path.join(out_dir, f"nmi_{corner}.json"), "w"), indent=1)
    return figs, meas


def latest(pattern):
    import glob
    runs = sorted(glob.glob(pattern), key=os.path.getmtime)
    return runs[-1] if runs else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("run", help="the chip's LibreLane run (asic-gf180mcu chip/runs/<tag>)")
    ap.add_argument("--core", help="z80_top's run the chip took its macro from (default: the latest beside it)")
    ap.add_argument("--magic", metavar="WORK", help="cut Magic's extraction (chip.py's working directory) "
                    "instead of the PDK's cell netlists on the SPEF")
    ap.add_argument("--corners", default="tt,ss,ff")
    ap.add_argument("--out", default=os.path.join(HERE, "chip", "gf180mcu"))
    ap.add_argument("--build", help="decks and waveforms (default: <run>/nmi)")
    a = ap.parse_args()
    run = os.path.abspath(a.run)
    core = os.path.abspath(a.core) if a.core else latest(os.path.join(run, "..", "..", "..", "runs", "RUN_*"))
    if not core:
        sys.exit("nmi: no z80_top run beside the chip's (--core)")
    os.makedirs(a.out, exist_ok=True)
    build = os.path.abspath(a.build or os.path.join(run, "nmi"))
    source = ("magic", os.path.abspath(a.magic), core, run) if a.magic else ("spef", core, run)
    ngspice = chip.tool("ngspice")
    jobs = []
    for corner in a.corners.split(","):
        sp, dat, nmi, nodes = deck(source, corner, build)
        jobs.append((corner, sp, dat, nmi, nodes,
                     subprocess.Popen([ngspice, "-b", sp], cwd=os.path.dirname(sp), stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, text=True)))
    bad = []
    for corner, sp, dat, nmi, nodes, p in jobs:          # the corners run side by side
        log = p.communicate()[0]
        open(os.path.join(os.path.dirname(sp), "nmi.log"), "w").write(log)
        if not os.path.exists(dat):
            sys.exit(f"nmi: ngspice failed ({os.path.dirname(sp)}/nmi.log)")
        figs, meas = figures(dat, nmi, nodes, corner, a.out, source[0])
        print(f"nmi {corner} ({source[0]}):")
        for c, m in meas.items():
            print(f"  {c}: {m}")
            # every case's (first) pulse caught, held for the core, then cleared by the next rising CLK edge
            bad += [f"{corner} {c}: {k} missing" for k in ("set_ns", "held_low_ns", "clear_after_clk_ns") if m[k] is None]
    for b in bad:
        print(f"nmi: FAIL {b}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
