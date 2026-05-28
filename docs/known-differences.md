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

Add rows as differences are discovered during verification. Each "watching" row should
gain a test that pins the chosen behavior or escalates it.
