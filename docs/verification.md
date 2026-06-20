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
6. **FUSE corpus** (`make fuse`): 1356 per-opcode tests with register / flag
   / IFF / IM / HALTED / cycle-count / memory diff per case. **Note**: FUSE's
   `tests.expected` was authored by Philip Kendall in 2006 for FUSE itself
   and was never silicon-validated; 7 cases differ from real silicon (see
   `tests/fuse/known-fuse-wrong.txt`). The runner tallies those as
   "known-FUSE-wrong" rather than FAIL so the gate stays at 100 % on the
   silicon-faithful side.
7. **Multi-oracle lockstep** (`scripts/lockstep_quint.c`): our C model vs the
   superzazu, chips/z80, suzukiplan/z80 and redcode/Z80 industry references —
   five-way register diff after every instruction on a CP/M `.com`. See
   [docs/oracles.md](oracles.md) for the per-oracle lineage and matrix.
8. **Patrik Rak z80test** (`make z80test`): doc / memptr / full ~470 tests
   covering documented + undocumented behavior, MEMPTR / WZ, SCF / CCF Q-leak,
   Banks block-instruction repeat fold-in. All silicon-validated (Rak on real
   48K Spectrum; Chandler on NEC + Visual Z80 Remix). **160 / 160 / 160 PASS.**
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

| Area                                                       | C model      | RTL          | C ↔ RTL parity              |
|------------------------------------------------------------|--------------|--------------|-----------------------------|
| Build scaffold                                             | done         | done         | n/a                         |
| Timing sequencer (phase / T / M, WAIT hook)                | done         | done         | done                        |
| ALU + flags (8-bit, inc / dec, rot-A, DAA / CPL / SCF / CCF) | done         | done         | done                        |
| Register file + WZ / MEMPTR                                | done         | done         | done                        |
| M1 fetch + refresh + core 8-bit ops                        | done         | done         | done                        |
| Memory r / w + (HL) RMW ops                                | done         | done         | done                        |
| 16-bit load / inc / dec / ADD HL                           | done         | done         | done                        |
| Branch / call / ret / rst / push / pop / DJNZ / JR         | done         | done         | done                        |
| I/O (IN / OUT n,A), EX / EXX / EX(SP)                      | done         | done         | done                        |
| CB prefix (rot / shift / BIT / RES / SET)                  | done         | done         | done                        |
| ED prefix (ADC / SBC, LD I/R, NEG, IM, RETI / RETN, …)     | done         | done         | done                        |
| ED block ops (LDI .. OTDR)                                 | done         | done         | done                        |
| DD / FD (IX / IY, IXH / IXL, (IX+d))                       | done         | done         | done                        |
| DDCB / FDCB (op (IX+d) + undoc copy)                       | done         | done         | done                        |
| Interrupts: NMI / INT IM0·1·2 / EI-delay                   | done         | done         | NMI (prog8) parity verified |
| HALT (loop + interrupt release)                            | done         | done         | full-run parity restored    |
| WAIT insertion / BUSREQ-BUSACK                             | done         | done         | done                        |
| Refresh (R inc, {I,R} on bus)                              | done         | done         | done                        |
| Undocumented (X / Y, MEMPTR, SCF / CCF Q variant)          | done         | done         | done                        |
| ZEXDOC                                                     | **67 / 67**  | —            | n/a (run on C model)        |
| ZEXALL                                                     | **67 / 67**  | —            | n/a (run on C model)        |

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
- **FUSE corpus** (`make fuse`): **1348 PASS + 8 known-FUSE-wrong**. The 8
  exceptions all live in `tests/fuse/known-fuse-wrong.txt` with
  silicon-validation citations: Banks-2018 block fold-in (edb9_2 CPDR
  repeat), Sean Young §4.1 Q-leak SCF/CCF (37_1 / 3f), boo-boo 2006
  MEMPTR (edba_1 INDR / edbb_1 OTDR / edb2_1 INIR / edb3_1 OTIR with
  BC=1), Brewer 2014 / Woodmass HALT2INT 2021 HALT-PC convention
  (test `76`). We deliberately sit on the silicon-faithful side; the
  pre-Banks/pre-MEMPTR/pre-Brewer FUSE expecteds are the outliers.
- **FUSE through RTL** (`make fuse_rtl`): **1348 PASS + 8 known-FUSE-wrong
  (matches C model)**. The earlier 14 testbench-init artifacts (post-reset
  M1 with stale m_addr) and the iverilog-12 sensitivity bug on `rf[hlp]`
  through `always @*` function calls were fixed earlier in the project.
- **Patrik Rak z80test** (`make z80test`): **160 / 160 / 160 PASS** since
  the 2026-06-18 silicon-faithfulness sweep: ULA-idle port-parity, SCF/CCF
  Q-leak formula correction, Banks-2018 LDIR/LDDR/CPIR/CPDR/INIR/INDR/
  OTIR/OTDR repeat-M-cycle YF/XF (and HF/PF/CF for IO-block) fold-in. All
  three variants (z80doc, z80memptr, z80full) clean.
- **5-oracle lockstep** (`scripts/lockstep_quint.c`): our C model +
  superzazu C + chips/z80 + suzukiplan/z80 + redcode/Z80 all report
  identical PC/AF/BC/DE/HL/IX/IY/SP after every one of **7,022,691
  instructions** of ZEXDOC3.
- **Gate-level signal trace** (`scripts/compare_signal_timing.py`,
  `make perfectz80`): perfectz80 (pure-C Visual Z80 netlist port, no
  Qt) vs C model on 200 phases of all 12 trace programs
  (`prog1..prog8` + `prog_rnd_01..04`): **12 / 12 PASS, 100 %
  control-pin parity**, with informational addr / data_o diffs on
  three random programs traceable to C1 (reset register init at
  `0xFFFF` vs perfectz80's `0x5555`). Same per-phase parity through
  the iverilog RTL (`make perfectz80_rtl`, 12/12 PASS) and through
  the LibreLane sky130-synthesised netlist
  (`make perfectz80_netlist`, 12/12 PASS — the "ultimate test"). CI
  uploads per-program `.vcd` files (both our pins at top scope and
  perfectz80's pins under `perfectz80.*` scope in the same file) as
  the `parity-vcds` + `netlist-vcds` artifacts (14-day retention).
- **Pin-scenario programs** (`make pin_scenarios` /
  `pin_scenarios_rtl` / `pin_scenarios_netlist`): 12 INT / NMI /
  WAIT / BUSREQ / RESET scenarios driven through the `.events`
  sidecar. **8 / 12 PASS cleanly on BOTH C model and iverilog RTL**
  after Steps 0-5 of the silicon-faithful sweep + the harness-side
  reset-window mask (`prog9_inta_im1`, `prog12_inta_im2`,
  **`prog15_busreq_m1`** (Step 5), `prog16_ei_delay`,
  **`prog17_reset`** (Step 4 simplified), `prog18_di_then_int`,
  `prog19_nmi_in_int`, `prog20_block_int`). 1 / 12 with a single
  ctrl-pin diff (`prog10_halt_nmi`). 3 / 12 surface informational
  WAIT / HALT-NOP diffs (`prog11_wait_mem` 142+, `prog13_halt_int` 144,
  `prog14_wait_io` 147) — root-caused to two classes (sub-T-state
  silicon timing + pz80 oracle-harness event-application offset);
  the C model's WAIT sample point is canonical per UM0080 §3.5.1.
  All make targets are informational gates; functional behaviour on
  all 12 is verified by Rak + FUSE + `make halt2int`. Full per-program
  mechanism + resolution-class table in `docs/perfect-branch.md`
  "Remaining differences vs perfectz80"; diff list summary in
  `docs/known-differences.md` row 14.
- **HALT-to-INT timing probe** (`make halt2int`): focused regression
  inspired by Mark Woodmass's HALT2INT v3 (2021). Sweeps INT-pulse
  timing across the HALT NOP M-cycle and verifies the INT-to-INTA
  delay stays in the silicon-faithful 3..8 T-state range (per
  Brewer 2014 + Z80 datasheet). Output on the current model: all 10
  sweep samples in 4..7 T (PASS). Complements the HALT-PC
  convention flip — together they cover the silicon properties
  HALT2INT v3 measures on real hardware without depending on
  Spectrum ULA contention modeling.
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

**Result: 1356/1356 (100%) PASS** through the RTL. The first-run gap (1342/1356)
turned out to be two distinct issues, both fixed:

1. **Testbench in-flight M1 from wrong address.** After `do_reset()` the RTL has
   already begun an M1 fetch with `m_addr=0` (from reset). The per-test pokes set
   `dut.rf[PC]` but not `dut.m_addr`, so for tests with non-zero PC the in-flight
   M1 read `mem[0]` (junk) instead of `mem[PC]`. For RST tests where the test's
   PC=0 and `mem[0]` happened to be the test's RST opcode, this caused the RST to
   execute twice — once from the in-flight M1, once for real. Fix: also poke
   `dut.m_addr = test_pc` after the do_reset, mirroring what `z80_set_pc()` does
   on the C side.

2. **iverilog 12 sensitivity bug on `rf[hlp]` through functions.** The ALU
   operand mux called `getri(rf_src_w)` which internally read `rf[hlp][...]`.
   iverilog's `always @*` failed to propagate the array-index dependency through
   the function call, so when `idx_w` transitioned 0→2 under DD/FD prefix the mux
   did not re-fire — it kept the unprefixed H/L value. Fix: replace the function
   with a continuous wire (`getri_src_val` / `getri_dst_val`) that reads
   `rf[hlp][...]` directly in the wire's RHS, so iverilog sees the dependency
   and the wire re-evaluates correctly.

This is independent of the transitive argument from `make compare` (C ≡ iverilog ≡
Verilator phase-by-phase over the 8 trace programs); both stand on their own.

## See also

  - [architecture.md](architecture.md) — the shared C / RTL design contract.
  - [known-differences.md](known-differences.md) — every deliberate or watching
    divergence, plus the multi-oracle status table.
  - [real-silicon-traces.md](real-silicon-traces.md) — how the sigrok KC85
    captures are used as a real-silicon T-state / sub-T-state oracle.
  - [research-notes.md](research-notes.md) — catalog of sources informing this
    core and the source-conflict precedence we follow.

(Updated as work proceeds. See git log for per-checkpoint detail.)
