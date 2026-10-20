#!/usr/bin/env python3
# event_sweep.py - RESET / INT / NMI timing sweep: the RTL against the
# perfectz80 gate-level netlist, half-cycle by half-cycle.
#
# Each case runs a small program (register dump at 0000, IM1 handler at 0038,
# NMI handler at 0066, setup at 0100, then a marker NOP at 0200 followed by
# the instruction under test) and drives RESET, INT or NMI pulses placed at
# a T-state of that instruction plus a fraction of a clock. The RTL side is
# tests/iverilog/tb_event_sweep.v (1/16-T event resolution), the reference
# is scripts/event_sweep_pz80.c (perfectz80, whole half-cycles; a pulse that
# spans no clock edge is applied and removed inside one half-cycle). Both
# dump the pins for every half-cycle from the marker fetch's T1.
#
# A case matches when the control pins (mreq iorq rd wr m1 rfsh halt busack)
# agree in every half-cycle, the address agrees wherever both sides transfer
# data (MREQ or IORQ with RD or WR) or refresh (RFSH low: I:R; idle addresses
# are don't-care), and the data agrees wherever both drive WR. Differences are checked
# against tests/event_sweep/baseline.txt: a difference that is not in the
# baseline, or that changed, fails the run. Cases in the baseline that now
# match are reported as a warning so the baseline can be trimmed.
#
# Differences are expected (see docs/event-sweep.md); the baseline lists
# each differing case with a hash of the RTL's compared pins, so any change
# to the RTL's behaviour in one of those cases also fails until the baseline
# is regenerated with --update-baseline and the diff reviewed.
#
# Usage:
#   scripts/event_sweep.py [--groups int,nmi,...] [--match STR] [--phases N]
#                          [--jobs N] [--update-baseline]
# Output: build/event_sweep/report.md, one side-by-side trace per differing
# case in build/event_sweep/diff/.
import argparse, collections, hashlib, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TB_VVP = os.path.join(ROOT, "build", "tb_event_sweep.vvp")
PZ80 = os.path.join(ROOT, "build", "bin", "event_sweep_pz80")
OUT = os.path.join(ROOT, "build", "event_sweep")
BASELINE = os.path.join(ROOT, "tests", "event_sweep", "baseline.txt")

NMI, INT, RESET = 0, 1, 4
PIN_NAMES = {NMI: "nmi", INT: "int", RESET: "reset"}

# name: (bytes, T-states on a Z80, note)
TARGETS = {
    "inc_a":    ([0x3c], 4, "no memory access"),
    "ld_a_hl":  ([0x7e], 7, "memory read"),
    "ld_hl_a":  ([0x77], 7, "memory write"),
    "inc_hl":   ([0x34], 11, "read + write"),
    "push_bc":  ([0xc5], 11, "two writes"),
    "ld_nn_hl": ([0x22, 0x00, 0x81], 16, "two operand reads, two writes"),
    "jr":       ([0x18, 0x02, 0x00, 0x00], 12, "taken relative branch"),
    "call":     ([0xcd, 0x00, 0x04], 17, "call (RET at 0400)"),
    "ld_ix_a":  ([0xdd, 0x77, 0x05], 19, "DD prefix, indexed write"),
    "neg":      ([0xed, 0x44], 8, "ED prefix"),
    "rlc_hl":   ([0xcb, 0x06], 15, "CB prefix, read + write"),
    "ldir":     ([0xed, 0xb0], 58, "repeating block move, BC=3"),
    "halt":     ([0x76], 4, "HALT"),
    "ld_a_i":   ([0xed, 0x57], 9, "LD A,I: P/V from IFF2"),
    "ld_a_r":   ([0xed, 0x5f], 9, "LD A,R: P/V from IFF2"),
}
BASIC = ["inc_a", "ld_a_hl", "ld_hl_a", "inc_hl"]
GROUPS = ["int", "nmi", "nmi_short", "nmi_reentry", "reset", "reset_release",
          "reset_wait", "reset_short", "combo"]


def program(target, im=1, ei=False, nmi_nops=0):
    """The sweep program as a 64K image (same layout as the hardware harness
    in hardesk's rp2350b_driver tests/event_sweep.cpp)."""
    ram = bytearray(0x10000)
    for i in range(0x100):
        ram[0x8000 + i] = (0x11 * (i + 1)) & 0xff

    def put(at, data):
        ram[at:at + len(data)] = bytes(data)
    # 0000: register dump (what a reset left), then on to the setup
    put(0x0000, [0xed, 0x73, 0x00, 0x70,          # LD (7000),SP
                 0xed, 0x43, 0x02, 0x70,          # LD (7002),BC
                 0xed, 0x53, 0x04, 0x70,          # LD (7004),DE
                 0x22, 0x06, 0x70,                # LD (7006),HL
                 0xed, 0x57, 0x32, 0x08, 0x70,    # LD A,I; LD (7008),A
                 0xf5,                            # PUSH AF (P/V = IFF2)
                 0xed, 0x5f, 0x32, 0x09, 0x70,    # LD A,R; LD (7009),A
                 0xc3, 0x00, 0x01])               # JP 0100
    # 0038: IM1 handler, counts in 6000
    put(0x0038, [0xf5, 0x3a, 0x00, 0x60, 0x3c, 0x32, 0x00, 0x60, 0xf1, 0xfb, 0xed, 0x4d])
    # 0066: NMI handler, counts in 6001
    put(0x0066, [0xf5, 0x3a, 0x01, 0x60, 0x3c, 0x32, 0x01, 0x60] + [0x00] * nmi_nops
                + [0xf1, 0xed, 0x45])
    # 0100: DI, SP=9000, IMn, BC=3 DE=8300 HL=8000 IX=8200 I=3F A=42, [EI], JP 0200
    put(0x0100, [0xf3, 0x31, 0x00, 0x90, 0xed, {0: 0x46, 1: 0x56, 2: 0x5e}[im],
                 0x01, 0x03, 0x00, 0x11, 0x00, 0x83, 0x21, 0x00, 0x80,
                 0xdd, 0x21, 0x00, 0x82, 0x3e, 0x3f, 0xed, 0x47, 0x3e, 0x42]
                + ([0xfb, 0x00] if ei else []) + [0xc3, 0x00, 0x02])
    # 0200: marker NOP, the target, 24 NOPs, HALT
    put(0x0200, [0x00] + list(target) + [0x00] * 24 + [0x76])
    put(0x0400, [0xc9])                           # CALL target: RET
    for i in range(0, 256, 2):                    # IM2 table at I=3F
        ram[0x3f00 + i], ram[0x3f01 + i] = 0x38, 0x00
    return ram


def write_hex(ram, path):
    with open(path, "w") as f:
        for base in range(0, 0x10000, 16):
            chunk = ram[base:base + 16]
            if any(chunk):
                f.write(f"@{base:04x} " + " ".join(f"{b:02x}" for b in chunk) + "\n")


def at(n, j):
    """Sixteenth of T-state n (n=0 the marker's T4, n=1 the target's T1),
    j/16 of a clock after its rising edge."""
    return (n + 3) * 16 + j


def case(name, group, target, pulses, run=None, **kw):
    """pulses: [(pin, lo, hi)] in sixteenths, hi=-1 never released."""
    length = TARGETS[target][1]
    c = {"name": name, "group": group, "target": target, "pulses": pulses,
         "run": run or (140 if target == "ldir" else 60 + length),
         "im": 1, "ei": False, "nmi_nops": 0, "inta_release": False,
         "int_at": None, "hold": None, "meta": {}}
    c.update(kw)
    return c


def cases(groups, phases):
    """phases: positions per T-state for 1 T and longer pulses (16 = every
    1/16 T as on hardware; 2 = one in each half, which is all a zero-delay
    simulation can tell apart). Sub-T pulses always sweep all 16."""
    step = 16 // phases
    ph = range(step // 2, 16, step)
    out = []
    want = lambda g: "all" in groups or g in groups
    if want("int"):          # IM1, EI, INT held until INTA
        for t in BASIC + ["halt", "ld_a_i", "ld_a_r"]:
            for n in range(TARGETS[t][1] + 2):
                for j in ph:
                    out.append(case(f"int_{t}_T{n}_p{j:02d}", "int", t, [(INT, at(n, j), -1)],
                                    ei=True, inta_release=True, meta={"n": n, "phase": j}))
    if want("nmi"):          # 1 T pulse: acceptance point and stacked PC
        for t in TARGETS:
            for n in range(TARGETS[t][1] + 2):
                for j in ph:
                    out.append(case(f"nmi_{t}_T{n}_p{j:02d}", "nmi", t,
                                    [(NMI, at(n, j), at(n, j) + 16)], meta={"n": n, "phase": j}))
    if want("nmi_short"):    # 1/16 T (~133 ns at 500 kHz) and 1/4 T pulses
        for w in (1, 4):
            for t in BASIC:
                for n in range(TARGETS[t][1] + 2):
                    for j in range(16):
                        out.append(case(f"nmis{w}_{t}_T{n}_p{j:02d}", "nmi_short", t,
                                        [(NMI, at(n, j), at(n, j) + w)],
                                        meta={"n": n, "phase": j, "width": w}))
    if want("nmi_reentry"):
        for j in (4, 12):    # held low for 40 T
            out.append(case(f"nmir_held_p{j:02d}", "nmi_reentry", "inc_hl",
                            [(NMI, at(2, j), at(42, j))], run=300, meta={"kind": "held"}))
        for gap in range(1, 8):    # two pulses before acceptance, both in INC (HL)
            out.append(case(f"nmir_two_gap{gap}", "nmi_reentry", "inc_hl",
                            [(NMI, at(2, 4), at(3, 4)), (NMI, at(3 + gap, 4), at(4 + gap, 4))],
                            run=300, meta={"kind": "two_before_accept", "gap": gap}))
        for gap in range(3, 14):   # second pulse during the NMI response
            for j in (4, 12):
                out.append(case(f"nmir_entry_gap{gap}_p{j:02d}", "nmi_reentry", "inc_a",
                                [(NMI, at(2, 4), at(3, 4)), (NMI, at(3 + gap, j), at(4 + gap, j))],
                                run=300, meta={"kind": "during_entry", "gap": gap}))
        for gap in range(14, 44, 3):  # second pulse inside a 16-NOP handler
            out.append(case(f"nmir_nested_gap{gap}", "nmi_reentry", "inc_a",
                            [(NMI, at(2, 4), at(3, 4)), (NMI, at(3 + gap, 4), at(4 + gap, 4))],
                            run=300, nmi_nops=16, meta={"kind": "nested", "gap": gap}))
        for ei in (True, False):   # INT pending from the NMI handler's fetch until RETN
            out.append(case(f"nmir_retn_int_ei{int(ei)}", "nmi_reentry", "inc_a",
                            [(NMI, at(2, 4), at(3, 4))], run=300, ei=ei, int_at=0x0066,
                            meta={"kind": "retn_int"}))
    if want("reset"):        # 4 T pulse at every T-state
        for t in BASIC + ["halt", "push_bc"]:
            for n in range(TARGETS[t][1] + 2):
                for j in ph:
                    out.append(case(f"reset_{t}_T{n}_p{j:02d}", "reset", t,
                                    [(RESET, at(n, j), at(n, j) + 64)], run=300, ei=True,
                                    meta={"n": n, "phase": j}))
    if want("reset_release"):  # release swept across a clock, 3 and 4 T after assertion
        for t in ("inc_a", "halt"):
            for base in (3, 4):
                for j in range(16):
                    out.append(case(f"resetr_{t}_{base}T_p{j:02d}", "reset_release", t,
                                    [(RESET, at(1, 0), at(1, 0) + base * 16 + j)], run=300,
                                    meta={"phase": j, "base": base}))
    if want("reset_wait"):   # RESET while a read is held by WAIT (15 T)
        for n in range(3, 19):
            for j in ph:
                out.append(case(f"resetw_T{n}_p{j:02d}", "reset_wait", "ld_a_hl",
                                [(RESET, at(n, j), at(n, j) + 64)], run=300, hold=(0x8000, 15),
                                meta={"n": n, "phase": j}))
    if want("reset_short"):  # below the 3-clock minimum, around M1 (special-reset window)
        for q in (4, 8, 16, 24, 32):
            for n in (1, 2, 3):
                for j in range(16):
                    out.append(case(f"resets_w{q}_T{n}_p{j:02d}", "reset_short", "inc_a",
                                    [(RESET, at(n, j), at(n, j) + q)], run=300,
                                    meta={"n": n, "phase": j, "width": q}))
    if want("combo"):
        for n in range(9, 31):     # INT taken after INC (HL), RESET around INTA / pushes / handler
            for j in ph:
                out.append(case(f"combo_int_reset_T{n}_p{j:02d}", "combo", "inc_hl",
                                [(INT, at(2, 4), -1), (RESET, at(n, j), at(n, j) + 64)], run=300,
                                ei=True, inta_release=True, meta={"kind": "int_then_reset"}))
        for n in range(3, 21):     # NMI taken after INC A, RESET around the response
            for j in ph:
                out.append(case(f"combo_nmi_reset_T{n}_p{j:02d}", "combo", "inc_a",
                                [(NMI, at(2, 4), at(3, 4)), (RESET, at(n, j), at(n, j) + 64)],
                                run=300, meta={"kind": "nmi_then_reset"}))
        for n in range(2, 10):     # NMI edge while RESET is low
            out.append(case(f"combo_nmi_in_reset_T{n}", "combo", "inc_a",
                            [(RESET, at(1, 0), at(9, 0)), (NMI, at(n, 8), at(n + 1, 8))],
                            run=300, meta={"kind": "nmi_during_reset"}))
        for j in range(0, 16, 4):  # NMI falls before RESET and is still low at release
            out.append(case(f"combo_nmi_held_reset_p{j:02d}", "combo", "inc_a",
                            [(NMI, at(1, j), at(31, j)), (RESET, at(3, 0), at(7, 0))],
                            run=300, meta={"kind": "nmi_held_across_reset"}))
        for j in range(0, 16, 4):  # INT held across RESET: IFF clear until EI
            out.append(case(f"combo_int_held_reset_p{j:02d}", "combo", "inc_a",
                            [(INT, at(1, j), -1), (RESET, at(1, 8), at(5, 8))], run=300,
                            ei=True, inta_release=True, meta={"kind": "int_held_across_reset"}))
    return out


def pz80_key(c):
    """perfectz80 resolves whole half-cycles, so cases whose transitions fall
    in the same half-cycles (and keep the same order inside one) give the same
    result: run each such set once."""
    def half(s):
        return None if s < 0 else s // 8
    pulses = tuple(sorted((p, half(lo), half(hi), lo // 8 == hi // 8 and hi >= 0)
                          for p, lo, hi in c["pulses"]))
    return (c["target"], c["im"], c["ei"], c["nmi_nops"], c["inta_release"], c["int_at"],
            c["hold"], c["run"], pulses)


def run_case(c, workdir, side):
    key = f"{c['target']}_{c['im']}{int(c['ei'])}{c['nmi_nops']}"
    prog = os.path.join(workdir, f"prog_{key}.hex")
    ev = os.path.join(workdir, f"{c['name']}.events")
    with open(ev, "w") as f:
        for p, lo, hi in c["pulses"]:
            f.write(f"{p} {lo} {hi}\n")
    if side == "rtl":
        cmd = ["vvp", "-n", TB_VVP, f"+prog={prog}", f"+events={ev}", f"+run={c['run']}"]
        if c["inta_release"]: cmd.append("+inta_release=1")
        if c["int_at"] is not None: cmd.append(f"+int_at={c['int_at']:04x}")
        if c["hold"]: cmd += [f"+hold={c['hold'][0]:04x}", f"+hold_t={c['hold'][1]}"]
    else:
        cmd = [PZ80, "-e", ev, "-r", str(c["run"])]
        if c["inta_release"]: cmd.append("-i")
        if c["int_at"] is not None: cmd += ["-a", f"{c['int_at']:04x}"]
        if c["hold"]: cmd += ["-H", f"{c['hold'][0]:04x}", "-T", str(c["hold"][1])]
        cmd.append(prog)
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        v = line.split()
        if len(v) == 12 and v[0].isdigit():
            rows.append([int(v[0]), int(v[1]), int(v[2], 16), int(v[3], 16)] + [int(x) for x in v[4:]])
    if not rows:
        raise RuntimeError(f"{side} {c['name']}: {out.strip()[-200:]}")
    return rows


# row: h clk addr data mreq iorq rd wr m1 rfsh halt busack
H, CLK, ADDR, DATA, MREQ, IORQ, RD, WR, M1, RFSH, HALT, BUSACK = range(12)
CONTROL = [("mreq", MREQ), ("iorq", IORQ), ("rd", RD), ("wr", WR), ("m1", M1),
           ("rfsh", RFSH), ("halt", HALT), ("busack", BUSACK)]


def addressed(r):
    """The address means something: a transfer (MREQ or IORQ with RD or
    WR) or a refresh (RFSH low: I:R). Idle addresses are don't-care."""
    return ((not r[MREQ] or not r[IORQ]) and (not r[RD] or not r[WR])) or not r[RFSH]


def first_difference(a, b):
    """(half, [what differs]) of the first mismatching half-cycle, or None."""
    bh = {r[H]: r for r in b}
    if not a or not b or a[0][H] != b[0][H]:
        return (a[0][H] if a else 0), ["start"]
    for ra in a:
        rb = bh.get(ra[H])
        if rb is None:
            return ra[H], ["length"]
        what = [n for n, i in CONTROL if ra[i] != rb[i]]
        if addressed(ra) and addressed(rb) and ra[ADDR] != rb[ADDR]:
            what.append("addr")
        if not ra[WR] and not rb[WR] and ra[DATA] != rb[DATA]:
            what.append("data")
        if what:
            return ra[H], what
    if len(a) != len(b):
        return min(len(a), len(b)), ["length"]
    return None


def bus_cycles(rows, halves=False):
    """Bus cycles from a trace: fXXXX opcode fetch, rXXXX read, wXXXX=DD
    write, iXXXX INTA, and '!' once nothing has been driven for 4 T (a
    reset). halves=True also returns the half-cycle each one starts in."""
    out, at, prev, idle = [], [], None, 0
    for r in rows:
        if not r[M1] and not r[MREQ] and not r[RD]: cur = f"f{r[ADDR]:04x}"
        elif not r[M1] and not r[IORQ]: cur = f"i{r[ADDR]:04x}"
        elif r[M1] and not r[MREQ] and not r[RD]: cur = f"r{r[ADDR]:04x}"
        elif not r[MREQ] and not r[WR]: cur = f"w{r[ADDR]:04x}={r[DATA]:02x}"
        else: cur = None
        idle = idle + 1 if cur is None and r[RFSH] else 0
        if idle == 8:
            out.append("!"); at.append(r[H])
        if cur and cur != prev and not (cur[0] == "w" and prev and prev[:5] == cur[:5]):
            out.append(cur); at.append(r[H])
        prev = cur
    return (out, at) if halves else out


def signature(rows):
    """Hash of everything the comparison looks at, so a baseline entry
    changes whenever the RTL's compared pins change."""
    h = hashlib.sha1()
    for r in rows:
        h.update(bytes([r[i] for _, i in CONTROL]))
        if addressed(r): h.update(r[ADDR].to_bytes(2, "little"))
        if not r[WR]: h.update(bytes([r[DATA]]))
    return h.hexdigest()[:12]


def summarize(c, rtl, pz):
    d = first_difference(rtl, pz)
    if d is None:
        return None
    half, what = d
    return {"key": f"{half} {','.join(what)} {signature(rtl)}", "first_half": half, "differs": what,
            "rz80": " ".join(bus_cycles(rtl)[:40]), "pz80": " ".join(bus_cycles(pz)[:40])}


def facts(c, rows):
    """What a trace means: when INT / NMI responses start, and for a RESET
    pulse how long the bus took to go idle, how many rising edges after the
    release the first T1 at 0000 came, and what the code at 0000 then
    stored in 7000..7009 (SP BC DE HL I R)."""
    seq, at = bus_cycles(rows, halves=True)
    # INT response: an INTA cycle. NMI response: an opcode fetch whose
    # address is then pushed (two writes) before the fetch from 0066.
    f = {"int": [h for b, h in zip(seq, at) if b[0] == "i"], "nmi": []}
    for i in range(len(seq) - 3):
        b, hi, lo, nxt = seq[i:i + 4]
        if (b[0] == "f" and hi[0] == lo[0] == "w" and nxt == "f0066"
                and int(hi[-2:] + lo[-2:], 16) == int(b[1:], 16)):
            f["nmi"].append(at[i])
    resets = [(lo // 8, hi // 8) for p, lo, hi in c["pulses"] if p == RESET]
    if resets:
        a, rel = resets[0]
        strobes = (MREQ, IORQ, RD, WR, M1)
        f["idle"] = next((r[H] - a for r in rows if r[H] >= a and all(r[i] for i in strobes)), None)
        t1 = next((r[H] for r in rows if r[H] >= a and not r[M1] and r[ADDR] == 0 and r[CLK]), None)
        f["restart"] = None if t1 is None else (t1 - rel + 1) // 2
        dump = {}
        for r in rows:
            if t1 is not None and r[H] > t1 and not r[MREQ] and not r[WR] and 0x7000 <= r[ADDR] < 0x700a:
                dump[r[ADDR] - 0x7000] = r[DATA]
        if len(dump) == 10:
            d = [dump[i] for i in range(10)]
            f["regs"] = (f"SP={d[1]:02x}{d[0]:02x} BC={d[3]:02x}{d[2]:02x} DE={d[5]:02x}{d[4]:02x} "
                         f"HL={d[7]:02x}{d[6]:02x} I={d[8]:02x}")
    return f


def cell(c, s, fr, fp):
    """Grid character for an INT / NMI case."""
    if s is None:
        return "."
    pin = "int" if any(p == INT for p, _, _ in c["pulses"]) else "nmi"
    a, b = fr[pin], fp[pin]
    if len(a) != len(b):
        return "m" if len(a) < len(b) else "x"
    if a and a[0] != b[0]:
        return "e" if a[0] < b[0] else "l"
    return "d"


GRID_LEGEND = ("`.` same pins as perfectz80; `e` / `l` RZ80 starts the response earlier / later; "
               "`m` RZ80 takes fewer responses (misses the pulse), `x` more; "
               "`d` same responses, other pins differ. Rows: T-state of the pulse start "
               "(M4 = the marker's T4, T1 = the target's T1); columns: 1/16 T after its rising edge.")


def grid(rows):
    phases = sorted({c["meta"]["phase"] for c, *_ in rows})
    tab = collections.defaultdict(dict)
    for c, s, fr, fp in rows:
        tab[c["meta"]["n"]][c["meta"]["phase"]] = cell(c, s, fr, fp)
    out = ["| T | " + " ".join(f"{j:x}" for j in phases) + " |", "|---|---|"]
    for n in sorted(tab):
        out.append(f"| {'M4' if n == 0 else f'T{n}'} | `" +
                   " ".join(tab[n].get(j, " ") for j in phases) + "` |")
    return out


def reset_section(rows):
    out = []
    for side, k in (("RZ80", 2), ("perfectz80", 3)):
        idle = collections.Counter(r[k].get("idle") for r in rows)
        restart = collections.Counter(r[k].get("restart") for r in rows)
        out.append(f"- {side}: RESET low to all strobes inactive (half-cycles): " +
                   ", ".join(f"{a}: {b}" for a, b in sorted(idle.items(), key=lambda x: (x[0] is None, x[0] or 0))))
        out.append(f"- {side}: rising edges from the release to the first T1 at 0000 (none = no restart): " +
                   ", ".join(f"{a}: {b}" for a, b in sorted(restart.items(), key=lambda x: (x[0] is None, x[0] or 0))))
        regs = collections.Counter(r[k].get("regs") for r in rows if r[k].get("regs"))
        for v, cnt in regs.most_common(4):
            out.append(f"  - {cnt} x registers stored by the code at 0000: `{v}`")
    return out


def write_diff(c, rtl, pz, path):
    names = "h clk addr data mreq iorq rd wr m1 rfsh halt busack"
    with open(path, "w") as f:
        f.write(f"# {c['name']}: pulses {c['pulses']} (pin lo hi in 1/16 T from the marker's T1)\n")
        f.write(f"# {'RZ80 (iverilog RTL)':<44} | perfectz80\n# {names:<44} | {names}\n")
        for i in range(max(len(rtl), len(pz))):
            fmt = lambda r: (f"{r[H]} {r[CLK]} {r[ADDR]:04x} {r[DATA]:02x} " +
                             " ".join(str(v) for v in r[MREQ:])) if r else ""
            a = rtl[i] if i < len(rtl) else None
            b = pz[i] if i < len(pz) else None
            mark = " " if a and b and fmt(a).split()[4:] == fmt(b).split()[4:] else "*"
            f.write(f"{mark} {fmt(a):<44} | {fmt(b)}\n")


def report(results, baseline, new, changed, fixed, path):
    by_group = collections.defaultdict(list)
    for c, s, fr, fp in results:
        by_group[c["group"]].append((c, s))
    out = ["# RESET / INT / NMI sweep: RTL vs perfectz80\n",
           "One line per group; a case matches when every half-cycle of pins agrees "
           "(see scripts/event_sweep.py).\n",
           "| group | cases | match | known difference | new | changed |", "|---|---|---|---|---|---|"]
    for g in GROUPS:
        rows = by_group.get(g)
        if not rows: continue
        m = sum(1 for _, s in rows if s is None)
        out.append(f"| {g} | {len(rows)} | {m} | "
                   f"{sum(1 for c, s in rows if s and c['name'] in baseline and c['name'] not in changed)} | "
                   f"{sum(1 for c, _ in rows if c['name'] in new)} | "
                   f"{sum(1 for c, _ in rows if c['name'] in changed)} |")
    for title, names in (("New differences", new), ("Changed differences", changed),
                         ("Known differences that now match", fixed)):
        if not names: continue
        out.append(f"\n## {title} ({len(names)})\n")
        res = dict((c["name"], s) for c, s, *_ in results)
        for n in sorted(names)[:200]:
            s = res.get(n)
            if s:
                out.append(f"- `{n}` first at half {s['first_half']} ({', '.join(s['differs'])})\n"
                           f"  - RZ80: `{s['rz80']}`\n  - pz80: `{s['pz80']}`")
            else:
                out.append(f"- `{n}`")
    full = collections.defaultdict(list)
    for r in results:
        full[r[0]["group"]].append(r)
    for g, title in (("int", "INT (IM1, EI, held until INTA)"), ("nmi", "NMI (1 T pulse)")):
        if not full.get(g): continue
        out.append(f"\n## {title}\n\n{GRID_LEGEND}\n")
        for t in TARGETS:
            rows = [r for r in full[g] if r[0]["target"] == t]
            if rows:
                out += [f"### `{t}` ({TARGETS[t][2]}, {TARGETS[t][1]} T)\n"] + grid(rows) + [""]
    if full.get("nmi_short"):
        out.append(f"\n## NMI pulses shorter than T\n\n{GRID_LEGEND}\n")
        for w in (1, 4):
            for t in BASIC:
                rows = [r for r in full["nmi_short"] if r[0]["target"] == t and r[0]["meta"]["width"] == w]
                if rows:
                    out += [f"### {w}/16 T pulse, `{t}`\n"] + grid(rows) + [""]
    if full.get("nmi_reentry"):
        out.append("\n## NMI re-entry\n\n| case | RZ80 NMI responses | perfectz80 NMI responses | INT |\n|---|---|---|---|")
        for c, s, fr, fp in full["nmi_reentry"]:
            out.append(f"| `{c['name']}` | {len(fr['nmi'])} | {len(fp['nmi'])} | {len(fr['int'])} / {len(fp['int'])} |")
    for g in ("reset", "reset_release", "reset_wait", "reset_short", "combo"):
        rows = [r for r in full.get(g, []) if any(p == RESET for p, _, _ in r[0]["pulses"])]
        if rows:
            out += [f"\n## {g} ({len(rows)} cases)\n"] + reset_section(rows)
    out.append("\n## Differing cases by group\n")
    for g in GROUPS:
        rows = [(c, s) for c, s in by_group.get(g, []) if s]
        if not rows: continue
        out.append(f"### {g} ({len(rows)})\n")
        for c, s in rows[:12]:
            out.append(f"- `{c['name']}` first at half {s['first_half']} ({', '.join(s['differs'])})\n"
                       f"  - RZ80: `{s['rz80']}`\n  - pz80: `{s['pz80']}`")
        if len(rows) > 12:
            out.append(f"- ... {len(rows) - 12} more (build/event_sweep/diff/)")
        out.append("")
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--groups", default="all")
    ap.add_argument("--match", default="")
    ap.add_argument("--phases", type=int, default=2, choices=(2, 4, 8, 16))
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--update-baseline", action="store_true")
    a = ap.parse_args()
    for exe in (TB_VVP, PZ80):
        if not os.path.exists(exe):
            sys.exit(f"missing {exe}: run `make event_sweep`")
    todo = [c for c in cases(a.groups.split(","), a.phases) if a.match in c["name"]]
    work = os.path.join(OUT, "work")
    diff_dir = os.path.join(OUT, "diff")
    for d in (work, diff_dir):
        os.makedirs(d, exist_ok=True)
    for f in os.listdir(diff_dir):
        os.remove(os.path.join(diff_dir, f))
    for c in todo:
        key = f"{c['target']}_{c['im']}{int(c['ei'])}{c['nmi_nops']}"
        path = os.path.join(work, f"prog_{key}.hex")
        if not os.path.exists(path):
            write_hex(program(TARGETS[c["target"]][0], c["im"], c["ei"], c["nmi_nops"]), path)

    pz_cases = {}
    for c in todo:
        pz_cases.setdefault(pz80_key(c), c)
    print(f"{len(todo)} cases ({len(pz_cases)} distinct for perfectz80), {a.jobs} jobs", flush=True)
    with ThreadPoolExecutor(a.jobs) as pool:
        pz_runs = {k: pool.submit(run_case, c, work, "pz80") for k, c in pz_cases.items()}
        rtl_runs = [(c, pool.submit(run_case, c, work, "rtl")) for c in todo]
        results = []
        for i, (c, fut) in enumerate(rtl_runs, 1):
            rtl, pz = fut.result(), pz_runs[pz80_key(c)].result()
            s = summarize(c, rtl, pz)
            if s:
                write_diff(c, rtl, pz, os.path.join(diff_dir, f"{c['name']}.txt"))
            results.append((c, s, facts(c, rtl), facts(c, pz)))
            if i % 250 == 0 or i == len(rtl_runs):
                print(f"  {i}/{len(rtl_runs)}", flush=True)

    # baseline.txt: `<case> <first differing half> <what differs> <RTL trace hash>`
    baseline = {}
    if os.path.exists(BASELINE):
        with open(BASELINE) as f:
            for line in f:
                if line.strip() and not line.startswith("#"):
                    name, key = line.split(None, 1)
                    baseline[name] = key.strip()
    ran = {c["name"] for c in todo}
    new = {c["name"] for c, s, *_ in results if s and c["name"] not in baseline}
    changed = {c["name"] for c, s, *_ in results if s and c["name"] in baseline and baseline[c["name"]] != s["key"]}
    fixed = {c["name"] for c, s, *_ in results if not s and c["name"] in baseline}
    report(results, baseline, new, changed, fixed, os.path.join(OUT, "report.md"))
    differing = sum(1 for _, s, *_ in results if s)
    print(f"{len(results) - differing} match, {differing} differ "
          f"({len(new)} new, {len(changed)} changed, {len(fixed)} known now match); "
          f"report: {os.path.relpath(os.path.join(OUT, 'report.md'), ROOT)}")

    if a.update_baseline:
        kept = {n: k for n, k in baseline.items() if n not in ran}
        kept.update({c["name"]: s["key"] for c, s, *_ in results if s})
        os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
        with open(BASELINE, "w") as f:
            f.write("# scripts/event_sweep.py --update-baseline: cases where the RTL differs from\n"
                    "# perfectz80. <case> <first differing half-cycle> <what> <RTL trace hash>\n")
            for n, k in sorted(kept.items()):
                f.write(f"{n} {k}\n")
        print(f"wrote {os.path.relpath(BASELINE, ROOT)} ({len(kept)} differing cases)")
        return 0
    for n in sorted(fixed):
        print(f"::warning::{n} matches perfectz80 now; trim it from the baseline (--update-baseline)")
    for n in sorted(new | changed)[:50]:
        print(f"::error::{n}: {'new' if n in new else 'changed'} difference from perfectz80 "
              f"(build/event_sweep/diff/{n}.txt)")
    return 1 if new or changed else 0


if __name__ == "__main__":
    sys.exit(main())
