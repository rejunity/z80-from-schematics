# Unsimplify audit — remaining followups

Snapshot of the silicon-faithfulness audit items that are still pending
after the first sweep on the `unsimplify` branch. These are dropped from
the live task list while the test-expansion work happens; pick them back
up from this file when ready.

Source: `docs/simplifications.md`. Status legend in that file applies.

## Completed (committed on `unsimplify`)

| ID  | Title                                                          | Commit     |
|-----|----------------------------------------------------------------|------------|
| D1  | Bump f_modified on EX AF,AF' so Q tracks swapped F             | `e296d63`  |
| B1  | Verify M1 deassert phase against perfectz80 — no simplification | `1215ab6` |
| B2  | NMI sampled at T_last.P per UM0080                             | `7ad9f1d`  |
| B3  | INT  sampled at T_last.P per UM0080                            | `7ad9f1d`  |
| E2  | Name WAIT sample edge + latch explicitly (structural cleanup)  | `780a524`  |
| A2  | Move inline 8-bit flag formulas into z80_alu                   | `7cdee5b`  |
| —   | C exec_step restructured to mirror V (mux + single u_alu call) | `6cfecb8`  |

## Pending — to resume later

Order below = leverage × risk (small → large).

### C1. Stop forcing `rf = 0xFFFF` on reset
- **Where**: `cmodel/z80_core.c` `z80_reset()`; `rtl/z80_core.v` synchronous reset block.
- **Now**: forces all 13 `rf` pairs to `0xFFFF` "for deterministic C↔RTL compare".
- **Silicon**: leaves them UNDEFINED at power-up. Spec only guarantees `PC=0, I=0, R=0, IFF1=IFF2=0, IM=0` (Zilog UM0080).
- **Fix**: drop the force; RTL leaves undefined regs at `'X`; external harnesses already poke their start state.
- **Update**: `docs/known-differences.md` row 1 → resolved.

### A1. Introduce `z80_idu` block
- **What**: route inline 16-bit address arithmetic through a dedicated IDU module.
- **Now (both C and V)**: `PC+1`, `SP±1`, `rp±1`, R-refresh (low-7-bit-only), `PC+disp` all done inline as `+ 16'd1` etc.
- **Silicon**: a separate Incrementer/Decrementer Unit on the address path handles every flag-free 16-bit arith.
- **Files**: `cmodel/z80_idu.c` (NEW; or inline in `z80_core.c` per no-new-.h rule); `rtl/z80_idu.v` (NEW).
- **Scope**: large — touches every PC++, SP±1, rp±1, R-refresh, PC+disp call site.

### A3. 16-bit ADD/ADC/SBC HL,rp via byte-twice z80_alu
- **Now**: inline 32-bit add in both C and V (`rtl/z80_core.v` line ~279 comment confesses: "16-bit ADD HL,rp (inline; IDU/ALU detail handled in C model too)").
- **Silicon**: 4-bit ALU is reused twice — low byte then high byte, carry chained.
- **Fix**: add `FLAG_ADD16` / `FLAG_ADC16` / `FLAG_SBC16` modes to `z80_alu` that internally call the existing 8-bit nibble-twice path twice with explicit carry forwarding.

### A4. Register file restructure (GP banks + sysreg + IR/I/R)
- **Now**: flat `rf[0..12]` array; `EX AF,AF'` and `EXX` do byte-swaps.
- **Silicon**: hardware bank-select muxes (no data movement on EX/EXX); separate `IX`/`IY`; system regs `PC`/`SP`/`WZ`; `IR` block with `I`/`R`.
- **Files**: `cmodel/z80_regfile.{c,v}` (extend existing); `cmodel/z80_sysreg.{c,v}` (probably new logical blocks within existing files — ask before adding `.h`).
- **Scope**: ~127 references in C, ~310 in V to migrate. Large.

### E1. Internal data-bus segments + switches
- **Now**: everything reads/writes `rf` directly; no `db1`/`db2`/`sb` model.
- **Silicon**: internal `db1[7:0]` / `db2[7:0]` / `sb[15:0]` segments with directional switches (`sw_1u/d`, `sw_2u/d`, `sw_4d`) gating data flow between pads, ALU, regfile, IDU, address latch.
- **Modeling**: one-hot muxes with synth-time bus-contention assertions (no literal internal tri-state).
- **Scope**: largest. Depends on A1/A4 having landed.

### F. Audit-only — validate-or-resolve
Pending silicon validation before judging as a simplification:

- **DDCB/FDCB displacement compute folding**: we fold 2T IX+d compute into the N-immediate read for `EXEC_LD_M_N` to save an M-cycle (matches FUSE's 19T). Verify against gate-level / sigrok whether silicon does this or runs an internal 5T cycle as the spec implies.
- **Block-op M-cycle break**: LDI/CPI/INI/OUTI cycle layout uses `BUSOP_INTERNAL` for the "wait" parts. Verify the silicon's exact break against a real-silicon trace.
- **suppress_decode hack**: post-reset and during M1 of NMI/INT-ack we set `suppress_decode = true` to discard the latched opcode. Real chip's NMI/INT M1 doesn't *decode* because IORQ is asserted instead of MREQ. Cross-check against perfectz80 / KC85 captures.

## Reference

- `docs/simplifications.md` — the full audit (this file is just the followup tail).
- `docs/known-differences.md` — `accepted` / `watching` / `resolved` rows.
- `docs/timing.md` — phase-by-phase model that the audit's "B" items map to.
- Source-of-truth for the silicon-faithful target: Baltazar Studios' Z80 Explorer
  (`exec_matrix.vh`) and the Visual Z80 netlist (via perfectz80).
