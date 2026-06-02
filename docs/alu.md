# ALU

Organized after Ken Shirriff's die analysis: the Z80 ALU is a **4-bit unit operated
twice** per 8-bit operation — low nibble first, then high nibble, with a carry chain
between the two passes. We model that structure explicitly rather than doing one opaque
8-bit `+`, so the C and RTL mirror the real datapath and the half-carry falls out of
the low-nibble pass naturally.

## Structure

```
   alu_op_a[7:0]   alu_op_b[7:0]   alu_cin
        |               |             |
        v               v             v
   +---------------------------------------+
   |   nibble pass 0 (bits 3:0)            |  -> res_lo[3:0], carry3  (= HF source)
   |   nibble pass 1 (bits 7:4) w/ carry3  |  -> res_hi[3:0], carry7  (= CF source)
   +---------------------------------------+
        |                         |
        v                         v
   alu_res[7:0]              alu_carry_out, alu_half_carry
```

- `alu_op`: `ADD ADC SUB SBC AND XOR OR CP` plus internal helpers used by the
  sequencer (`INC` and `DEC` reuse ADD/SUB with b=1 and a carry-in/flag policy;
  16-bit ops run the nibble unit four times: lo, hi of low byte, lo, hi of high byte,
  threading carry).
- Subtraction is performed as `a + (~b) + 1` (two's complement) so add and subtract
  share the adder; `HF`/`CF` are read from the inter-nibble and final carries with the
  sub-polarity applied (borrow = ~carry for the flag sense the Z80 reports).
- Logic ops (`AND/OR/XOR`) bypass the carry chain; `CP` is a `SUB` whose result is
  discarded for the register but used for flags (with the operand-sourced X/Y, see
  `docs/flags.md`).

## Decimal / rotate helpers

- **DAA** uses the nibble carries plus current `HF/CF/NF` to choose the `0x06`/`0x60`
  correction (algorithm in `z80_alu.c`), keeping the BCD adjust within
  the same flag-producing path.
- **Rotates/shifts** (`RLC/RRC/RL/RR/SLA/SRA/SLL/SRL` and the A-register fast forms)
  are a separate shifter feeding the same flag logic; the carry comes from the bit
  rotated out.

## 16-bit operations

`ADD/ADC/SBC HL,rp` and the address-path increment/decrement use a 16-bit add built
from the 4-bit unit (four passes) for `ADD/ADC/SBC HL`, while the dedicated address
incrementer/decrementer (`z80_addr`) handles PC/SP/R/`(rp)±` updates — matching the
Z80, where the IDU (increment/decrement unit) is separate from the main ALU.

## Verification

`tests/common/test_alu.c` exercises every op across boundary operands
(`00 01 7F 80 FE FF`) and all carry-in values, checking `alu_res`, `alu_carry_out`,
`alu_half_carry`, and the derived flags against hand-computed and reference values.
The nibble-twice structure is checked to produce identical results to a reference
8-bit computation for all 2^17 (a,b,cin) add/sub combinations.
