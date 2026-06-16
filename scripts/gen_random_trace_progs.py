#!/usr/bin/env python3
"""Generate seeded-random Z80 trace programs.

The output programs are deterministic from their seed (same seed always
produces the same bytes). They're meant to be run through `make perfectz80`
to compare per-half-cycle pin behavior between our C model / RTL and the
perfectz80 gate-level netlist on opcode mixes that no hand-written program
would cover.

We avoid a few opcode classes that would make trace comparison meaningless
or break the runner:

  - HALT (0x76)               — would freeze the trace prematurely
  - All branches / calls      — would jump to undefined code
    JP nn / JP cc,nn / JR / DJNZ / CALL nn / CALL cc,nn / RET / RST
  - DD/FD prefix              — fine on its own, but bias is low; we
                                allow them with low weight since they're
                                a single-byte prefix
  - CB displacement (DDCB)    — handled inside the DD/FD weight

Stack ops (PUSH/POP) are allowed since the program presets SP via an
implicit reset value the test driver knows. (`tracegen` / `tb_z80.v` /
`perfectz80_runner` all start SP at the same value.)

Usage:
    scripts/gen_random_trace_progs.py <seed> <n_bytes> [<output_path>]

Default n_bytes=48, default output is stdout in @addr-prefixed hex format.
"""
import sys
import random

# Hand-curated whitelist of "safe" opcodes (single-byte unless followed by
# operand bytes — those are filled with random bytes from the same RNG).
# (opcode, operand_byte_count). 1-byte opcodes use operand_count=0. For
# multi-byte instructions (e.g. CB / DD / FD prefix), we emit the prefix
# here and the runtime handles operand expansion in the next loop iter.
SAFE_NO_OPERAND = [
    0x00,                                        # NOP
    0x07, 0x0F, 0x17, 0x1F,                      # RLCA / RRCA / RLA / RRA
    0x27,                                        # DAA
    0x2F, 0x37, 0x3F,                            # CPL / SCF / CCF
    0xF3, 0xFB,                                  # DI / EI
    0x08,                                        # EX AF,AF'
    0xD9,                                        # EXX
    0xEB, 0xE3,                                  # EX DE,HL / EX (SP),HL
    # 8-bit r,r' loads (0x40..0x7F minus 0x76 HALT)
] + [op for op in range(0x40, 0x80) if op != 0x76] + [
    # 8-bit ALU on register/operand (0x80..0xBF)
] + list(range(0x80, 0xC0)) + [
    # INC/DEC r (0x04, 0x0C, ..., 0x3C; 0x05, 0x0D, ..., 0x3D)
] + [op for op in range(0x04, 0x40, 8)] + [op for op in range(0x05, 0x40, 8)] + [
    # INC/DEC rp (0x03, 0x13, 0x23, 0x33 / 0x0B, 0x1B, 0x2B, 0x3B)
    0x03, 0x13, 0x23, 0x33,
    0x0B, 0x1B, 0x2B, 0x3B,
    # ADD HL,rp (0x09, 0x19, 0x29, 0x39)
    0x09, 0x19, 0x29, 0x39,
]

# Opcodes with one immediate operand byte.
SAFE_ONE_OPERAND = [
    0x06, 0x0E, 0x16, 0x1E, 0x26, 0x2E, 0x3E,    # LD B/C/D/E/H/L/A,n
    0x36,                                        # LD (HL),n
    0xC6, 0xCE, 0xD6, 0xDE,                      # ADD/ADC/SUB/SBC A,n
    0xE6, 0xEE, 0xF6, 0xFE,                      # AND/XOR/OR/CP n
]

# Opcodes with two immediate operand bytes.
SAFE_TWO_OPERAND = [
    0x01, 0x11, 0x21, 0x31,                      # LD BC/DE/HL/SP,nn
    0x22, 0x2A,                                  # LD (nn),HL / LD HL,(nn)
    0x32, 0x3A,                                  # LD (nn),A / LD A,(nn)
]

# CB prefix: rotates/shifts/BIT/RES/SET — single follow-up byte is the
# CB-opcode itself.
CB_PREFIX = 0xCB

# ED prefix: pick a safe subset of the 4 quadrants. We exclude block-op
# variants here because their 5T-extension cycles depend on BC and could
# cause infinite loops in a 200-phase window if BC happens to be large.
# Safe ED subset: register ops, LD I/R, NEG, IM 0/1/2, fixed-cycle 16-bit
# ADC/SBC, LD rp,(nn) / LD (nn),rp.
SAFE_ED_OPCODES = [
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,   # IN B,(C) / OUT (C),B / SBC HL,BC / LD (nn),BC / NEG / RETN / IM 0 / LD I,A
    0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4F,         # IN C,(C) / OUT (C),C / ADC HL,BC / LD BC,(nn) / NEG / RETI / LD R,A
    0x50, 0x51, 0x52, 0x53, 0x54, 0x56, 0x57,         # IN D,(C) / OUT (C),D / SBC HL,DE / LD (nn),DE / NEG / IM 1 / LD A,I
    0x58, 0x59, 0x5A, 0x5B, 0x5E, 0x5F,               # IN E,(C) / OUT (C),E / ADC HL,DE / LD DE,(nn) / IM 2 / LD A,R
    0x67, 0x6F,                                       # RRD / RLD
]

# Bias weights — picked empirically so the resulting trace has good
# coverage of the M-cycle taxonomy without one class dominating.
WEIGHTS = {
    "no_operand":  60,
    "one_operand": 20,
    "two_operand": 10,
    "cb":           5,
    "ed":           5,
}


def gen_program(seed: int, n_bytes: int) -> bytes:
    rng = random.Random(seed)
    out = bytearray()
    while len(out) < n_bytes:
        choice = rng.choices(
            list(WEIGHTS.keys()),
            weights=list(WEIGHTS.values()),
            k=1,
        )[0]
        room = n_bytes - len(out)
        if choice == "no_operand":
            out.append(rng.choice(SAFE_NO_OPERAND))
        elif choice == "one_operand" and room >= 2:
            out.append(rng.choice(SAFE_ONE_OPERAND))
            out.append(rng.randint(0, 0xFF))
        elif choice == "two_operand" and room >= 3:
            out.append(rng.choice(SAFE_TWO_OPERAND))
            out.append(rng.randint(0, 0xFF))
            out.append(rng.randint(0, 0xFF))
        elif choice == "cb" and room >= 2:
            out.append(CB_PREFIX)
            out.append(rng.randint(0, 0xFF))   # any CB-suffix is well-defined
        elif choice == "ed" and room >= 2:
            out.append(0xED)
            out.append(rng.choice(SAFE_ED_OPCODES))
        else:
            out.append(0x00)                    # NOP padding when room < op size
    return bytes(out)


def format_hex_program(seed: int, prog: bytes) -> str:
    """Format as the @addr-prefixed hex format used by tests/traces/*.hex.

    Use Verilog `//` line comments (NOT `#`): iverilog's $readmemh — used
    by the iverilog testbench loader — silently produces 00 bytes when it
    hits a `#` line, while it accepts `//` per the IEEE Verilog spec.
    """
    lines = [
        f"// Random Z80 trace program — generated by scripts/gen_random_trace_progs.py",
        f"// seed={seed}  bytes={len(prog)}",
    ]
    for i in range(0, len(prog), 16):
        chunk = prog[i:i + 16]
        lines.append(" ".join(f"{b:02X}" for b in chunk))
    return "\n".join(lines) + "\n"


def main() -> int:
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    seed = int(sys.argv[1])
    n_bytes = int(sys.argv[2]) if len(sys.argv) > 2 else 48
    out_path = sys.argv[3] if len(sys.argv) > 3 else None
    prog = gen_program(seed, n_bytes)
    text = format_hex_program(seed, prog)
    if out_path:
        with open(out_path, "w") as f:
            f.write(text)
        print(f"wrote {out_path}  ({len(prog)} bytes, seed={seed})")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
