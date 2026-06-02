# Undocumented behavior

Tracks the undocumented opcodes, flags, and internal-register effects the core must
reproduce, with the rule and where it is enforced. Filled in as coverage grows;
ultimate arbiter is ZEXALL + die/gate-level evidence.

## Undocumented flags (X/Y, bit3/bit5)
See `docs/flags.md`. Summary of the non-result-bit cases:
- `CP`, `CPI/CPD…`: X/Y from operand / `A - (HL) - HF`, not the result.
- `BIT b,(HL)` / `BIT b,(IX+d)`: X/Y from WZ (MEMPTR) high byte.
- `SCF`/`CCF`: X/Y from A (NMOS variant; see `docs/known-differences.md`).
- 16-bit `ADD/ADC/SBC`: X/Y from result high byte.
- block `LD`/`CP`: X/Y from a derived temp, not the result.

## MEMPTR / WZ update rules
WZ is an internal 16-bit temp, observable only via `BIT n,(HL)` X/Y. Update points
(`ctl_wz_op`), per the MEMPTR community document + die analysis:
- `LD A,(BC/DE)` / `LD (BC/DE),A`: WZ = addr+1 (low from addr+1, high from A on writes).
- `LD A,(nn)` / `LD (nn),A`: WZ = nn+1 (writes: WZ_high = A).
- `LD (nn),rp` / `LD rp,(nn)`: WZ = nn+1.
- `EX (SP),HL/IX/IY`: WZ = the value loaded.
- ` JP cc,nn` / `JP nn`: WZ = nn.  `JR`/`DJNZ` taken: WZ = dest.
- `CALL`/`RST`/`RET` (cc): WZ = dest. `RETI/RETN`: WZ = dest.
- `IN A,(n)`: WZ = (A<<8 | n)+1. `OUT (n),A`: WZ_low = (n+1)&0xFF, WZ_high = A.
- `IN/OUT (C)`: WZ = BC+1.
- block `LDxR/CPxR/INxR/OTxR` and `LDI/…`: per-instruction WZ adjustments (table in
  `z80_core.c`).
- 16-bit `ADD/ADC/SBC HL,rp`: WZ = HL+1 (before the add).

## Undocumented opcodes
- **DD/FD half-index regs**: `IXH IXL IYH IYL` as operands of `INC/DEC/LD/ALU` when the
  base would use `H`/`L` (but not when the operand is memory `(HL)` → becomes `(IX+d)`).
- **DDCB/FDCB**: `op (IX+d)` for `z!=6` also writes the result to register `r[z]`.
- **`SLL`/`SL1`** (CB rot y=6): shift left, bit0 := 1.
- **ED undocumented**: `NEG`/`RETN`/`IM` duplicates across the ED page; `IN F,(C)`
  / `IN (C)` (ED70) sets flags, discards data; `OUT (C),0` (ED71, NMOS outputs 0).
- **Unstable**: none relied upon; any that depend on analog behavior are documented as
  out-of-scope in known-differences.

## Internal-register / timing quirks
- `LD A,I` / `LD A,R`: PF = IFF2, and there is a documented race where an interrupt
  arriving during the instruction can clear PF.
- `R` register: only bits 6:0 increment on each M1; bit 7 is preserved from the last
  `LD R,A`.
