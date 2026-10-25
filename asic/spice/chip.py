#!/usr/bin/env python3
"""chip.py - the whole chip at transistor level: z80_chip's layout,
extracted, running a short program in ngspice, its pins drawn the way the
Z80's datasheet draws its timing diagrams (Zilog PS0178, Figures 5-7).

  extract    Magic extracts the chip run's GDS: every cell's transistors
             and the wires' capacitance (~25 min for GF180)
  reference  the program on the RTL (tb_chip_ref.v, iverilog): the pins at
             every clock edge -- when the memory has to drive the data bus,
             and what the transistors have to do
  simulate   ngspice: power-up, 4 clocks of RESET, then LD A,5Ah /
             LD (8000h),A / OUT (42h),A / IN A,(43h) / HALT at 4 MHz,
             15 ns clock edges, 100 pF on every pin, a memory that drives
             D in the RTL's read windows (hours); with --segment K/N only
             the K-th of N windows, from the node voltages the window
             before it ended with (CI jobs have 6 hours each)
  stitch     the windows' pins into the one run figures reads
  figures    the pins checked against the RTL, every output parameter of
             the datasheet measured on every cycle that has it (CLK at
             50 %, outputs at 1.5 V) against its limit and STA's number;
             Figures 5-7 as SVG

usage: chip.py RUN [--work DIR] [--corner tt] [--threads N] [--segment K/N]
               [--steps extract,reference,simulate,stitch,figures] [--sta FILE]
  RUN        a LibreLane chip run of asic-gf180mcu (chip/runs/<tag>), or
             with --gds, only a name for the work directory's default
  --gds      the chip's GDS, for extract (default RUN/final/gds/z80_chip.gds)
  --sta      pin_timing.py's output for the run, for STA's numbers
  --segment  simulate the K-th of N windows (1-based)
"""
import argparse, bisect, json, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import svgplot  # noqa: E402
import pads  # noqa: E402

PDK_ROOT = os.path.join(os.environ.get("CIEL_HOME", os.path.expanduser("~/.ciel")), "ciel",
                        "gf180mcu/versions/54435919abffb937387ec956209f9cf5fd2dfbee/gf180mcuD")
CORNERS = {"tt": ("typical", 25, 5.0), "ss": ("ss", 125, 4.5), "ff": ("ff", -40, 5.5)}
PERIOD, EDGE, LOAD = 250.0, 15.0, 100.0       # ns, ns (10-90 %), pF
OUT_LEVEL = 1.5                               # V: PS0178's VOH = VOL
PINS = ("A[0] A[10] A[11] A[12] A[13] A[14] A[15] A[1] A[2] A[3] A[4] A[5] A[6] A[7] A[8] A[9] BUSAK_n "
        "BUSRQ_n CLK D[0] D[1] D[2] D[3] D[4] D[5] D[6] D[7] HALT_n INT_n IORQ_n M1_n MREQ_n NMI_n RD_n "
        "RESET_n RFSH_n VDD VSS WAIT_n WR_n").split()        # z80_chip's ports, in Magic's order
INPUTS = {"CLK", "RESET_n", "WAIT_n", "INT_n", "NMI_n", "BUSRQ_n"}
node = lambda p: "p_" + p.replace("[", "").replace("]", "").lower()


def sh(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if p.returncode != 0:
        sys.exit(f"chip: {' '.join(cmd[:3])} failed:\n{(p.stderr or p.stdout)[-2000:]}")
    return p


def tool(name):
    """ngspice or Magic, the pinned builds (pads.TOOLS)"""
    return pads.tool(name, who="chip")


# ------------------------------------------------------------- extract
MAGIC_SCRIPT = """crashbackups stop
drc off
locking disable
gds readonly true
gds read {gds}
load z80_chip
select top cell
cd {ext}
extract do local
extract do capacitance
extract do coupling
extract no resistance
extract all
ext2spice lvs
ext2spice cthresh 0.5
ext2spice -o {out}
quit -noprompt
"""


def extract(run, work, gds=None):
    out = os.path.join(work, "z80_chip.spice")
    gds = os.path.abspath(gds or os.path.join(run, "final", "gds", "z80_chip.gds"))
    ext = os.path.join(work, "ext")
    os.makedirs(ext, exist_ok=True)
    tcl = os.path.join(work, "extract.tcl")
    open(tcl, "w").write(MAGIC_SCRIPT.format(gds=gds, ext=ext, out=out))
    env = dict(os.environ, PDK_ROOT=os.path.dirname(PDK_ROOT))
    magic = tool("magic")
    rc = os.path.join(PDK_ROOT, "libs.tech", "magic", "gf180mcuD.magicrc")
    print("chip: extracting the layout with Magic (~25 min)", flush=True)
    p = subprocess.run([magic, "-dnull", "-noconsole", "-rcfile", rc, tcl], capture_output=True, text=True, env=env)
    open(os.path.join(work, "extract.log"), "w").write(p.stdout + p.stderr)
    if not os.path.exists(out):
        sys.exit("chip: Magic wrote no netlist (extract.log)")
    return out


# ------------------------------------------------------------- reference
def reference(work):
    rtl = os.path.join(ROOT, "rtl")
    srcs = sorted(os.path.join(rtl, f) for f in os.listdir(rtl) if f.endswith(".v"))
    vvp = os.path.join(work, "ref.vvp")
    sh(["iverilog", "-g2005", f"-I{rtl}", "-o", vvp, os.path.join(HERE, "tb_chip_ref.v")] + srcs)
    out = sh(["vvp", "-n", vvp]).stdout
    open(os.path.join(work, "ref.txt"), "w").write(out)
    return out


def edges_of(ref):
    """[(time, clk, reset_n, {pin: value})] at every clock edge of the RTL"""
    out = []
    for l in ref.splitlines():
        if l.startswith("EDGE"):
            f = l.split()
            out.append((float(f[1]), int(f[2]), int(f[3]), dict(x.split("=") for x in f[4:])))
    return out


def read_windows(edges):
    """[(from, to, byte)]: the RTL's clock edges between which RD is low
    with MREQ or IORQ, and the byte the memory / port answers"""
    win, cur = [], None
    for t, clk, rst, kv in edges:
        rd = kv["RD"] == "0" and (kv["MREQ"] == "0" or kv["IORQ"] == "0")
        if rd and cur is None:
            cur = [t, None, int(kv["DI"], 16)]
        elif not rd and cur is not None:
            cur[1] = t
            win.append(tuple(cur))
            cur = None
    return win


# ------------------------------------------------------------- simulate
SUPPLY = {"VDD", "VSS", "DVDD", "DVSS", "0", "GND", "VNW", "VPW"}


def cval(v):
    v = v.lower()
    m = {"f": 1e-15, "p": 1e-12, "a": 1e-18, "n": 1e-9, "u": 1e-6}
    return float(v[:-1]) * m[v[-1]] if v[-1] in m else float(v)


def prepare_netlist(src, dst):
    """The extracted netlist for simulation: no fill / decap cells (MOS
    capacitors on the ideal supplies), and each subcircuit's capacitors
    lumped per node to ground. Magic's hierarchical extraction leaves
    coupling between nets and, at the top level, negative corrections
    aimed at nodes inside subcells (by names a SPICE netlist cannot reach);
    the lumped version keeps each net's total. Magic names a subcell's own
    nodes a_<x>_<y>#, and ngspice keeps '#' for devices' internal nodes:
    wrnodev leaves such names out of a window's end state, and the window
    after it would start every latch inside a flip-flop at 0 V. They are
    renamed a_<x>_<y>_h (no name in the netlist ends in _h)."""
    keep, caps, n_caps, dropped = [], None, 0, 0
    for line in open(src).read().replace("\n+", " ").splitlines():
        t = line.split()
        if t and t[0].lower() == ".subckt":
            caps = {}
        if t and t[0].lower() == ".ends":
            for n, c in caps.items():
                if c > 1e-18:
                    keep.append(f"Clump{n_caps} {n} 0 {c:.4e}")
                    n_caps += 1
            caps = None
        if t and t[0][0] in "Cc" and caps is not None:
            if any(n.count("/") > 1 for n in t[1:3]):
                continue
            for n in t[1:3]:
                if n not in SUPPLY and not n.endswith("VSUBS"):
                    caps[n] = caps.get(n, 0.0) + cval(t[3])
            continue
        if t and t[0][0] in "Xx" and re.search(r"fillcap|decap", [x for x in t[1:] if "=" not in x][-1]):
            dropped += 1
            continue
        keep.append(line)
    open(dst, "w").write("\n".join(keep).replace("#", "_h") + "\n")
    return dropped, n_caps


# The core clock's root crosses 50 % this long after the CLK pin crosses the
# pad's threshold, rising / falling (ns; probed in the fine runs; the leaves
# follow 0.4-1.1 ns later).
CLK_ROOT = {"tt": (2.9, 2.1), "ss": (4.3, 3.5), "ff": (2.1, 1.4)}
TMAX = 10.0       # ns: the longest step, where the chip has settled


def clock_breaks(corner, edges, vdd):
    """[times]: corners for a source that only makes breakpoints, over the
    core clock's edges. ngspice's step control (on the devices' charges,
    trtol 7) lets a single 1-2 ns step carry the CLK pad's Schmitt trigger,
    the clock tree and every flip-flop through an edge: the delays inside
    it collapse, a flip-flop can settle in its old state, and Newton's
    method can land latches elsewhere in their other state with their
    clocks still (both seen, at tt and at ff). After a breakpoint ngspice
    restarts with a tenth of the gap to the next, so corners every 0.5 ns,
    from just before the pin's ramp crosses the pad's threshold (pads.json)
    until 1 ns after the clock's root has crossed 50 % (CLK_ROOT), keep those
    steps at 0.2 ns or less. From there the switching chip keeps its own
    steps short (0.03-0.1 ns), and more corners would only restart them.
    The edge that releases RESET keeps them to 7 ns after the threshold."""
    dc = json.load(open(os.path.join(HERE, "pads", "gf180mcu", "pads.json")))["corners"][corner]["cells"][
        "gf180mcu_fd_io__in_s"]["dc"]
    r = EDGE / 0.8
    trst = next(t0 for t0, c, rst, kv in edges if rst == 1)
    out = []
    for t0, clk, rst, kv in edges:
        t_sw = t0 - r / 2 + r * (dc["vt_rise"] / vdd if clk else 1 - dc["vt_fall"] / vdd)
        lead = 7.0 if t0 == trst else CLK_ROOT[corner][0 if clk else 1] + 1.0
        out += [t_sw - 0.5 + 0.5 * k for k in range(int(lead / 0.5) + 2)]
    assert all(b > a for a, b in zip(out, out[1:])) and out[0] > 0
    return out


def windows(edges, n):
    """[(from, to)] ns: the run cut into n windows of about equal length,
    each cut 15 ns before a clock edge's 50 % (5.6 ns before its ramp),
    where the chip has settled since the edge before"""
    tstop = edges[-1][0] + 100
    cuts = []
    for k in range(1, n):
        t0 = next(t for t, *_ in edges if t >= tstop * k / n)
        cuts.append(t0 - 15.0)
    b = [0.0] + cuts + [tstop]
    return list(zip(b, b[1:]))


def state_file(work, corner, t):
    """the node voltages at t ns (ngspice's wrnodev: .ic lines)"""
    return os.path.join(work, corner, "seg", f"state_{t:.0f}.ic")


def segment_file(work, corner, t):
    return os.path.join(work, corner, "seg", f"chip_{t:.0f}.dat")


def deck(work, corner, edges, threads, tstop, t_from=0.0):
    """the deck, from t_from on: every source as the time from t_from
    sees it, the chip starting from the node voltages the window before
    ended with -- the chip has settled there (windows()), and they are
    every node's, the latches inside the flip-flops' included
    (prepare_netlist's renaming)"""
    mc, temp, vdd = CORNERS[corner]
    d = os.path.join(work, corner)
    os.makedirs(d, exist_ok=True)

    def at(pts, t):
        for (ta, va), (tb, vb) in zip(pts, pts[1:]):
            if ta <= t <= tb:
                return va if tb == ta else va + (vb - va) * (t - ta) / (tb - ta)
        return pts[-1][1] if t > pts[-1][0] else pts[0][1]

    def seen(pts):   # [(ns, V)] from t_from on
        if t_from <= 0:
            return [(a, b) for a, b in pts if a >= 0]
        return [(0.0, at(pts, t_from))] + [(a - t_from, b) for a, b in pts if a > t_from]

    def pwl(pts):
        return "PWL(" + " ".join(f"{a:.3f}n {b:g}" for a, b in seen(pts)) + ")"
    net = os.path.join(d, "chip.net")
    dropped, n_caps = prepare_netlist(os.path.join(work, "z80_chip.spice"), net)
    t = f"{PDK_ROOT}/libs.tech/ngspice"
    L = [f"* z80_chip at transistor level, GF180 {corner}: power-up, RESET, the program at 4 MHz",
         f".include {t}/design.ngspice", f".lib {t}/sm141064.ngspice {mc}"]
    L += [f".lib {t}/sm141064.ngspice {k}_{mc}" for k in ("diode", "res", "moscap")]
    # rshunt: 1 Tohm from every node to ground -- at power-up (uic) some
    # transistor-internal nodes have no path anywhere
    L += [f".include {net}", f".temp {temp}", ".options klu rshunt=1e12",
          f"V_vdd {node('VDD')} 0 {pwl([(0, 0), (20, vdd)])}", f"V_vss {node('VSS')} 0 0"]

    def ramp(t0, a, b):
        r = EDGE / 0.8
        return [(t0 - r / 2, a), (t0 + r / 2, b)]
    pts = [(0, 0)]
    for t0, clk, rst, kv in edges:
        pts += ramp(t0, vdd * (1 - clk), vdd * clk)
    L.append(f"V_clk {node('CLK')} 0 {pwl(pts)}")
    trst = next(t0 for t0, c, r, kv in edges if r == 1)
    L.append(f"V_rst {node('RESET_n')} 0 {pwl([(0, 0)] + ramp(trst, 0, vdd))}")
    for p in ("WAIT_n", "INT_n", "NMI_n", "BUSRQ_n"):
        L.append(f"V_{p.lower()} {node(p)} 0 {pwl([(0, 0), (20, vdd)])}")
    brk = [a for a, _ in seen([(0, 0)] + [(b, 0) for b in clock_breaks(corner, edges, vdd)])][1:]
    L.append("V_brk brk 0 PWL(0 0")
    for k in range(0, len(brk), 16):
        L.append("+ " + " ".join(f"{b:.3f}n 0" for b in brk[k:k + 16]))
    L[-1] += ")"
    L.append("R_brk brk 0 1meg")
    # the memory: through a 50 ohm switch, from 40 ns after the RTL's RD
    # edge to 60 ns after the edge that ends the read
    en = [(0, 0)]
    win = read_windows(edges)
    for a, b, _ in win:
        en += [(a + 40, 0), (a + 41, vdd), (b + 60, vdd), (b + 61, 0)]
    L += [f"V_men men 0 {pwl(en)}",
          ".model swbus sw vt=2.5 vh=0.2 ron=50 roff=1e10"]
    for k in range(8):
        vals = [(0, 0)]
        for a, b, byte in win:
            vals += [(a + 20, vals[-1][1]), (a + 25, vdd * ((byte >> k) & 1))]
        L += [f"V_mem{k} mem{k} 0 {pwl(vals)}",
              f"S_mem{k} mem{k} {node(f'D[{k}]')} men 0 swbus", f"R_pull{k} {node(f'D[{k}]')} {node('VDD')} 100k"]
    for p in PINS:
        if p not in INPUTS and p not in ("VDD", "VSS"):
            L.append(f"C_{node(p)} {node(p)} 0 {LOAD}p")
    L.append("Xchip " + " ".join(node(p) for p in PINS) + " z80_chip")
    save = " ".join(f"v({node(p)})" for p in PINS if p not in ("VDD", "VSS")) + " v(men)"
    if t_from <= 0 and tstop == edges[-1][0] + 100:      # the whole run
        dat, end, sp = os.path.join(d, "chip.dat"), [], os.path.join(d, "chip.sp")
    else:                                                # a window of it
        os.makedirs(os.path.join(d, "seg"), exist_ok=True)
        dat, sp = segment_file(work, corner, t_from), os.path.join(d, "seg", f"chip_{t_from:.0f}.sp")
        end = [f"wrnodev {state_file(work, corner, tstop)}"]
        if t_from > 0:
            L.append(f".include {state_file(work, corner, t_from)}")
    L += [".control", f"set num_threads={threads}", "set wr_singlescale", "set wr_vecnames", "option numdgt=7",
          f"save {save}", f"tran 0.1n {tstop - t_from:.3f}n 0 {TMAX:g}n uic", f"wrdata {dat} {save}"] + end + [
          "rusage all", "quit", ".endc", ".end"]
    open(sp, "w").write("\n".join(L) + "\n")
    print(f"chip: {sp}: {dropped} fill cells dropped, {n_caps} lumped capacitors, {len(win)} read windows, "
          f"{len(brk)} breakpoints over the clock's edges", flush=True)
    return sp


def simulate(work, corner, edges, threads, segment=None):
    """the whole run, or the window segment = (k, n) of windows(edges, n):
    its pins in seg/chip_<from>.dat (in the run's time), its end state in
    seg/state_<to>.ic for the window after it"""
    t_from, tstop = (0.0, edges[-1][0] + 100) if not segment else windows(edges, segment[1])[segment[0] - 1]
    if t_from > 0 and not os.path.exists(state_file(work, corner, t_from)):
        sys.exit(f"chip: no state to start the window at {t_from:.0f} ns from ({state_file(work, corner, t_from)})")
    sp = deck(work, corner, edges, threads, tstop, t_from)
    ngspice = tool("ngspice")
    print(f"chip: ngspice, {t_from / 1000:.2f} .. {tstop / 1000:.2f} us (hours)", flush=True)
    d = os.path.dirname(sp)
    log = os.path.join(d, "chip.log" if not segment else f"chip_{t_from:.0f}.log")
    p = subprocess.run([ngspice, "-b", sp], cwd=d, capture_output=True, text=True)
    open(log, "w").write(p.stdout + p.stderr)
    dat = os.path.join(d, "chip.dat") if not segment else segment_file(work, corner, t_from)
    if not os.path.exists(dat):
        sys.exit(f"chip: ngspice failed ({log})")
    if t_from > 0:  # back to the run's time
        lines = open(dat).read().splitlines()
        out = [lines[0]] + [" ".join([f"{float(r.split()[0]) + t_from * 1e-9:.9e}"] + r.split()[1:])
                            for r in lines[1:] if r.strip()]
        open(dat, "w").write("\n".join(out) + "\n")


def stitch(work, corner, edges, n):
    """chip.dat from the n windows' pins (each window's first point is the
    one before it ended with)"""
    rows = []
    for k, (t_from, _) in enumerate(windows(edges, n)):
        f = segment_file(work, corner, t_from)
        if not os.path.exists(f):
            sys.exit(f"chip: window {k + 1} of {n} has not run ({f})")
        lines = [l for l in open(f).read().splitlines() if l.strip()]
        rows += lines[2:] if rows else lines     # header, first point: the window before's last
    open(os.path.join(work, corner, "chip.dat"), "w").write("\n".join(rows) + "\n")
    print(f"chip: {n} windows stitched, {len(rows) - 1} points", flush=True)


# ------------------------------------------------------------- measure
def load(dat):
    """time (ns), {pin: volts}, the memory's enable"""
    lines = [l.split() for l in open(dat) if l.strip()]
    head = [h.lower() for h in lines[0]]
    cols = list(zip(*[[float(x) for x in r] for r in lines[1:]]))
    w = dict(zip(head, cols))
    return [x * 1e9 for x in w["time"]], {p: w[f"v({node(p)})"] for p in PINS if p not in ("VDD", "VSS")}, w["v(men)"]


def slope(t, v, k):
    """v's tangent at timepoint k for a monotone cubic through the
    timepoints (Fritsch-Carlson: flat where v turns)"""
    d0 = (v[k] - v[k - 1]) / (t[k] - t[k - 1]) if k > 0 else None
    d1 = (v[k + 1] - v[k]) / (t[k + 1] - t[k]) if k + 1 < len(t) else None
    if d0 is None or d1 is None:
        return d1 if d0 is None else d0
    if d0 * d1 <= 0:
        return 0.0
    h0, h1 = t[k] - t[k - 1], t[k + 1] - t[k]
    return 3 * (h0 + h1) / ((2 * h1 + h0) / d0 + (h1 + 2 * h0) / d1)


def crossing(t, v, level, rising, after, within=150.0):
    """the first time in (after, after + within] that v passes level
    upwards (rising) or downwards, on the monotone cubic through the
    timepoints around it: on a slow pin edge ngspice takes steps up to TMAX,
    and a straight line across one put the crossing up to 0.6 ns off the
    fine runs' (2 ns steps), the cubic within 0.25 ns"""
    k = max(bisect.bisect_right(t, after), 1)
    while k < len(t) and t[k] <= after + within:
        a, b = v[k - 1] - level, v[k] - level
        if (rising and a < 0 <= b) or (not rising and a > 0 >= b):
            if b == a:
                return t[k]
            h, m0, m1 = t[k] - t[k - 1], slope(t, v, k - 1), slope(t, v, k)
            f = lambda s: ((2 * s ** 3 - 3 * s ** 2 + 1) * a + (s ** 3 - 2 * s ** 2 + s) * h * m0
                           + (3 * s ** 2 - 2 * s ** 3) * b + (s ** 3 - s ** 2) * h * m1)
            lo, hi = 0.0, 1.0           # f(0) = a and f(1) = b straddle 0: bisect
            for _ in range(40):
                mid = (lo + hi) / 2
                lo, hi = (lo, mid) if (f(mid) >= 0) == rising else (mid, hi)
            return t[k - 1] + h * (lo + hi) / 2
        k += 1
    return None


def value_at(t, v, at):
    k = min(max(bisect.bisect_left(t, at), 1), len(t) - 1)
    return v[k]


BUS = {"A": [f"A[{i}]" for i in range(16)], "D": [f"D[{i}]" for i in range(8)]}
CTRL = {"M1_n": "M1", "MREQ_n": "MREQ", "IORQ_n": "IORQ", "RD_n": "RD", "WR_n": "WR", "RFSH_n": "RFSH",
        "HALT_n": "HALT"}                     # the pins and their names in tb_chip_ref.v's lines


def cycles_of(edges):
    """the RTL's machine cycles: [(kind, [rising-edge indices of its T
    states + the next cycle's first], [falling-edge indices])]"""
    rise = [i for i, e in enumerate(edges) if e[1] == 1 and e[2] == 1]
    starts = [i for i in rise if edges[i][3]["T"] == "1"]
    out = []
    for a, b in zip(starts, starts[1:]):
        r = [i for i in rise if a <= i <= b]
        f = [i + 1 for i in r[:-1]]
        kv = [edges[i][3] for i in range(a, b)]
        low = lambda s: any(x[s] == "0" for x in kv)
        both = lambda s1, s2: any(x[s1] == "0" and x[s2] == "0" for x in kv)
        kind = ("fetch" if both("M1", "MREQ") else "mread" if both("MREQ", "RD") else
                "mwrite" if both("MREQ", "WR") else "ioread" if both("IORQ", "RD") else
                "iowrite" if both("IORQ", "WR") else "other")
        out.append((kind, r, f))
    return out


# per kind of cycle: (parameter, reference edge (T state, rise/fall), pin, transition);
# the I/O cycles' datasheet T3 is their fourth clock (T1, T2, TW*, T3)
DELAYS = {
    "fetch": [(6, (1, "r"), "A", None), (19, (1, "r"), "M1_n", "fall"), (8, (1, "f"), "MREQ_n", "fall"),
              (13, (1, "f"), "RD_n", "fall"), (9, (3, "r"), "MREQ_n", "rise"), (14, (3, "r"), "RD_n", "rise"),
              (20, (3, "r"), "M1_n", "rise"), (21, (3, "r"), "RFSH_n", "fall"), (6, (3, "r"), "A", None),
              (8, (3, "f"), "MREQ_n", "fall"), (12, (4, "f"), "MREQ_n", "rise"), (22, (5, "r"), "RFSH_n", "rise")],
    "mread": [(6, (1, "r"), "A", None), (8, (1, "f"), "MREQ_n", "fall"), (13, (1, "f"), "RD_n", "fall"),
              (12, (3, "f"), "MREQ_n", "rise"), (23, (3, "f"), "RD_n", "rise")],
    "mwrite": [(6, (1, "r"), "A", None), (8, (1, "f"), "MREQ_n", "fall"), (53, (1, "f"), "D", None),
               (30, (2, "f"), "WR_n", "fall"), (12, (3, "f"), "MREQ_n", "rise"), (32, (3, "f"), "WR_n", "rise")],
    "iowrite": [(6, (1, "r"), "A", None), (53, (1, "f"), "D", None), (27, (2, "r"), "IORQ_n", "fall"),
                (34, (2, "r"), "WR_n", "fall"), (28, (4, "f"), "IORQ_n", "rise"), (32, (4, "f"), "WR_n", "rise")],
    "ioread": [(6, (1, "r"), "A", None), (27, (2, "r"), "IORQ_n", "fall"), (24, (2, "r"), "RD_n", "fall"),
               (28, (4, "f"), "IORQ_n", "rise"), (23, (4, "f"), "RD_n", "rise")],
}
# separations: (parameter, later event, earlier event) by parameter number
SEPARATIONS = {
    "fetch": [(7, (8, 0), (6, 0)), (10, (8, 1), (9, 0)), (11, (12, 0), (8, 1))],
    "mread": [(7, (8, 0), (6, 0)), (11, (12, 0), (8, 0))],
    "mwrite": [(7, (8, 0), (6, 0)), (29, (30, 0), (53, 0)), (31, (32, 0), (30, 0))],
    "iowrite": [(26, (27, 0), (6, 0)), (33, (34, 0), (53, 0)), (31, (32, 0), (34, 0))],
    "ioread": [(26, (27, 0), (6, 0))],
}


def bus_event(t, w, edges, i, bus, t_ref):
    """when the bus is valid after the RTL changes it at edge i: the last
    of its changing bits through OUT_LEVEL (None if nothing changes, NaN
    if a bit does not change within 150 ns)"""
    key = "A" if bus == "A" else "DO"
    old, new = int(edges[i - 1][3][key], 16), int(edges[i][3][key], 16)
    times = []
    for k, pin in enumerate(BUS[bus]):
        was = value_at(t, w[pin], t_ref - 5) > OUT_LEVEL
        want = (new >> k) & 1
        if was != want:
            c = crossing(t, w[pin], OUT_LEVEL, bool(want), t_ref - 5)
            if c is None:
                return float("nan")      # the RTL changes it here, the transistors did not
            times.append(c)
    return max(times) if times else None


def measure(t, w, edges):
    """[(cycle kind, cycle index, {(param, occurrence): (t_ref, t_event)},
    [(param, t_ref): edges the RTL makes that the pins did not within
    150 ns])]"""
    out = []
    for n, (kind, r, f) in enumerate(cycles_of(edges)):
        if kind not in DELAYS:
            continue
        ev, seen, miss = {}, {}, []
        for pid, (T, rf), pin, trans in DELAYS[kind]:
            idx = (r if rf == "r" else f)
            if T - 1 >= len(idx):
                continue
            i = idx[T - 1]
            t_ref = edges[i][0]
            if pin in BUS:
                te = bus_event(t, w, edges, i, pin, t_ref)
            else:
                te = crossing(t, w[pin], OUT_LEVEL, trans == "rise", t_ref - 5)
                a, b = ("0", "1") if trans == "rise" else ("1", "0")
                if te is None and edges[i - 1][3][CTRL[pin]] == a and edges[i][3][CTRL[pin]] == b:
                    te = float("nan")
            occ = seen.get(pid, 0)
            seen[pid] = occ + 1
            if te is not None and te != te:
                miss.append((pid, t_ref))
            elif te is not None:
                ev[(pid, occ)] = (t_ref, te)
        out.append((kind, n, ev, miss))
    return out


def check_functional(t, w, edges):
    """the transistor-level pins against the RTL at every clock edge after
    RESET, 110 ns after it (just before the next one): mismatches"""
    bad = []
    for t0, clk, rst, kv in edges:
        if not rst or t0 < 200:
            continue
        at = t0 + 110
        if at > t[-1]:
            break
        for pin, key in CTRL.items():
            if (value_at(t, w[pin], at) > OUT_LEVEL) != (kv[key] == "1"):
                bad.append((t0, pin, kv[key]))
        a = sum(1 << k for k, p in enumerate(BUS["A"]) if value_at(t, w[p], at) > OUT_LEVEL)
        if a != int(kv["A"], 16):
            bad.append((t0, "A", kv["A"], f"{a:04x}"))
        if kv["DD"] == "1":
            d = sum(1 << k for k, p in enumerate(BUS["D"]) if value_at(t, w[p], at) > OUT_LEVEL)
            if d != int(kv["DO"], 16):
                bad.append((t0, "D", kv["DO"], f"{d:02x}"))
    return bad


def datasheet():
    """{parameter: (symbol, bound, 4 MHz limit)}, the starred minimums at
    this clock as pin_timing.py computes them"""
    sys.path.insert(0, os.path.join(HERE, "..", "timing"))
    import pin_timing
    con = pin_timing.Contract("4", LOAD)
    return {pid: (r["symbol"], r["bound"], con.limit(pid)) for pid, r in con.req.items()}


def sta_numbers(path, corner):
    """{parameter: value at the corner} from pin_timing.py's printed table"""
    if not path or not os.path.exists(path):
        return {}
    lines = open(path).read().splitlines()
    head = next((l.split() for l in lines if l.strip().startswith("#") and "parameter" in l), None)
    if not head or corner not in head:
        return {}
    corners = head[head.index("limit") + 1:head.index("worst")]
    out = {}
    for l in lines:
        f = l.split()        # id symbol <= limit c1 c2 c3 slack
        if f and f[0].isdigit():
            try:
                out[int(f[0])] = float(f[4 + corners.index(corner)])
            except (ValueError, IndexError):
                pass
    return out


# ------------------------------------------------------------- figures
FONT = "font-family='Helvetica, Arial, sans-serif'"
ROWS = {
    "fetch": ("Instruction opcode fetch (PS0178 Figure 5)", ["CLK", "A", "MREQ_n", "RD_n", "M1_n", "RFSH_n", "D"]),
    "mread": ("Memory read cycle (PS0178 Figure 6)", ["CLK", "A", "MREQ_n", "RD_n", "D"]),
    "mwrite": ("Memory write cycle (PS0178 Figure 6)", ["CLK", "A", "MREQ_n", "WR_n", "D"]),
    "iowrite": ("Output cycle, OUT (PS0178 Figure 7)", ["CLK", "A", "IORQ_n", "WR_n", "D"]),
    "ioread": ("Input cycle, IN (PS0178 Figure 7)", ["CLK", "A", "IORQ_n", "RD_n", "D"]),
}
T_NAMES = {"iowrite": ["T1", "T2", "TW*", "T3"], "ioread": ["T1", "T2", "TW*", "T3"]}


def simplify(pts, tol=0.25):
    """drop the points of a polyline that lie on the line through their
    neighbours (within tol pixels): flat stretches become two points"""
    if len(pts) < 3:
        return pts
    out = [pts[0]]
    for k in range(1, len(pts) - 1):
        (x0, y0), (x1, y1), (x2, y2) = out[-1], pts[k], pts[k + 1]
        if x2 == x0 or abs(y1 - (y0 + (y2 - y0) * (x1 - x0) / (x2 - x0))) > tol:
            out.append(pts[k])
    out.append(pts[-1])
    return out


def polyline(pts, colour, width):
    return (f"<polyline points='{' '.join(f'{x:.1f},{y:.1f}' for x, y in simplify(pts))}' fill='none' "
            f"stroke='{colour}' stroke-width='{width}'/>")


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def label_svg(name):
    """active-low pins overlined, as the datasheet writes them"""
    if name.endswith("_n"):
        return f"<tspan text-decoration='overline'>{esc(name[:-2])}</tspan>"
    return {"A": "A0-A15", "D": "D0-D7", "CLK": "CLOCK"}.get(name, esc(name))


def bus_segments(t, w, pins, t0, t1, vdd, step=0.5):
    """[(from, to, value or None)]: settled stretches (every bit below
    0.8 V or above 2.0 V) and the changes between them"""
    segs, cur = [], None
    k = t0
    while k <= t1:
        vals = [value_at(t, w[p], k) for p in pins]
        ok = all(v < 0.8 or v > 2.0 for v in vals)
        val = sum(1 << i for i, v in enumerate(vals) if v > OUT_LEVEL) if ok else None
        if cur and cur[2] == val:
            cur[1] = k
        else:
            cur = [k, k, val]
            segs.append(cur)
        k += step
    return segs


def diagram(path, title, sub, t0, t1, t, w, men, edges, kind, cyc, ann, vdd):
    """One timing diagram, the datasheet's way: the clock with its T states,
    the buses as bands crossing over where they change, the strobes, and
    each measured parameter as a dimension between the clock edge (50 %)
    and the pin (1.5 V)."""
    rows = ROWS[kind][1]
    X0, X1, LH = 120, 1150, 34                     # time axis; signal height
    X = lambda tt: X0 + (tt - t0) / (t1 - t0) * (X1 - X0)
    # stack each row's dimensions so they do not overlap
    levels, rowann = {}, {r: [] for r in rows}
    for a in sorted(ann, key=lambda a: min(a[1], a[2])):
        rowann[a[3]].append(a)
    for r, lst in rowann.items():
        ends = []
        for a in lst:
            lo_x, hi_x = sorted((X(a[1]), X(a[2])))
            for li, e in enumerate(ends):
                if e < lo_x - 70:
                    ends[li] = hi_x
                    levels[id(a)] = li
                    break
            else:
                levels[id(a)] = len(ends)
                ends.append(hi_x)
    heights = [LH + 18 + 13 * len({levels[id(a)] for a in rowann[r]}) for r in rows]
    top = 74
    H = top + sum(heights) + 26 + 15 * ((len(sub) + 1) // 2)
    W = X1 + 30
    o = [f"<svg xmlns='http://www.w3.org/2000/svg' width='{W}' height='{H}' viewBox='0 0 {W} {H}'>",
         f"<rect width='{W}' height='{H}' fill='white'/>",
         f"<text x='{X0}' y='24' font-size='16' font-weight='bold' {FONT}>{esc(title)}</text>",
         f"<text x='{X0}' y='42' font-size='11.5' fill='#444' {FONT}>z80_chip, GF180 layout extracted to transistors (Magic), "
         f"ngspice; 4 MHz, 15 ns clock edges, 100 pF loads. Dimensions: CLK at 50 % to the pin at 1.5 V.</text>"]
    # T states over the clock, vertical lines at every clock edge
    rises = [i for i in cyc[1]]
    names = T_NAMES.get(kind, ["T1", "T2", "T3", "T4"])
    for n, i in enumerate(rises[:-1]):
        a, b = edges[i][0], edges[rises[n + 1]][0]
        o.append(f"<text x='{(X(a) + X(b)) / 2:.1f}' y='64' font-size='12' text-anchor='middle' {FONT}>"
                 f"{names[n] if n < len(names) else ''}</text>")
    for tt, clk, rst, kv in edges:
        if t0 <= tt <= t1:
            o.append(f"<line x1='{X(tt):.1f}' y1='{top}' x2='{X(tt):.1f}' y2='{top + sum(heights)}' stroke='#e4e4e4'/>")
    y = top
    ypos = {}
    for r, h in zip(rows, heights):
        hi, lo = y + 6, y + 6 + LH - 8
        ypos[r] = (hi, lo)
        o.append(f"<text x='{X0 - 12}' y='{(hi + lo) / 2 + 4:.1f}' font-size='12.5' text-anchor='end' {FONT}>{label_svg(r)}</text>")
        if r in BUS:
            segs = bus_segments(t, w, BUS[r], t0, t1, vdd)
            for k, (a, b, v) in enumerate(segs):
                if v is None:
                    prev = next((s for s in reversed(segs[:k]) if s[2] is not None), None)
                    nxt = next((s for s in segs[k + 1:] if s[2] is not None), None)
                    if prev and nxt:
                        o.append(f"<path d='M{X(a):.1f},{hi} L{X(b):.1f},{lo} M{X(a):.1f},{lo} L{X(b):.1f},{hi}' "
                                 f"stroke='#1f4f9a' fill='none' stroke-width='1.4'/>")
                    continue
                o.append(f"<path d='M{X(a):.1f},{hi} L{X(b):.1f},{hi} M{X(a):.1f},{lo} L{X(b):.1f},{lo}' "
                         f"stroke='#1f4f9a' fill='none' stroke-width='1.4'/>")
                if X(b) - X(a) > 44:
                    txt = f"{v:04X}" if r == "A" else f"{v:02X}"
                    if r == "D":         # who put the value there, just after it settled
                        at = a + 5
                        mem = value_at(t, men, at) > vdd / 2
                        cpu = next((kv["DD"] for tt, c, rs, kv in reversed(edges) if tt <= at), "0") == "1"
                        port = kind == "ioread" and at >= edges[cyc[1][0]][0]
                        txt += ((" from the port" if port else " from memory") if mem else
                                " from the CPU" if cpu else " (bus floating)")
                    o.append(f"<text x='{(X(a) + X(b)) / 2:.1f}' y='{(hi + lo) / 2 + 4:.1f}' font-size='11' "
                             f"text-anchor='middle' {FONT}>{esc(txt)}</text>")
        else:
            pts, k = [], t0
            while k <= t1:
                v = value_at(t, w[r], k)
                pts.append((X(k), lo - min(max(v / vdd, -0.05), 1.05) * (lo - hi)))
                k += 0.5
            o.append(polyline(pts, "#111" if r == "CLK" else "#1f4f9a", 1.5))
            if r != "CLK":
                yl = lo - OUT_LEVEL / vdd * (lo - hi)
                o.append(f"<line x1='{X0}' y1='{yl:.1f}' x2='{X1}' y2='{yl:.1f}' stroke='#9db4dc' stroke-dasharray='2 4'/>")
        y += h
    # the dimensions
    for a in ann:
        pid, t_ref, t_evt, r, text = a
        hi, lo = ypos[r]
        ya = lo + 13 + 13 * levels[id(a)]
        xa, xb = X(t_ref), X(t_evt)
        clk_lo = ypos["CLK"][1] - (ypos["CLK"][1] - ypos["CLK"][0]) / 2
        o.append(f"<line x1='{xa:.1f}' y1='{clk_lo:.1f}' x2='{xa:.1f}' y2='{ya:.1f}' stroke='#c0392b' stroke-width='0.7' stroke-dasharray='3 3'/>")
        o.append(f"<line x1='{xb:.1f}' y1='{(hi + lo) / 2:.1f}' x2='{xb:.1f}' y2='{ya:.1f}' stroke='#c0392b' stroke-width='0.7' stroke-dasharray='3 3'/>")
        d = 4 if xb >= xa else -4
        o.append(f"<path d='M{xa:.1f},{ya:.1f} L{xb:.1f},{ya:.1f} M{xb - d:.1f},{ya - 3:.1f} L{xb:.1f},{ya:.1f} L{xb - d:.1f},{ya + 3:.1f}' "
                 f"stroke='#c0392b' fill='none' stroke-width='1.1'/>")
        xl = max(xa, xb) + 6
        o.append(f"<circle cx='{xl + 7:.1f}' cy='{ya:.1f}' r='7' fill='white' stroke='#c0392b'/>")
        o.append(f"<text x='{xl + 7:.1f}' y='{ya + 3.5:.1f}' font-size='9' text-anchor='middle' fill='#c0392b' {FONT}>{pid}</text>")
        o.append(f"<text x='{xl + 17:.1f}' y='{ya + 4:.1f}' font-size='10.5' fill='#c0392b' {FONT}>{esc(text)}</text>")
    # what was measured, against the datasheet and STA
    ys = top + sum(heights) + 22
    for k, s in enumerate(sub):
        o.append(f"<text x='{X0 + (k % 2) * 520}' y='{ys + 15 * (k // 2)}' font-size='11' {FONT}>{esc(s)}</text>")
    o.append("</svg>")
    open(path, "w").write("\n".join(o) + "\n")


PROGRAM = {0x0000: "LD A,5Ah", 0x0002: "LD (8000h),A", 0x0005: "OUT (42h),A", 0x0007: "IN A,(43h)", 0x0009: "HALT"}
KIND_NAMES = {"fetch": "M1 fetch", "mread": "read", "mwrite": "write", "ioread": "IN", "iowrite": "OUT", "other": ""}


THEMES = {   # the README's (light) and the site's (dark, a logic analyser's screen)
    "light": {"bg": "white", "fg": "#111", "muted": "#555", "grid": "#d4d4d4", "clock": "#111",
              "line": "#1f4f9a", "bus": "#1f4f9a", "text": "#111"},
    "dark": {"bg": "#0d1117", "fg": "#d6dbe1", "muted": "#8b949e", "grid": "#262c36", "clock": "#a5b1c2",
             "line": "#3fb950", "bus": "#e3b341", "text": "#e3b341"},
}


def overview(path, title, t, w, edges, vdd, theme="light"):
    """The whole run on one page, as a logic analyser shows it: every pin,
    the machine cycles and the instructions over them."""
    c = THEMES[theme]
    fs = 1.3 if theme == "dark" else 1.0          # read at the site's width
    cyc = cycles_of(edges)
    t0, t1 = 0.0, edges[-1][0] + 60
    X0, X1 = 100, 1500
    X = lambda tt: X0 + (tt - t0) / (t1 - t0) * (X1 - X0)
    rows = ["CLK", "RESET_n", "A", "D", "M1_n", "MREQ_n", "IORQ_n", "RD_n", "WR_n", "RFSH_n", "HALT_n"]
    RH, top = 30, 82
    H = top + RH * len(rows) + 30
    W = X1 + 20
    o = [f"<svg xmlns='http://www.w3.org/2000/svg' width='{W}' height='{H}' viewBox='0 0 {W} {H}'>",
         f"<rect width='{W}' height='{H}' fill='{c['bg']}'/>"]
    if title:   # the site's version has its caption instead
        o.append(f"<text x='{X0}' y='22' font-size='{15.0*fs:.1f}' font-weight='bold' fill='{c['fg']}' {FONT}>{esc(title)}</text>")
    for kind, r, f in cyc:
        a, b = edges[r[0]][0], edges[r[-1]][0]
        o.append(f"<line x1='{X(a):.1f}' y1='48' x2='{X(a):.1f}' y2='{H - 26}' stroke='{c['grid']}'/>")
        o.append(f"<text x='{(X(a) + X(b)) / 2:.1f}' y='74' font-size='{9.5*fs:.1f}' text-anchor='middle' fill='{c['muted']}' {FONT}>"
                 f"{KIND_NAMES[kind]}</text>")
        if kind == "fetch":
            pc = int(edges[r[0]][3]["A"], 16)
            if pc in PROGRAM:
                o.append(f"<text x='{X(a) + 2:.1f}' y='58' font-size='{11.0*fs:.1f}' font-weight='bold' fill='{c['fg']}' {FONT}>"
                         f"{esc(PROGRAM[pc])}</text>")
    for k, row in enumerate(rows):
        hi = top + k * RH + 4
        lo = hi + RH - 12
        o.append(f"<text x='{X0 - 8}' y='{(hi + lo) / 2 + 4:.1f}' font-size='{11.0*fs:.1f}' text-anchor='end' fill='{c['fg']}' {FONT}>"
                 f"{label_svg(row)}</text>")
        if row in BUS:
            for a, b, v in bus_segments(t, w, BUS[row], t0, t1, vdd, step=1.0):
                if v is None:
                    continue
                o.append(f"<path d='M{X(a):.1f},{hi} L{X(b):.1f},{hi} M{X(a):.1f},{lo} L{X(b):.1f},{lo}' "
                         f"stroke='{c['bus']}' fill='none'/>")
                if X(b) - X(a) > (30 if row == "A" else 16) * fs:
                    o.append(f"<text x='{(X(a) + X(b)) / 2:.1f}' y='{(hi + lo) / 2 + 3.5 * fs:.1f}' font-size='{9.0*fs:.1f}' "
                             f"text-anchor='middle' fill='{c['text']}' {FONT}>{(f'{v:04X}' if row == 'A' else f'{v:02X}')}</text>")
        else:
            pts, k2 = [], t0
            while k2 <= t1:
                v = value_at(t, w[row], k2)
                pts.append((X(k2), lo - min(max(v / vdd, -0.05), 1.05) * (lo - hi)))
                k2 += 1.0
            o.append(polyline(pts, c["clock"] if row == "CLK" else c["line"], 1))
    for us in range(0, int(t1 / 1000) + 1):
        o.append(f"<text x='{X(us * 1000):.1f}' y='{H - 10}' font-size='{10.0*fs:.1f}' text-anchor='middle' fill='{c['muted']}' {FONT}>"
                 f"{us} us</text>")
    o.append("</svg>")
    open(path, "w").write("\n".join(o) + "\n")


def figures(work, corner, edges, out, sta_file):
    vdd = CORNERS[corner][2]
    t, w, men = load(os.path.join(work, corner, "chip.dat"))
    ds = datasheet()
    sta = sta_numbers(sta_file, corner)
    bad = check_functional(t, w, edges)
    meas = measure(t, w, edges)
    cycles = cycles_of(edges)
    os.makedirs(out, exist_ok=True)
    # every parameter over every cycle that has it
    allv, lost = {}, {}
    for kind, n, ev, miss in meas:
        for (pid, occ), (tr, te) in ev.items():
            allv.setdefault(pid, []).append(te - tr)
        for pid, tr in miss:
            lost.setdefault(pid, []).append(tr)
        for pid, (p_late, o_late), (p_early, o_early) in SEPARATIONS.get(kind, []):
            if (p_late, o_late) in ev and (p_early, o_early) in ev:
                allv.setdefault(pid, []).append(ev[(p_late, o_late)][1] - ev[(p_early, o_early)][1])
    rows = []
    for pid in sorted(set(allv) | set(lost)):
        sym, bound, lim = ds[pid]
        vals = allv.get(pid, [])
        worst = (max(vals) if bound == "max" else min(vals)) if vals else None
        ok = bool(vals) and pid not in lost and (worst <= lim if bound == "max" else worst >= lim)
        rows.append({"id": pid, "symbol": sym, "bound": bound, "limit": lim,
                     "spice_min": min(vals) if vals else None, "spice_max": max(vals) if vals else None,
                     "n": len(vals), "missing_at_ns": lost.get(pid, []), "sta": sta.get(pid), "pass": ok})
    # the figures: of each kind the first cycle whose address changes at T1
    figs = []
    for kind in ROWS:
        cand = [(k, n, ev) for k, n, ev, _ in meas if k == kind and (6, 0) in ev]
        if not cand:
            continue
        _, n, ev = cand[0]
        cyc = cycles[n]
        t0 = edges[cyc[1][0]][0] - 70
        t1 = edges[cyc[1][-1]][0] + 95
        ann = []
        for (pid, occ), (tr, te) in sorted(ev.items(), key=lambda x: x[1][0]):
            spec = next(d for d in DELAYS[kind] if d[0] == pid)
            pin = [d for d in DELAYS[kind] if d[0] == pid][occ][2]
            row = pin if pin in ROWS[kind][1] else None
            if row and t0 <= min(tr, te) and max(tr, te) <= t1:
                ann.append((pid, tr, te, row, f"{te - tr:.1f} ns"))
        sub = []
        seen = set()
        for pid, *_ in DELAYS[kind] + [(s[0],) for s in SEPARATIONS.get(kind, [])]:
            if pid in seen or pid not in allv:
                continue
            seen.add(pid)
            sym, bound, lim = ds[pid]
            mine = [v for (p, o), (tr, te) in ev.items() if p == pid for v in [te - tr]]
            if not mine:
                sep = [s for s in SEPARATIONS.get(kind, []) if s[0] == pid]
                if sep and sep[0][1] in ev and sep[0][2] in ev:
                    mine = [ev[sep[0][1]][1] - ev[sep[0][2]][1]]
            if not mine:
                continue
            s = f"{pid:2d} {sym}: {', '.join(f'{v:.1f}' for v in mine)} ns ({'max' if bound == 'max' else 'min'} {lim:g}"
            if pid in sta:
                s += f"; STA {sta[pid]:.1f}"
            sub.append(s + ")")
        title = ROWS[kind][0]
        path = os.path.join(out, f"{kind}.svg")
        diagram(path, title, sub, t0, t1, t, w, men, edges, kind, cyc, ann, vdd)
        figs.append(path)
    title = f"z80_chip at transistor level (GF180 {corner}): power-up, RESET, the program at 4 MHz"
    for theme, name, head in (("light", "program.svg", title), ("dark", "program-dark.svg", "")):
        overview(os.path.join(out, name), head, t, w, edges, vdd, theme)
        figs.append(os.path.join(out, name))
    res = {"corner": corner, "functional_mismatches": [list(map(str, b)) for b in bad], "parameters": rows,
           "figures": [os.path.basename(f) for f in figs]}
    json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)
    print(f"chip: pins against the RTL at every clock edge after RESET: {len(bad)} mismatches")
    for b in bad[:10]:
        print("   ", b)
    print(f"\n  {'#':>2} {'parameter':16} {'limit':>9}  {'SPICE (every cycle)':>22}  {'STA':>6}")
    for r in rows:
        lim = f"{'<=' if r['bound'] == 'max' else '>='}{r['limit']:6.1f}"
        sp = f"{r['spice_min']:.1f} .. {r['spice_max']:.1f} ({r['n']})" if r["n"] else "-"
        st = f"{r['sta']:.1f}" if r["sta"] is not None else "-"
        print(f"  {r['id']:2d} {r['symbol']:16} {lim:>9}  {sp:>22}  {st:>6}  {'ok' if r['pass'] else 'FAIL'}"
              + (f"  (no edge within 150 ns of the clock at {', '.join(f'{x:.0f}' for x in r['missing_at_ns'])} ns)"
                 if r["missing_at_ns"] else ""))
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("run", help="the chip's LibreLane run (asic-gf180mcu chip/runs/<tag>)")
    ap.add_argument("--work", help="working directory (default: <run>/transistor)")
    ap.add_argument("--corner", default="tt", choices=sorted(CORNERS))
    ap.add_argument("--threads", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--steps", default="extract,reference,simulate,figures")
    ap.add_argument("--segment", help="K/N: simulate the K-th of N windows; stitch takes N")
    ap.add_argument("--gds", help="the chip's GDS for extract (default <run>/final/gds/z80_chip.gds)")
    ap.add_argument("--sta", help="pin_timing.py's output for the run (STA's numbers beside SPICE's)")
    ap.add_argument("--out", help="figures and results.json (default: chip/gf180mcu/, other corners "
                                  "in chip/gf180mcu/<corner>/)")
    a = ap.parse_args()
    out = a.out or os.path.join(HERE, "chip", "gf180mcu", *([] if a.corner == "tt" else [a.corner]))
    work = os.path.abspath(a.work or os.path.join(a.run, "transistor"))
    os.makedirs(work, exist_ok=True)
    steps = a.steps.split(",")
    if "extract" in steps and not os.path.exists(os.path.join(work, "z80_chip.spice")):
        extract(a.run, work, a.gds)
    ref = reference(work) if "reference" in steps or not os.path.exists(os.path.join(work, "ref.txt")) \
        else open(os.path.join(work, "ref.txt")).read()
    edges = edges_of(ref)
    seg = tuple(int(x) for x in a.segment.split("/")) if a.segment else None
    if "simulate" in steps:
        simulate(work, a.corner, edges, a.threads, seg)
    if "stitch" in steps:
        stitch(work, a.corner, edges, seg[1] if seg else 1)
    if "figures" in steps:
        res = figures(work, a.corner, edges, out, a.sta)
        failed = [p["symbol"] for p in res["parameters"] if not p["pass"]]
        if res["functional_mismatches"] or failed:
            print(f"chip: {len(res['functional_mismatches'])} pin mismatches, {len(failed)} AC parameters failed")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
