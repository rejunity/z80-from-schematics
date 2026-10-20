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
| 1  | Reset register initialisation                 | reference ambiguity       | resolved |
| 2  | SCF / CCF X / Y flags (NMOS Q variant)        | undocumented / flag       | resolved |
| 3  | `OUT (C),0` (ED71)                            | undocumented              | accepted |
| 4  | M1 deassert phase                             | timing                    | resolved |
| 5  | `LD A,I` / `LD A,R` PF interrupt race         | undocumented / interrupt  | resolved |
| 6  | Verilator build on host                       | environment               | resolved |
| 7  | DDCB / FDCB `(IX+d)` ZEXDOC subtests          | tooling (stale binary)    | resolved |
| 8  | Interrupt / HALT subsystem in RTL             | resolved                  | resolved |
| 9  | HALT PC convention                            | undocumented / convention | resolved |
| 10 | `IN A,(n)` / `IN r,(C)` port-input value      | reference ambiguity       | resolved |
| 11 | `DD 36` / `FD 36` cycle count                 | timing                    | resolved |
| 12 | `INI` / `IND` / `INIR` / `INDR` flag set      | undocumented / flag       | watching |
| 13 | `LDIR` / `LDDR` Q-leak into next instruction  | undocumented / flag       | watching |
| 14 | Pin-scenario vs perfectz80 (informational)    | timing                    | resolved |
| 15 | RTL clocking: reset / BUSREQ settle vs C model | timing                    | resolved |
| 16 | BUSREQ / BUSACK vs the NMOS Z80               | timing / bus              | resolved |
| 17 | RESET / INT / NMI sweep vs perfectz80         | timing / interrupt        | resolved |
| 18 | Refresh and idle address bus                  | timing / bus              | resolved |
| 19 | RESET between an instruction and its write-back | timing / reset          | watching |

Notes for each row:

  1. Reset register init — **resolved 2026-10-20.** perfectz80 comes up with
     every register pair at 5555 and keeps every register across RESET; only
     PC, I, R, IFF1 / IFF2 and IM are cleared. RZ80's power-on reset
     (`z80_top.por_n`, not a Z80 pin; `z80_reset()` in C) loads 5555, and
     RESET keeps every register, WZ and the exchange flip-flops
     (docs/timing.md "RESET"). The real NMOS part comes up with random
     registers; the ASIC chip ties `por_n` off and RESET alone initialises
     the control state.

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

  5. `LD A,I/R` PF interrupt race — **resolved 2026-10-20.** On the NMOS
     Z80 an INT accepted at the end of LD A,I / LD A,R clears IFF2 while
     the instruction writes P/V from it, so P/V reads 0. perfectz80 shows
     it from every INT start up to the last T-state's rising edge (pushed F
     29 instead of 2D); an NMI accepted there leaves P/V = IFF2. RZ80 used
     IFF2 unconditionally. The C model and the RTL now clear P/V when the
     boundary's decision accepts an INT; the event sweep's LD A,I / LD A,R
     cases and `test_int.c` pin it. (The CMOS Z84C00 fixed the race.)

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
     (2026-10-20)**: 14 / 14 PASS through the RTL and through the C model,
     the address included in every phase. Until then `perfectz80_runner` set NMI, INT and
     WAIT high again at every phase, so perfectz80 saw a one-phase pulse
     where the C model and the RTL held the scenario's level from its
     `lo` event to its `hi` event; it now holds the level too. That
     difference alone made `prog11_wait_mem`, `prog13_halt_int` and
     `prog14_wait_io` differ in ~145 / 200 phases; with it gone both WAIT
     programs pass, like `prog21_wait_ldir` and `prog22_wait_cpir`.
     `prog10_halt_nmi` and `prog13_halt_int` pass since the HALT pin
     rises in the low half of the last T4 (row 17), `prog17_reset` since
     both models run the die's sampled RESET, `prog15_busreq_m1` since
     both grant and release BUSREQ as the die does (row 16).

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
     - **Reset** — resolved 2026-10-20: both models run the die's sampled
       RESET (docs/timing.md "RESET"); `prog17_reset` and the sweep's 544
       RESET cases match perfectz80.
     - **BUSREQ release** — resolved 2026-10-20: both models grant and
       release as the die does (row 16).
     - **Pins during a bus grant**: both now idle their strobes while
       BUSACK is low. The previous 2× RTL kept driving the frozen
       M-cycle's strobes.
     - **M1 opcode latch under WAIT**: the RTL now, like the C model,
       defers the opcode latch / PC++ / R++ until the rising edge that
       ends the last Tw (the original 2× RTL double-latched).
     Watching: fold the reset / BUSREQ settle into the RTL once the
     pin-scenario gates become hard.

 16. **BUSREQ / BUSACK vs the NMOS Z80 — resolved 2026-10-20.** Found by
     sweeping BUSREQ through perfectz80; both models now do what the die
     does (docs/timing.md "BUSREQ / BUSACK"):
     - **Grant point**: after *any* machine cycle (PS0178 Figure 10), the
       next one held until the release -- inside an instruction too (after
       the M1 of `LD SP,nn`, after each operand read). The models granted
       only at instruction boundaries.
     - **Sample edge**: BUSREQ is sampled on the rising edge that starts the
       M-cycle's last T-state, like INT (TsBUSREQ / ThBUSREQ); the models
       sampled the edge that ends it.
     - **BUSACK edges**: BUSACK falls on the rising edge after the sample
       (TdCr(BUSACKf)). On release the sample is on a rising edge, BUSACK
       rises on the next falling edge (TdCf(BUSACKr)) and the held M-cycle
       starts on the rising edge after that; the shortest grant is one
       T-state.
     - **M-cycle boundaries**: the die's, internal cycles included -- ADD /
       ADC / SBC HL,rr now run internal M-cycles of 4 + 3 T-states, and the
       1- and 2-T internal states after an M1 or a read are part of it.
     The event sweep's `busreq` group (1 T and 4 T at every T-state of 24
     instructions) and the BUSREQ-with-INT / NMI / HALT combos match;
     `prog15_busreq_m1` passes in both models.

 17. RESET / INT / NMI / BUSREQ / WAIT sweep vs perfectz80 (`make
     event_sweep`, see [event-sweep.md](event-sweep.md), from hardesk) —
     pulses at every T-state of 24 instructions, diffed per half-cycle
     against perfectz80, the address and BUSACK included. **5879 of 5905
     cases match (2026-10-20)**; the 26 in `tests/event_sweep/baseline.txt`
     are row 19 (RESET in ADD HL,BC / INC BC's write-back), and CI fails on
     any other difference. RESET is the die's sampled RESET with the special
     reset and registers kept (row 1); BUSREQ the die's grant (row 16).

 18. **Refresh and idle address bus vs perfectz80 — resolved
     2026-10-20.** The address now agrees with perfectz80 in every
     half-cycle, in the C model and the RTL, on every trace program, pin
     scenario and event-sweep case; `compare_signal_timing.py` and the
     sweep gate on it.
     - **Refresh.** perfectz80 drives I:R, the R from before this M1's
       increment (the first M1 after reset refreshes 0000); RZ80 stepped R
       at the T3 rising edge and drove I:(R+1). R now steps on the rising
       edge that ends the refresh (T4 of an M1, the second refresh T-state
       of an INTA). Matters to DRAM controllers that use the refresh
       address and to the ZX81, whose display hangs its line counter on A6
       of the refresh address.
     - **Internal T-states.** The bus keeps the last address: the refresh
       address after an M1 ("ir:1" in the Spectrum's contention tables),
       the operand address after a read ("hl:1", "pc+1:1"), and so through
       an M1's T5 / T6, IM1 / IM2's seventh INTA T-state, the reset idle
       and a bus grant. RZ80 drove its address latch, which carries the
       IDU / EA work of those T-states (468 of 2,215 phases differed);
       the pins now hold instead.

 19. **RESET between an instruction and its write-back** — the die
     finishes some results in the next M1, which a RESET that stops that
     M1 early loses (perfectz80, RESET at every T-state; the register
     dump after reset shows it):
     - INC A: H and N in the next M1's T2, A and the other flags in T3;
     - LD A,(HL): A in the next M1's T2; POP DE: DE in its T1; EX DE,HL:
       the swap in its T3;
     - ADD HL,rr: L as the die's M2 ends, H in the next M1's T1, the flags
       in its T2 / T3; INC rr: the pair late in the M1's T5 / T6.
     RZ80 writes every result on the instruction's last edge (ADD HL,rr
     and INC rr theirs earlier still) and keeps them. The push SP-1, which
     the die does on the edge that ends the previous cycle, is modelled.
     The sweep's `reset_add_hl_bc` (20) and `reset_inc_bc` (6) cases are
     these, in the baseline; RESET with WAIT is untested. Modelling it means
     holding each class's writes until the T-state the die does them in.

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
