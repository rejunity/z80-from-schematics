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
| 4  | M1 deassert phase                             | timing                    | accepted |
| 5  | `LD A,I` / `LD A,R` PF interrupt race         | undocumented / interrupt  | watching |
| 6  | Verilator build on host                       | environment               | resolved |
| 7  | DDCB / FDCB `(IX+d)` ZEXDOC subtests          | tooling (stale binary)    | resolved |
| 8  | Interrupt / HALT subsystem in RTL             | resolved                  | resolved |
| 9  | HALT PC convention                            | undocumented / convention | resolved |
| 10 | `IN A,(n)` / `IN r,(C)` port-input value      | reference ambiguity       | resolved |
| 11 | `DD 36` / `FD 36` cycle count                 | timing                    | resolved |

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

  4. M1 deassert phase — we deassert `m1_n` at T3.P; some references show it
     late in T2. Externally M1 is low T1–T2, high by T3 in both.

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

  9. HALT PC convention — FUSE test `76` expects PC unchanged after HALT (the
     real Z80 re-fetches the HALT byte until interrupt). `EXEC_HALT` now backs
     PC up to the HALT byte; `begin_next()` re-advances PC by 1 when an NMI or
     INT exits the halted state so the pushed return address still points past
     HALT. Mirrored to the RTL.

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

Add rows as new differences are discovered. Each "watching" row should gain a
test that pins the chosen behaviour or escalates it.


## Multi-oracle harness status

| Oracle                                                     | Workload                                            | Result                                                    |
|------------------------------------------------------------|-----------------------------------------------------|-----------------------------------------------------------|
| Our RTL (Verilog) via iverilog + Verilator (`make compare`)| 8 trace programs × 400 phases                       | identical phase-by-phase across C, iverilog and Verilator |
| superzazu C Z80   (`scripts/lockstep.c`)                   | 7.0 M instr (ZEXDOC3)                               | identical regs + memory                                   |
| chips/z80.h pure-C (`scripts/lockstep_triple.c`)           | 7.0 M instr (ZEXDOC3)                               | identical regs (with chips's overlap-PC adjustment)       |
| suzukiplan/z80 C++ (`scripts/lockstep_quad.c`)             | 7.0 M instr (ZEXDOC3)                               | identical regs across all four emulators                  |
| FUSE / Frank D. Cringle (`make fuse`)                      | 1356 cases                                          | **1356 / 1356  (100 %)**                                  |
| FUSE through RTL via iverilog (`make fuse_rtl`)            | 1356 cases                                          | **1356 / 1356  (100 %)**                                  |
| Real KC85 silicon — sync   (`make silicon_cycles`)         | 50 classified opcodes (kc85-cpuclk.sr)              | **50 OK** (4 with /WAIT attribution); 0 emu mismatches    |
| Real KC85 silicon — 20 MHz (`make silicon_async`)          | CPU clock + sub-T pin offsets + 9-opcode re-sample  | **CPU ≈ 1.767 MHz**, M1 / MREQ / RD / WR at spec offsets   |
| MAME Z80 differential                                      | —                                                   | resolved via suzukiplan (MAME's `z80.cpp` ties tightly to MAME's device framework) |
| perfectz80 gate-level netlist                              | per-half-cycle pin trace on prog1                   | runs cleanly at gate level; signal-timing diff vs C model 93 – 96 % (sub-cycle convention) |
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
