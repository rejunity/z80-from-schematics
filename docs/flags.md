# Flags subsystem

The F register bit layout and per-operation update rules. Lives inside the ALU
module (`cmodel/z80_alu.c` and `rtl/z80_alu.v`) — the per-class flag computers
stay exposed as helpers in C so they remain independently unit-testable.
Documented behavior follows Zilog UM0080; undocumented X/Y behavior follows die
analysis + ZEXALL.

## F register layout

```
bit:  7   6   5   4   3   2   1   0
      S   Z   Y   H   X  P/V  N   C
```

- **SF** (bit7): sign = result bit 7.
- **ZF** (bit6): zero = (result == 0).
- **YF** (bit5): *undocumented*. Normally a copy of **result bit 5**.
- **HF** (bit4): half-carry (carry/borrow between bit 3 and bit 4).
- **XF** (bit3): *undocumented*. Normally a copy of **result bit 3**.
- **PF** (bit2): parity (logic/rotate) **or** signed overflow (add/sub).
- **NF** (bit1): add/subtract flag (0 after add-like, 1 after sub-like).
- **CF** (bit0): carry/borrow.

> Convention note: we name bit5 `YF` and bit3 `XF` (X = bit3, Y = bit5), matching the
> brief's "result bits 3 and 5". Some references swap the letters; the bit positions
> are what matter.

## Per-operation rules (`ctl_flag_mode`)

### `FLAG_ADD8` — ADD/ADC A,src  (cin = 0 for ADD, CF for ADC)
```
res9 = a + b + cin
SF=res.7  ZF=(res8==0)  YF=res.5  XF=res.3
HF = ((a&0xF)+(b&0xF)+cin) & 0x10
PF = (~(a^b) & (a^res8) & 0x80) != 0     # signed overflow
NF = 0
CF = res9 & 0x100
```

### `FLAG_SUB8` — SUB/SBC A,src / CP src / NEG  (cin = 0 / CF)
```
res9 = a - b - cin
SF=res.7  ZF=(res8==0)  HF = ((a&0xF)-(b&0xF)-cin) & 0x10
PF = ((a^b) & (a^res8) & 0x80) != 0
NF = 1   CF = res9 & 0x100
YF=res.5  XF=res.3
# CP exception: YF/XF copied from the OPERAND b, not from res8.
```

### `FLAG_LOGIC` — AND/OR/XOR
```
SF=res.7  ZF=(res==0)  YF=res.5  XF=res.3
HF = (op==AND) ? 1 : 0      PF = parity(res)   NF=0   CF=0
```

### `FLAG_INC8` / `FLAG_DEC8` — INC r / DEC r (8-bit)
```
INC: HF=((r&0xF)==0xF)  PF=(r==0x7F)  NF=0
DEC: HF=((r&0xF)==0x00) PF=(r==0x80)  NF=1
both: SF=res.7 ZF=(res==0) YF=res.5 XF=res.3 ; CF unchanged
```

### `FLAG_ROT_A` — RLCA/RRCA/RLA/RRA
```
CF = bit rotated out   HF=0   NF=0
SF,ZF,PF unchanged     YF=A_res.5  XF=A_res.3
```

### `FLAG_ROT` — CB rotates/shifts (RLC/RRC/RL/RR/SLA/SRA/SLL/SRL)
```
CF = bit shifted/rotated out
SF=res.7 ZF=(res==0) YF=res.5 XF=res.3   HF=0  PF=parity(res)  NF=0
```

### `FLAG_BIT` — BIT b,src
```
t = src & (1<<b)
ZF = (t==0)   PF = ZF   SF = (b==7) ? (t!=0) : 0
HF = 1   NF = 0   CF unchanged
YF/XF source:
  BIT b,r       -> r.5 / r.3
  BIT b,(HL)    -> WZ_high.5 / WZ_high.3   (MEMPTR)
  BIT b,(IX+d)  -> (addr_high).5 / .3      (= WZ_high)
```

### `FLAG_ADD16` — ADD HL,rp / ADD IX,rp
```
res17 = HL + rp
HF = carry from bit 11   CF = carry from bit 15   NF=0
YF = res.13   XF = res.11   (i.e. high-byte bits 5/3)
SF,ZF,PF unchanged
```

### `FLAG_ADC16` / `FLAG_SBC16` — ADC/SBC HL,rp (ED)
```
SF=res.15  ZF=(res16==0)  HF=carry/borrow from bit11
PF = 16-bit signed overflow   NF = (SBC?1:0)   CF = carry/borrow from bit15
YF=res.13  XF=res.11
```

### `FLAG_DAA`
Decimal adjust of A using HF/CF/NF; see algorithm in `z80_alu.c`. Sets SF/ZF/YF/XF
from the adjusted A, PF=parity, CF per correction, HF per nibble carry/borrow, NF
unchanged. Pinned by DAA vectors + ZEXALL `<daa>`.

### `FLAG_SCF` / `FLAG_CCF`
```
SCF: CF=1  HF=0  NF=0
CCF: HF=old CF  CF=~old CF  NF=0
both: YF,XF = ((Q ^ F) | A) & 0x28   # Zilog NMOS: A after a flag writer, else A | F ("Q" below)
SF,ZF,PF unchanged
```
Q is the F the previous instruction wrote, or 0 if it wrote none: right after
a flag-writing instruction X / Y come from A alone, otherwise from A | F
(docs/undocumented.md; docs/known-differences.md rows 2 and 29-31).

### `FLAG_CPL`
```
A = ~A   HF=1  NF=1   YF=A.5 XF=A.3 ; SF,ZF,PF,CF unchanged
```

### `FLAG_NEG`  (= SUB with a=0,b=A; uses FLAG_SUB8 path)

### `FLAG_BLOCK_LD` — LDI/LDD/LDIR/LDDR
```
n = A + transferred_byte
HF=0  NF=0  PF=(BC_after != 0)
YF = n.1   XF = n.3       # note YF from bit1 of (A+byte), XF from bit3
SF,ZF,CF unchanged
```

### `FLAG_BLOCK_CP` — CPI/CPD/CPIR/CPDR
```
res = A - (HL)   (HF from this subtract)
SF=res.7  ZF=(res==0)  HF as sub  NF=1  PF=(BC_after != 0)  CF unchanged
n = res - HF
YF = n.1   XF = n.3
```

### `FLAG_BLOCK_IO` — INI/IND/OUTI/OUTD/…
Per documented block-IO flag rules (involve a temp sum with C±1 / B); pinned by
z80test (INI, IND, INIR, INDR, OUTI, OUTD, OTIR, OTDR) and FUSE -- ZEXALL has no
I/O tests. Implemented in `z80_alu.c` and detailed there.

### Block repeats
When LDIR / LDDR / CPIR / CPDR / INIR / INDR / OTIR / OTDR repeat, the extra
5-T internal M-cycle rewrites YF / XF from PC bits 13 / 11, and INxR / OTxR
also HF and PF (David Banks 2018; docs/undocumented.md). The C model's
`blk_rep_f`, the RTL alike.

## Q

Q is the emulators' name (redcode/Z80, Patrik Rak's `z80ccf`) for what SCF
and CCF take X and Y from besides A: F when the instruction before them wrote
the flags, else 0. X and Y come from `A` after a flag writer and from `A | F`
after anything else. The models keep it as a register (`q` in `cmodel/`,
`reg_q` in `rtl/`): F where an instruction's F write lands, 0 at the end of
one that writes none, and 0 at a prefix.

The die has no Q register. In perfectz80 (the Visual Z80 netlist):

- The flags an instruction writes wait for the next M1 -- an opcode fetch,
  a prefix's, an INTA, the fetch an NMI discards, the one that restarts
  after a RESET. From that M1's T1.N the latch of nodes 541 / 1587 holds
  them pending, and in its T3.N the AF word line (node 677) opens with
  `rl_wr` (678): they go into F (`reg_f*` of the live bank, `ex_af`).
- An M1 with nothing pending opens the AF word line in the same half-cycle
  to read AF onto the u bus (`r_u`, 692, low).
- SCF and CCF form X and Y in their M1's T4 from A and what that access
  left: `A | F` after the read, `A` after the write.
- Node 3273 goes low with the latch and stays low until the next M1's
  T1.N: through the instruction that M1 fetched, it says "Q = F".

So Q = F means "this M1 wrote the flags left pending", and every M1 uses
the pending flags up: DD 37 and FD 3F take X and Y from `A | F` whatever
came before, and so does an SCF after EX AF,AF', EXX, POP AF or an
interrupt response, none of which writes the flags. The flags stay pending
through a RESET whose cut ends after the T1 of the M1 they wait for and
before its T3.N writes them -- an INTA's, by its second automatic Tw -- and
through another cut on the T1 of the M1 that restarts (its T1.N takes no
new ones): that M1, at 0000, writes them (F is then the cut's f_cut,
docs/timing.md "Register write-back"), and a CCF there takes X and Y from
A. A cut anywhere else leaves none pending.

`make event_sweep` compares Q with the die in every case: perfectz80's trace
has 3273 (as P) next to F and WZ, and the die's Q in an M1 is F where P is
set in its T4.P, else 0 (docs/event-sweep.md "State"). A CCF or SCF
predicted from that Q matches what the die computes in every one of the
3,300 the `q_*` groups run.

## Parity helper
`parity(x)` = 1 when the number of set bits in `x` is even (PF=1 means even parity).

All rules are exercised by `tests/common/test_flags.c` and cross-checked by ZEXALL,
z80test and FUSE.
