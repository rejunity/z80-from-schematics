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
| 7 | DDCB/FDCB `(IX+d)` zexdoc subtests | undocumented (to investigate) | ZEXDOC passes **64/67** subtests. The 3 failures are all DDCB/FDCB: `bit n,(ix+d)`, `shf/rot (ix+d)`, `<set,res> n,(ix+d)`. In isolation our behavior matches documented + standard-undocumented rules (verified: undoc copy-to-r[z] for all z incl. FDCB; BIT X/Y from (IX+d) high byte = `0x28` case; rotate result/flags identical to the passing `(HL)` form). Needs a reference-emulator diff to pinpoint (likely a subtle X/Y-from-WZ or per-iteration undoc detail). All other zexdoc subtests pass flag-exact. | investigating |
| 8 | Interrupt/HALT subsystem in RTL | C/RTL divergence (temporary) | The C model implements NMI, INT IM0/1/2, EI-delay, HALT looping, WAIT, and BUSREQ/BUSACK (22 tests pass). The Verilog RTL does not yet mirror these, so post-HALT the C trace loops at the HALT while the RTL runs forward. `scripts/compare_traces.py` truncates at HALT entry; all instruction execution up to HALT matches exactly. RTL interrupt mirror is the next parity task. | tracked |

Add rows as differences are discovered during verification. Each "watching" row should
gain a test that pins the chosen behavior or escalates it.
