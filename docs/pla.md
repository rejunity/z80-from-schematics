# PLA decode & control word

The decoder is a compact, ROM-like table that maps `(prefix_state, opcode)` to a
**control word** of named lines. The exact bit layout lives in `cmodel/z80.h`
(`z80_control_t`) and is mirrored by `localparam` bit positions in `rtl/z80_pla.v`;
this document explains the structure and the conventions.

## Opcode decomposition (Dinu fields)

Every opcode byte is split as:

```
  bit:  7 6 5 4 3 2 1 0
        [x] [ y ] [ z ]
            [p] q
  x = opcode[7:6]
  y = opcode[5:3]      p = y >> 1   (opcode[5:4])
  z = opcode[2:0]      q = y & 1    (opcode[3])
```

These fields index the standard selection tables, which is how the real PLA groups
decode. Using them keeps the table small and faithful instead of 256+ ad-hoc rows.

### Selection tables
- **r[z]** (8-bit reg by z, and y for dst): `0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A`.
  Under DD/FD prefix, `H`/`L`/`(HL)` become `IXH`/`IXL`/`(IX+d)` (resp. IY) — except
  that for the `(HL)` case the index register + displacement is used and H/L stay
  un-substituted when the real operand is memory (the classic DD/FD quirk).
- **rp[p]** (16-bit, SP variant): `0=BC 1=DE 2=HL 3=SP`. Under DD/FD, `HL→IX/IY`.
- **rp2[p]** (16-bit, AF variant for push/pop): `0=BC 1=DE 2=HL 3=AF`.
- **cc[y]** (condition): `0=NZ 1=Z 2=NC 3=C 4=PO 5=PE 6=P 7=M`.
- **alu[y]** (ALU op): `0=ADD 1=ADC 2=SUB 3=SBC 4=AND 5=XOR 6=OR 7=CP`.
- **rot[y]** (CB rotates/shifts): `0=RLC 1=RRC 2=RL 3=RR 4=SLA 5=SRA 6=SLL* 7=SRL`
  (`SLL`/`SL1` is undocumented).

## Instruction spaces (prefix state)

`prefix_state` ∈ { NONE, CB, ED, DD, FD, DDCB, FDCB }.

- **NONE**: base table. `x=0`: misc/16-bit/loads/rot-A; `x=1`: `LD r,r'`/`HALT`;
  `x=2`: `ALU A,r`; `x=3`: ret/jp/call/push/pop/rst/IO/EX/prefixes.
- **CB**: `x=0` rotates/shifts on r[z]; `x=1` `BIT y,r`; `x=2` `RES y,r`;
  `x=3` `SET y,r`.
- **ED**: sparse — block ops (`x=2`, `z<4`, `y>=4`), `IN/OUT (C)`, 16-bit `ADC/SBC HL`,
  `LD (nn),rp`/`LD rp,(nn)`, `LD I/R,A` & `LD A,I/R`, `NEG`, `RETI/RETN`, `IM`.
  All other ED opcodes are NOPs (documented as such).
- **DD/FD**: act as IX/IY modifiers on the following opcode. A DD/FD before another
  DD/FD/ED is "ignored" (only the last index prefix wins); the chain costs one M1 each.
- **DDCB/FDCB**: after DD/FD then CB, the order is `DD CB d op`: the displacement `d`
  and the CB opcode are read as operand reads; the operation targets `(IX+d)` and,
  for `z != 6`, *also* copies the result into register r[z] (undocumented).

## Decoded control word

The PLA produces a `z80_control_t` (see `cmodel/z80.h` for the exact struct; the RTL
inlines an equivalent bundle of named signals into `rtl/z80_core.v`). Conceptual
field groups:

| Group           | Field(s) (C names)                  | Purpose                                                          |
|-----------------|-------------------------------------|------------------------------------------------------------------|
| Execution kind  | `exec` (`EXEC_*`)                   | which executor handler runs (LD_R_R, ALU_R, JP, etc.)            |
| Sequencer       | `seq` (`SEQ_*`)                     | M-cycle template selector (see below)                            |
| Reg select      | `rf_src`, `rf_dst`                  | source / destination reg or pair (`y`, `z`, `rp_sel`, …)         |
| ALU             | `alu_op` (`ALU_*`)                  | ALU operation; carry-in is selected per `flag_mode`              |
| Flags           | `flag_mode` (`FM_*`)                | which flag-update rule fires (`docs/flags.md`)                   |
| Addressing      | `addr_src`, `wz_op`                 | 16-bit source for the bus + MEMPTR / WZ update rule              |
| Prefix / idx    | `idx`, `use_disp`                   | DD / FD index map (IX / IY) + need-displacement flag             |
| Condition       | `cc` (`CC_*`)                       | condition-code selector for JR cc / JP cc / RET cc / CALL cc     |
| Special         | `special` (`SPC_*`)                 | undocumented / edge-case hooks (DDCB copy, OUT (C),0, NEG, …)    |

### Sequence templates (`seq`)

The sequencer expands a template into M-cycles. Core templates (extended as coverage
grows), each starting after the M1 fetch of the opcode:

| Template      | M-cycles after fetch                       | Example opcodes                                              |
|---------------|--------------------------------------------|--------------------------------------------------------------|
| `SEQ_NONE`    | (none)                                     | `NOP`, `LD r,r'`, `ALU A,r`, `INC r` (reg), `EX`, `DI` / `EI`|
| `SEQ_IMM8`    | MRD (n)                                    | `LD r,n`, `ALU A,n`                                          |
| `SEQ_IMM16`   | MRD (lo), MRD (hi)                         | `LD rp,nn`, `JP nn` (+ exec)                                 |
| `SEQ_MRD_HL`  | MRD (HL)                                   | `LD r,(HL)`, `ALU A,(HL)`                                    |
| `SEQ_MWR_HL`  | MWR (HL)                                   | `LD (HL),r`, `LD (HL),n` (+ IMM8)                            |
| `SEQ_RMW_HL`  | MRD (HL), MWR (HL)                         | `INC` / `DEC (HL)`, CB ops on `(HL)`                         |
| `SEQ_JR`      | MRD (d) [+ 5T if taken]                    | `JR`, `DJNZ`, `JR cc`                                        |
| `SEQ_CALL`    | MRD (lo), MRD (hi), [MWR, MWR]             | `CALL`, `CALL cc`, `RST`                                     |
| `SEQ_RET`     | MRD (lo), MRD (hi)                         | `RET`, `RET cc`, `RETI` / `RETN`                             |
| `SEQ_PUSH`    | MWR (hi), MWR (lo)                         | `PUSH rp`                                                    |
| `SEQ_POP`     | MRD (lo), MRD (hi)                         | `POP rp`                                                     |
| `SEQ_IO`      | IORD / IOWR                                | `IN A,(n)`, `OUT (n),A`, `IN` / `OUT (C)`                    |
| `SEQ_IDX_D`   | MRD (d) then base template on `(IX+d)`     | DD / FD memory forms                                         |
| `SEQ_BLOCK`   | per block-instruction microflow            | `LDIR` / `CPIR` / `INIR` / `OTIR` …                          |

Internal-only extra T-states (e.g. the 5 padding T-states added to `IX+d` address
formation, or the 7-T-state `(HL)`-RMW write delay) are represented as `INTERNAL`
bus_op cycles in the template so the trace stays cycle-accurate.

## Table generation

`cmodel/z80_pla.c` builds the control rows from the decomposition above (not a 256-row
hand table): a small set of generators emit rows per group, with explicit overrides for
irregular opcodes (`x=0` misc column, ED specials, undocumented). `rtl/z80_pla.v`
implements the identical mapping as combinational logic over `{prefix_state, opcode}`.
Every line is documented in this file and exercised by `tests/common/test_pla.c`.
