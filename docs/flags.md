# Flags subsystem

The F register bit layout and per-operation update rules. Implemented as an explicit,
independently testable unit (`cmodel/z80_flags.c`, `rtl/z80_flags.v`). Documented
behavior follows Zilog UM0080; undocumented X/Y behavior follows die analysis + ZEXALL.

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
Decimal adjust of A using HF/CF/NF; see algorithm in `z80_flags.c`. Sets SF/ZF/YF/XF
from the adjusted A, PF=parity, CF per correction, HF per nibble carry/borrow, NF
unchanged. Pinned by DAA vectors + ZEXALL `<daa>`.

### `FLAG_SCF` / `FLAG_CCF`
```
SCF: CF=1  HF=0  NF=0
CCF: HF=old CF  CF=~old CF  NF=0
both: YF = A.5  XF = A.3      # NMOS "A"-based variant (see known-differences)
SF,ZF,PF unchanged
```

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
ZEXALL. Implemented in `z80_flags.c` and detailed there.

## Parity helper
`parity(x)` = 1 when the number of set bits in `x` is even (PF=1 means even parity).

All rules are exercised by `tests/common/test_flags.c` and cross-checked by ZEXALL.
