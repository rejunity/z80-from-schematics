# RZ80 — Z80 ready for FPGA/ASIC, but also runs in C

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
    make fuse            # FUSE / Frank D. Cringle 1356-case suite on the C model
    make fuse_rtl        # FUSE 1356-case suite driven through the RTL via iverilog
    make prelim          # prelim.com CP/M instruction test
    make zexdoc          # full ZEXDOC                                 (~1  min)
    make zexall          # full ZEXALL                                 (~16 min)
    make silicon_cycles  # per-opcode T-state check vs real KC85 silicon (sigrok)
    make silicon_async   # real CPU clock + sub-T-state pin offsets from 20 MHz capture
    make basic           # NASCOM BASIC 4.7 on the C model
    make tinybasic       # 1 KiB Tiny BASIC on the C model
    make all-tests       # every gate above in sequence
    make clean

Requires: a C99 compiler, `iverilog`, `verilator`, `python3`, `make`. The Verilator
build needs a working C++17 toolchain (Apple clang 21+ or any modern gcc / clang).


## Status

|     | Gate                                                            | Result                              |
|:---:|-----------------------------------------------------------------|-------------------------------------|
| ✅  | C unit tests                                                    | PASS                                |
| ✅  | ZEXDOC                                                          | 67 / 67                             |
| ✅  | ZEXALL                                                          | 67 / 67                             |
| ✅  | FUSE / Cringle (C)                                              | **1356 / 1356**  (100 %)            |
| 🟡  | FUSE / Cringle (through RTL via iverilog)                       | **1342 / 1356**  (98.97 %) \*       |
| ✅  | C ↔ iverilog ↔ Verilator, phase-by-phase                        | identical, 8 trace programs         |
| ✅  | 4-way oracle lockstep (mine + superzazu + chips + suzukiplan)   | identical, 7,022,691 instr.         |
| ✅  | Real KC85 silicon — sync   capture (`make silicon_cycles`)      | 50 / 50 OK,  0 emu mismatches       |
| ✅  | Real KC85 silicon — 20 MHz capture (`make silicon_async`)       | CPU ≈ 1.767 MHz, pins ✓             |
| 🚧  | Gate-level signal trace vs perfectz80 (Visual Z80 netlist)      | 93 – 96 % control-pin perfect       |

Legend: ✅ pass / 100 % &nbsp; 🟡 ≥ 95 % (close, known artifacts) &nbsp; 🚧 < 95 % (work in progress).

\* 14 testbench-init artifacts (post-reset M1 overhead), NOT RTL bugs.

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
  - [docs/research-notes.md](docs/research-notes.md) — catalog of sources that informed
    this core, with trust levels and source-conflict precedence.
  - [docs/real-silicon-traces.md](docs/real-silicon-traces.md) — how the sigrok KC85
    logic-analyzer captures are decoded and turned into a real-silicon T-state /
    sub-T-state-timing oracle.

Tests and ROMs:

  - [tests/basic/README.md](tests/basic/README.md) — the BASIC ROMs and their I/O
    conventions.

The brief that this project follows: [z80_core_project_BRIEF.md](z80_core_project_BRIEF.md).
