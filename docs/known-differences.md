# Known differences

Deliberate or unresolved differences from references, with classification per the
brief (documented / undocumented / timing / flag / MEMPTR / interrupt-refresh / test
bug / reference ambiguity).

| # | Area | Class | Difference | Status |
|---|---|---|---|---|
| 1 | Reset register init | reference ambiguity | Real Z80 leaves SP/AF/main regs undefined at power-up; we force `SP=FFFF, AF=FFFF`, alternates/IX/IY=FFFF for deterministic C↔RTL compare. | accepted |
| 2 | SCF/CCF X/Y flags | undocumented / flag | NMOS Q-register variant implemented: X/Y derived from `(A | Q)`, where Q holds the F value left by the previous F-modifying instruction (else 0). Brings FUSE to **1356/1356 (100%)**. Mirrored to the RTL (Q committed at instruction-done by comparing rf_n[AF][7:0] vs rf[AF][7:0]). | resolved |
| 3 | `OUT (C),0` (ED71) | undocumented | NMOS outputs `0x00`; CMOS outputs `0xFF`. We implement NMOS (`0x00`). | accepted |
| 4 | M1 deassert phase | timing | We deassert `m1_n` at T3.P; some references show it late in T2. Externally M1 is low T1–T2, high by T3 in both. | accepted |
| 5 | `LD A,I/R` PF interrupt race | undocumented / interrupt | The documented race that clears PF if INT arrives during the instruction is modeled only at the sequencer's INT sample point; corner timing may differ from silicon. | watching |
| 6 | Verilator build on host | environment (not a core diff) | Apple clang 14 cannot compile the newer macOS SDK's C++17 libc++ headers, so Verilator cannot build here. RTL is verified via iverilog (C↔RTL traces identical). `make verilator` skips gracefully; the TB builds on a healthy C++17 toolchain. | env-blocked |
| 7 | DDCB/FDCB `(IX+d)` zexdoc subtests | tooling (stale binary) | Earlier we reported 64/67 ZEXDOC with the 3 DDCB/FDCB `(IX+d)` subtests failing. Investigation showed this was a **stale `build/bin/zexrunner` binary** linked against an older `libz80.a` from before the DDCB completion commit. Rebuilding zexrunner against the current library passes 67/67 ZEXDOC and 67/67 ZEXALL. Cross-checked by lockstep diff against the superzazu C Z80 reference (`/tmp/z80ref/`): identical registers and memory at every instruction across the full 5.76 B-instruction run. **Root cause for the stale binary:** `make cmodel` rebuilt `libz80.a` but did not relink dependent binaries — see Makefile fix (`make cmodel` now also rebuilds `$(BIN)/zexrunner` and `$(BIN)/tracegen`). | resolved |
| 8 | Interrupt/HALT subsystem in RTL | resolved | The RTL now mirrors the C interrupt engine (NMI, INT IM0/1/2, EI-delay, HALT loop, WAIT, BUSREQ/BUSACK). Full-run C↔RTL parity restored (no HALT truncation); NMI acceptance verified phase-by-phase via prog8_nmi. | resolved |
| 9 | HALT PC convention | undocumented / convention | FUSE test `76` expects PC unchanged after HALT (the real Z80 re-fetches the HALT byte until interrupt). Our C model advances PC past HALT and tracks the loop separately; externally indistinguishable except in single-step tests that snapshot PC. Real fix: leave PC at the HALT opcode in the C model. | to fix |
| 10 | `IN A,(n)` / `IN r,(C)` port-input value | reference ambiguity / harness | FUSE expects port reads to return data derived from the address bus on the IN cycle (high byte = A for `IN A,(n)`; B for `IN r,(C)`); our default `port_read` returns `0x00`. Affects FUSE: `db`, `db_1..3`, `ed40/48/50/58/60/68/70/78`, INI/IND/INIR/INDR (`eda2*`, `edaa*`, `edb2*`, `edba*`). Need a configurable port-read shim that matches the FUSE convention for the harness; the core port-read path is correct. | to fix (harness shim) |
| 11 | `DD 36` / `FD 36` cycle count | timing | FUSE expects 19 T-states for `LD (IX+d),n`/`LD (IY+d),n`; we count 22. Real Z80 collapses the prefix M1 into the next M-cycle's R-counter increment and merges the displacement/immediate fetches; our sequencer issues an extra 3-T M-cycle. Localized to DD/FD-prefixed `LD (idx+d),n`. | to fix |

Add rows as differences are discovered during verification. Each "watching" row should
gain a test that pins the chosen behavior or escalates it.

## Multi-oracle harness status

| Oracle | Workload | Result | Note |
|---|---|---|---|
| Our RTL (Verilog) via iverilog + Verilator (`make compare`) | 8 trace programs × 400 phases | identical phase-by-phase across all three | C, iverilog, Verilator agree on every signal |
| superzazu C Z80 (`scripts/lockstep.c`) | 7.0 M instr (ZEXDOC3) | identical regs + memory | per-instruction lockstep |
| chips/z80.h pure-C (`scripts/lockstep_triple.c`) | 7.0 M instr (ZEXDOC3) | identical regs (with chips's overlap-PC adjustment) | three-way triangulation: mine + superzazu + chips |
| suzukiplan/z80 C++ (`scripts/lockstep_quad.c`) | 7.0 M instr (ZEXDOC3) | identical regs across all 4 emulators | four-way triangulation incl. a MAME-spirit reference |
| FUSE / Frank D. Cringle (`make fuse`) | 1356 cases | **1356/1356 (100%)** | Q-register variant for SCF/CCF closes the last 2 |
| MAME Z80 differential (task 18) | — | resolved via suzukiplan | MAME's z80.cpp can't be cleanly extracted from the MAME device-framework; suzukiplan/z80 substituted as same-tier industry reference |
| perfectz80 gate-level netlist (task 19, partial) | per-half-cycle pin trace on prog1 | runs cleanly at gate level | scripts/perfectz80_runner.c + scripts/compare_signal_timing.py. Signal-timing diff vs C model has an alignment offset (reset-release convention + sub-cycle pin sample point); framework is in place, alignment lookup table is the remaining work |
| Z80 Explorer (Qt) gate-level | n/a | not used | Qt-coupled; gate-level data is the same Visual Z80 netlist as perfectz80, so headless perfectz80 covers it |

Speed (no trace, same workload, this host):
- C model:   6.56 Minstr/s on ZEXDOC3 (~14 min for full ZEXALL).
- Verilator: 0.31 Minstr/s on ZEXDOC3 (~21× slower than C).
- perfectz80 gate-level: ~10 k phases/s — use only for short, targeted comparisons.
