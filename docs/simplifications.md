# Unsimplifications audit

Catalog of the modeling shortcuts found in the C model and the Verilog RTL
where they deviate from the real Z80 silicon (Zilog UM0080 / die
reverse-engineering / perfectz80 / Z80 Explorer), and what became of each.
The audit started on the `unsimplify` branch; the items marked done or
resolved have landed on main since.

Sorted by category. Each entry: **where**, **what we do now**, **what the
silicon does**, **fix plan**, **status**. A shortcut here is a difference
in how the model is built, not in what it does: the pins and the
registers a program sees match perfectz80 (docs/known-differences.md
lists where they don't).

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

The C model does all of these as inline `+ 1` / `- 1` / `+ (int8_t)disp` in
`cmodel/z80_core.c`; the RTL did too, in `rtl/z80_core.v`, until
optimise-luts gave it an IDU (`rtl/z80_addr.v`, below). The
wip-like-z80-microarch branch had a `z80_idu` module that never merged.

**Fix**: one IDU in each, every inline 16-bit add/sub-1 and displacement add
routed through it.

**Status**: RTL done (optimise-luts): `rtl/z80_addr.v` has one IDU on the
address-side bus for every 16-bit +/-1 (PC+1, SP+/-1, rp+/-1, block pointer
steps, MEMPTR +1 rules) and, next to it, one effective-address adder for
PC+e / IX+d. Block instructions use the IDU on T-states where the address
latch holds, like the die (table in `docs/architecture.md` section 3). R's
7-bit refresh increment keeps its own small incrementer (on the edge that
ends the refresh). C model: still inline arithmetic.

### A2. Inline 8-bit ALU ops outside `z80_alu`

EXEC_NEG, EXEC_LD_A_IR, EXEC_IN_C, EXEC_RRD / EXEC_RLD and EXEC_BLOCK
(LDI/CPI/INI/OUTI variants) used to compute their result and flags
**inline** in the sequencer instead of going through the ALU.

**Fix**: `z80_alu` modes `FLAG_NEG`, `FLAG_LD_A_I`, `FLAG_IN`, `FLAG_RRD`,
`FLAG_RLD`, `FLAG_BLOCK_LD`, `FLAG_BLOCK_CP`, `FLAG_BLOCK_IO`.

**Status**: done, in both: the C model's `alu_eval` (`cmodel/z80_core.c`)
and the RTL's ALU select (`rtl/z80_core.v`) pick these modes and call
`z80_alu` / `rtl/z80_alu.v` like every other instruction
(`cmodel/z80.h` lists each mode's port use).

### A3. Inline 16-bit ADD/ADC/SBC HL,rp (should be byte-twice ALU)

`EXEC_ADD_HL_RP` / `EXEC_ADC16` / `EXEC_SBC16` compute the 16-bit result in
one step: `do_add16` / `do_adc16` in `cmodel/z80_core.c`, a 16-bit adder in
`rtl/z80_core.v`.

The real Z80 does these byte-at-a-time on the 4-bit ALU: low byte first, then
high byte with the carry chained, in its two internal M-cycles of 4 and 3
T-states (which the models now keep, known difference 16; a RESET cut
between them leaves the low byte written, row 19). Our 8-bit `z80_alu` already implements the
byte-twice structure for 8-bit ADD/SUB; the 16-bit ops should be two ALU passes
with explicit `cf2` / `hf2` carry forwarding.

**Fix**: add `FLAG_ADD16` / `FLAG_ADC16` / `FLAG_SBC16` modes to `z80_alu`,
either as two passes driven by the sequencer or as one 16-bit-shaped mode that
internally calls the 8-bit nibble code twice. Either way the inline
`rf[hlp] + rf[rp_sel_w]` in core goes away.

**Status**: partly: the RTL has one shared 16-bit adder/subtractor (with the
half-carry taken from the carry into bit 12) for all three instructions, next
to the ALU; it is still one pass, not byte-twice through `z80_alu`. The C
model computes the result in one pass too, then writes it on the die's
edges: the low byte in the first internal M-cycle, the high byte in the
second, F in the next M1 (`a16_hold`).

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

**Status**: RTL done (optimise-luts): `rtl/z80_regfile.v` holds the pairs
behind four exchange flip-flops (EXX bank, AF bank, DE/HL swap per bank);
EX / EXX flip them, no data moves. WZ is the operand latch as on the die,
so the old `tmpl` / `tmp16` temporaries are gone. PC / SP / WZ are loads of
the same file rather than a separate `z80_sysreg`; I and R stay in the core.
C model: still copies on EX / EXX (`wb_fire`, in the next M1 as the die
flips its flip-flops).

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

### B2. NMI sampling (RESOLVED)

NMI is edge-triggered. `z80_phase_step()` latches a falling edge of `nmi_n`
in every phase (`nmi_pending`), except while RES2 is set and during the NMI
response itself, where the die loses an edge; the decision is the sample
at the rising edge of the last T-state of the M-cycle (`nmi_sampled`, with
INT's). The RTL takes a pulse of any width through `z80_top`'s pulse
catcher (`nmi_hold`), as the die's NMI flip-flop does. Pinned by the event
sweep's `nmi`, `nmi_short`, `nmi_reentry` and `reset_nmi` groups and the
pin scenarios `prog8_nmi`, `prog10_halt_nmi`, `prog19_nmi_in_int`
(known differences 17, 24, 27).

**Status**: resolved.

### B3. INT sampling (RESOLVED)

INT is level-triggered, sampled at the rising edge that starts the last
T-state of the M-cycle (PS0178 Figure 8, TsINT / ThINT; perfectz80):
`int_sampled` in the C model, `z80_top`'s `int_n_rise` in the RTL. Pinned by
the event sweep's `int`, `im2` and `im0` groups.

**Status**: resolved.

### B4. Data latch and strobe edges (RESOLVED)

The die takes an opcode or an INTA byte at the rising edge that ends T2 (or
the last Tw) from the bus in its low half, and read data at T3's falling
edge from the bus in its high half; MREQ / RD rise on their own edges
(T3.P in an M1, T3.N in a read). The C model's `is_latch_phase()` and the
RTL's `latch_pos` / `latch_neg` take those edges (known difference 28,
the event sweep's `latch` group).

**Status**: resolved.

## C. Reset state

### C1. Register values at power-on and after RESET (RESOLVED)

The model used to force every register pair to 0xFFFF on reset "for
deterministic C↔RTL compare". perfectz80 comes up with every pair at 5555
and keeps every register across RESET; only PC, I, R, IFF1 / IFF2 and IM
are cleared. The power-on reset (`z80_reset()`, RTL `por_n`) now loads 5555
and RESET keeps every register, WZ and the exchange flip-flops (known
difference 1, docs/timing.md "RESET"). The real NMOS part comes up with
random registers; the ASIC chip ties `por_n` off.

**Status**: resolved.

## D. Q register

Q is the F the previous instruction wrote, or 0 (docs/undocumented.md);
SCF / CCF take X / Y from `A | (F XOR Q)`. The die has no Q register: Q
is whether the M1 that starts an instruction wrote flags the one before
left pending (docs/flags.md "Q"). The models keep Q as a register (`q`,
`reg_q`); three corners differed from the die, found with perfectz80 on
2026-10-24 and fixed the same day.

### D1. EX AF,AF' sets Q (RESOLVED — known difference 29)

The models ended EX AF,AF' with Q = the F it swaps in (`wb_defer` in
`cmodel/z80_core.c`, `reg_q_n` in `rtl/z80_core.v`, the `EXAF` step kind in
`cfast/gen_fast.py`); the die leaves Q = 0, so a SCF / CCF after it takes
X / Y from A | F. An earlier version of this entry called the bump
correct because z80full's test 129 passes -- but z80full doesn't follow
EX AF,AF' with a CCF. z80ccf does, and failed its test 129 (CRC 3E5C2221,
expected 4CFA32A0).

**Fix**: Q = 0 at the end of EX AF,AF'. z80ccf passes 160 / 160.

**Status**: resolved.

### D2. A prefix keeps Q (RESOLVED — known difference 30)

A prefix's M1 takes the flags left pending and writes none: Q = 0 for the
opcode after DD, FD, ED or CB. The models carried Q through the prefix, so
`DD 3F` / `FD 37` after a flag write took X / Y from A alone.

**Fix**: Q = 0 when a prefix is decoded (`set_prefix_from_ir`, the RTL's
prefix step).

**Status**: resolved.

### D3. A RESET cut and Q (RESOLVED — known difference 31)

The flags left pending survive a RESET whose cut ends from the T2 of the
M1 they wait for to the T3 whose low half writes them (an INTA's: to its
second automatic Tw) -- the T1.N took them -- and another cut on the T1 of
the M1 that restarts; that M1 writes them (F as `f_cut` left it, row 19):
Q is that F. Any other cut leaves none pending: Q = 0. The models
left Q as it was at any cut (`if (!cut)` in `wb_fire_op` / `wb_fire` and the
RTL's fire).

**Fix**: Q = the F the fire leaves (0 for a CPI / INI / OUTI cut on T1,
which writes none), and Q = 0 at any cut outside that window.

**Status**: resolved.

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

**Status**: partly: the RTL has the 16-bit address-side bus (`abus` in
`rtl/z80_addr.v`) between the register file, the IDU and the address latch;
the 8-bit data side is still a set of selects (register byte read, ALU
result, data latch) rather than modelled db1/db2 segments.

### E2. WAIT sample point

Resolved: the C model samples `wait_n` on entry to T2.N for M1/MRD/MWR or
the automatic Tw.N (T3.N in the model) for IORD/IOWR, or the second
automatic Tw.N (state 4.N) for INTA, then retains that
decision through the following phase advance. The RTL samples `wait_n` on
the falling edge of that T-state (`rtl/z80_top.v`, `wait_n_neg`) and uses the
saved bit on the following rising edge.

**Status**: resolved; `make bus_controls` checks short pulses on either side
of the falling-edge sample.

## F. Other audit candidates

### F1. Block-instruction flags and the z80test failures (RESOLVED)

Patrik Rak's z80test used to fail INI / IND (z80doc and z80full 098 /
099), INIR→NOP' / INDR→NOP' (z80memptr and z80full 102 / 103), SCF / CCF
(z80full 001 / 002, then 007 SCF+CCF) and LDIR→NOP' / LDDR→NOP' (z80full
089 / 090) -- the same CRCs as superzazu, floooh/chips and redcode, so
first taken for a corner every emulator misses (`make z80test` carried
baselines of 2 / 2 / 10 failures). Each had its own cause (known
differences 12 and 13):

- **INI / IND**: the port value. Rak's CRCs were captured on a 48K
  Spectrum, whose ULA answers an idle read with `(addr & 1) ? FF : BF`;
  the runners now return that (`io_ula_idle` in `cmodel/z80_sim.h`).
- **The repeats' flags**: in the 5-T internal M-cycle of a repeating
  LDIR / LDDR / CPIR / CPDR / INIR / INDR / OTIR / OTDR the die
  overwrites Y and X from PC bits 13 and 11, and INxR / OTxR also H and
  P/V (David Banks 2018; `blk_rep_f` in the C model, mirrored in the
  RTL).
- **WZ in INxR / OTxR's repeat**: WZ = PC + 1 (boo-boo et al.'s MEMPTR
  paper, Rak's z80memptr).
- **SCF / CCF**: X / Y = `A | (F XOR Q)`, not `A | Q` (redcode's Zilog NMOS
  model).

Now z80doc, z80memptr, z80full, z80flags and z80ccf pass: 160 tests each,
the SCF / CCF variants for NEC and ST parts skipped by the suite as not
this CPU. z80ccf failed one test, 129, until D1 was fixed.

**False lead** (commit `093f95b`, reverted in `b4f5aca`): a Q that
persisted across instructions broke ZEXALL's `<daa,cpl,scf,ccf>` CRC (run
`27650481834`). Q is 0 after any instruction that doesn't write F.

**FUSE corpus provenance** (researched 2026-06-18):
- The 1356-case `tests.in` / `tests.expected` was **authored by Philip
  Kendall in 2006** for FUSE, **not by Frank D. Cringle**. Cringle's
  separate `zexall.com` / `zexdoc.com` CP/M exercisers are a different
  corpus (those WERE silicon-checksummed against a real Z80).
- No FUSE README, commit message, or Changelog entry documents the
  methodology used to produce `tests.expected`. The community consensus
  is that it is **emulator-derived (FUSE-self-referential)**, not
  silicon-captured.
- floooh/chips's own `z80-fuse.c` test harness already blacklists FUSE
  `BIT n,(HL)` X/Y expectations as "FUSE handles those wrong" —
  community-acknowledged FUSE errors on undocumented bits.
- The "`WZ = PC+1` during the extra 5-T repeat M-cycle" behavior was
  discovered on real silicon via boo-boo et al.'s 2006 MEMPTR paper
  (Zilog chips, multiple testers), confirmed by Patrik Rak's 2012
  validation on a real 48K Spectrum, and refined by Richard Chandler
  on NEC Z80 + Visual Z80 Remix in Rak's v1.2a (z80test PR #3).

**FUSE on the die** (`make fuse_pz80`): perfectz80 agrees with FUSE's
expected values in 1350 of the 1356 cases. The other six -- the four INxR /
OTxR WZ cases (`edb2_1`, `edb3_1`, `edba_1`, `edbb_1`), CPDR's X / Y
(`edb9_2`) and HALT's PC (`76`) -- are in `tests/fuse/silicon.expected`,
which the C model, the RTL and redcode/Z80 (`scripts/redcode_fuse_runner.c`)
are checked against instead: 1356 / 1356 each, none skipped. SCF / CCF's
`37_1` and `3f` were the runners' doing (they started with Q = F; the die
has Q = 0 after the registers are loaded).

### F2. SCF / CCF for other CPUs (NEC, ST)

z80test's SCF (NEC) / CCF (NEC) / SCF (ST) / CCF (ST) tests (003-006) are
the X / Y of other makers' parts; the suite finds which kind of CPU it runs
on and skips the others. RZ80 models the Zilog NMOS part. A runtime switch
to another maker's SCF / CCF is not planned (no current use case).

### F3. Validated as silicon-faithful (no change needed)

- **DDCB/FDCB displacement compute folding**: matches FUSE's DDCB / FDCB
  cases and perfectz80's pins (the event sweep's `opcodes` group runs every
  DD CB / FD CB opcode); known-differences row 11.
- **NMI/INT M1-ack suppress-decode**: matches `prog8_nmi` and perfectz80's
  pins. The pin combination during INTA (M1+IORQ asserted together) does
  suppress the decode pipeline correctly.

## Plan

### Resolved
A2 (ALU modes), B1 (M1 deassert), B2 (NMI sample), B3 (INT sample), B4
(data latch), C1 (reset state), D1-D3 (Q), F1 (block flags, SCF / CCF).

### Open
None. D1-D3 (Q: EX AF,AF', prefixes, RESET cuts; known differences
29-31) were resolved on 2026-10-24.

### Remaining structural work, sorted by leverage × risk

1. **A1 / A4 in the C model**: one IDU path and exchange flip-flops
   instead of inline arithmetic and copies (the RTL has both).
2. **A3**: 16-bit ADD / ADC / SBC byte-twice through `z80_alu`.
3. **E1**: internal data-bus segments (largest; the RTL has the
   address-side bus).

After each: every CI gate, the event sweep (`make event_sweep`) and
`make cfast_identity` -- the pins and registers must stay identical.
