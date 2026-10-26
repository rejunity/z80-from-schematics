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
# internal and idle T-states, where the bus keeps its last address -- the
# data agrees on every pad both drive, the same pads are driven, and F, WZ
# and Q agree in T4.P of every M1 (state_samples). Any case that differs
# fails the run: there is no list of tolerated differences.
#
# --c-model also runs every case through the C model (scripts/event_sweep_c.c)
# and fails on any half-cycle where it differs from the RTL, F, WZ and Q
# included: the two are phase-identical.
#
# The `latch` group drives the data bus itself: in one read -- an opcode
# fetch, a memory or I/O read, an INTA -- the right byte is on the bus in
# one half-cycle only and a decoy in every other, so each case shows
# whether the byte a model takes is the one the die takes (docs/timing.md
# "The data latch").
#
# perfectz80's side comes from its recorded traces in tests/event_sweep/pz80/
# (one file per group, MANIFEST.txt naming the harness they were recorded
# with) and runs live only for a case they don't have; --recorded-only never
# runs it and fails on such a case, so the RTL can be checked against the
# die with iverilog and Python alone. --pz80-only skips the RTL and the C
# model: with --verify-recorded it checks recordings against perfectz80
# live, with --record (and --no-cache) it records groups again.
#
# Usage:
#   scripts/event_sweep.py [--groups int,nmi,...] [--match STR] [--phases N]
#                          [--jobs N] [--c-model] [--recorded-only]
#                          [--record] [--verify-recorded N|all] [--pz80-only]
# Output: build/event_sweep/report.md, one side-by-side trace per differing
# case in build/event_sweep/diff/.
#
# EVENT_SWEEP_TB_VVP (the RTL testbench build) and EVENT_SWEEP_PZ80 (the
# recordings' directory) in the environment point elsewhere, for a
# repository that carries copies of them.
import argparse, collections, hashlib, lzma, os, random, re, subprocess, sys
from concurrent.futures import Future, ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TB_VVP = os.environ.get("EVENT_SWEEP_TB_VVP", os.path.join(ROOT, "build", "tb_event_sweep.vvp"))
PZ80 = os.path.join(ROOT, "build", "bin", "event_sweep_pz80")
CMODEL = os.path.join(ROOT, "build", "bin", "event_sweep_c")
OUT = os.path.join(ROOT, "build", "event_sweep")
# perfectz80's traces, recorded (--record): tests/event_sweep/pz80/<group>.txt.xz,
# and MANIFEST.txt: the harness they were recorded with, and each file's cases
CACHE = os.environ.get("EVENT_SWEEP_PZ80", os.path.join(ROOT, "tests", "event_sweep", "pz80"))
MANIFEST = os.path.join(CACHE, "MANIFEST.txt")

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
          "im0", "im0_combo", "opcodes", "classes", "classes_reset", "opcodes_reset", "fuzz",
          "fuzz_reset", "reset_m1wait", "reset_busreq", "reset_nmi", "latch",
          "q_ops", "q_seq", "q_events", "q_reset", "wz_int", "blk_events"]
# run only when named (--groups): RESET at every T-state of every opcode
EXPLICIT = {"opcodes_reset"}
FUZZ_N, FUZZ_SEED = 2000, 1     # the recorded fuzz cases; --fuzz-seed / --fuzz-n for others

# ---- the data latch (the `latch` group) -------------------------------------
# One read of each kind, at a fixed T-state of a short program: its T1.P
# half-cycle (from the marker's T1), the right byte and a decoy that changes
# what the program does -- another opcode, another stored value, another IM2
# table entry. Each read is swept with the right byte in one half-cycle from
# its T1.P to its T3.N and the decoy in the others, without and with one Tw
# (WAIT low across the edge that samples it). The store after each load
# shows the byte taken; an opcode shows in the cycles that follow.
STORE_A = [0x32, 0x10, 0x70]                            # LD (7010),A
LATCH_READS = [   # (name, kind, code, T1.P half, right byte, decoy, what)
    ("m1", "m1", [0x7e] + STORE_A, 8, 0x7e, 0x81, "opcode fetch: LD A,(HL) / ADD A,C"),
    ("m1_dd", "m1", [0xdd, 0x7e, 0x05] + STORE_A, 8, 0xdd, 0x22, "prefix fetch: DD / LD (nn),HL"),
    ("m1_dd_op", "m1", [0xdd, 0x7e, 0x05] + STORE_A, 16, 0x7e, 0x81, "the opcode fetch after DD"),
    ("mrd_hl", "mrd", [0x7e] + STORE_A, 16, 0x11, 0xee, "memory read: LD A,(HL)"),
    ("mrd_n", "mrd", [0x3e, 0x5a] + STORE_A, 16, 0x5a, 0xa5, "operand read: LD A,n"),
    ("mrd_pop_lo", "mrd", [0xc1, 0xed, 0x43, 0x10, 0x70], 16, 0x00, 0xff, "POP BC, low byte"),
    ("mrd_pop_hi", "mrd", [0xc1, 0xed, 0x43, 0x10, 0x70], 22, 0x00, 0xff, "POP BC, high byte"),
    ("mrd_ixd", "mrd", [0xdd, 0x7e, 0x05] + STORE_A, 24, 0x05, 0xfa, "displacement: LD A,(IX+d)"),
    ("mrd_ddcb_d", "mrd", [0xdd, 0xcb, 0x05, 0x06], 24, 0x05, 0xfa, "DD CB d op: d"),
    ("mrd_ddcb_op", "mrd", [0xdd, 0xcb, 0x05, 0x06], 30, 0x06, 0xf9, "DD CB d op: op, a read, not a fetch"),
    ("iord_n", "iord", [0xdb, 0x10] + STORE_A, 22, 0xff, 0x00, "I/O read: IN A,(n)"),
    ("iord_c", "iord", [0xed, 0x50, 0xed, 0x53, 0x10, 0x70], 24, 0xff, 0x00, "I/O read: IN D,(C)"),
    ("iord_ini", "iord", [0xed, 0xa2], 26, 0xff, 0x00, "I/O read: INI (stored at HL)"),
    ("inta_im2", "inta", [0x3c], 16, 0x40, 0xbf, "INTA, IM2 vector: table at 3F40 / 3FBF"),
    ("inta_im0", "inta", [0x3c], 16, 0xff, 0x00, "INTA, IM0 opcode: RST 38 / NOP"),
]
# per kind: the T-states of the read, T1 to T3 (Twa: an automatic wait
# state, the datasheet's TW*), and the one whose falling edge samples WAIT:
# the one Tw a case adds follows it
LATCH_HALVES = {"m1": ["T1", "T2", "T3"], "mrd": ["T1", "T2", "T3"],
                "iord": ["T1", "T2", "Twa", "T3"], "inta": ["T1", "T2", "Twa1", "Twa2", "T3"]}
LATCH_WAIT_AT = {"m1": 1, "mrd": 1, "iord": 2, "inta": 3}   # index of that T-state

# ---- Q, F and WZ (the q_* groups, wz_int, blk_events) ----------------------
# Every case compares F, WZ and Q with the die's where state_samples takes
# them; these groups aim at them. A = 42 and F = A9 (S Y X C) before the
# instruction under test, so an SCF or CCF after it shows Q: X and Y of
# F | A when Q = 0, of A alone when Q = F.
Q_AF = (0xc5, 0x01, 0xa9, 0x42, 0xc5, 0xf1, 0xc1)   # PUSH BC; LD BC,42A9; PUSH BC; POP AF; POP BC
# flag writers: CP 28 (X Y from the operand), INC B (27 -> 28), BIT 3,B,
# CPI, ADD HL,BC, SCF, LD A,I
Q_FLAG_OPS = {"cp": [0xfe, 0x28], "incb": [0x04], "bit3b": [0xcb, 0x58], "cpi": [0xed, 0xa1],
              "addhl": [0x09], "scf": [0x37], "ldai": [0xed, 0x57]}
Q_FLAG_SETUP = (0x06, 0x27)                          # LD B,27 (INC B -> 28; BIT 3,B: Z clear)
# what can stand between a flag writer and SCF / CCF: instructions that
# write no flags (Q = 0 after them on the die), prefixes, and controls
# that do (OR A)
Q_BETWEEN = {"nop": [0x00], "exaf": [0x08], "exx": [0xd9], "exdehl": [0xeb], "popaf": [0xf1],
             "pushbc": [0xc5], "ldbn": [0x06, 0x28], "ldab": [0x78], "ldhlb": [0x70],
             "ldbhl": [0x46], "incbc": [0x03], "di": [0xf3], "ei": [0xfb], "im1": [0xed, 0x56],
             "ldia": [0xed, 0x47], "ldra": [0xed, 0x4f], "ed00": [0xed, 0x00], "ed77": [0xed, 0x77],
             "ed7f": [0xed, 0x7f], "dd": [0xdd], "fd": [0xfd], "dddd": [0xdd, 0xdd],
             "fddd": [0xfd, 0xdd], "ddfd": [0xdd, 0xfd], "dded00": [0xdd, 0xed, 0x00],
             "jr0": [0x18, 0x00], "jp": None, "call": None, "outna": [0xd3, 0x10],
             "inan": [0xdb, 0x10], "ldsphl": None, "exsphl": [0xe3], "ora": [0xb7]}
# short Q sequences for the pin sweeps
Q_SEQS = {"cp_scf": [0xfe, 0x28, 0x37], "cp_exaf_ccf": [0xfe, 0x28, 0x08, 0x3f],
          "cp_ddscf": [0xfe, 0x28, 0xdd, 0x37], "incb_nop_ccf": [0x04, 0x00, 0x3f],
          "addhl_scf": [0x09, 0x37], "cp_halt": [0xfe, 0x28, 0x76]}
# the M1 after a flag writer: RESET cuts it at every half-cycle
Q_RESET_SEQS = {"cp": [0xfe, 0x28, 0x00], "incb": [0x04, 0x00], "decb": [0x05, 0x00],
                "bit3b": [0xcb, 0x58, 0x00], "addhl": [0x09, 0x00], "adchl": [0xed, 0x4a, 0x00],
                "cpi": [0xed, 0xa1, 0x00], "ldai": [0xed, 0x57, 0x00], "scf": [0x37, 0x00],
                "exaf": [0x08, 0x00], "neg": [0xed, 0x44, 0x00], "rlb": [0xcb, 0x10, 0x00]}
# the instructions that load WZ (MEMPTR); RET returns past itself
WZ_OPS = {"ld_a_nn": [0x3a, 0x00, 0x80], "ld_nn_a": [0x32, 0x00, 0x81], "ld_a_bc": [0x0a],
          "ld_a_de": [0x1a], "ld_bc_a": [0x02], "ld_de_a": [0x12], "jp": None, "call": [0xcd, 0x00, 0x04],
          "ret": [0xc9], "rst38": [0xff], "jr": [0x18, 0x00], "djnz": [0x10, 0x00],
          "ex_sp_hl": [0xe3], "ld_hl_nn": [0x2a, 0x00, 0x80], "ld_nn_hl": [0x22, 0x00, 0x81],
          "ld_sp_nn": [0xed, 0x7b, 0x00, 0x90], "add_hl_bc": [0x09], "adc_hl_bc": [0xed, 0x4a],
          "sbc_hl_bc": [0xed, 0x42], "in_a_n": [0xdb, 0x10], "out_n_a": [0xd3, 0x10],
          "in_b_c": [0xed, 0x40], "out_c_b": [0xed, 0x41], "rld": [0xed, 0x6f], "rrd": [0xed, 0x67],
          "ld_a_ixd": [0xdd, 0x7e, 0x05], "bit_ixd": [0xdd, 0xcb, 0x05, 0x46],
          "bit_hl": [0xcb, 0x46], "add_ix_bc": [0xdd, 0x09], "ld_ix_nn": [0xdd, 0x2a, 0x00, 0x80],
          "ldi": [0xed, 0xa0], "cpi": [0xed, 0xa1], "ini": [0xed, 0xa2], "outi": [0xed, 0xa3],
          "ldd": [0xed, 0xa8], "cpd": [0xed, 0xa9], "ind": [0xed, 0xaa], "outd": [0xed, 0xab]}
WZ_SETUP = {"ret": (0x21, 0x02, 0x02, 0xe5)}       # LD HL,0202; PUSH HL: RET to the NOP after it
# the repeating block instructions, two rounds (BC = 2, or B = 2)
BLK_OPS = {"ldir": 0xb0, "cpir": 0xb1, "inir": 0xb2, "otir": 0xb3,
           "lddr": 0xb8, "cpdr": 0xb9, "indr": 0xba, "otdr": 0xbb}


def q_between(name, at):
    """The bytes of a Q_BETWEEN entry placed at address `at`"""
    nxt = at + 3
    return {"jp": [0xc3, nxt & 0xff, nxt >> 8], "call": [0xcd, nxt & 0xff, nxt >> 8],
            "ldsphl": [0xf9]}.get(name) or Q_BETWEEN[name]


def wz_op(name):
    """The bytes of a WZ_OPS entry at 0201 (after the marker)"""
    return [0xc3, 0x04, 0x02] if name == "jp" else WZ_OPS[name]


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


def fuzz_cases(group, n, seed):
    """n random cases: 3-10 instructions drawn from every opcode program, a
    random F and BC (1-4), interrupt mode (IM0 with an RST byte, IM1, IM2
    with an even, odd or FF vector), EI or not, and one to three pulses of
    NMI, INT (held until INTA), WAIT, BUSREQ -- and RESET in `fuzz_reset` --
    anywhere in the first 40 T-states, of random widths. Case i of a seed is
    always the same."""
    ops = all_opcodes()
    pins = (NMI, INT, WAIT, BUSREQ) + ((RESET,) if group == "fuzz_reset" else ())
    out = []
    for i in range(n):
        r = random.Random(f"{group} {seed} {i}")
        code = []
        for _ in range(r.randint(3, 10)):
            code += r.choice(ops)[1]
        im = r.choice((0, 1, 2))
        vec = (r.choice((0xc7, 0xcf, 0xd7, 0xdf, 0xe7, 0xef, 0xf7, 0xff)) if im == 0 else
               r.choice((0x40, 0x41, 0xff)) if im == 2 else 0xff)
        pulses = []
        for _ in range(r.randint(1, 3)):
            pin = r.choice(pins)
            lo = at(r.randint(0, 40), r.randrange(16))
            width = {NMI: r.choice((1, 4, 16, 48)), INT: -1, WAIT: r.choice((16, 32, 48)),
                     BUSREQ: r.choice((16, 32, 64)), RESET: r.choice((48, 64, 96))}[pin]
            pulses.append((pin, lo, -1 if width < 0 else lo + width))
        out.append(case(f"{group}_s{seed}_{i:05d}", group, f"{group}{i}", pulses, run=200,
                        code=code, setup=(0x01, r.randrange(256), 0x42, 0xc5, 0xf1, 0x01, r.randint(1, 4), 0x00),
                        im=im, ei=r.random() < 0.7, vec=vec,
                        inta_release=any(p == INT for p, _, _ in pulses),
                        meta={"pins": sorted({PIN_NAMES[p] for p, _, _ in pulses})}))
    return out


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


def program(target, im=1, ei=False, nmi_nops=0, setup=(), full_dump=False, dump_pre=()):
    """The sweep program as a 64K image (same layout as the hardware harness
    in hardesk's rp2350b_driver tests/event_sweep.cpp). setup: bytes run
    after the register setup, before [EI and] the jump to the marker.
    full_dump: the code at 0000 stores every register a RESET keeps --
    A and F first (PUSH AF), IX, IY and the alternates too -- for the cases
    with a RESET pulse. dump_pre: bytes run before that dump (CCF: the
    restart shows the Q the reset left in the F it pushes)."""
    ram = bytearray(0x10000)
    for i in range(0x100):
        ram[0x8000 + i] = (0x11 * (i + 1)) & 0xff

    def put(at, data):
        ram[at:at + len(data)] = bytes(data)
    # 0000: register dump (what a reset left), then on to the setup
    if full_dump:
        put(0x0000, list(dump_pre) + [0xf5,       # PUSH AF: A and F as the reset left them
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
         "busreq_walk": False, "wait_walk": False, "dwin": None, "dump_pre": ()}
    c.update(kw)
    c["code"], c["setup"], c["dump_pre"] = tuple(c["code"]), tuple(c["setup"]), tuple(c["dump_pre"])
    c["full_dump"] = any(p == RESET for p, _, _ in pulses)
    return c


def prog_id(c):
    """What the program image depends on."""
    return (c["code"], c["setup"], c["im"], c["ei"], c["nmi_nops"], c["full_dump"], c["dump_pre"])


def image(c):
    """The case's program image (program())"""
    return program(c["code"], c["im"], c["ei"], c["nmi_nops"], c["setup"], c["full_dump"], c["dump_pre"])


def cases(groups, phases, fuzz=(FUZZ_N, FUZZ_SEED)):
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
    if want("reset_m1wait"):   # RESET while WAIT stretches an M1 (Tw after T2)
        for t in ("inc_a", "ld_a_hl"):
            for w in (1, 2, 4):                 # WAIT low from the target's T2.P, w T
                for n in range(1, 12):
                    for j in (0, 4, 8, 12):
                        out.append(case(f"resetm1w_{t}_w{w}_T{n}_p{j:02d}", "reset_m1wait", t,
                                        [(WAIT, at(2, 0), at(2, 0) + 16 * w), (RESET, at(n, j), at(n, j) + 64)],
                                        run=300, meta={"n": n, "phase": j, "width": w}))
    if want("reset_busreq"):   # RESET before, in and after a bus grant
        for t in ("inc_a", "ld_a_hl", "push_bc"):
            for w in (2, 6):                    # BUSREQ low from the target's T1, w T
                for n in range(1, 14):
                    for j in (0, 4, 8, 12):
                        out.append(case(f"resetbr_{t}_w{w}_T{n}_p{j:02d}", "reset_busreq", t,
                                        [(BUSREQ, at(1, 4), at(1, 4) + 16 * w), (RESET, at(n, j), at(n, j) + 64)],
                                        run=300, meta={"n": n, "phase": j, "width": w}))
    if want("reset_nmi"):      # an NMI edge from RESET's release through the restart
        for rw in (3, 4, 6):                    # RESET from the target's T1, rw T
            for d in range(-2, 8):              # NMI falls d T after the release, j/16 more
                for j in (0, 4, 8, 12):
                    lo = at(1, 0) + 16 * rw + 16 * d + j
                    out.append(case(f"resetnmi_r{rw}_d{d}_p{j:02d}", "reset_nmi", "inc_a",
                                    [(RESET, at(1, 0), at(1, 0) + 16 * rw), (NMI, lo, lo + 16)],
                                    run=300, meta={"rw": rw, "d": d, "phase": j}))
    for g in ("fuzz", "fuzz_reset"):    # random code, modes and pin events
        if want(g):
            out += fuzz_cases(g, *fuzz)
    if want("opcodes_reset"):  # RESET at every T-state of every opcode, one position per T-state
        lengths = opcode_lengths()
        for name, code, setup in all_opcodes():
            for n in range(lengths[name] + 2):
                out.append(case(f"opr_{name}_T{n}", "opcodes_reset", name, [(RESET, at(n, 4), at(n, 4) + 64)],
                                code=code, setup=setup, run=300, meta={"n": n, "phase": 4}))
    if want("latch"):        # the data latch: the right byte in one half-cycle of a read
        for name, kind, code, t1, vv, dd, what in LATCH_READS:
            for nw in (0, 1):
                states = list(LATCH_HALVES[kind])
                pulses = []
                if kind == "inta":   # IM2 / IM0, EI, INT taken after INC A
                    pulses.append((INT, at(2, 4), -1))
                    kw = dict(im=2 if name == "inta_im2" else 0, ei=True, inta_release=True,
                              vec=vv)
                else:
                    kw = {}
                if nw:               # WAIT low across the falling edge that samples it
                    i = LATCH_WAIT_AT[kind]
                    lo = (t1 + 2 * i) * 8 + 4
                    pulses.append((WAIT, lo, lo + 16))
                    states.insert(i + 1, "Tw")
                halves = [f"{st}{'PN'[k]}" for st in states for k in (0, 1)]
                r1 = t1 + len(halves) - 1
                m = {"read": name, "kind": kind, "what": what, "waits": nw, "halves": halves}
                out.append(case(f"latch_{name}_w{nw}_plain", "latch", name, list(pulses), run=80,
                                code=code, meta=dict(m, half=None), **kw))
                for k, hn in enumerate(halves):
                    out.append(case(f"latch_{name}_w{nw}_{k:02d}{hn}", "latch", name, list(pulses),
                                    run=80, code=code, dwin=(t1, r1, t1 + k, vv, dd),
                                    meta=dict(m, half=k), **kw))
    if want("q_ops"):        # every opcode, then SCF or CCF: Q as each one leaves it
        for name, code, setup in all_opcodes():
            for tail, t in ((0x37, "scf"), (0x3f, "ccf")):
                out.append(case(f"qop_{name}_{t}", "q_ops", name, [], code=list(code) + [tail],
                                setup=setup or Q_AF, run=120, meta={"kind": t}))
    if want("q_seq"):        # a flag writer, an instruction that writes none, then SCF or CCF
        for fn, fcode in Q_FLAG_OPS.items():
            for bn in Q_BETWEEN:
                for tail, t in ((0x37, "scf"), (0x3f, "ccf")):
                    code = fcode + q_between(bn, 0x0201 + len(fcode)) + [tail]
                    out.append(case(f"qseq_{fn}_{bn}_{t}", "q_seq", fn, [], code=code,
                                    setup=Q_AF + Q_FLAG_SETUP, run=100, meta={"kind": f"{bn} {t}"}))
    if want("q_events"):     # each pin at every T-state of the Q sequences
        for qn, code in Q_SEQS.items():
            length = 4 * len(code) + 8
            for n in range(length + 2):
                for j in ph:
                    kw = {"code": code, "setup": Q_AF + Q_FLAG_SETUP}
                    m = {"n": n, "phase": j, "seq": qn}
                    for name, pins, extra in (
                            ("int1", [(INT, at(n, j), -1)], dict(ei=True, inta_release=True)),
                            ("int2", [(INT, at(n, j), -1)], dict(im=2, ei=True, inta_release=True, vec=0x40)),
                            ("int0", [(INT, at(n, j), -1)], dict(im=0, ei=True, inta_release=True, vec=0xff)),
                            ("nmi", [(NMI, at(n, j), at(n, j) + 16)], {}),
                            ("reset", [(RESET, at(n, j), at(n, j) + 64)], dict(run=300, dump_pre=(0x3f,))),
                            ("busreq1", [(BUSREQ, at(n, j), at(n, j) + 16)], {}),
                            ("busreq4", [(BUSREQ, at(n, j), at(n, j) + 64)], {}),
                            ("wait1", [(WAIT, at(n, j), at(n, j) + 16)], {}),
                            ("wait3", [(WAIT, at(n, j), at(n, j) + 48)], {})):
                        out.append(case(f"qev_{qn}_{name}_T{n}_p{j:02d}", "q_events", qn, pins,
                                        meta=dict(m, kind=name), **dict(dict(kw, run=100), **extra)))
    if want("q_reset"):      # RESET at every half-cycle of the M1 after a flag writer; CCF restarts
        for qn, code in Q_RESET_SEQS.items():
            for n in range(1, 4 * len(code) + 12):
                for j in ph:
                    out.append(case(f"qrst_{qn}_T{n}_p{j:02d}", "q_reset", qn,
                                    [(RESET, at(n, j), at(n, j) + 64)], code=code,
                                    setup=Q_AF + Q_FLAG_SETUP, run=300, dump_pre=(0x3f,),
                                    meta={"n": n, "phase": j}))
    if want("wz_int"):       # INT and NMI at every T-state of each instruction that loads WZ
        for wn in WZ_OPS:
            code = wz_op(wn)
            length = 26 if code[0] in (0xdd, 0xfd) else 24 if code[0] in (0xed, 0xcb, 0xcd) else 20
            for n in range(length + 2):
                for j in ph:
                    kw = {"code": code, "setup": WZ_SETUP.get(wn, ()), "run": 100}
                    out += [case(f"wzi_{wn}_int_T{n}_p{j:02d}", "wz_int", wn, [(INT, at(n, j), -1)],
                                 ei=True, inta_release=True, meta={"n": n, "phase": j, "kind": "int"}, **kw),
                            case(f"wzi_{wn}_nmi_T{n}_p{j:02d}", "wz_int", wn,
                                 [(NMI, at(n, j), at(n, j) + 16)], meta={"n": n, "phase": j, "kind": "nmi"},
                                 **kw)]
    if want("blk_events"):   # two rounds of each repeating block instruction, a pin at every T-state
        for bn, op in BLK_OPS.items():
            setup = (0x06, 0x02) if op & 2 else (0x01, 0x02, 0x00)
            if op & 8:       # down: HL / DE from the top of their blocks
                setup += (0x21, 0x01, 0x80, 0x11, 0x01, 0x83)
            code = [0xed, op]
            for n in range(44):
                kw = {"code": code, "setup": setup, "run": 120}
                m = {"n": n, "block": bn}
                for j in ph:
                    out += [case(f"blk_{bn}_int_T{n}_p{j:02d}", "blk_events", bn, [(INT, at(n, j), -1)],
                                 ei=True, inta_release=True, meta=dict(m, phase=j, kind="int"), **kw),
                            case(f"blk_{bn}_nmi_T{n}_p{j:02d}", "blk_events", bn,
                                 [(NMI, at(n, j), at(n, j) + 16)], meta=dict(m, phase=j, kind="nmi"), **kw)]
                out += [case(f"blk_{bn}_reset_T{n}", "blk_events", bn, [(RESET, at(n, 4), at(n, 4) + 64)],
                             meta=dict(m, phase=4, kind="reset"), dump_pre=(0x3f,), **dict(kw, run=300)),
                        case(f"blk_{bn}_busreq_T{n}", "blk_events", bn, [(BUSREQ, at(n, 4), at(n, 4) + 16)],
                             meta=dict(m, phase=4, kind="busreq"), **kw),
                        case(f"blk_{bn}_wait_T{n}", "blk_events", bn, [(WAIT, at(n, 4), at(n, 4) + 16)],
                             meta=dict(m, phase=4, kind="wait"), **kw)]
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
            c["busreq_walk"], c["wait_walk"], pulses) + ((("dwin", c["dwin"]),) if c["dwin"] else ())


def pz80_here():
    """perfectz80's harness sources are in this tree (not in a repository
    that carries the recordings alone)"""
    return os.path.exists(os.path.join(ROOT, "scripts", "event_sweep_pz80.c"))


def manifest():
    """{'harness-id': ..., 'harness-version': ..., 'files': {name: (cases, sha, commit)}}"""
    out = {"files": {}}
    if not os.path.exists(MANIFEST):
        return out
    for line in open(MANIFEST):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"([a-z-]+):\s*(\S+)$", line)
        if m:
            out[m.group(1)] = m.group(2)
            continue
        f = line.split()
        if len(f) == 4 and f[0].endswith(".txt.xz"):
            out["files"][f[0]] = (int(f[1]), f[2], f[3])
    return out


def write_manifest(hid, written, command):
    """MANIFEST.txt after --record: the harness, and every group file's
    cases, sha256 and the commit it was recorded at (this one's parent for
    the files just written)"""
    old = manifest()["files"]
    head = git_head()
    rows = []
    for f in sorted(os.listdir(CACHE)):
        if not f.endswith(".txt.xz"):
            continue
        data = open(os.path.join(CACHE, f), "rb").read()
        plain = lzma.decompress(data)
        cases = plain.count(b"\n@") + plain.startswith(b"@")
        commit = head if f in written else old.get(f, (0, "", "unknown"))[2]
        rows.append(f"{f} {cases} {hashlib.sha256(data).hexdigest()[:16]} {commit}")
    with open(MANIFEST, "w") as out:
        out.write("# perfectz80's recorded traces for scripts/event_sweep.py: one file per group,\n"
                  "# each trace under @<key>, the key hashing the harness id below with the case\n"
                  "# (cache_key()). Recorded by perfectz80 live: make record_pz80 (or\n"
                  "# scripts/event_sweep.py --groups <g> --record --no-cache --pz80-only);\n"
                  "# make event_sweep_verify runs perfectz80 again against every recording.\n"
                  f"# Last written by: {command}\n"
                  f"harness-id: {hid}\n"
                  f"harness-version: {harness_version()}\n"
                  "# file cases sha256[:16] recorded-at\n")
        out.write("\n".join(rows) + "\n")


def git_head():
    try:
        return subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True).stdout.strip() or "unknown"
    except OSError:
        return "unknown"


def harness_version():
    """HARNESS_VERSION in scripts/event_sweep_pz80.c: bumped when a change
    to the harness changes what it prints for a case it ran before."""
    if not pz80_here():
        return manifest().get("harness-version", "?")
    src = open(os.path.join(ROOT, "scripts", "event_sweep_pz80.c")).read()
    m = re.search(r"^#define HARNESS_VERSION (\d+)", src, re.M)
    if not m:
        sys.exit("scripts/event_sweep_pz80.c: no #define HARNESS_VERSION")
    return int(m.group(1))


def harness_id():
    """perfectz80 (its netlist and simulator), the pad-driver reader and the
    harness's version: a recorded trace holds while these are unchanged.
    The harness's own source is not hashed -- an option added for new cases
    leaves the old ones' output alone -- so a change that does alter it must
    bump HARNESS_VERSION, and --verify-recorded (CI) catches one that
    didn't. Without the sources here (a repository that carries the
    recordings alone), the id MANIFEST.txt names."""
    if not pz80_here():
        hid = manifest().get("harness-id")
        if not hid:
            sys.exit(f"no perfectz80 harness here and no harness-id in {MANIFEST}")
        return hid
    h = hashlib.sha1(f"event_sweep_pz80 v{harness_version()}\n".encode())
    refs = os.path.join(ROOT, "scripts", "refs", "perfectz80")
    files = [os.path.join(ROOT, "scripts", f) for f in ("pz80_pads.c", "pz80_pads.h")]
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
            if line.startswith("#"):        # the file's header
                continue
            if line.startswith("@"):
                if key: out[key] = "".join(lines)
                key, lines = line[1:].strip(), []
            else:
                lines.append(line)
        if key: out[key] = "".join(lines)
    return out


def save_cache(group, texts, hid, command):
    """texts: {key: perfectz80's output} of one group's cases"""
    os.makedirs(CACHE, exist_ok=True)
    with lzma.open(os.path.join(CACHE, f"{group}.txt.xz"), "wt", preset=9 | lzma.PRESET_EXTREME) as f:
        f.write(f"# group {group}: perfectz80's traces, harness {hid} (HARNESS_VERSION "
                f"{harness_version()}), recorded at {git_head()} by: {command}\n")
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


def dwin_arg(d):
    r0, r1, w, vv, dd = d
    return f"{r0},{r1},{w},{vv:02x},{dd:02x}"


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
        if c["dwin"]: cmd.append("+dwin=" + dwin_arg(c["dwin"]))
    else:                    # perfectz80, or the C model: the same options
        cmd = [PZ80 if side == "pz80" else CMODEL, "-e", ev, "-r", str(c["run"])]
        if c["inta_release"]: cmd.append("-i")
        if c["int_at"] is not None: cmd += ["-a", f"{c['int_at']:04x}"]
        if c["hold"]: cmd += ["-H", f"{c['hold'][0]:04x}", "-T", str(c["hold"][1])]
        if c["vec"] != 0xff: cmd += ["-v", f"{c['vec']:02x}"]
        if c["ops"]: cmd += ["-o", "".join(f"{b:02x}" for b in c["ops"])]
        if c["busreq_walk"]: cmd.append("-B")
        if c["wait_walk"]: cmd.append("-W")
        if c["dwin"]: cmd += ["-D", dwin_arg(c["dwin"])]
        cmd.append(prog)
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    return out if raw else parse_rows(out, f"{side} {c['name']}")


def parse_rows(out, what):
    rows = []
    for line in out.splitlines():
        v = line.split()
        if len(v) == 18 and v[0].isdigit():
            rows.append([int(v[0]), int(v[1]), int(v[2], 16), int(v[3], 16)] +
                        [int(x) for x in v[4:12]] + [int(x, 16) for x in v[12:]])
    if not rows:
        raise RuntimeError(f"{what}: {out.strip()[-200:]}")
    return rows


# row: h clk addr data mreq iorq rd wr m1 rfsh halt busack aen den cen f wz q
# (q: the models' Q; perfectz80's P, its pending-flags bit)
H, CLK, ADDR, DATA, MREQ, IORQ, RD, WR, M1, RFSH, HALT, BUSACK, AEN, DEN, CEN, F, WZ, Q = range(18)
CONTROL = [("mreq", MREQ), ("iorq", IORQ), ("rd", RD), ("wr", WR), ("m1", M1),
           ("rfsh", RFSH), ("halt", HALT), ("busack", BUSACK)]
# the pads driven: address, data, MREQ IORQ RD WR M1 RFSH HALT BUSACK
ENABLES = [("addr_en", AEN), ("data_en", DEN), ("ctl_en", CEN)]
STATE = [("F", 1), ("WZ", 2), ("Q", 3)]     # fields of a state sample


def state_samples(rows, die):
    """{half: (F, WZ, Q)}: the state an instruction leaves, as the next
    one meets it -- in T4.P of every M1-type cycle (an opcode fetch, an
    INTA, the fetch an NMI discards; T3.P is the first half-cycle of its
    refresh). The die writes the pending results of the instruction before
    in that M1's T1.N (EX AF), T2.N (registers, EXX, EX DE,HL) and T3.N
    (F), the models on the edges that end its T1 and T2, so both have them
    by T4.P, and none has started the new instruction's. The die has no
    Q register (docs/flags.md "Q"): its Q is F where this M1 wrote the
    flags left pending (P), else 0."""
    out = {}
    for i in range(1, len(rows) - 2):
        r, a, b = rows[i], rows[i + 1], rows[i + 2]
        if r[CLK] and not r[RFSH] and rows[i - 1][RFSH] and not a[RFSH] and not b[RFSH]:
            q = (b[F] if b[Q] else 0) if die else b[Q]
            out[b[H]] = (b[F], b[WZ], q)
    return out


def addressed(r):
    """The address is compared in every half-cycle: transfers, refreshes,
    and the internal and idle T-states, where the bus keeps its last
    address."""
    return True


def first_difference(a, b, die=False):
    """(half, [what differs]) of the first mismatching half-cycle, or None.
    The pins in every half-cycle. die: b is perfectz80's, and the state
    (F, WZ, Q) is compared where state_samples takes it. Else the two are
    models, and F, WZ and Q must match in every half-cycle too."""
    bh = {r[H]: r for r in b}
    if not a or not b or a[0][H] != b[0][H]:
        return (a[0][H] if a else 0), ["start"]
    sa, sb = (state_samples(a, False), state_samples(b, True)) if die else ({}, {})
    for ra in a:
        rb = bh.get(ra[H])
        if rb is None:
            return ra[H], ["length"]
        what = [n for n, i in CONTROL if ra[i] != rb[i]]
        if addressed(ra) and addressed(rb) and ra[ADDR] != rb[ADDR]:
            what.append("addr")
        if (ra[DATA] ^ rb[DATA]) & ra[DEN] & rb[DEN]:     # the pads both drive
            what.append("data")
        what += [n for n, i in ENABLES if ra[i] != rb[i]]
        if die:
            x, y = sa.get(ra[H]), sb.get(ra[H])
            if x != y:
                what += [n for n, k in STATE if x and y and x[k - 1] != y[k - 1]] or ["samples"]
        else:
            what += [n for n, i in (("F", F), ("WZ", WZ), ("Q", Q)) if ra[i] != rb[i]]
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


def state_text(samples, what):
    """The differing fields of the state samples, as text for the report"""
    k = [i for n, i in STATE if n in what] or [1, 2, 3]
    fmt = {1: "F={:02x}", 2: "WZ={:04x}", 3: "Q={:02x}"}
    return " ".join(f"{h}:" + ",".join(fmt[i].format(v[i - 1]) for i in k) for h, v in sorted(samples.items()))


def summarize(c, rtl, pz):
    d = first_difference(rtl, pz, die=True)
    if d is None:
        return None
    half, what = d
    out = {"first_half": half, "differs": what,
           "rz80": " ".join(bus_cycles(rtl)[:40]), "pz80": " ".join(bus_cycles(pz)[:40])}
    if any(n in what for n, _ in STATE):    # the state, from the first differing sample
        sr, sp = state_samples(rtl, False), state_samples(pz, True)
        out["rz80"] = state_text({h: v for h, v in sr.items() if h >= half}, what)
        out["pz80"] = state_text({h: v for h, v in sp.items() if h >= half}, what)
    return out


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


def write_diff(c, rtl, pz, path, die=True):
    """Side by side, one half-cycle a line: `*` where the pins differ;
    with the die on the right, its state samples (state_samples) after the
    lines they are taken in, `!` where they differ. The models' rows end
    in F WZ Q, the die's in F WZ P."""
    names = "h clk addr data mreq iorq rd wr m1 rfsh halt busack aen den cen f wz q"
    right = ("perfectz80 (q: P)", names) if die else ("RZ80 (iverilog RTL)", names)
    left = "RZ80 (iverilog RTL)" if die else "C model"
    sa, sb = (state_samples(rtl, False), state_samples(pz, True)) if die else ({}, {})
    with open(path, "w") as f:
        f.write(f"# {c['name']}: pulses {c['pulses']} (pin lo hi in 1/16 T from the marker's T1)\n")
        f.write(f"# {left:<68} | {right[0]}\n# {names:<68} | {right[1]}\n")
        for i in range(max(len(rtl), len(pz))):
            fmt = lambda r: (f"{r[H]} {r[CLK]} {r[ADDR]:04x} {r[DATA]:02x} " +
                             " ".join(str(v) for v in r[MREQ:AEN]) +
                             f" {r[AEN]:04x} {r[DEN]:02x} {r[CEN]:02x} {r[F]:02x} {r[WZ]:04x} {r[Q]:02x}") if r else ""
            a = rtl[i] if i < len(rtl) else None
            b = pz[i] if i < len(pz) else None
            n = 15 if die else 18                # the die's state is compared in the samples
            mark = " " if a and b and fmt(a).split()[4:n] == fmt(b).split()[4:n] else "*"
            f.write(f"{mark} {fmt(a):<68} | {fmt(b)}\n")
            h = a[H] if a else None
            if h in sa or h in sb:
                x, y = sa.get(h), sb.get(h)
                show = lambda v: f"F={v[0]:02x} WZ={v[1]:04x} Q={v[2]:02x}" if v else "-"
                f.write(f"{'!' if x != y else ' '}   state sample {show(x):<52} | {show(y)}\n")


def latch_section(latch):
    """The `latch` group as a grid: for each read, the half-cycle whose byte
    perfectz80 takes (its trace equals the run with the right byte all
    through the read); `!` where the RTL takes another."""
    same = lambda a, b: first_difference(a, b) is None
    out = ["\n## The data latch\n",
           "Each read with the right byte on the bus in one half-cycle only and a decoy in the "
           "others (`latch` group): `●` perfectz80 takes the byte of that half-cycle, `·` it takes "
           "the decoy; `!` the RTL differs there. Twa: an automatic wait state (TW*), Tw: one "
           "WAIT adds.\n"]
    for kind, title in (("m1", "Opcode fetch (M1)"), ("mrd", "Memory read"), ("iord", "I/O read"),
                        ("inta", "Interrupt acknowledge (INTA)")):
        reads = [r for r in LATCH_READS if r[1] == kind]
        cols = []
        for name, *_ in reads:
            for nw in (0, 1):
                key = f"latch_{name}_w{nw}_plain"
                if key in latch:
                    for h in latch[key][2]["halves"]:
                        if h not in cols: cols.append(h)
        order = {"T1": 0, "T2": 1, "Twa": 2, "Twa1": 2, "Twa2": 3, "Tw": 4, "T3": 5}
        cols.sort(key=lambda h: (order[h[:-1]], h[-1] == "N"))
        rows = []
        for name, _, _, _, _, _, what in reads:
            for nw in (0, 1):
                key = f"latch_{name}_w{nw}_plain"
                if key not in latch:
                    continue
                prtl, ppz, m = latch[key]
                cells = {}
                for k, h in enumerate(m["halves"]):
                    n = f"latch_{name}_w{nw}_{k:02d}{h}"
                    if n not in latch:
                        continue
                    rtl, pz, _ = latch[n]
                    tp, tr = same(pz, ppz), same(rtl, prtl)
                    cells.setdefault(h, "")
                    cells[h] += ("●" if tp else "·") + ("" if tp == tr else "!")
                rows.append(f"| `{name}` {what} | {nw} | " + " | ".join(cells.get(h, "") for h in cols) + " |")
        if rows:
            out += [f"### {title}\n", "| read | Tw | " + " | ".join(cols) + " |",
                    "|---|---|" + "---|" * len(cols)] + rows + [""]
    return out


def report(results, path, latch=None):
    by_group = collections.defaultdict(list)
    for c, s, fr, fp in results:
        by_group[c["group"]].append((c, s))
    out = ["# RESET / INT / NMI sweep: RTL vs perfectz80\n",
           "One line per group; a case matches when every half-cycle of pins agrees "
           "(see scripts/event_sweep.py).\n",
           "| group | cases | match | differ |", "|---|---|---|---|"]
    for g in GROUPS:
        rows = by_group.get(g)
        if not rows: continue
        m = sum(1 for _, s in rows if s is None)
        out.append(f"| {g} | {len(rows)} | {m} | {len(rows) - m} |")
    differ = {c["name"] for c, s, *_ in results if s}
    for title, names in (("Differences", differ),):
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
    if latch:
        out += latch_section(latch)
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
        c["prog_sha"] = hashlib.sha1(image(c)).hexdigest()[:16]
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
            shas[pid] = hashlib.sha1(image(c)).hexdigest()[:16]
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
    global OUT
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--groups", default="all")
    ap.add_argument("--match", default="")
    ap.add_argument("--phases", type=int, default=2, choices=(2, 4, 8, 16))
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--c-model", action="store_true",
                    help="also run the C model and require it to match the RTL in every half-cycle")
    ap.add_argument("--record", action="store_true",
                    help="write perfectz80's traces of the groups run to tests/event_sweep/pz80/")
    ap.add_argument("--no-cache", action="store_true", help="run perfectz80 for every case")
    ap.add_argument("--shard", default="", metavar="K/N",
                    help="run every Nth case from the Kth (0-based): CI splits the sweep over N jobs")
    ap.add_argument("--fuzz-seed", type=int, default=FUZZ_SEED,
                    help=f"the fuzz groups' seed ({FUZZ_SEED}: the recorded cases)")
    ap.add_argument("--fuzz-n", type=int, default=FUZZ_N, help="cases per fuzz group")
    ap.add_argument("--out", default=OUT, help="report and work directory (build/event_sweep)")
    ap.add_argument("--classes", action="store_true",
                    help="print the opcodes' timing classes from the recorded traces and exit")
    ap.add_argument("--verify-recorded", default="0", metavar="N|all",
                    help="also run N (or all) of the cases' recorded perfectz80 traces live and fail "
                         "on any difference (a harness change that needed a HARNESS_VERSION bump)")
    ap.add_argument("--recorded-only", action="store_true",
                    help="never run perfectz80: its side from the recordings only, and fail on a "
                         "case they don't have (iverilog and Python alone)")
    ap.add_argument("--pz80-only", action="store_true",
                    help="run neither the RTL nor the C model: perfectz80 alone, for "
                         "--verify-recorded and --record")
    ap.add_argument("--verify-seed", type=int, default=None,
                    help="the --verify-recorded sample's seed (default: a random one, printed)")
    a = ap.parse_args()
    OUT = os.path.abspath(a.out)
    if a.classes:
        timing_classes(load_cache(), harness_id())
        return 0
    n_verify = a.verify_recorded
    if n_verify != "all":
        n_verify = int(n_verify)
    if a.pz80_only and a.c_model:
        sys.exit("--pz80-only runs no model: drop --c-model")
    if a.pz80_only and not (n_verify or a.record):
        sys.exit("--pz80-only needs --verify-recorded or --record")
    if a.recorded_only and (a.record or a.no_cache):
        sys.exit("--recorded-only never runs perfectz80: no --record / --no-cache")
    need = (() if a.pz80_only else (TB_VVP,)) + ((CMODEL,) if a.c_model else ())
    if not a.recorded_only or n_verify:
        need += (PZ80,)
    for exe in need:
        if not os.path.exists(exe):
            sys.exit(f"missing {exe}: run `make event_sweep`")
    k, n = (int(x) for x in a.shard.split("/")) if a.shard else (0, 1)
    todo = [c for i, c in enumerate(cases(a.groups.split(","), a.phases, (a.fuzz_n, a.fuzz_seed)))
            if a.match in c["name"] and i % n == k]
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
            ram = image(c)
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
    if (a.recorded_only or n_verify == "all") and hits < len(pz_cases):
        missing = sorted(c["name"] for c in pz_cases.values() if cache_key(c, hid) not in cache)
        rec_hid = manifest().get("harness-id")
        if rec_hid and rec_hid != hid:
            print(f"::error::the recordings are of perfectz80 harness {rec_hid} ({MANIFEST}), "
                  f"this tree's is {hid}: record them again (make record_pz80)")
        for n in missing[:50]:
            print(f"::error::{n}: no recorded perfectz80 trace (scripts/event_sweep.py "
                  f"--groups <its group> --record)")
        print(f"{len(missing)} cases have no recorded perfectz80 trace")
        return 1
    verify = []
    if n_verify:               # recorded traces, run again live: a sample, or all of them
        rec = sorted((k for k, c in pz_cases.items() if cache_key(c, hid) in cache),
                     key=lambda k: pz_cases[k]["name"])
        if n_verify == "all":
            verify = rec
            print(f"verifying all {len(verify)} recorded perfectz80 traces live", flush=True)
        else:
            seed = a.verify_seed if a.verify_seed is not None else random.randrange(1 << 30)
            verify = random.Random(seed).sample(rec, min(n_verify, len(rec)))
            print(f"verifying {len(verify)} recorded perfectz80 traces live (--verify-seed {seed})",
                  flush=True)
    with ThreadPoolExecutor(a.jobs) as pool:
        pz_runs = {}
        for k, c in pz_cases.items():
            text = cache.get(cache_key(c, hid))
            pz_runs[k] = (done((parse_rows(text, f"recorded pz80 {c['name']}"), text)) if text is not None
                          else pool.submit(run_pz80, c, work))
        rtl_runs = [] if a.pz80_only else [
            (c, pool.submit(run_case, c, work, "rtl"),
             pool.submit(run_case, c, work, "c") if a.c_model else None) for c in todo]
        v_runs = [(pz_cases[k], pool.submit(run_pz80, pz_cases[k], work)) for k in verify]
        results, c_differs, latch = [], [], {}
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
                    write_diff(c, cm, rtl, os.path.join(diff_dir, f"{c['name']}.c_vs_rtl.txt"), die=False)
            results.append((c, s, facts(c, rtl), facts(c, pz)))
            if c["group"] == "latch":
                latch[c["name"]] = (rtl, pz, c["meta"])
            if i % 250 == 0 or i == len(rtl_runs):
                print(f"  {i}/{len(rtl_runs)}", flush=True)
        v_bad = []
        for i, (c, fut) in enumerate(v_runs, 1):
            if fut.result()[1] != cache[cache_key(c, hid)]:
                v_bad.append(c["name"])
            if a.pz80_only and (i % 1000 == 0 or i == len(v_runs)):
                print(f"  verified {i}/{len(v_runs)}", flush=True)

    if a.record:               # perfectz80's traces of the groups run, for later runs
        command = "scripts/event_sweep.py " + " ".join(sys.argv[1:])
        texts = collections.defaultdict(dict)
        for c in todo:
            texts[c["group"]][cache_key(c, hid)] = pz_runs[pz80_key(c)].result()[1]
        for g, t in texts.items():
            save_cache(g, t, hid, command)
        write_manifest(hid, {f"{g}.txt.xz" for g in texts}, command)
        print(f"recorded perfectz80's traces of {', '.join(sorted(texts))} in {os.path.relpath(CACHE, ROOT)}/")

    differ = sorted(c["name"] for c, s, *_ in results if s)
    if not a.pz80_only:
        report(results, os.path.join(OUT, "report.md"), latch)
        print(f"{len(results) - len(differ)} match, {len(differ)} differ; "
              f"report: {os.path.relpath(os.path.join(OUT, 'report.md'), ROOT)}")
    if a.c_model:
        print(f"C model: {len(results) - len(c_differs)} cases identical to the RTL, {len(c_differs)} differ")
        for n, (half, what) in sorted(c_differs)[:50]:
            print(f"::error::{n}: the C model differs from the RTL at half {half} ({', '.join(what)}; "
                  f"build/event_sweep/diff/{n}.c_vs_rtl.txt, C left)")
    if verify:
        print(f"recorded perfectz80 traces run again live: {len(verify) - len(v_bad)} identical, "
              f"{len(v_bad)} differ")
        for n in sorted(v_bad):
            print(f"::error::{n}: perfectz80 now prints something else than its recorded trace: "
                  f"a harness change that needs HARNESS_VERSION bumped and the groups recorded again "
                  f"(scripts/event_sweep_pz80.c)")

    for n in differ[:50]:
        print(f"::error::{n}: differs from perfectz80 (build/event_sweep/diff/{n}.txt)")
    return 1 if differ or c_differs or (verify and v_bad) else 0


if __name__ == "__main__":
    sys.exit(main())
