# Unsimplifications audit

Catalog of every modeling shortcut found in the C model and the Verilog RTL
that deviates from the real Z80 silicon (Zilog UM0080 / die reverse-engineering /
perfectz80 / Z80 Explorer). The `unsimplify` branch removes them.

Sorted by category. Each entry: **where**, **what we do now**, **what the
silicon does**, **fix plan**, **status**.

## A. Datapath blocks the silicon has, that we collapsed

### A1. Inline 16-bit arithmetic (should be IDU)

The real Z80 has a separate **IDU** (Incrementer/Decrementer Unit) — a 16-bit
unit on the address path. It handles every 16-bit address arithmetic that does
not produce flags:

- PC + 1 (every fetch + every operand byte read)
- SP ± 1 (PUSH / POP / CALL / RET / RST / NMI / INT)
- rp ± 1 (`INC rp` / `DEC rp` / block-op pointer updates HL+/-1, BC-1, DE+/-1)
- R-register refresh increment (low 7 bits only — bit 7 preserved)
- PC + signed displacement (`JR`, `JR cc`, `DJNZ`)

We do all of these as inline `+ 16'd1` / `- 16'd1` / `+ {{8{disp[7]}}, disp}` in
`rtl/z80_core.v` and `cmodel/z80_core.c`. The wip-like-z80-microarch branch had
a `z80_idu` module but it never merged.

**Fix**: add `cmodel/z80_idu.c` + `rtl/z80_idu.v`, route all the inline 16-bit
add/sub-1 + disp-add through it.

**Status**: pending.

### A2. Inline 8-bit ALU ops outside `z80_alu`

EXEC_NEG, EXEC_LD_A_IR, EXEC_IN_C, EXEC_RRD / EXEC_RLD, EXEC_BLOCK
(LDI/CPI/INI/OUTI variants) compute their result and flags **inline** in the
sequencer (`rtl/z80_core.v` / `cmodel/z80_core.c`) instead of going through the
ALU. This was deferred during the c-like-verilog refactor; the new ALU modes
exist on the wip branch but never merged.

**Fix**: extend `z80_alu` with `FLAG_NEG`, `FLAG_LD_A_I`, `FLAG_IN`, `FLAG_RRD`,
`FLAG_RLD`, `FLAG_BLOCK_LD`, `FLAG_BLOCK_CP`, `FLAG_BLOCK_IO` modes. Move the
inlined formulas there. The sequencer calls `z80_alu(...)` like everything else.

**Status**: pending.

### A3. Inline 16-bit ADD/ADC/SBC HL,rp (should be byte-twice ALU)

`EXEC_ADD_HL_RP` / `EXEC_ADC16` / `EXEC_SBC16` compute the 16-bit result inline
(`rtl/z80_core.v` line 279 has a comment: *"16-bit ADD HL,rp (inline; IDU/ALU
detail handled in C model too)"*).

The real Z80 does these byte-at-a-time on the 4-bit ALU: low byte first, then
high byte with the carry chained. Our 8-bit `z80_alu` already implements the
byte-twice structure for 8-bit ADD/SUB; the 16-bit ops should be two ALU passes
with explicit `cf2` / `hf2` carry forwarding.

**Fix**: add `FLAG_ADD16` / `FLAG_ADC16` / `FLAG_SBC16` modes to `z80_alu`,
either as two passes driven by the sequencer or as one 16-bit-shaped mode that
internally calls the 8-bit nibble code twice. Either way the inline
`rf[hlp] + rf[rp_sel_w]` in core goes away.

**Status**: pending.

### A4. Register file as flat 13×16 array

Real Z80 has:
- **GP register file**: BC, DE, HL, AF + their alternates (BC', DE', HL', AF'),
  with hardware bank swaps for `EX AF,AF'` and `EXX`.
- **IX, IY**: separate from the GP file.
- **System register file**: PC, SP, WZ, IR (and the I, R bytes inside IR).

We model the whole thing as `rf[0..12]` — a flat array. `EX AF,AF'` and `EXX`
do explicit byte swaps in C / non-blocking swaps in Verilog.

**Fix**: split into `z80_regfile` (GP) + `z80_sysreg` (PC/SP/WZ) + `z80_ir`
(I, R, IR), with hardware-style bank-select for EX/EXX (a single mux output,
no data movement). Already prototyped on `wip-like-z80-microarch` but not
merged.

**Status**: pending.

## B. Timing / phase shortcuts

### B1. M1 deassert phase (RESOLVED — no simplification at our resolution)

Direct gate-level cross-check (`make perfectz80`, which steps perfectz80's
transistor-level netlist one half-cycle at a time and compares pin samples)
shows perfectz80 reads `m1_n=0` at T2.N and `m1_n=1` at T3.P — exactly what
our model produces. The "real chip deasserts late in T2" description in some
references is the continuous-time analog edge; at the half-cycle sample
resolution at which we model (and at which the gate-level reference operates),
M1 is high starting T3.P, and our model matches.

The model is silicon-faithful at the granularity at which it models. No code
change. `docs/known-differences.md` row 4 is now marked resolved with the
verification path noted.

**Status**: resolved.

### B2. NMI sampling on every phase step

NMI is edge-triggered. We latch a falling edge on `pins.nmi_n` on EVERY
`z80_phase_step()` call (`cmodel/z80_core.c`). The silicon samples NMI at a
specific phase — the rising edge of the last T-state of the last M-cycle of
the current instruction. Same for the Verilog (`always @*` NMI capture).

The current model: any low pulse during the instruction sets `nmi_pending`,
and acceptance happens at instruction boundary. The silicon: the pulse has to
straddle a specific sample point.

**Fix**: gate the edge capture to the right phase. Cross-check with
`prog8_nmi` trace + the FUSE NMI subtest if any.

**Status**: pending.

### B3. INT sampling at instruction boundary only

Same family: `INT` is level-triggered, sampled at the rising edge of the last
T-state of the last M-cycle. We sample once per instruction at `begin_next()`.
Corner timing (level deasserting mid-instruction) may differ.

**Fix**: same approach as B2.

**Status**: pending.

### B4. Latch + deassert co-phase (RESOLVED)

Used to combine `data latch` and `MREQ/RD deassert` at T3.N. Fixed in
`97c09fb` — latch now at T_last.P, deassert at T_last.N.

**Status**: resolved.

## C. Reset state

### C1. Force registers to 0xFFFF on reset

`cmodel/z80_core.c` `z80_reset()`: forces all 13 rf pairs to 0xFFFF "for
deterministic C↔RTL compare". Real silicon leaves these UNDEFINED at power-up
(though reset does force `PC=0, I=0, R=0, IFF1=IFF2=0, IM=0` per Zilog UM0080).

**Fix**: in RTL, leave undefined regs at `'X` (synthesis-friendly default). In
C, leave them at whatever `z80_init()` (which is `memset` 0) gave them.
External test harnesses (FUSE / lockstep / compare) explicitly poke their start
state anyway. Document in `docs/known-differences.md` row 1 as resolved.

**Status**: pending.

## D. Q register

### D1. EX AF,AF' doesn't bump f_modified

`EXEC_EX_AF` does `rf[AF] ↔ rf[AF2]` directly without going through `setF`, so
`f_modified` stays false → at instruction end Q = 0 instead of (new F).

Per Sean Young's *Undocumented Z80 Documented* §4.1, Q is set to F whenever F
is modified. EX AF,AF' changes F (by swap) and should bump Q.

**Fix**: in EX AF,AF' handler, set `f_modified = true` (or call `setF` with
the swapped value). Mirror in RTL.

**Status**: pending — write a small test to confirm SCF after EX AF gives
expected X/Y from `(A | new_F)`.

## E. Internal bus / external pin protocols

### E1. No internal data bus segments

Real Z80 has internal `db1` / `db2` / `sb` bus segments with directional
switches (sw_1/2/4) gating data flow between the pad ring, ALU, register
file, IDU, and address latch. We model none of it — everything reads/writes
`rf` array slots directly.

**Fix**: long-term, model the bus segments + tri-state-to-mux conversion (no
literal tri-state, synthesizable). This is what the `wip-like-z80-microarch`
branch started. Plumbing-only on the C side; meaningful on the RTL side for
gate-level fidelity.

**Status**: pending — large.

### E2. WAIT sample point not phase-precise

`cmodel/z80_core.c` `is_wait_phase()`: samples `wait_n` at T2.N for M1/MRD/MWR
and T3.N for IORD/IOWR. Per Zilog UM0080 these match the spec sample
points. But our implementation samples *every* phase and stalls if wait_n is
low and the phase predicate matches. The silicon samples ONCE at the spec
edge and latches the decision until the next sample point.

**Fix**: convert is_wait_phase to a per-cycle latch rather than per-phase
continuous polling. Probably no observable difference, but the model becomes
silicon-faithful.

**Status**: pending (low impact, low risk).

## F. Other audit candidates (not yet validated)

- **DDCB/FDCB displacement compute**: we fold the 2T IX+d compute into the
  N-immediate read for `EXEC_LD_M_N` to save an M-cycle (mentioned in row 11
  of known-differences as the fix to match FUSE's 19T). Worth confirming the
  silicon actually does that vs the conceptual "internal 5T cycle" the
  Zilog manual implies.

- **Block-op M-cycle structure**: LDI/CPI/INI/OUTI have a quirky cycle
  layout. We use INTERNAL cycles for the "wait" parts; real silicon's exact
  break between M-cycles needs verifying against a real-silicon trace.

- **Suppress-decode hack**: post-reset and during M1 of NMI/INT-ack we set
  `suppress_decode = true` to discard the latched opcode. The real chip does
  this differently — the M1 cycle of an interrupt ack doesn't even *decode*
  because IORQ is asserted instead of MREQ. Worth re-examining.

## Plan

Work order (roughly by leverage × risk):

1. **A1**: introduce `z80_idu` (well understood, prototype already exists).
2. **A2**: move inline 8-bit ALU ops into `z80_alu` modes.
3. **A3**: 16-bit ALU ops via byte-twice in `z80_alu`.
4. **D1**: Q after EX AF (small).
5. **B1**: M1 deassert phase to match silicon.
6. **B2, B3**: NMI / INT sample phase precision.
7. **C1**: reset-state un-force.
8. **A4**: register file restructure (large).
9. **E1**: internal bus segments (largest; defer until A1–A4 done).
10. **E2**: WAIT sample latch (cleanup).

After each: full gate run (`make all-tests`) — must stay 100 % across every
existing oracle (FUSE, fuse_rtl, compare, lockstep, silicon_cycles,
silicon_async, perfectz80).
