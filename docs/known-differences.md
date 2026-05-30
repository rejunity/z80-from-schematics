# Known differences

Deliberate or unresolved differences from references, with classification per the
brief (documented / undocumented / timing / flag / MEMPTR / interrupt-refresh / test
bug / reference ambiguity).

| # | Area | Class | Difference | Status |
|---|---|---|---|---|
| 1 | Reset register init | reference ambiguity | Real Z80 leaves SP/AF/main regs undefined at power-up; we force `SP=FFFF, AF=FFFF`, alternates/IX/IY=FFFF for deterministic C↔RTL compare. | accepted |
| 2 | SCF/CCF X/Y flags | undocumented / flag | NMOS chips vary; some derive X/Y from `A`, some from `A|F`, some via the "Q" register. We implement the `A`-based variant. May need the Q variant to pass a given ZEXALL build. | watching |
| 3 | `OUT (C),0` (ED71) | undocumented | NMOS outputs `0x00`; CMOS outputs `0xFF`. We implement NMOS (`0x00`). | accepted |
| 4 | M1 deassert phase | timing | We deassert `m1_n` at T3.P; some references show it late in T2. Externally M1 is low T1–T2, high by T3 in both. | accepted |
| 5 | `LD A,I/R` PF interrupt race | undocumented / interrupt | The documented race that clears PF if INT arrives during the instruction is modeled only at the sequencer's INT sample point; corner timing may differ from silicon. | watching |
| 6 | Verilator build on host | environment (not a core diff) | Apple clang 14 cannot compile the newer macOS SDK's C++17 libc++ headers, so Verilator cannot build here. RTL is verified via iverilog (C↔RTL traces identical). `make verilator` skips gracefully; the TB builds on a healthy C++17 toolchain. | env-blocked |
| 7 | DDCB/FDCB `(IX+d)` zexdoc subtests | tooling (stale binary) | Earlier we reported 64/67 ZEXDOC with the 3 DDCB/FDCB `(IX+d)` subtests failing. Investigation showed this was a **stale `build/bin/zexrunner` binary** linked against an older `libz80.a` from before the DDCB completion commit. Rebuilding zexrunner against the current library passes 67/67 ZEXDOC and 67/67 ZEXALL. Cross-checked by lockstep diff against the superzazu C Z80 reference (`/tmp/z80ref/`): identical registers and memory at every instruction across the full 5.76 B-instruction run. **Root cause for the stale binary:** `make cmodel` rebuilt `libz80.a` but did not relink dependent binaries — see Makefile fix (`make cmodel` now also rebuilds `$(BIN)/zexrunner` and `$(BIN)/tracegen`). | resolved |
| 8 | Interrupt/HALT subsystem in RTL | resolved | The RTL now mirrors the C interrupt engine (NMI, INT IM0/1/2, EI-delay, HALT loop, WAIT, BUSREQ/BUSACK). Full-run C↔RTL parity restored (no HALT truncation); NMI acceptance verified phase-by-phase via prog8_nmi. | resolved |

Add rows as differences are discovered during verification. Each "watching" row should
gain a test that pins the chosen behavior or escalates it.
