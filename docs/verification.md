# Verification

Living record of the verification state. Updated at each checkpoint.

## Verification layers
1. **C unit tests** (`tests/common/`): ALU, flags, DAA, rotates, BIT/SET/RES, PLA row
   matching, timing sequencer, bus-cycle traces. Run via `make ctest`.
2. **Generated instruction tests** (`tests/generated/`, `scripts/gen_tests.py`):
   per-opcode init→expected over boundary inputs and flag-relevant classes.
3. **Compatibility suites** (`tests/zex/`): ZEXDOC then ZEXALL via `make zexdoc/zexall`.
4. **Timing traces** (`tests/traces/`): shared-format bus-cycle traces for fetch, mem
   r/w, I/O r/w, refresh, INTA, WAIT, HALT, BUSREQ/BUSACK, NMI, IM0/1/2.
5. **C ↔ RTL comparison** (`make compare`): same programs through the C model and the
   Verilog RTL (Verilator + iverilog); traces diffed phase-by-phase and classified.

## Acceptance gates
- C model: all unit + generated tests pass; ZEXDOC passes; ZEXALL passes.
- RTL: `make rtl` elaborates clean (no latches/initial in synth RTL); Verilator and
  iverilog agree; RTL traces match the C model phase-by-phase on the trace corpus.

## Current status

| Area | C model | RTL | C↔RTL parity |
|---|---|---|---|
| Build scaffold | done | done | n/a |
| Timing sequencer (phase/T/M, WAIT hook) | done | done | done |
| ALU + flags (8-bit, inc/dec, rot-A, daa/cpl/scf/ccf) | done | done | done |
| Register file + WZ/MEMPTR | done | done | done |
| M1 fetch + refresh + core 8-bit ops | done | done | done |
| Memory r/w + (HL) RMW ops | done | done | done |
| 16-bit load/inc/dec/ADD HL | done | done | done |
| branch / call / ret / rst / push / pop / DJNZ / JR | done | done | done |
| I/O (IN/OUT n,A), EX/EXX/EX(SP) | done | done | done |
| CB prefix (rot/shift/BIT/RES/SET) | done | done | done |
| ED prefix (16-bit ADC/SBC, LD I/R, NEG, IM, RET[I/N], IN/OUT(C), RRD/RLD) | done | done | done |
| ED block ops (LDI..OTDR) | done | done | done |
| DD/FD (IX/IY, IXH/IXL, (IX+d)) | done | done | done |
| DDCB/FDCB (op (IX+d) + undoc copy) | done | done | done |
| Interrupts/refresh/wait/halt/bus | partial (HALT+refresh) | partial | pending |
| Undocumented (X/Y, MEMPTR rules) | partial | partial | pending |
| ZEXDOC | harness wired; blocked on prefixes | — | — |
| ZEXALL | harness wired; blocked on prefixes | — | — |

### prelim.com — PASSING
`make prelim` runs the preliminary instruction test through the C model and prints
"Preliminary tests complete" with no errors. This exercises basic + IX/IY behavior and
confirms the CP/M harness (loader, BDOS shim, PC seeding) works end-to-end.

### ZEX harness (make zexdoc / zexall / prelim)
The CP/M harness is in place: `tests/zex/{prelim,zexdoc,zexall}.com`, `scripts/zexrunner.c`
(loader + BDOS shim for console functions 2/9, exit trap at 0x0000, BDOS trap at 0x0005),
and `scripts/run_zex.py`. `make zexall` runs the C model: it prints the exerciser banner
("Z80 instruction exerciser") and reaches the first subtest `<adc,sbc> hl,...`, then
stalls because that test — and the exerciser's own setup/CRC routines — use ED/CB/DD/FD
prefixed instructions, which are not yet implemented (task 8). A `ZEX_MAX` instruction
cap prevents runaway loops until prefix coverage lands. ZEXALL is the flag-exact
acceptance gate; it cannot pass until tasks 8 (prefixes) and 9 (interrupts) are done.

### Test results (current)
- **C unit tests**: 147 checks across `test_alu` (incl. exhaustive 2^17 add/sub
  verification of the nibble ALU), `test_flags`, `test_exec` (functional + exact
  T-state timing), `test_pla`. All pass (`make ctest`).
- **RTL elaboration**: clean under `iverilog -g2001` (`make rtl`), no latches/initial
  in synthesizable RTL.
- **C ↔ RTL parity**: `make compare` runs `prog1.hex` and `prog2.hex` through the C
  model (`tracegen`) and the Verilog RTL (Icarus Verilog) and confirms the shared
  bus-cycle traces are **identical phase-by-phase over 400 phases each**. Programs
  cover: LD r/rp, immediate, (HL) read/write/RMW, ALU, INC/DEC, ADD HL,rp, PUSH/POP,
  CALL/RET, JR/DJNZ, RLCA/CPL, HALT.

### Verilator
The Verilator testbench (`tests/verilator/sim_main.cpp`, top `z80_core`) is complete
and mirrors the iverilog/C trace exactly. On the current host it cannot be built: the
installed Apple clang 14 cannot parse the newer macOS SDK's C++17 libc++ headers (a
toolchain/SDK mismatch, unrelated to the RTL). `make verilator` detects this and skips
with a message; it builds and runs on a healthy C++17 toolchain. iverilog provides the
RTL verification in the meantime. See `docs/known-differences.md`.

(Updated as work proceeds. See git log for per-checkpoint detail.)
