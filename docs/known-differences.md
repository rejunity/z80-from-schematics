# Known differences

Deliberate or unresolved differences from reference behaviour, classified per the
brief (documented / undocumented / timing / flag / MEMPTR / interrupt-refresh /
test bug / reference ambiguity).

Status legend:

    accepted     conscious design choice; will not change
    watching     monitoring; no known test failure but worth keeping an eye on
    resolved     fix has landed in both the C model and the RTL


## Differences

| #  | Area                                          | Class                     | Status   |
|----|-----------------------------------------------|---------------------------|----------|
| 1  | Reset register initialisation                 | reference ambiguity       | accepted |
| 2  | SCF / CCF X / Y flags (NMOS Q variant)        | undocumented / flag       | resolved |
| 3  | `OUT (C),0` (ED71)                            | undocumented              | accepted |
| 4  | M1 deassert phase                             | timing                    | resolved |
| 5  | `LD A,I` / `LD A,R` PF interrupt race         | undocumented / interrupt  | watching |
| 6  | Verilator build on host                       | environment               | resolved |
| 7  | DDCB / FDCB `(IX+d)` ZEXDOC subtests          | tooling (stale binary)    | resolved |
| 8  | Interrupt / HALT subsystem in RTL             | resolved                  | resolved |
| 9  | HALT PC convention                            | undocumented / convention | resolved |
| 10 | `IN A,(n)` / `IN r,(C)` port-input value      | reference ambiguity       | resolved |
| 11 | `DD 36` / `FD 36` cycle count                 | timing                    | resolved |
| 12 | `INI` / `IND` / `INIR` / `INDR` flag set      | undocumented / flag       | watching |
| 13 | `LDIR` / `LDDR` Q-leak into next instruction  | undocumented / flag       | watching |
| 14 | Pin-scenario vs perfectz80 (informational)    | timing                    | watching |
| 15 | RTL clocking: reset / BUSREQ settle vs C model | timing                    | watching |
| 16 | BUSREQ / BUSACK vs the NMOS Z80               | timing / bus              | watching |
| 17 | RESET / INT / NMI sweep vs perfectz80         | timing / interrupt        | watching |
| 18 | Idle address bus (refresh resolved)           | timing / bus              | watching |

Notes for each row:

  1. Reset register init — real Z80 leaves SP / AF / main regs undefined at
     power-up; we force `SP=FFFF, AF=FFFF`, alternates / IX / IY = `FFFF` for
     deterministic C↔RTL compare.

  2. SCF / CCF X / Y flags — NMOS Q-register variant implemented: X / Y derived
     from `(A | Q)`, where Q holds the F value left by the previous F-modifying
     instruction (else 0). Brings FUSE to **1356 / 1356 (100 %)**. Mirrored to the
     RTL (Q committed at instruction-done by comparing `rf_n[AF][7:0]` against
     `rf[AF][7:0]`).

  3. `OUT (C),0` — NMOS outputs `0x00`; CMOS outputs `0xFF`. We implement
     NMOS (`0x00`).

  4. M1 deassert phase — our `m1_n` goes high at T3.P (deassert transition
     between T2.N and T3.P). Verified against the perfectz80 gate-level
     netlist via per-half-cycle `cpu_step()` — perfectz80 reads `m1_n=0` at
     T2.N and `m1_n=1` at T3.P, exactly matching our model. "Deassert late
     in T2" descriptions in some references refer to the continuous-time
     analog edge; at half-cycle sample resolution (the resolution at which
     we model and compare against the gate-level reference) M1 is high
     starting at T3.P, which is what both models produce. No code change
     needed; `make perfectz80` is the gate of record.

  5. `LD A,I/R` PF interrupt race — the documented race that clears PF if INT
     arrives during the instruction is modelled only at the sequencer's INT
     sample point; corner timing may differ from silicon.

  6. Verilator build on host — earlier blocked on Apple clang 14 vs macOS SDK
     C++17 libc++. Resolved with Apple clang 21 + Verilator 5.042 from Homebrew;
     `make verilator` and `make compare`'s Verilator leg both work.

  7. DDCB / FDCB `(IX+d)` ZEXDOC subtests — earlier we reported 64 / 67 ZEXDOC
     with three DDCB / FDCB `(IX+d)` subtests failing. Investigation showed this
     was a stale `build/bin/zexrunner` binary linked against an older
     `libz80.a`. Rebuilding zexrunner passes 67 / 67 ZEXDOC and 67 / 67 ZEXALL;
     `make cmodel` now also relinks `$(BIN)/zexrunner` and `$(BIN)/tracegen`.

  8. Interrupt / HALT subsystem in RTL — the RTL now mirrors the C interrupt
     engine (NMI, INT IM0 / 1 / 2, EI-delay, HALT loop, WAIT, BUSREQ / BUSACK).
     Full-run C↔RTL parity restored; NMI acceptance verified phase-by-phase
     via `prog8_nmi`.

  9. HALT PC convention — **REVERSED 2026-06-18 to be silicon-faithful per
     Brewer 2014 + Woodmass HALT2INT 2021**. The Z80 silicon advances PC
     past the HALT byte during HALT's M1 commit and KEEPS it there
     throughout the HALT-NOP loop; INT / NMI exit accepts that PC
     unchanged. Earlier we decremented PC back to the HALT byte (matching
     FUSE test `76`'s pre-Brewer expectation) and re-incremented on
     interrupt exit. The PC was correct at instruction boundaries but
     the intermediate trace diverged from perfectz80 by 145 / 200 phases
     on `prog13_halt_int`. The silicon-faithful convention now mirrors
     perfectz80 exactly (bus addr 110 / 200 match, up from 107). FUSE
     test `76` enters the known-FUSE-wrong list alongside the other
     post-2014 silicon-faithful divergences. Mirrored to RTL
     (`rtl/z80_core.v` `EXEC_HALT` + interrupt-accept paths). The
     specific divergence redcode/Z80's end-to-end FUSE run also hit
     (`/scripts/redcode_fuse_runner.c` reports the same `76 PC=0001`).

 10. `IN A,(n)` / `IN r,(C)` port-input value — FUSE expects port reads to
     return the high byte of the I/O address bus (`A` for `IN A,(n)`, `B` for
     `IN r,(C)`, `B` for INI / IND / INIR / INDR). `scripts/fuse_runner.c`
     pre-fills `s->io[a] = a >> 8` so port reads return the FUSE-spec value;
     the core port-read path is unchanged.

 11. `DD 36` / `FD 36` cycle count — FUSE expected 19 T-states for
     `LD (IX+d),n` / `LD (IY+d),n`; we used to count 22. The (IX+d) / (IY+d)
     prefix preamble now folds the 2T IX+d compute into the N immediate-read
     M-cycle (5 T) for `EXEC_LD_M_N` instead of emitting a separate 5 T internal
     cycle. Mirrored to RTL.

 12. `INI` / `IND` / `INIR` / `INDR` flag set — **resolved 2026-06-18.**
     Three changes landed together:
     - **ULA-idle port-parity IO** (`io_ula_idle` in `cmodel/z80_sim.h`):
       IN returns `(addr & 1) ? 0xFF : 0xBF`, matching redcode's static
       `cpu_in` for the 48K Spectrum ULA convention Rak's CRCs were
       captured against (commit `07b257f`).
     - **Banks-2018 INIR/INDR/OTIR/OTDR repeat fold-in**: the 5-T
       internal M-cycle of the repeat overwrites YF=PC.13, XF=PC.11,
       and reworks HF/PF/CF per David Banks' 2018 reverse-engineering
       (commit `9f1acb2`). C and RTL synced.
     - **WZ = PC + 1** during repeat (commit `b654110`) per
       boo-boo et al. 2006 MEMPTR / Rak's z80memptr 1.2a.
     `make z80test` now passes 160 / 160 / 160. Some FUSE cases
     (`edba_1`, `edb2_1`, `edbb_1`, `edb3_1`, `edb9_2`) had their
     expected pre-Banks values; listed in
     [tests/fuse/known-fuse-wrong.txt](../tests/fuse/known-fuse-wrong.txt).

 13. `LDIR` / `LDDR` / `INIR` / `INDR` Q-leak into next instruction —
     **resolved 2026-06-18** as part of #12 (the Banks fold-in is
     exactly the "Q-flag carry-over" effect this row described).

 14. Pin-scenario vs perfectz80 (`make pin_scenarios` /
     `pin_scenarios_rtl` / `pin_scenarios_netlist`) — fourteen trace
     programs (`prog9_inta_im1`..`prog22_wait_cpir`) drive INT / NMI /
     WAIT / BUSREQ / RESET through the `.events` sidecar and diff
     against perfectz80 per-half-cycle. **Current status
     (2026-10-20)**: 12 / 14 PASS cleanly through the RTL, 13 / 14
     through the C model. Until then `perfectz80_runner` set NMI, INT and
     WAIT high again at every phase, so perfectz80 saw a one-phase pulse
     where the C model and the RTL held the scenario's level from its
     `lo` event to its `hi` event; it now holds the level too. That
     difference alone made `prog11_wait_mem`, `prog13_halt_int` and
     `prog14_wait_io` differ in ~145 / 200 phases; with it gone both WAIT
     programs pass, like `prog21_wait_ldir` and `prog22_wait_cpir`.
     `prog10_halt_nmi` and `prog13_halt_int` pass since the HALT pin
     rises in the low half of the last T4 (row 17). Left:
     - `prog17_reset` (RTL 131 / 200, C 1 / 200): RESET restart and
       filter (rows 15, 17).
     - `prog15_busreq_m1` (RTL 109 / 200, C passes): the RTL resumes a
       T-state early after BUSREQ is released (rows 15, 16).

     `.events` is now consumed by all paths — C tracegen, iverilog
     RTL, Verilator, LibreLane gate-level netlist — via per-pin
     `+<pin>_lo / +<pin>_hi` plusargs. Make targets are informational
     (exit 0 even on divergence); current divergences are folded into
     the simplifications-audit list rather than gating CI. Functional
     behaviour on all of these is verified by Rak + FUSE +
     `make halt2int` even where the per-phase pin trace differs.
     When all of them reach control-pin parity, this row flips to
     `resolved` and the gates become hard.

     Additionally, `scripts/compare_signal_timing.py` (used by
     `make perfectz80` / `_rtl` / `_netlist`) compares `addr` and
     `data_o` with an **address-bus don't-care tolerance**: phases
     where no data transfer is in progress on either side (refresh
     windows, idle phases between M-cycles, T1.P setup) are treated as
     matches regardless of `addr` value; the refresh address itself is
     gated separately (row 18). With the tolerance in place,
     `addr` matches at **100 %** on 10/12 trace programs (8 hand + 4
     random), with the residual 95-98 % on `prog_rnd_02` / `prog_rnd_03`
     being the reset-register-init delta (row 1) surfacing via PUSH
     instructions. `data_o` matches at **100 %** wherever a write
     window is defined. Promote bus diffs to gating with `BUS_STRICT=1`
     env var.

 15. **RTL clocking (`cmos-clock`) — RTL vs C-model reset / BUSREQ
     settle**. The RTL now runs on the Z80 clock with every core register
     on the rising edge; the thin `rtl/z80_top.v` wrapper re-times pins
     and three inputs to the falling edge (`docs/clocking.md`). The
     retiming is trace-identical to the C model on `make compare`, FUSE,
     Rak and BASIC. Deliberate residual RTL↔C differences, all confined
     to the informational pin-scenario programs:
     - **Reset**: the C model carries silicon's ~5-half-cycle reset-assert
       filter and ~4-half-cycle release settle (`prog17_reset`). The RTL
       resets asynchronously the moment `reset_n` falls and starts T1 of
       the first M1 on the first rising edge after release.
     - **BUSREQ release**: the C model settles 2 half-cycles after
       `busreq_n` rises (`prog15_busreq_m1`); the RTL samples `busreq_n`
       on the rising edge and restarts M1 on that edge. (The previous 2×
       RTL could restart on a falling edge and run phase-swapped
       thereafter; that is fixed.)
     - **Pins during a bus grant**: both now idle their strobes while
       BUSACK is low. The previous 2× RTL kept driving the frozen
       M-cycle's strobes.
     - **M1 opcode latch under WAIT**: the RTL now, like the C model,
       defers the opcode latch / PC++ / R++ until the rising edge that
       ends the last Tw (the original 2× RTL double-latched).
     Watching: fold the reset / BUSREQ settle into the RTL once the
     pin-scenario gates become hard.

 16. **BUSREQ / BUSACK vs the NMOS Z80 (silicon and datasheet)** —
     found by sweeping BUSREQ through perfectz80, the C model and the RTL
     (the pin_scenarios harness never compares BUSACK, and its BUSREQ
     events are long steps):
     - **Grant point**: the NMOS Z80 grants the bus at the end of *any*
       machine cycle (PS0178 Figure 10; perfectz80 grants after the M1 of
       `LD SP,nn` and again after each of its operand reads) and then
       resumes the same instruction. Both models grant only at instruction
       boundaries, so DMA can wait up to a whole instruction longer.
     - **Sample edge**: BUSREQ is sampled on the rising edge that starts
       the M-cycle's last T-state, like INT (TsBUSREQ / ThBUSREQ); both
       models sample the edge that ends it, a T-state later.
     - **BUSACK edges**: BUSACK falls on the rising edge after the sample
       (TdCr(BUSACKf)); the C model drops it half a clock late. On release
       the sample is on a rising edge, BUSACK rises on the next falling
       edge (TdCf(BUSACKr)) and M1 starts on the rising edge after that.
       The C model raises BUSACK with M1, half a clock late; the RTL
       starts M1 on the sampling edge itself, a T-state early. The C model
       freezes its half-cycle counter during the grant, so a release that
       arrives before a falling edge restarts it out of step with the
       clock.
     - **M-cycle boundaries**: silicon's grant points follow the
       datasheet's M-cycle split, internal cycles included (`ADD HL,BC`:
       4 + 4 + 3 T-states, grants at each boundary). The models merge some
       internal cycles (`ADD HL,BC`: M1 + one 7-T-state cycle).
     Watching: a separate branch reworks both sequencers (grant at every
     M-cycle end, the datasheet's M-cycle splits, the BUSACK edges) and
     gates it on BUSREQ sweeps against perfectz80 for every instruction
     class.

 17. RESET / INT / NMI sweep vs perfectz80 (`make event_sweep`, see
     [event-sweep.md](event-sweep.md), from hardesk) — pulses at every
     T-state of 13 instructions, diffed per half-cycle against perfectz80.
     545 of 2287 cases differ, all listed in `tests/event_sweep/baseline.txt`
     (CI fails on any new or changed difference). The differences, in
     perfectz80's terms:
     - After RETN with INT pending, one instruction runs before INTA.
     - RESET is sampled at rising edges, the current bus cycle finishes,
       T1 comes 3 rising edges after release, SP / BC / DE / HL are kept
       (row 1), and RESET covering only the rising edge into an M1 T2 is
       a special reset that keeps I. RZ80 resets asynchronously and
       restarts on the first rising edge (row 15).

 18. **Refresh and idle address bus vs perfectz80** — the pin
     comparisons treat the address as don't-care outside transfers and
     refreshes (row 14). Comparing it in every phase, on all trace and
     pin-scenario programs up to their first control-pin difference:
     - **Refresh — resolved 2026-10-20.** perfectz80 drives I:R, the R
       from before this M1's increment (the first M1 after reset
       refreshes 0000); RZ80 used to step R at the T3 rising edge and
       drive I:(R+1). R now steps on the rising edge that ends the
       refresh (T4 of an M1, the second refresh T-state of an INTA), in
       the C model and the RTL, and every refresh phase matches. Matters
       to DRAM controllers that use the refresh address and to the ZX81,
       whose display hangs its line counter on A6 of the refresh
       address. `compare_signal_timing.py` gates on it and the event
       sweep compares it.
     - **Internal T-states** (no MREQ / IORQ): the address differs in
       468 of 2,215 phases; in 178 of them perfectz80 still shows the
       last refresh address (IR), as the Spectrum's contention tables
       assume ("ir:1"), and the RTL shows another register.
     Every phase with MREQ or IORQ and RD or WR (3,971) agrees.

Add rows as new differences are discovered. Each "watching" row should gain a
test that pins the chosen behaviour or escalates it.


## Multi-oracle harness status

| Oracle                                                     | Workload                                            | Result                                                    |
|------------------------------------------------------------|-----------------------------------------------------|-----------------------------------------------------------|
| Our RTL (Verilog) via iverilog + Verilator (`make compare`)| 8 trace programs × 400 phases                       | identical phase-by-phase across C, iverilog and Verilator |
| superzazu C Z80   (`scripts/lockstep.c`)                   | 7.0 M instr (ZEXDOC3)                               | identical regs + memory                                   |
| chips/z80.h pure-C (`scripts/lockstep_triple.c`)           | 7.0 M instr (ZEXDOC3)                               | identical regs (with chips's overlap-PC adjustment)       |
| suzukiplan/z80 C++ (`scripts/lockstep_quad.c`)             | 7.0 M instr (ZEXDOC3)                               | identical regs across all four emulators                  |
| redcode/Z80 (`scripts/lockstep_quint.c`)                   | 7.0 M instr (ZEXDOC3)                               | identical regs across all five emulators                  |
| FUSE corpus (Kendall 2006) (`make fuse`)                   | 1356 cases                                          | **1348 PASS + 8 known-FUSE-wrong (silicon-faithful)**     |
| FUSE through RTL via iverilog (`make fuse_rtl`)            | 1356 cases                                          | **1348 PASS + 8 known-FUSE-wrong (matches C)**            |
| Patrik Rak z80test (`make z80test`)                        | doc / memptr / full (~470 micro-tests across three)  | **160 / 160 / 160 PASS** (all silicon-faithful)           |
| ZEXALL 14-test subset via Verilator RTL (`make zexall_subset_rtl`) | 550 M instr through Verilator                 | **14 / 14** PASS; ~17 min on CI                            |
| Real KC85 silicon — sync   (`make silicon_cycles`)         | 50 classified opcodes (kc85-cpuclk.sr)              | **50 OK** (4 with /WAIT attribution); 0 emu mismatches    |
| Real KC85 silicon — 20 MHz (`make silicon_async`)          | CPU clock + sub-T pin offsets + 9-opcode re-sample  | **CPU ≈ 1.767 MHz**, M1 / MREQ / RD / WR at spec offsets   |
| MAME Z80 differential                                      | —                                                   | resolved via suzukiplan (MAME's `z80.cpp` ties tightly to MAME's device framework) |
| perfectz80 gate-level netlist                              | per-half-cycle pin trace on prog1 / prog2 / prog3_cb | **100 %** control-pin parity (signal-timing convention now matches the silicon falling-edge transition) |
| Z80 Explorer (Qt) gate-level                               | n/a                                                 | not used — Qt-coupled; same Visual Z80 netlist already covered by perfectz80 |


## Speed (no trace, same workload, this host)

    C model:                6.56  Minstr/s on ZEXDOC3 (~14 min for full ZEXALL)
    Verilator RTL:          0.31  Minstr/s on ZEXDOC3 (~21× slower than C)
    perfectz80 gate level:  ~10   k  phases/s — use only for short, targeted comparisons


## See also

  - [verification.md](verification.md) — current verification state, layers and
    acceptance gates.
  - [real-silicon-traces.md](real-silicon-traces.md) — how the sigrok KC85 traces
    are decoded and used as a real-silicon oracle.
  - [research-notes.md](research-notes.md) — sources, trust levels, source-conflict
    precedence.
  - [architecture.md](architecture.md), [timing.md](timing.md), [pla.md](pla.md),
    [flags.md](flags.md), [alu.md](alu.md), [undocumented.md](undocumented.md) —
    the design contract that the differences above are deviations from.
