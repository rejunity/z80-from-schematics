# RZ80 — Z80 ready for FPGA/ASIC, but also runs in C

[![CI](https://github.com/rejunity/z80-from-schematics/actions/workflows/ci.yml/badge.svg)](https://github.com/rejunity/z80-from-schematics/actions/workflows/ci.yml)

A reverse-engineered, schematic-faithful Z80-compatible CPU core, implemented in
parallel as a **C99 hardware-like model** and a **Verilog-2001 RTL core**.

The design follows the original Zilog Z80's *internal organization* — PLA-style
instruction decode, an explicit M-cycle / T-state timing sequencer, named control
signals, an internal register / bus datapath, a Shirriff-informed ALU, an explicit
flags subsystem (including undocumented behavior), refresh, and interrupts — rather
than being a high-level instruction interpreter.

The C model is the fast executable reference; the Verilog RTL implements the *same
conceptual architecture* and is checked against the C model phase-by-phase.


## Layout

    cmodel/    C99 hardware-like model
    rtl/       Verilog-2001 synthesizable RTL
    tests/     unit + instruction + timing-trace tests, ZEX suites, sim TBs
    scripts/   Python and C test generators, oracle runners, trace comparators
    docs/      design contract, research notes, verification, known differences


## Run BASIC!

The very first thing you can do on this faithful Z80 reconstruction is to run
classical BASIC.

    make basic

This will take you back in time to the BASIC prompt:

    Z80 BASIC Ver 4.7c
    Copyright (C) 1978 by Microsoft
    31948 Bytes free
    Ok
    10 PRINT "HELLO Z80"
    20 GOTO 10
    RUN
    HELLO Z80
    HELLO Z80
    HELLO Z80
    HELLO Z80
    HELLO Z80
    Break in 10
    Ok

See [tests/basic/README.md](tests/basic/README.md) for the BASIC ROMs we vendor
(NASCOM BASIC 4.7 and 1 KiB Tiny BASIC), the I/O conventions, and the runner's
interactive controls (Enter, Backspace, Ctrl-C / Ctrl-Space = BREAK, Ctrl-\\ = exit).


## Build & test

    make cmodel          # build the C model static lib + CLI binaries
    make ctest           # C unit / instruction tests
    make rtl             # elaborate the Verilog RTL (iverilog -t null)
    make iverilog        # build the Icarus Verilog simulation
    make verilator       # build the Verilator simulation
    make traces          # emit shared-format bus-cycle traces
    make compare         # diff C, iverilog, Verilator traces phase-by-phase
    make fuse            # FUSE 1356-case suite on C model (1348 PASS + 8 known-FUSE-wrong)
    make fuse_rtl        # FUSE 1356-case suite through RTL via iverilog (matches C)
    make prelim          # prelim.com CP/M instruction test
    make zexdoc          # full ZEXDOC                                 (~1  min)
    make zexall          # full ZEXALL                                 (~16 min)
    make silicon_cycles  # per-opcode T-state check vs real KC85 silicon (sigrok)
    make silicon_async   # real CPU clock + sub-T-state pin offsets from 20 MHz capture
    make perfectz80          # gate-level signal-trace diff vs perfectz80 Visual-Z80 netlist (C model)
    make perfectz80_rtl      # same, but trace source is the iverilog RTL testbench
    make synth               # LibreLane (yosys) synthesis → build/synth/z80_core.nl.v
    make iverilog_netlist    # build gate-level iverilog testbench with sky130 cell models
    make perfectz80_netlist  # diff the synthesised sky130 netlist vs perfectz80
    make basic               # NASCOM BASIC 4.7 on the C model
    make tinybasic           # 1 KiB Tiny BASIC on the C model
    make basic_c_tests       # canned-script BASIC ROM regression via C model    (~0.5 s)
    make basic_rtl_tests     # same canned-script regression via Verilator RTL   (~1 s)
    make basic_netlist_tests # same canned-script regression on synthesised gates (~9 min)
    make z80test             # Patrik Rak z80test (doc / memptr / full) baseline
    make zexall_subset_c     # curated 14-test ZEXALL slice via C model         (~90 s)
    make zexall_subset_rtl   # same 14-test slice via Verilator                 (~15-25 min)
    make zexall_rtl          # full ZEXALL via Verilator (local-only; not in CI — known fails)
    make pin_scenarios          # INT/NMI/WAIT/BUSREQ/RESET scenarios — C tracegen path
    make pin_scenarios_rtl      # same scenarios — iverilog RTL path
    make pin_scenarios_netlist  # same scenarios — LibreLane gate-level netlist path
    make all-tests           # every gate above in sequence
    make clean

Requires: a C99 compiler, `iverilog`, `verilator`, `python3`, `make`. The Verilator
build needs a working C++17 toolchain (Apple clang 21+ or any modern gcc / clang).

The LibreLane gate-level flow (`make synth` / `_netlist*`) additionally requires
Nix + LibreLane. Install via the Determinate Systems installer with the
fossi-foundation substituter configured (see [docs/librelane-flow.md](docs/librelane-flow.md)):

    curl --proto '=https' --tlsv1.2 -fsSL https://install.determinate.systems/nix \
      | sh -s -- install --no-confirm --extra-conf "
          extra-substituters = https://nix-cache.fossi-foundation.org
          extra-trusted-public-keys = nix-cache.fossi-foundation.org:3+K59iFwXqKsL7BNu6Guy0v+uTlwsxYQxjspXzqLYQs=
          extra-experimental-features = nix-command flakes
        "
    nix profile install github:librelane/librelane
    nix profile install nixpkgs#verilator   # required for gate-level BASIC (Verilator ≥ 5.030)


## Status

|     | Gate                                                            | Result                              |
|:---:|-----------------------------------------------------------------|-------------------------------------|
| ✅  | C unit tests (267 checks across 11 binaries; incl. per-T-state pin sequence) | PASS                   |
| ✅  | ZEXDOC                                                          | 67 / 67                             |
| ✅  | ZEXALL                                                          | 67 / 67                             |
| ✅  | ZEXALL 14-test subset via Verilator RTL (main + nightly)        | 14 / 14 (~17 min)                   |
| ✅  | FUSE corpus, C model                                            | **1348 / 1356 + 8 known-FUSE-wrong** (100 %) |
| ✅  | FUSE corpus, through RTL (iverilog)                             | **1348 / 1356 + 8 known-FUSE-wrong** (100 %) |
| ✅  | Patrik Rak z80test (doc / memptr / full)                        | **160 / 160 / 160** (100 %)         |
| ✅  | BASIC ROM via C model (NASCOM + Tiny BASIC, canned scripts)     | 4 / 4 subtests                      |
| ✅  | BASIC ROM via Verilator RTL (NASCOM + Tiny BASIC)               | 4 / 4 subtests                      |
| ✅  | BASIC ROM on yosys-synthesised sky130 gate-level netlist        | 4 / 4 subtests (~9 min)             |
| ✅  | C ↔ iverilog ↔ Verilator, phase-by-phase                        | identical, 8 hand + 4 random programs |
| ✅  | 5-way oracle lockstep (mine + superzazu + chips + suzukiplan + redcode) | identical, 7,022,691 instr.    |
| ✅  | Real KC85 silicon — sync   capture (`make silicon_cycles`)      | 50 / 50 OK,  0 emu mismatches       |
| ✅  | Real KC85 silicon — 20 MHz capture (`make silicon_async`)       | CPU ≈ 1.767 MHz, pins ✓             |
| ✅  | Gate-level vs perfectz80 — **C model** path                     | 100 % ctrl pins; `addr` 100 % on 10/12 (don't-care tolerance); `data_o` 100 % |
| ✅  | Gate-level vs perfectz80 — **iverilog RTL** path                | 100 % ctrl pins, same 12 programs    |
| ✅  | Gate-level vs perfectz80 — **LibreLane sky130 netlist** path    | 100 % ctrl pins, same 12 programs ("ultimate test") |
| 🚧  | Pin-scenario programs vs perfectz80 (12 programs across C / RTL / netlist paths) | informational; surfaces real audit findings |

Legend: ✅ pass / 100 % &nbsp; 🟡 ≥ 95 % (close, known artifacts) &nbsp; 🚧 < 95 % (work in progress).

The repository is kept buildable at every checkpoint.


## Documentation

Design contract (read these in order to understand the implementation):

  - [docs/architecture.md](docs/architecture.md) — clocking, phases, pins, datapath,
    module mapping between C and RTL.
  - [docs/timing.md](docs/timing.md) — M-cycle / T-state / phase model; per-bus-cycle
    timing diagrams matching the C model and the RTL.
  - [docs/pla.md](docs/pla.md) — PLA decode table and the control-word fields.
  - [docs/alu.md](docs/alu.md) — ALU operations and the nibble-carry chain.
  - [docs/flags.md](docs/flags.md) — flag update rules (documented and undocumented).
  - [docs/undocumented.md](docs/undocumented.md) — X / Y flags, WZ / MEMPTR, the
    DDCB / FDCB undoc copy, OUT (C),0 and friends.

Verification and reverse-engineering background:

  - [docs/verification.md](docs/verification.md) — current verification state, layers,
    acceptance gates, results.
  - [docs/known-differences.md](docs/known-differences.md) — every deliberate or
    watching divergence from reference behavior, plus the multi-oracle status table.
  - [docs/oracles.md](docs/oracles.md) — the four Z80 emulators
    (chips, superzazu, suzukiplan, redcode) used as oracles, their
    authorship, lineage, strengths, and how we triangulate against
    them. Includes the pass/fail matrix on Patrik Rak's z80test and
    the FUSE corpus.
  - [docs/research-notes.md](docs/research-notes.md) — catalog of sources that informed
    this core, with trust levels and source-conflict precedence.
  - [docs/real-silicon-traces.md](docs/real-silicon-traces.md) — how the sigrok KC85
    logic-analyzer captures are decoded and turned into a real-silicon T-state /
    sub-T-state-timing oracle.

Tests and ROMs:

  - **[tests/README.md](tests/README.md)** — comprehensive test-suite
    overview: every test type, what it covers, how to run it, current
    result, and links to the per-subdirectory READMEs (`tests/common/`,
    `tests/fuse/`, `tests/zex/`, `tests/z80test/`, `tests/basic/`,
    `tests/traces/`, `tests/sigrok/`).
  - [docs/ring3-az80-oracle.md](docs/ring3-az80-oracle.md) — design sketch
    for the deferred second gate-level oracle (gdevic/A-Z80).
  - [docs/simplifications.md](docs/simplifications.md) — silicon-faithfulness
    audit (A1, A3, A4, B, C1, D, E1, F) — the source of truth for the
    deliberate vs unintended divergences from gate-level Z80.
  - [docs/librelane-flow.md](docs/librelane-flow.md) — the LibreLane (Yosys
    + sky130) gate-level synthesis flow: how `make synth` produces the
    netlist, the sky130 cell-library + Verilator version gotcha, the CI
    cache strategy.
  - [docs/perfect-branch.md](docs/perfect-branch.md) — record of the
    `perfect` branch's targeted closures: address-bus don't-care
    tolerance, VCD output, `.events` in RTL testbenches, pin-scenario
    RTL / netlist gates.

The perfectz80 comparison scripts also emit per-program VCD waveforms
to `build/vcd/<prog>.<source>.vcd` containing both our pins (top scope)
and perfectz80's pins (`perfectz80.*` scope) — open with GTKWave /
Surfer to inspect side-by-side from one file. Disable with `EMIT_VCD=0`.

The brief that this project follows: [z80_core_project_BRIEF.md](z80_core_project_BRIEF.md).
