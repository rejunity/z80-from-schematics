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
# and the address agree in every half-cycle -- transfers, refreshes, and the
# internal and idle T-states, where the bus keeps its last address -- and the
# data agrees wherever both drive WR. Differences are checked
# against tests/event_sweep/baseline.txt: a difference that is not in the
# baseline, or that changed, fails the run. Cases in the baseline that now
# match are reported as a warning so the baseline can be trimmed.
#
# Differences are expected (see docs/event-sweep.md); the baseline lists
# each differing case with a hash of the RTL's compared pins, so any change
# to the RTL's behaviour in one of those cases also fails until the baseline
# is regenerated with --update-baseline and the diff reviewed.
#
# --c-model also runs every case through the C model (scripts/event_sweep_c.c)
# and fails on any half-cycle where it differs from the RTL: the two are
# phase-identical, so the baseline covers both.
#
# Usage:
#   scripts/event_sweep.py [--groups int,nmi,...] [--match STR] [--phases N]
#                          [--jobs N] [--c-model] [--update-baseline]
# Output: build/event_sweep/report.md, one side-by-side trace per differing
# case in build/event_sweep/diff/.
import argparse, collections, hashlib, lzma, os, subprocess, sys
from concurrent.futures import Future, ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TB_VVP = os.path.join(ROOT, "build", "tb_event_sweep.vvp")
PZ80 = os.path.join(ROOT, "build", "bin", "event_sweep_pz80")
CMODEL = os.path.join(ROOT, "build", "bin", "event_sweep_c")
OUT = os.path.join(ROOT, "build", "event_sweep")
BASELINE = os.path.join(ROOT, "tests", "event_sweep", "baseline.txt")
# perfectz80's traces, recorded (--record): tests/event_sweep/pz80/<group>.txt.xz
CACHE = os.path.join(ROOT, "tests", "event_sweep", "pz80")

NMI, INT, WAIT, BUSREQ, RESET = 0, 1, 2, 3, 4
PIN_NAMES = {NMI: "nmi", INT: "int", WAIT: "wait", BUSREQ: "busreq", RESET: "reset"}

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
    "add_hl_bc": ([0x09], 11, "internal M-cycles of 4 + 3 T"),
    "adc_hl_bc": ([0xed, 0x4a], 15, "ED prefix, internal M-cycles of 4 + 3 T"),
    "ex_sp_hl": ([0xe3], 19, "read, read+1, write, write+2"),
    "djnz":     ([0x10, 0x02, 0x00, 0x00], 13, "taken (B=0 -> FF), M1 + 1, internal 5 T"),
    "rld":      ([0xed, 0x6f], 18, "read, internal 4 T, write"),
    "inc_bc":   ([0x03], 6, "M1 of 6 T"),
    "in_a_n":   ([0xdb, 0x10], 11, "I/O read"),
    "out_n_a":  ([0xd3, 0x10], 11, "I/O write"),
    "ini":      ([0xed, 0xa2], 16, "I/O read, memory write"),
    "ei_run":   ([0xfb, 0xfb, 0xfb], 12, "three EIs: INT waits for the instruction after the last"),
    "ei_di":    ([0xfb, 0xf3], 8, "EI then DI: INT never taken"),
    "dd_run":   ([0xdd, 0xdd, 0xdd, 0x21, 0x00, 0x82], 22, "DD DD + LD IX,8200: prefixes are not instructions"),
    "fd_dd":    ([0xfd, 0xdd, 0x21, 0x00, 0x82], 18, "FD then DD + LD IX,8200: the last prefix counts"),
    "dd_ed":    ([0xdd, 0xed, 0x44], 12, "DD before an ED opcode (NEG)"),
    "dd_nop":   ([0xdd, 0x00], 8, "DD before an opcode without an index form (NOP)"),
    "dd_cb":    ([0xdd, 0xcb, 0x05, 0x06], 23, "DD CB d op: RLC (IX+5)"),
}
# the INT sweep's targets; the NMI, BUSREQ and WAIT sweeps take all of TARGETS
INT_TARGETS = ["inc_a", "ld_a_hl", "ld_hl_a", "inc_hl", "halt", "ld_a_i", "ld_a_r", "add_hl_bc",
               "ex_sp_hl", "djnz", "in_a_n", "ei_run", "ei_di", "dd_run", "fd_dd", "dd_ed",
               "dd_nop", "dd_cb"]
# bytes on the bus during INTA in the IM2 sweep: even, odd, and FF (an idle bus)
IM2_VECS = (0x40, 0x41, 0xff)
# IM0: the instructions an interrupting device hands the CPU (its INTA byte,
# then one byte per INTA cycle or memory read until it has none left)
IM0_SEQS = {
    "rst38": [0xff], "rst00": [0xc7], "rst08": [0xcf], "call": [0xcd, 0x34, 0x12],
    "jp": [0xc3, 0x34, 0x12], "ld_a_n": [0x3e, 0x99], "jr": [0x18, 0x10], "inc_bc": [0x03],
    "halt": [0x76], "ei": [0xfb], "reti": [0xed, 0x4d], "ld_ix_nn": [0xdd, 0x21, 0x34, 0x12],
    "rlc_a": [0xcb, 0x07], "jp_ix": [0xdd, 0xe9], "ld_ixd_n": [0xdd, 0x36, 0x05, 0x99],
    "rlc_ixd": [0xdd, 0xcb, 0x05, 0x06], "ex_sp_hl": [0xe3], "ldir": [0xed, 0xb0],
    "push_bc": [0xc5], "ld_a_nn": [0x3a, 0x00, 0x80],
}
BASIC = ["inc_a", "ld_a_hl", "ld_hl_a", "inc_hl"]
GROUPS = ["int", "nmi", "nmi_short", "nmi_reentry", "reset", "reset_release",
          "reset_wait", "reset_short", "combo", "busreq", "wait", "im2", "im2_combo",
          "im0", "im0_combo", "opcodes", "classes", "classes_reset", "opcodes_reset"]
# run only when named (--groups): RESET at every T-state of every opcode
EXPLICIT = {"opcodes_reset"}

# ---- every opcode (the `opcodes` group) ------------------------------------
# Operands: d = 05 (IX+5 / IY+5), n = 99, nn = 0300 (zeros: a NOP run, safe
# to jump to, read and write), e = 02 (skips two NOPs).
D, N, NN, E = 0x05, 0x99, (0x00, 0x03), 0x02
UNPREFIXED_N = {0x06, 0x0e, 0x16, 0x1e, 0x26, 0x2e, 0x36, 0x3e, 0xc6, 0xce, 0xd6, 0xde,
                0xe6, 0xee, 0xf6, 0xfe, 0xd3, 0xdb}
UNPREFIXED_E = {0x10, 0x18, 0x20, 0x28, 0x30, 0x38}
UNPREFIXED_NN = {0x01, 0x11, 0x21, 0x31, 0x22, 0x2a, 0x32, 0x3a, 0xc3, 0xcd} | \
                {0xc2 + 8 * k for k in range(8)} | {0xc4 + 8 * k for k in range(8)}
ED_NN = {0x43, 0x4b, 0x53, 0x5b, 0x63, 0x6b, 0x73, 0x7b}
# opcodes whose (HL) operand becomes (IX+d) after DD / FD
INDEXED = {0x34, 0x35, 0x36, 0x46, 0x4e, 0x56, 0x5e, 0x66, 0x6e, 0x7e, 0x70, 0x71, 0x72,
           0x73, 0x74, 0x75, 0x77, 0x86, 0x8e, 0x96, 0x9e, 0xa6, 0xae, 0xb6, 0xbe}
CONDITIONAL = UNPREFIXED_E - {0x10, 0x18} | {0xc0 + 8 * k for k in range(8)} | \
              {0xc2 + 8 * k for k in range(8)} | {0xc4 + 8 * k for k in range(8)}
IO_REPEAT = {0xb2, 0xb3, 0xba, 0xbb}      # INIR OTIR INDR OTDR: B = 0 would run 256 times


def operands(op):
    """An unprefixed opcode's operand bytes."""
    if op in UNPREFIXED_N: return [N]
    if op in UNPREFIXED_E: return [E]
    if op in UNPREFIXED_NN: return list(NN)
    return []


# The die's timing classes that TARGETS doesn't cover: every opcode program
# grouped by perfectz80's control pins in its plain run and its BUSREQ and
# WAIT walks (the recorded `opcodes` traces; `--classes` prints the table),
# one program of each. The `classes` group sweeps each at every T-state.
CLASS_REPS = [   # (opcode program, T-states, class size): scripts/event_sweep.py classes
    ("01", 10, 32), ("10", 13, 1), ("10_b1", 8, 1), ("18", 12, 7), ("22", 16, 1), ("2a", 16, 1),
    ("32", 13, 1), ("36", 10, 1), ("3a", 13, 1), ("c0", 11, 12), ("c0_fff", 5, 12), ("c2", 10, 13),
    ("c4", 17, 13), ("c7", 11, 1), ("c9", 10, 1), ("cf", 11, 1), ("d3", 11, 1), ("d7", 11, 1),
    ("db", 11, 1), ("df", 11, 1), ("e7", 11, 1), ("e9", 4, 1), ("ef", 11, 1), ("f7", 11, 1),
    ("ff", 11, 1), ("cb46", 12, 8), ("ed40", 12, 8), ("ed41", 12, 8), ("ed43", 20, 6),
    ("ed45", 14, 10), ("ed4b", 20, 6), ("eda0", 16, 2), ("eda1", 16, 2), ("eda3", 16, 2),
    ("edb1", 21, 2), ("edb2", 21, 2), ("edb2_b2", 21, 2), ("edb3", 21, 2), ("edb3_b2", 21, 2),
    ("dd01", 14, 32), ("dd02", 11, 4), ("dd03", 10, 18), ("dd06", 11, 38), ("dd10", 17, 2),
    ("dd18", 16, 6), ("dd32", 17, 2), ("dd34", 23, 4), ("dd36", 19, 2), ("dd3a", 17, 2),
    ("dd46", 19, 30), ("dd76", 8, 2), ("ddc0", 15, 8), ("ddc2", 14, 10), ("ddc4", 21, 10),
    ("ddc5", 15, 8), ("ddc7", 15, 2), ("ddcf", 15, 2), ("ddd3", 15, 2), ("ddd7", 15, 2),
    ("dddb", 15, 2), ("dddf", 15, 2), ("dde3", 23, 2), ("dde7", 15, 2), ("dde9", 8, 2),
    ("ddef", 15, 2), ("ddf7", 15, 2), ("ddff", 15, 2), ("ddcb40", 20, 128),
]


def all_opcodes():
    """[(name, bytes, setup)] for every opcode: unprefixed, CB, ED, DD / FD
    and DD CB / FD CB; flag-dependent ones also with F = 00 and FF, DJNZ
    with B = 1, the repeating I/O block instructions with B = 2. setup:
    bytes the program runs before the marker."""
    out = []
    flags = lambda f: (0x01, f, 0x42, 0xc5, 0xf1, 0x01, 0x03, 0x00)  # LD BC,42ff; PUSH BC; POP AF; LD BC,3
    for op in range(256):
        if op in (0xcb, 0xdd, 0xed, 0xfd): continue
        code = [op] + operands(op)
        out.append((f"{op:02x}", code, ()))
        if op in CONDITIONAL:
            out += [(f"{op:02x}_f00", code, flags(0x00)), (f"{op:02x}_fff", code, flags(0xff))]
        if op == 0x10:
            out.append(("10_b1", code, (0x06, 0x01)))
    for op in range(256):
        out.append((f"cb{op:02x}", [0xcb, op], ()))
    for op in range(256):
        code = [0xed, op] + (list(NN) if op in ED_NN else [])
        out.append((f"ed{op:02x}", code, ()))
        if op in IO_REPEAT:
            out.append((f"ed{op:02x}_b2", code, (0x06, 0x02)))
    for p in (0xdd, 0xfd):
        for op in range(256):
            if op in (0xcb, 0xdd, 0xed, 0xfd): continue
            code = [p, op] + ([D] if op in INDEXED else []) + operands(op)
            out.append((f"{p:02x}{op:02x}", code, ()))
        for op in range(256):
            out.append((f"{p:02x}cb{op:02x}", [p, 0xcb, D, op], ()))
    return out


def program(target, im=1, ei=False, nmi_nops=0, setup=(), full_dump=False):
    """The sweep program as a 64K image (same layout as the hardware harness
    in hardesk's rp2350b_driver tests/event_sweep.cpp). setup: bytes run
    after the register setup, before [EI and] the jump to the marker.
    full_dump: the code at 0000 stores every register a RESET keeps --
    A and F first (PUSH AF), IX, IY and the alternates too -- for the cases
    with a RESET pulse."""
    ram = bytearray(0x10000)
    for i in range(0x100):
        ram[0x8000 + i] = (0x11 * (i + 1)) & 0xff

    def put(at, data):
        ram[at:at + len(data)] = bytes(data)
    # 0000: register dump (what a reset left), then on to the setup
    if full_dump:
        put(0x0000, [0xf5,                        # PUSH AF: A and F as the reset left them
                     0xed, 0x73, 0x00, 0x70,      # LD (7000),SP (after the push)
                     0xed, 0x43, 0x02, 0x70,      # LD (7002),BC
                     0xed, 0x53, 0x04, 0x70,      # LD (7004),DE
                     0x22, 0x06, 0x70,            # LD (7006),HL (I and R: RESET clears them)
                     0xdd, 0x22, 0x0a, 0x70,      # LD (700A),IX
                     0xfd, 0x22, 0x0c, 0x70,      # LD (700C),IY
                     0x08, 0xf5, 0x08,            # EX AF,AF'; PUSH AF; EX AF,AF'
                     0xd9,                        # EXX
                     0xed, 0x43, 0x10, 0x70,      # LD (7010),BC'
                     0xed, 0x53, 0x12, 0x70,      # LD (7012),DE'
                     0x22, 0x14, 0x70,            # LD (7014),HL'
                     0xd9, 0xc3, 0x00, 0x01])     # EXX; JP 0100
    else:
      put(0x0000, [0xed, 0x73, 0x00, 0x70,        # LD (7000),SP
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
                + list(setup) + ([0xfb, 0x00] if ei else []) + [0xc3, 0x00, 0x02])
    # 0200: marker NOP, the target, 24 NOPs, HALT
    put(0x0200, [0x00] + list(target) + [0x00] * 24 + [0x76])
    put(0x0400, [0xc9])                           # CALL target: RET
    for i in range(0, 256, 2):                    # IM2 table at I=3F
        ram[0x3f00 + i], ram[0x3f01 + i] = 0x38, 0x00
    # an odd vector reads 0038's bytes the other way round, 3800, and FF
    # reads 3FFF (00) and 4000: 3800 too, where a JP leads to the handler
    ram[0x4000] = 0x38
    put(0x3800, [0xc3, 0x38, 0x00])               # JP 0038
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
    """pulses: [(pin, lo, hi)] in sixteenths, hi=-1 never released. target:
    a TARGETS name, or any name with code= (the bytes under test). A case
    with a RESET pulse gets the full register dump at 0000."""
    length = TARGETS[target][1] if target in TARGETS else 0
    c = {"name": name, "group": group, "target": target, "pulses": pulses,
         "run": run or (140 if target == "ldir" else 60 + length),
         "code": tuple(TARGETS[target][0]) if target in TARGETS else (), "setup": (),
         "im": 1, "ei": False, "nmi_nops": 0, "inta_release": False,
         "int_at": None, "hold": None, "vec": 0xff, "ops": (), "meta": {},
         "busreq_walk": False, "wait_walk": False}
    c.update(kw)
    c["code"], c["setup"] = tuple(c["code"]), tuple(c["setup"])
    c["full_dump"] = any(p == RESET for p, _, _ in pulses)
    return c


def prog_id(c):
    """What the program image depends on."""
    return (c["code"], c["setup"], c["im"], c["ei"], c["nmi_nops"], c["full_dump"])


def cases(groups, phases):
    """phases: positions per T-state for 1 T and longer pulses (16 = every
    1/16 T as on hardware; 2 = one in each half, which is all a zero-delay
    simulation can tell apart). Sub-T pulses always sweep all 16."""
    step = 16 // phases
    ph = range(step // 2, 16, step)
    out = []
    want = lambda g: ("all" in groups and g not in EXPLICIT) or g in groups
    if want("int"):          # IM1, EI, INT held until INTA
        for t in INT_TARGETS:
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
        for t in BASIC + ["halt", "push_bc", "add_hl_bc", "inc_bc", "in_a_n", "out_n_a"]:
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
        for n in range(0, 6):      # INT / NMI / HALT+INT with BUSREQ at the same boundary
            for j in ph:
                out.append(case(f"combo_int_busreq_T{n}_p{j:02d}", "combo", "inc_a",
                                [(INT, at(2, 4), -1), (BUSREQ, at(n, j), at(n, j) + 32)],
                                ei=True, inta_release=True, meta={"kind": "int_busreq"}))
                out.append(case(f"combo_nmi_busreq_T{n}_p{j:02d}", "combo", "inc_a",
                                [(NMI, at(2, 4), at(3, 4)), (BUSREQ, at(n, j), at(n, j) + 32)],
                                meta={"kind": "nmi_busreq"}))
                out.append(case(f"combo_halt_busreq_T{n}_p{j:02d}", "combo", "halt",
                                [(INT, at(6, 4), -1), (BUSREQ, at(n + 4, j), at(n + 4, j) + 32)],
                                ei=True, inta_release=True, meta={"kind": "halt_int_busreq"}))
        for j in range(0, 16, 4):  # INT held across RESET: IFF clear until EI
            out.append(case(f"combo_int_held_reset_p{j:02d}", "combo", "inc_a",
                            [(INT, at(1, j), -1), (RESET, at(1, 8), at(5, 8))], run=300,
                            ei=True, inta_release=True, meta={"kind": "int_held_across_reset"}))
    if want("busreq"):       # BUSREQ low 1 T and 4 T from every T-state: grant, BUSACK, resume
        for t in TARGETS:
            for n in range(TARGETS[t][1] + 2):
                for j in ph:
                    for w in (1, 4):
                        out.append(case(f"busreq{w}_{t}_T{n}_p{j:02d}", "busreq", t,
                                        [(BUSREQ, at(n, j), at(n, j) + 16 * w)],
                                        meta={"n": n, "phase": j, "width": w}))
    if want("wait"):         # WAIT low 1 T and 3 T from every T-state
        for t in TARGETS:
            for n in range(TARGETS[t][1] + 2):
                for j in ph:
                    for w in (1, 3):
                        out.append(case(f"wait{w}_{t}_T{n}_p{j:02d}", "wait", t,
                                        [(WAIT, at(n, j), at(n, j) + 16 * w)],
                                        meta={"n": n, "phase": j, "width": w}))
    if want("im2"):          # IM2, EI, INT held until INTA, with an even, odd and FF vector
        for v in IM2_VECS:
            for t in BASIC + ["halt"]:
                for n in range(TARGETS[t][1] + 2):
                    for j in ph:
                        out.append(case(f"im2_{v:02x}_{t}_T{n}_p{j:02d}", "im2", t, [(INT, at(n, j), -1)],
                                        im=2, ei=True, inta_release=True, vec=v,
                                        meta={"n": n, "phase": j, "vec": v}))
    if want("im2_combo"):    # INT taken after INC A in IM2 (INTA at T5, pushes at T12 and T15,
        # table reads at T18 and T21, the handler from T24); a second pin from every T-state
        for n in range(4, 29):
            for j in ph:
                for name, pin, w in (("nmi", NMI, 16), ("wait1", WAIT, 16), ("wait3", WAIT, 48),
                                     ("busreq1", BUSREQ, 16), ("busreq4", BUSREQ, 64),
                                     ("reset", RESET, 64)):
                    out.append(case(f"im2c_{name}_T{n}_p{j:02d}", "im2_combo", "inc_a",
                                    [(INT, at(2, 4), -1), (pin, at(n, j), at(n, j) + w)],
                                    run=300 if pin == RESET else None, im=2, ei=True,
                                    inta_release=True, vec=0x40,
                                    meta={"kind": name, "n": n, "phase": j}))
    if want("im0"):          # IM0, EI, INT taken after INC A; the device hands over an instruction
        for name, seq in IM0_SEQS.items():
            for j in (4, 12):
                out.append(case(f"im0_{name}_p{j:02d}", "im0", "inc_a", [(INT, at(2, j), -1)],
                                run=160, im=0, ei=True, inta_release=True, vec=seq[0],
                                ops=tuple(seq[1:]), meta={"seq": name, "phase": j}))
    if want("im0_combo"):    # IM0 CALL 1234 after INC A (INTA at T5, operand reads at T11 and
        # T14, pushes at T18 and T21, the fetch at 1234 from T24); a second pin from every T-state
        for n in range(4, 29):
            for j in ph:
                for name, pin, w in (("nmi", NMI, 16), ("wait1", WAIT, 16), ("wait3", WAIT, 48),
                                     ("busreq1", BUSREQ, 16), ("busreq4", BUSREQ, 64),
                                     ("reset", RESET, 64)):
                    out.append(case(f"im0c_{name}_T{n}_p{j:02d}", "im0_combo", "inc_a",
                                    [(INT, at(2, 4), -1), (pin, at(n, j), at(n, j) + w)],
                                    run=300 if pin == RESET else None, im=0, ei=True,
                                    inta_release=True, vec=0xcd, ops=(0x34, 0x12),
                                    meta={"kind": name, "n": n, "phase": j}))
    if want("classes") or want("classes_reset"):  # one opcode of each timing class TARGETS doesn't cover
        ops = {n: (code, setup) for n, code, setup in all_opcodes()}
        for rep, length, _ in CLASS_REPS:
            code, setup = ops[rep]
            kw = {"code": code, "setup": setup}
            for n in range(length + 2):
                for j in ph:
                    m = {"n": n, "phase": j, "class": rep}
                    if want("classes_reset"):
                        out.append(case(f"cls_{rep}_reset_T{n}_p{j:02d}", "classes_reset", rep,
                                        [(RESET, at(n, j), at(n, j) + 64)], run=300, ei=True,
                                        meta=dict(m, kind="reset"), **kw))
                    if not want("classes"):
                        continue
                    out += [case(f"cls_{rep}_nmi_T{n}_p{j:02d}", "classes", rep,
                                 [(NMI, at(n, j), at(n, j) + 16)], run=60 + length, meta=dict(m, kind="nmi"), **kw),
                            case(f"cls_{rep}_int_T{n}_p{j:02d}", "classes", rep, [(INT, at(n, j), -1)],
                                 run=60 + length, ei=True, inta_release=True, meta=dict(m, kind="int"), **kw)]
                    for w in (1, 4):
                        out.append(case(f"cls_{rep}_busreq{w}_T{n}_p{j:02d}", "classes", rep,
                                        [(BUSREQ, at(n, j), at(n, j) + 16 * w)], run=60 + length,
                                        meta=dict(m, kind=f"busreq{w}"), **kw))
                    for w in (1, 3):
                        out.append(case(f"cls_{rep}_wait{w}_T{n}_p{j:02d}", "classes", rep,
                                        [(WAIT, at(n, j), at(n, j) + 16 * w)], run=60 + length,
                                        meta=dict(m, kind=f"wait{w}"), **kw))
    if want("opcodes_reset"):  # RESET at every T-state of every opcode, one position per T-state
        lengths = opcode_lengths()
        for name, code, setup in all_opcodes():
            for n in range(lengths[name] + 2):
                out.append(case(f"opr_{name}_T{n}", "opcodes_reset", name, [(RESET, at(n, 4), at(n, 4) + 64)],
                                code=code, setup=setup, run=300, meta={"n": n, "phase": 4}))
    if want("opcodes"):      # every opcode: plain, BUSREQ and WAIT walked through it, INT, NMI
        for name, code, setup in all_opcodes():
            kw = {"code": code, "setup": setup, "run": 120}
            out += [case(f"op_{name}_plain", "opcodes", name, [], meta={"kind": "plain"}, **kw),
                    case(f"op_{name}_busreq", "opcodes", name, [(BUSREQ, 0, -1)], busreq_walk=True,
                         meta={"kind": "busreq walk"}, **dict(kw, run=200)),
                    case(f"op_{name}_wait", "opcodes", name, [], wait_walk=True,
                         meta={"kind": "wait walk"}, **kw),
                    case(f"op_{name}_int", "opcodes", name, [(INT, at(0, 4), -1)], ei=True,
                         inta_release=True, meta={"kind": "int"}, **kw),
                    case(f"op_{name}_nmi", "opcodes", name, [(NMI, at(1, 4), at(2, 4))],
                         meta={"kind": "nmi"}, **kw)]
    return out


def pz80_key(c):
    """perfectz80 resolves whole half-cycles, so cases whose transitions fall
    in the same half-cycles (and keep the same order inside one) give the same
    result: run each such set once."""
    def half(s):
        return None if s < 0 else s // 8
    pulses = tuple(sorted((p, half(lo), half(hi), lo // 8 == hi // 8 and hi >= 0)
                          for p, lo, hi in c["pulses"]))
    return (prog_id(c), c["inta_release"], c["int_at"], c["hold"], c["run"], c["vec"], c["ops"],
            c["busreq_walk"], c["wait_walk"], pulses)


def harness_id():
    """perfectz80 (its netlist and simulator) and the sweep's harness: a
    recorded trace holds while these are unchanged."""
    h = hashlib.sha1()
    refs = os.path.join(ROOT, "scripts", "refs", "perfectz80")
    files = [os.path.join(ROOT, "scripts", f) for f in ("event_sweep_pz80.c", "pz80_pads.c", "pz80_pads.h")]
    files += sorted(os.path.join(refs, f) for f in os.listdir(refs) if f.endswith((".c", ".h")))
    for f in files:
        h.update(open(f, "rb").read())
    return h.hexdigest()[:16]


def cache_key(c, hid):
    """A perfectz80 run by what it depends on: the harness, the program's
    bytes, the options and the pulses as perfectz80 resolves them."""
    return hashlib.sha1(repr((hid, c["prog_sha"]) + pz80_key(c)[1:]).encode()).hexdigest()[:20]


def load_cache():
    """{key: perfectz80's output} from every recorded group"""
    out = {}
    if not os.path.isdir(CACHE):
        return out
    for f in sorted(os.listdir(CACHE)):
        if not f.endswith(".txt.xz"):
            continue
        key, lines = None, []
        for line in lzma.open(os.path.join(CACHE, f), "rt"):
            if line.startswith("@"):
                if key: out[key] = "".join(lines)
                key, lines = line[1:].strip(), []
            else:
                lines.append(line)
        if key: out[key] = "".join(lines)
    return out


def save_cache(group, texts):
    """texts: {key: perfectz80's output} of one group's cases"""
    os.makedirs(CACHE, exist_ok=True)
    with lzma.open(os.path.join(CACHE, f"{group}.txt.xz"), "wt", preset=9 | lzma.PRESET_EXTREME) as f:
        for k in sorted(texts):
            f.write(f"@{k}\n{texts[k]}")


def done(value):
    f = Future()
    f.set_result(value)
    return f


def run_pz80(c, workdir):
    """(rows, output) of perfectz80"""
    out = run_case(c, workdir, "pz80", raw=True)
    return parse_rows(out, f"pz80 {c['name']}"), out


def run_case(c, workdir, side, raw=False):
    prog = c["prog"]
    ev = os.path.join(workdir, f"{c['name']}.{side}.events")    # the sides run in parallel
    with open(ev, "w") as f:
        for p, lo, hi in c["pulses"]:
            f.write(f"{p} {lo} {hi}\n")
    if side == "rtl":
        cmd = ["vvp", "-n", TB_VVP, f"+prog={prog}", f"+events={ev}", f"+run={c['run']}"]
        if c["inta_release"]: cmd.append("+inta_release=1")
        if c["int_at"] is not None: cmd.append(f"+int_at={c['int_at']:04x}")
        if c["hold"]: cmd += [f"+hold={c['hold'][0]:04x}", f"+hold_t={c['hold'][1]}"]
        if c["vec"] != 0xff: cmd.append(f"+inta_vec={c['vec']:02x}")
        if c["ops"]:
            cmd += [f"+inta_n={len(c['ops'])}",
                    "+inta_ops=" + "".join(f"{b:02x}" for b in reversed(c["ops"]))]
        if c["busreq_walk"]: cmd.append("+busreq_walk=1")
        if c["wait_walk"]: cmd.append("+wait_walk=1")
    else:                    # perfectz80, or the C model: the same options
        cmd = [PZ80 if side == "pz80" else CMODEL, "-e", ev, "-r", str(c["run"])]
        if c["inta_release"]: cmd.append("-i")
        if c["int_at"] is not None: cmd += ["-a", f"{c['int_at']:04x}"]
        if c["hold"]: cmd += ["-H", f"{c['hold'][0]:04x}", "-T", str(c["hold"][1])]
        if c["vec"] != 0xff: cmd += ["-v", f"{c['vec']:02x}"]
        if c["ops"]: cmd += ["-o", "".join(f"{b:02x}" for b in c["ops"])]
        if c["busreq_walk"]: cmd.append("-B")
        if c["wait_walk"]: cmd.append("-W")
        cmd.append(prog)
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    return out if raw else parse_rows(out, f"{side} {c['name']}")


def parse_rows(out, what):
    rows = []
    for line in out.splitlines():
        v = line.split()
        if len(v) == 15 and v[0].isdigit():
            rows.append([int(v[0]), int(v[1]), int(v[2], 16), int(v[3], 16)] +
                        [int(x) for x in v[4:12]] + [int(x, 16) for x in v[12:]])
    if not rows:
        raise RuntimeError(f"{what}: {out.strip()[-200:]}")
    return rows


# row: h clk addr data mreq iorq rd wr m1 rfsh halt busack aen den cen
H, CLK, ADDR, DATA, MREQ, IORQ, RD, WR, M1, RFSH, HALT, BUSACK, AEN, DEN, CEN = range(15)
CONTROL = [("mreq", MREQ), ("iorq", IORQ), ("rd", RD), ("wr", WR), ("m1", M1),
           ("rfsh", RFSH), ("halt", HALT), ("busack", BUSACK)]
# the pads driven: address, data, MREQ IORQ RD WR M1 RFSH HALT BUSACK
ENABLES = [("addr_en", AEN), ("data_en", DEN), ("ctl_en", CEN)]


def addressed(r):
    """The address is compared in every half-cycle: transfers, refreshes,
    and the internal and idle T-states, where the bus keeps its last
    address."""
    return True


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
        what += [n for n, i in ENABLES if ra[i] != rb[i]]
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
        h.update(bytes([r[AEN] & 0xff, r[AEN] >> 8, r[DEN], r[CEN]]))
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
    names = "h clk addr data mreq iorq rd wr m1 rfsh halt busack aen den cen"
    with open(path, "w") as f:
        f.write(f"# {c['name']}: pulses {c['pulses']} (pin lo hi in 1/16 T from the marker's T1)\n")
        f.write(f"# {'RZ80 (iverilog RTL)':<56} | perfectz80\n# {names:<56} | {names}\n")
        for i in range(max(len(rtl), len(pz))):
            fmt = lambda r: (f"{r[H]} {r[CLK]} {r[ADDR]:04x} {r[DATA]:02x} " +
                             " ".join(str(v) for v in r[MREQ:AEN]) +
                             f" {r[AEN]:04x} {r[DEN]:02x} {r[CEN]:02x}") if r else ""
            a = rtl[i] if i < len(rtl) else None
            b = pz[i] if i < len(pz) else None
            mark = " " if a and b and fmt(a).split()[4:] == fmt(b).split()[4:] else "*"
            f.write(f"{mark} {fmt(a):<56} | {fmt(b)}\n")


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
    if full.get("im2"):
        out.append(f"\n## INT in IM2 (EI, held until INTA; I = 3F)\n\n{GRID_LEGEND}\n")
        for v in IM2_VECS:
            for t in TARGETS:
                rows = [r for r in full["im2"] if r[0]["target"] == t and r[0]["vec"] == v]
                if rows:
                    out += [f"### vector {v:02X}, `{t}` ({TARGETS[t][1]} T)\n"] + grid(rows) + [""]
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


def opcode_lengths():
    """{program: T-states} of the `opcodes` group's programs, from their
    recorded plain traces: from the first opcode fetch to the next
    instruction's"""
    cache, hid = load_cache(), harness_id()
    out = {}
    for c in cases(["opcodes"], 2):
        if c["meta"]["kind"] != "plain":
            continue
        c["prog_sha"] = hashlib.sha1(program(c["code"], c["im"], c["ei"], c["nmi_nops"], c["setup"],
                                             c["full_dump"])).hexdigest()[:16]
        text = cache.get(cache_key(c, hid))
        if text is None:
            sys.exit(f"{c['name']}: no recorded trace (run --groups opcodes --record first)")
        rows = parse_rows(text, c["name"])
        fetch = [r[H] for i, r in enumerate(rows) if r[H] >= 8 and not r[M1] and r[CLK] and r[MREQ]
                 and r[IORQ] and rows[i - 1][M1]]
        k = 2 if c["code"][0] in (0xcb, 0xed, 0xdd, 0xfd) else 1
        out[c["target"]] = (fetch[k] - fetch[0]) // 2 if len(fetch) > k else 24
    return out


def timing_classes(cache, hid):
    """Group the `opcodes` group's programs by perfectz80's control pins
    (plain run, BUSREQ walk, WAIT walk) from the recorded traces: prints
    each class, its size, its first program and its T-states (from the
    first opcode fetch to the fetch after the instruction), and which
    TARGETS cover it -- where CLASS_REPS comes from."""
    todo = cases(["opcodes"], 2)
    shas = {}
    for c in todo:
        pid = prog_id(c)
        if pid not in shas:
            shas[pid] = hashlib.sha1(program(c["code"], c["im"], c["ei"], c["nmi_nops"], c["setup"],
                                             c["full_dump"])).hexdigest()[:16]
        c["prog_sha"] = shas[pid]
    pins = lambda rows: tuple((r[M1], r[MREQ], r[IORQ], r[RD], r[WR], r[RFSH], r[HALT], r[BUSACK]) for r in rows)
    runs = collections.defaultdict(dict)
    for c in todo:
        text = cache.get(cache_key(c, hid))
        if text is None:
            sys.exit(f"{c['name']}: no recorded trace (run --groups opcodes --record first)")
        runs[c["target"]][c["meta"]["kind"]] = parse_rows(text, c["name"])
    classes = collections.OrderedDict()
    for name, k in runs.items():
        classes.setdefault((pins(k["plain"]), pins(k["busreq walk"]), pins(k["wait walk"])), []).append(name)
    codes = {n: tuple(code) for n, code, _ in all_opcodes()}
    known = {tuple(v[0]): k for k, v in TARGETS.items()}
    print(f"{len(runs)} programs, {len(classes)} timing classes")
    for names in classes.values():
        rows = runs[names[0]]["plain"]
        fetch = [r[H] for i, r in enumerate(rows) if r[H] >= 8 and not r[M1] and r[CLK] and r[MREQ]
                 and r[IORQ] and rows[i - 1][M1]]       # T1 of each opcode fetch after the marker's
        k = 2 if codes[names[0]][0] in (0xcb, 0xed, 0xdd, 0xfd) else 1
        length = (fetch[k] - fetch[0]) // 2 if len(fetch) > k else None
        cover = sorted({known[codes[n]] for n in names if codes[n] in known})
        print(f"{names[0]:10s} {length} T  {len(names):4d} programs  covered by {cover or '-'}: "
              f"{' '.join(names[:10])}{' ...' if len(names) > 10 else ''}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--groups", default="all")
    ap.add_argument("--match", default="")
    ap.add_argument("--phases", type=int, default=2, choices=(2, 4, 8, 16))
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--c-model", action="store_true",
                    help="also run the C model and require it to match the RTL in every half-cycle")
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--record", action="store_true",
                    help="write perfectz80's traces of the groups run to tests/event_sweep/pz80/")
    ap.add_argument("--no-cache", action="store_true", help="run perfectz80 for every case")
    ap.add_argument("--shard", default="", metavar="K/N",
                    help="run every Nth case from the Kth (0-based): CI splits the sweep over N jobs")
    ap.add_argument("--classes", action="store_true",
                    help="print the opcodes' timing classes from the recorded traces and exit")
    a = ap.parse_args()
    if a.classes:
        timing_classes(load_cache(), harness_id())
        return 0
    for exe in (TB_VVP, PZ80) + ((CMODEL,) if a.c_model else ()):
        if not os.path.exists(exe):
            sys.exit(f"missing {exe}: run `make event_sweep`")
    k, n = (int(x) for x in a.shard.split("/")) if a.shard else (0, 1)
    todo = [c for i, c in enumerate(cases(a.groups.split(","), a.phases))
            if a.match in c["name"] and i % n == k]
    if a.shard and a.update_baseline:
        sys.exit("--update-baseline needs every case: run it without --shard")
    work = os.path.join(OUT, "work")
    diff_dir = os.path.join(OUT, "diff")
    for d in (work, diff_dir):
        os.makedirs(d, exist_ok=True)
    for f in os.listdir(diff_dir):
        os.remove(os.path.join(diff_dir, f))
    progs = {}
    for c in todo:             # one program file per distinct image, named by its hash
        pid = prog_id(c)
        if pid not in progs:
            ram = program(c["code"], c["im"], c["ei"], c["nmi_nops"], c["setup"], c["full_dump"])
            sha = hashlib.sha1(ram).hexdigest()[:16]
            path = os.path.join(work, f"prog_{sha}.hex")
            if not os.path.exists(path):
                write_hex(ram, path)
            progs[pid] = (path, sha)
        c["prog"], c["prog_sha"] = progs[pid]

    pz_cases = {}
    for c in todo:
        pz_cases.setdefault(pz80_key(c), c)
    hid = harness_id()
    cache = {} if a.no_cache else load_cache()
    hits = sum(1 for c in pz_cases.values() if cache_key(c, hid) in cache)
    print(f"{len(todo)} cases ({len(pz_cases)} distinct for perfectz80: {hits} recorded, "
          f"{len(pz_cases) - hits} to run), {a.jobs} jobs", flush=True)
    with ThreadPoolExecutor(a.jobs) as pool:
        pz_runs = {}
        for k, c in pz_cases.items():
            text = cache.get(cache_key(c, hid))
            pz_runs[k] = (done((parse_rows(text, f"recorded pz80 {c['name']}"), text)) if text is not None
                          else pool.submit(run_pz80, c, work))
        rtl_runs = [(c, pool.submit(run_case, c, work, "rtl"),
                     pool.submit(run_case, c, work, "c") if a.c_model else None) for c in todo]
        results, c_differs = [], []
        for i, (c, fut, cfut) in enumerate(rtl_runs, 1):
            rtl, pz = fut.result(), pz_runs[pz80_key(c)].result()[0]
            s = summarize(c, rtl, pz)
            if s:
                write_diff(c, rtl, pz, os.path.join(diff_dir, f"{c['name']}.txt"))
            if cfut:
                cm = cfut.result()
                d = first_difference(cm, rtl)
                if d:
                    c_differs.append((c["name"], d))
                    write_diff(c, cm, rtl, os.path.join(diff_dir, f"{c['name']}.c_vs_rtl.txt"))
            results.append((c, s, facts(c, rtl), facts(c, pz)))
            if i % 250 == 0 or i == len(rtl_runs):
                print(f"  {i}/{len(rtl_runs)}", flush=True)

    if a.record:               # perfectz80's traces of the groups run, for later runs
        texts = collections.defaultdict(dict)
        for c in todo:
            texts[c["group"]][cache_key(c, hid)] = pz_runs[pz80_key(c)].result()[1]
        for g, t in texts.items():
            save_cache(g, t)
        print(f"recorded perfectz80's traces of {', '.join(sorted(texts))} in {os.path.relpath(CACHE, ROOT)}/")

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
    if a.c_model:
        print(f"C model: {len(results) - len(c_differs)} cases identical to the RTL, {len(c_differs)} differ")
        for n, (half, what) in sorted(c_differs)[:50]:
            print(f"::error::{n}: the C model differs from the RTL at half {half} ({', '.join(what)}; "
                  f"build/event_sweep/diff/{n}.c_vs_rtl.txt, C left)")

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
    return 1 if new or changed or c_differs else 0


if __name__ == "__main__":
    sys.exit(main())
