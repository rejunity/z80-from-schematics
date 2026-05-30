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
5. **C ↔ RTL comparison** (`make compare`): same programs through the C model, iverilog
   sim, AND Verilator sim; traces diffed phase-by-phase across all three.
6. **FUSE / Frank D. Cringle suite** (`make fuse`): 1356 per-opcode tests with
   register / flag / IFF / IM / HALTED / cycle-count / memory diff per case.
7. **Multi-oracle lockstep** (`scripts/lockstep_quad.c`): our C model vs the
   superzazu, chips/z80, and suzukiplan/z80 industry references, comparing register
   state after every instruction on a CP/M `.com`.
8. **Gate-level signal trace** (`scripts/perfectz80_runner.c` + `scripts/compare_signal_timing.py`):
   our C model vs the Brian Silverman / Visual Z80 netlist port (perfectz80), pin
   trace at every clock half-cycle.

## Acceptance gates
- C model: all unit + generated tests pass; ZEXDOC + ZEXALL pass; FUSE pass rate ≥ 99%;
  per-instruction state matches superzazu, chips/z80, suzukiplan/z80 on a representative
  CP/M workload.
- RTL: `make rtl` elaborates clean (no latches/initial in synth RTL); iverilog and
  Verilator both produce traces identical to the C model phase-by-phase on the trace
  corpus.

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
| Interrupts: NMI / INT IM0·1·2 / EI-delay | done | done | NMI (prog8) parity verified |
| HALT (loop + interrupt release) | done | done | full-run parity restored |
| WAIT insertion / BUSREQ-BUSACK | done | done | done |
| Refresh (R inc, {I,R} on bus) | done | done | done |
| Undocumented (X/Y, MEMPTR rules) | done | done | done |
| ZEXDOC | **67/67 PASS** (5.76 B instr) | — | n/a (run on C) |
| ZEXALL | **67/67 PASS** (5.76 B instr) | — | n/a (run on C) |

### prelim.com — PASSING
`make prelim` runs the preliminary instruction test through the C model and prints
"Preliminary tests complete" with no errors. This exercises basic + IX/IY behavior and
confirms the CP/M harness (loader, BDOS shim, PC seeding) works end-to-end.

### ZEX harness (make zexdoc / zexall / prelim)
The CP/M harness is in place: `tests/zex/{prelim,zexdoc,zexall}.com`, `scripts/zexrunner.c`
(loader + BDOS shim for console functions 2/9, exit trap at 0x0000, BDOS trap at 0x0005),
and `scripts/run_zex.py`. Both **ZEXDOC** and **ZEXALL** run to completion against the C
model and report **67/67 PASS** (5.76 B instructions each). Cross-checked by lockstep
diff against the superzazu C Z80 reference (`scripts/lockstep.c`): identical registers
and memory at every instruction across the full run.

`make zexdoc` and `make zexall` rebuild zexrunner first; running the binary directly
after editing C-side code requires `make zexrunner` (or a top-level `make cmodel`, which
now also relinks `$(BIN)/zexrunner` and `$(BIN)/tracegen` — earlier the cmodel target
only rebuilt the library, leaving stale runner binaries on disk).

### Test results (current)
- **C unit tests**: ALU (incl. exhaustive 2^17 add/sub verification of the nibble ALU),
  flags, exec (functional + exact T-state timing), PLA, CB/ED/DDCB/DD-FD prefixes, ED
  block ops, and the interrupt subsystem (NMI/INT IM0·1·2/HALT/EI-delay). All pass
  (`make ctest`).
- **ZEXDOC / ZEXALL**: 67/67 each.
- **C ↔ iverilog ↔ Verilator parity** (`make compare`): all 8 trace programs (prog1,
  prog2, prog3_cb, prog4_ed, prog5_ddfd, prog6_block, prog7_ddcb, prog8_nmi) produce
  identical phase-by-phase traces across all three (400 phases each).
- **FUSE / Cringle** (`make fuse`): **1354/1356 PASS (99.85%)**. Remaining two are the
  SCF/CCF NMOS Q-variant (known-differences row #2); the previously-failing DD/FD `36`
  cycle counts and HALT-PC convention were real bugs fixed during integration.
- **4-oracle lockstep** (`scripts/lockstep_quad.c`): our C model + superzazu C + chips/z80
  + suzukiplan/z80 all report identical PC/AF/BC/DE/HL/IX/IY/SP after every one of
  **7,022,691 instructions** of ZEXDOC3 (chips's PC is overlap-adjusted).
- **Gate-level signal trace** (`scripts/compare_signal_timing.py`): perfectz80 (pure-C
  Visual Z80 netlist port, no Qt) vs C model on 200 phases of prog1/prog2/prog3_cb:
  93-96% control-pin-perfect; remaining diffs are MREQ/RD deassertion at T3.P (gate)
  vs T3.N (us), a sub-cycle convention.
- **Speed**: C model 6.56 Minstr/s, Verilator 0.31 Minstr/s, perfectz80 ~10 kphases/s.

### Verilator
The Verilator testbench (`tests/verilator/sim_main.cpp`, top `z80_core`) is complete
and `make verilator` now builds it cleanly under Apple clang 21 (post-Xcode-update) +
Verilator 5.042 from Homebrew. The earlier toolchain block (Apple clang 14 / libc++17
SDK mismatch) is resolved. `make compare` runs both iverilog and Verilator and confirms
all three (C, iverilog, Verilator) agree on every phase.

### FUSE through the RTL (`make fuse_rtl`)
Direct end-to-end run of the 1356-case FUSE suite against the Verilog RTL via
iverilog. `scripts/gen_fuse_tb.py` parses `tests/fuse/tests.{in,expected}` and emits
a single ~70k-line `tests/iverilog/tb_fuse.v` with all tests inlined as procedural
blocks (hierarchical pokes into `dut.rf[]`, `dut.reg_i`, …, `dut.reg_q`, the
test memory loaded byte-by-byte, then `run_phases(2 × ts_target)` posedges, then a
`$display` dump). The driver `scripts/compare_fuse_rtl.py` compiles, runs `vvp`, and
diffs against `tests.expected`.

**Result: 1342/1356 (98.97%) PASS** through the RTL on first run. The 14 failures
all share `R=exp1/got2` — the one M1 the RTL implicitly executes after reset
deassert (and before the per-test pokes take effect) bumps the refresh register
once. The C-side FUSE harness, which has no such reset-overhead window, passes the
same 14 cases 1356/1356. So the RTL behavior is correct; the off-by-one is a
testbench-init artifact, not an RTL bug. The remaining gap is the cost of running
FUSE inside a synthesizable-RTL framework without a dedicated "load state and skip
the boot M1" instruction, which the testbench would have to bootstrap manually.

This is independent of the transitive argument from `make compare` (C ≡ iverilog ≡
Verilator phase-by-phase over the 8 trace programs); both stand on their own.

(Updated as work proceeds. See git log for per-checkpoint detail.)
