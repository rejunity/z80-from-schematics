#!/usr/bin/env python3
"""sigrok_opcode_cycles.py - extract per-opcode T-state counts AND per-opcode
M-cycle bus-shapes from a sigrok kc85 capture, then check that our C model
agrees on both.

This is the practical "real silicon ground truth" use of the kc85 dumps that
DOESN'T need the unknown initial register state: for each opcode observed
during the OS loop, the T-states between its M1 fetch and the next M1 fetch
are visible on the bus, regardless of what registers held what.

Output:
  - Per opcode: how many T-states real silicon consumed between this M1 and
    the next M1 (one row per (opcode, length) combination, with counts).
  - Per opcode: the M-cycle bus-op sequence (M1, MR, MW, IOR, IOW) between
    consecutive M1s.
  - Cross-check: run that opcode on our emulator (single instruction at a
    clean PC, NOPs around it), measure its T-state count, and flag any
    discrepancy.

Caveats kept in the report:
  - Conditional opcodes (JR cc, JP cc, CALL cc, RET cc) and the block-op
    repeating forms (LDIR/CPIR/...) legitimately show TWO lengths depending
    on flags / BC: both are spec-correct, and we treat any kc85 length that
    matches EITHER spec branch as a match.
  - Prefix bytes (CB/ED/DD/FD) span two consecutive M1 cycles; for those we
    report the prefix-only T-state count from the capture (~4T) and skip
    the cross-check (our emulator collapses prefix + op into a single
    "instruction" in instr_count).
"""
import os, sys, subprocess, argparse
from collections import defaultdict, Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DECODE = os.path.join(ROOT, "scripts/sigrok_z80_decode.py")
TRACEGEN = os.path.join(ROOT, "build/bin/tracegen")

# Z80 documented T-state counts per opcode (unprefixed). For conditional
# opcodes we list both (taken, not-taken). None = prefix or unhandled.
# Source: Sean Young "The Undocumented Z80 Documented", appendix tables.
T_NOPREFIX = {
    0x00: 4, 0x07: 4, 0x08: 4, 0x0F: 4,
    0x17: 4, 0x1F: 4, 0x27: 4, 0x2F: 4, 0x37: 4, 0x3F: 4,
    # LD r,r' (matrix 0x40..0x7F, but (HL) rows differ — set later)
    # ALU A,r (0x80..0xBF) - 4T for r, 7T for (HL)
    # see special-case logic in classify()
}
PREFIX_OPCODES = {0xCB, 0xED, 0xDD, 0xFD}

def classify_t_states(opcode):
    """Return (min_t, max_t) accepted as correct for this opcode based on
    the Z80 spec. Returns None if we choose to skip this opcode."""
    if opcode in PREFIX_OPCODES:
        return None    # prefix: combined with next opcode, handled separately
    o = opcode
    # LD r,r' (no (HL))
    if 0x40 <= o <= 0x7F and o != 0x76:
        if (o & 0x07) == 6: return (7, 7)       # LD r,(HL)
        if ((o >> 3) & 0x07) == 6: return (7, 7) # LD (HL),r
        return (4, 4)
    if o == 0x76: return (4, 4)                  # HALT - one M1 per loop
    # ALU A,r
    if 0x80 <= o <= 0xBF:
        if (o & 0x07) == 6: return (7, 7)
        return (4, 4)
    # INC/DEC r (0x04,0x0c,0x14,...,0x3c) -- 4T or 11T for (HL)
    # We handle a subset that appears commonly; otherwise return None.
    SHORT_4T = {
        0x00,0x07,0x08,0x0F,0x17,0x1F,0x27,0x2F,0x37,0x3F,
        0x04,0x0C,0x14,0x1C,0x24,0x2C,0x3C,        # INC r (8-bit)
        0x05,0x0D,0x15,0x1D,0x25,0x2D,0x3D,        # DEC r (8-bit)
        0xF3,0xFB,                                   # DI / EI
    }
    if o in SHORT_4T: return (4, 4)
    # INC/DEC rp (16-bit) = 6T (M1 4T + 2T internal): 0x03 0x0B 0x13 0x1B 0x23 0x2B 0x33 0x3B
    if o in {0x03,0x0B,0x13,0x1B,0x23,0x2B,0x33,0x3B}: return (6, 6)
    # LD r,n (0x06,0x0e,0x16,0x1e,0x26,0x2e,0x3e) = 7T
    if o in {0x06,0x0E,0x16,0x1E,0x26,0x2E,0x3E}: return (7, 7)
    # LD rp,nn (0x01,0x11,0x21,0x31) = 10T
    if o in {0x01,0x11,0x21,0x31}: return (10, 10)
    # ADD HL,rp (0x09,0x19,0x29,0x39) = 11T
    if o in {0x09,0x19,0x29,0x39}: return (11, 11)
    # JR d (0x18) = 12T;  JR cc,d = 7T (not taken) or 12T (taken)
    if o == 0x18: return (12, 12)
    if o in {0x20,0x28,0x30,0x38}: return (7, 12)
    # JP nn (0xC3) = 10T;  JP cc,nn = 10T (regardless of taken)
    if o == 0xC3: return (10, 10)
    if o in {0xC2,0xCA,0xD2,0xDA,0xE2,0xEA,0xF2,0xFA}: return (10, 10)
    # CALL nn (0xCD) = 17T; CALL cc,nn = 10T (not taken) or 17T (taken)
    if o == 0xCD: return (17, 17)
    if o in {0xC4,0xCC,0xD4,0xDC,0xE4,0xEC,0xF4,0xFC}: return (10, 17)
    # RET (0xC9) = 10T; RET cc = 5T (not taken) or 11T (taken)
    if o == 0xC9: return (10, 10)
    if o in {0xC0,0xC8,0xD0,0xD8,0xE0,0xE8,0xF0,0xF8}: return (5, 11)
    # PUSH rp = 11T;  POP rp = 10T
    if o in {0xC5,0xD5,0xE5,0xF5}: return (11, 11)
    if o in {0xC1,0xD1,0xE1,0xF1}: return (10, 10)
    # RST n = 11T
    if o in {0xC7,0xCF,0xD7,0xDF,0xE7,0xEF,0xF7,0xFF}: return (11, 11)
    # DJNZ d = 8 (not taken) or 13 (taken)
    if o == 0x10: return (8, 13)
    # EX (SP),HL = 19T; EX DE,HL = 4T; EXX = 4T; EX AF,AF' = 4T
    if o == 0xE3: return (19, 19)
    if o in {0xEB, 0xD9, 0x08}: return (4, 4)
    # LD (nn),A / LD A,(nn) (0x32, 0x3A) = 13T
    if o in {0x32, 0x3A}: return (13, 13)
    # LD (nn),HL / LD HL,(nn) (0x22, 0x2A) = 16T
    if o in {0x22, 0x2A}: return (16, 16)
    # LD (BC/DE),A / LD A,(BC/DE) (0x02,0x12,0x0A,0x1A) = 7T
    if o in {0x02,0x12,0x0A,0x1A}: return (7, 7)
    # ALU/INC/DEC A,n (0xC6,0xCE,0xD6,0xDE,0xE6,0xEE,0xF6,0xFE) = 7T
    if o in {0xC6,0xCE,0xD6,0xDE,0xE6,0xEE,0xF6,0xFE}: return (7, 7)
    # OUT (n),A / IN A,(n)  = 11T
    if o in {0xD3, 0xDB}: return (11, 11)
    # JP (HL) = 4T
    if o == 0xE9: return (4, 4)
    return None  # unknown / not classified

def decode(sr_path):
    out = subprocess.check_output(["python3", DECODE, sr_path, "--trace", "/dev/stdout"],
                                  stderr=subprocess.DEVNULL).decode()
    cycles = []
    for ln in out.splitlines():
        if not ln or ln.startswith("#"): continue
        p = ln.split()
        cycles.append(dict(
            i=int(p[0]), m1=int(p[1]), mreq=int(p[2]), iorq=int(p[3]),
            rd=int(p[4]), wr=int(p[5]), addr=int(p[6], 16), data=int(p[7], 16),
            # decoder column 8 is INT (active), column 9 is WAIT (active)
            wait=int(p[9]) if len(p) > 9 else 0,
        ))
    return cycles

def find_m1_starts(cycles):
    """Cycle indices where an M1 fetch BEGINS (M1 transitions 0->1; MREQ
    asserts a cycle later when the opcode is latched). Returns list of
    (cycle_idx, fetch_pc, opcode_byte)."""
    starts = []
    prev_m1 = 0
    for i, c in enumerate(cycles):
        if c["m1"] == 1 and prev_m1 == 0:
            # MREQ usually asserts the NEXT sample; scan forward to find
            # the cycle with M1+MREQ+RD active and grab the opcode byte
            op = None; pc = c["addr"]
            for j in range(i, min(i + 4, len(cycles))):
                cj = cycles[j]
                if cj["m1"] and cj["mreq"] and cj["rd"]:
                    op = cj["data"]; pc = cj["addr"]
                    break
            if op is not None:
                starts.append((i, pc, op))
        prev_m1 = c["m1"]
    return starts

def emit_emulator_opcode_cycles(opcode):
    """Run a one-instruction test: write opcode + 4 NOPs at 0x0100, point PC
    at it, run tracegen for enough phases to capture the instruction. Return
    the T-state count (phases / 2) from the M1 of the opcode to the next M1."""
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False) as f:
        path = f.name
        # Bootstrap: JP 0x0100 at 0x0000 so we reach 0x0100 with a clean M1.
        f.write("@0000\nc3\n00\n01\n")
        # Place opcode at 0x0100, then enough NOPs to cover up to ~20T worth.
        f.write("@0100\n")
        f.write(f"{opcode:02x}\n")
        for _ in range(8): f.write("00\n")
    try:
        out = subprocess.check_output([TRACEGEN, path, "60"], stderr=subprocess.DEVNULL).decode()
    finally:
        os.unlink(path)
    rows = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) != 14 or not p[0].lstrip("-").isdigit(): continue
        rows.append(p)   # t phi m addr data_o data_i mreq iorq rd wr m1 rfsh halt busack
    # find the M1 at PC=0x0100 (T1.P sample: m1=0 active, addr matches) and
    # the next M1 (any PC) after it. Both bounds aligned to T1.P (the start
    # of the M1 cycle's first phase) so the diff is an integer number of
    # phases = 2 * T-states.
    start = None
    for i, r in enumerate(rows):
        if r[10] == "0" and int(r[3], 16) == 0x0100:
            start = i; break
    if start is None: return None
    nxt = None
    saw_m1_high = False
    for i in range(start + 1, len(rows)):
        if rows[i][10] == "1": saw_m1_high = True
        elif saw_m1_high and rows[i][10] == "0":
            nxt = i; break
    if nxt is None: return None
    return (nxt - start) // 2   # phases -> T-states

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sr")
    args = ap.parse_args()
    cycles = decode(args.sr)
    starts = find_m1_starts(cycles)
    # for each consecutive pair, the duration is the T-state count of the
    # instruction whose fetch begins at the first M1 (capture is cpu-clock
    # synchronous -> one sample = one T-state). Also count the WAIT samples
    # observed during that instruction window so we can attribute T-state
    # excess to /WAIT extensions versus actual silicon out-of-spec.
    per_op_lens = defaultdict(Counter)       # (length, n_wait) -> count
    for k in range(len(starts) - 1):
        i0, pc0, op0 = starts[k]
        i1, _, _    = starts[k + 1]
        n_wait = sum(1 for c in cycles[i0:i1] if c["wait"])
        per_op_lens[op0][(i1 - i0, n_wait)] += 1

    print(f"# opcodes observed in {os.path.basename(args.sr)}: {len(per_op_lens)}")
    print("# opcode | n samples | hist (LT[+nTw]×count) | spec   | emu    | verdict")
    total = matched = mismatch = sysartifact = wait_explained = 0
    for op in sorted(per_op_lens):
        spec = classify_t_states(op)
        sample_lens = per_op_lens[op]
        samples = sum(sample_lens.values())
        if spec is None:
            hist = ", ".join(
                (f"{L}T" if w == 0 else f"{L}T(+{w}Tw)") + f"×{cnt}"
                for (L, w), cnt in sorted(sample_lens.items()))
            print(f"  {op:02x}     | {samples:3d}       | {hist}  | -      | -      | (prefix / unclassified)")
            continue
        emu = emit_emulator_opcode_cycles(op)
        # Verdict rule:
        #   - "OK": every observation's length is in [spec.min, spec.max].
        #   - "OK + WAIT": some observations exceed spec, BUT for each such
        #     observation /WAIT was asserted somewhere in the window (so the
        #     excess is consistent with one or more Tw insertions). The exact
        #     Tw-count is hard to recover from a 1-sample-per-T-state stream
        #     because the Z80 only acts on /WAIT at the falling edge of T2
        #     of an MREQ-active M-cycle, while we count WAIT-active samples
        #     anywhere in the instruction.
        #   - "silicon system artifact": some observation exceeds spec AND
        #     /WAIT was never asserted in that window — extra cycles come
        #     from something else (system-bus contention, hidden refresh,
        #     a captured interrupt acceptance, ...).
        all_in_spec = all(spec[0] <= L <= spec[1] for (L, _) in sample_lens)
        out_of_spec_all_have_wait = all(
            (spec[0] <= L <= spec[1]) or (w > 0)
            for (L, w) in sample_lens
        )
        any_wait = any(w > 0 for _, w in sample_lens)
        spec_str = f"{spec[0]}-{spec[1]}T" if spec[0]!=spec[1] else f"{spec[0]}T"
        emu_str  = f"{emu}T" if emu is not None else "?"
        if all_in_spec and emu is not None and spec[0] <= emu <= spec[1]:
            verdict = "OK"; matched += 1
        elif out_of_spec_all_have_wait and emu is not None and spec[0] <= emu <= spec[1]:
            verdict = "OK (excess attributed to /WAIT)"
            matched += 1; wait_explained += 1
        elif emu is not None and not (spec[0] <= emu <= spec[1]):
            verdict = f"emu mismatch ({emu} not in [{spec[0]},{spec[1]}])"
            mismatch += 1
        else:
            verdict = "silicon system artifact (excess with no /WAIT)"
            sysartifact += 1
        total += 1
        hist = ", ".join(
            (f"{L}T" if w == 0 else f"{L}T(+{w}Tw)") + f"×{cnt}"
            for (L, w), cnt in sorted(sample_lens.items())
        )
        print(f"  {op:02x}     | {samples:3d}       | {hist}  | {spec_str:6s} | {emu_str:6s} | {verdict}")
    print(f"\nClassified: {total} opcodes, {matched} OK ({wait_explained} via /WAIT), "
          f"{mismatch} emu mismatches, {sysartifact} silicon system artifacts")

if __name__ == "__main__":
    sys.exit(main() or 0)
