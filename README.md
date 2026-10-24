# RZ80 — Z80 ready for FPGA/ASIC, but also runs in C

[![CI](https://github.com/rejunity/z80-from-schematics/actions/workflows/ci.yml/badge.svg)](https://github.com/rejunity/z80-from-schematics/actions/workflows/ci.yml)
[![Targets](https://github.com/rejunity/z80-from-schematics/actions/workflows/targets.yml/badge.svg)](https://github.com/rejunity/z80-from-schematics/actions/workflows/targets.yml)

**Latest build:** [ASIC layouts, GDS and FPGA bitstreams](https://rejunity.github.io/z80-from-schematics/)
(sky130, IHP SG13G2, GF180MCU with 7- and 9-track cells, and the IHP and GF180 chips with a Z80 pad ring;
IcePi Zero, Tang Primer 25K, iCEBreaker, pico-ice, iCESugar).

A reverse-engineered, schematic-faithful Z80-compatible CPU core, implemented in
parallel as a **C99 hardware-like model** and a **Verilog-2001 RTL core**.

The design follows the original Zilog Z80's *internal organization* — PLA-style
instruction decode, an explicit M-cycle / T-state timing sequencer, named control
signals, an internal register / bus datapath, a Shirriff-informed ALU, an explicit
flags subsystem (including undocumented behavior), refresh, and interrupts — rather
than being a high-level instruction interpreter.

The C model is the fast executable reference; the Verilog RTL implements the *same
conceptual architecture* and is checked against the C model phase-by-phase.
For emulators, `cfast/` generates the model into a phase engine with the same pins
every half clock; [its speed against other C Z80 emulators](cfast/BENCHMARK.md).


## Layout

    cmodel/    C99 hardware-like model
    cfast/     the model generated into a fast phase engine for emulators
               (its speed against other C Z80 emulators: cfast/BENCHMARK.md)
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
    make bus_controls    # bus-contract testbenches: decoder skew, WAIT edge, WR through Tw, INTA
    make iverilog        # build the Icarus Verilog simulation
    make verilator       # build the Verilator simulation
    make compare         # diff C, iverilog, Verilator traces phase-by-phase
    make test            # the quick subset: ctest, rtl, bus_controls, compare
    make fuse            # FUSE's 1356 cases on the C model (6 against the die's values)
    make fuse_rtl        # the same through the RTL via iverilog
    make fuse_pz80       # the same on perfectz80, the die                   (~2 min)
    make prelim          # prelim.com CP/M instruction test
    make zexdoc          # full ZEXDOC                                 (~25 min)
    make zexall          # full ZEXALL                                 (~25 min)
    make phase_identity  # every pin of every phase against PHASE_REF=<rev> (a model change)
    make cfast_identity  # cfast phase by phase against the C model    (~2 min)
    make cfast_bench     # cfast against four other C emulators on ZEXDOC (cfast/BENCHMARK.md)
    make silicon_cycles  # per-opcode T-state check vs real KC85 silicon (sigrok)
    make silicon_async   # real CPU clock + sub-T-state pin offsets from 20 MHz capture
    make halt2int        # INT-to-INTA delay at ten points of a HALT, against the die's
    make lockstep        # our model and four other emulators, instruction by instruction (ZEXDOC3)
    make perfectz80          # gate-level signal-trace diff vs perfectz80 Visual-Z80 netlist (C model)
    make perfectz80_rtl      # same, but trace source is the iverilog RTL testbench
    make synth               # LibreLane (yosys) synthesis → build/synth/z80_top.nl.v
    make iverilog_netlist    # build gate-level iverilog testbench with sky130 cell models
    make perfectz80_netlist  # diff the synthesised sky130 netlist vs perfectz80
    make basic               # NASCOM BASIC 4.7 on the C model
    make tinybasic           # 1 KiB Tiny BASIC on the C model
    make basic_c_tests       # canned-script BASIC ROM regression via C model    (~0.5 s)
    make basic_rtl_tests     # same canned-script regression via Verilator RTL   (~1 s)
    make basic_netlist_tests # same canned-script regression on synthesised gates (~9 min)
    make z80test             # Patrik Rak z80test: z80doc, z80flags, z80memptr, z80full
    make z80test_rtl         # the same four through the Verilator RTL       (6-15 min a variant)
    make zexall_subset_c     # curated 14-test ZEXALL slice via C model         (~2.5 min)
    make zexall_subset_rtl   # same 14-test slice via Verilator                 (~25-55 min)
    make zexall_rtl          # full ZEXALL via Verilator (hours; not in CI, the 14-test subset is)
    make pin_scenarios          # INT/NMI/WAIT/BUSREQ/RESET scenarios — C tracegen path
    make pin_scenarios_rtl      # same scenarios — iverilog RTL path
    make pin_scenarios_netlist  # same scenarios — LibreLane gate-level netlist path
    make event_sweep            # RESET / INT / NMI / BUSREQ / WAIT at every T-state of 31 instructions and every opcode, IM0/1/2, vs perfectz80
    make all-tests           # every gate that needs only a C compiler, iverilog and Verilator (~1.5 h)
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
    nix profile install github:NixOS/nixpkgs/nixos-26.05#verilator   # gate-level BASIC: Verilator 5.048 (≥ 5.030 for UDP tables; 5.050+ mis-simulates the sky130 cell models)


## Status

|     | Gate                                                            | Result                              |
|:---:|-----------------------------------------------------------------|-------------------------------------|
| ✅  | C unit tests (1,069 checks across 13 binaries; incl. per-T-state pin sequence) | PASS                 |
| ✅  | ZEXDOC                                                          | 67 / 67                             |
| ✅  | ZEXALL                                                          | 67 / 67                             |
| ✅  | ZEXALL 14-test subset via Verilator RTL (main + nightly)        | 14 / 14 (~25-55 min)                |
| ✅  | Cringle's prelim                                                | PASS                                |
| ✅  | FUSE corpus, C model                                            | **1356 / 1356**, none skipped (6 against the die's values, `tests/fuse/silicon.expected`) |
| ✅  | FUSE corpus, through RTL (iverilog)                             | **1356 / 1356**                     |
| ✅  | FUSE corpus on the die (perfectz80, `make fuse_pz80`)           | **1356 / 1356** (1350 as FUSE expects, the 6 above as the die runs them) |
| ✅  | Patrik Rak z80test (z80doc / z80flags / z80memptr / z80full)    | **160 / 160** each, C model and RTL |
| 🟡  | Patrik Rak z80ccf; Q (SCF / CCF's X / Y) vs perfectz80          | z80ccf 159 / 160 (test 129, `EX AF,AF'`); Q differs from the die after `EX AF,AF'`, a DD / FD prefix and a RESET cut ([known differences 29-31](docs/known-differences.md); fix in progress on branch `nmos-q`) |
| ✅  | BASIC ROM via C model (NASCOM + Tiny BASIC, canned scripts)     | 4 / 4 subtests                      |
| ✅  | BASIC ROM via Verilator RTL (NASCOM + Tiny BASIC)               | 4 / 4 subtests                      |
| ✅  | BASIC ROM on yosys-synthesised sky130 gate-level netlist        | 4 / 4 subtests (~9 min)             |
| ✅  | C ↔ iverilog ↔ Verilator, phase-by-phase                        | identical, 10 hand + 4 random programs |
| ✅  | cfast ↔ C model, every pin of every phase (`make cfast_identity`) | identical, 3.84 billion phases (both frames and the one-header build) |
| ✅  | 5-way oracle lockstep (mine + superzazu + floooh/chips + suzukiplan + redcode) | identical registers, 7,022,691 instr. (F whole against redcode; X / Y masked for the three that predate the 2018 block-repeat flags) |
| ✅  | Real KC85 silicon — sync   capture (`make silicon_cycles`)      | 50 / 50 OK,  0 emu mismatches       |
| ✅  | Real KC85 silicon — 20 MHz capture (`make silicon_async`)       | CPU ≈ 1.767 MHz, pins ✓             |
| ✅  | HALT-to-INT T-state timing (`make halt2int`)                    | 10/10 delays equal the die's (perfectz80; Brewer 2014 / Woodmass HALT2INT 2021) |
| ✅  | Gate-level vs perfectz80 — **C model** path                     | 14/14 PASS: control pins and the address in every half-cycle (transfers, refresh, internal T-states), the data under WR |
| ✅  | Gate-level vs perfectz80 — **iverilog RTL** path                | 14/14 PASS, the same 14 programs    |
| ✅  | Gate-level vs perfectz80 — **LibreLane sky130 netlist** path    | 14/14 PASS, the same 14 programs ("ultimate test") |
| ✅  | Pin-scenario programs vs perfectz80 (15 INT/NMI/WAIT/BUSREQ/RESET scenarios) | 15/15 in the C model, the RTL and the LibreLane netlist, the address included in every phase |
| ✅  | RESET / INT / NMI / BUSREQ / WAIT timing sweep vs perfectz80 (`make event_sweep`, [docs/event-sweep.md](docs/event-sweep.md)) | 37,846 / 37,846 cases pin-identical in every half-cycle (address, BUSACK and the pad drivers included; after a RESET, every register it keeps), the C model identical to the RTL in all; so is `opcodes_reset`, RESET at every T-state of every opcode (27,768 cases) |

Legend: ✅ pass / 100 % &nbsp; 🟡 ≥ 95 % (close, known artifacts) &nbsp; 🚧 < 95 % (work in progress).

The repository is kept buildable at every checkpoint.


## Documentation

Design contract (read these in order to understand the implementation):

  - [docs/architecture.md](docs/architecture.md) — clocking, phases, pins, datapath,
    module mapping between C and RTL.
  - [docs/clocking.md](docs/clocking.md) — why the RTL runs on the Z80 clock
    itself with every register on the rising edge, how the thin `z80_top`
    wrapper re-creates the both-edges pin behaviour with a few falling-edge
    flops, and what to check in the synthesized netlist (GF180 / sky130).
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
  - [docs/known-differences.md](docs/known-differences.md) — every difference
    from the die found so far, resolved, accepted or open, plus the multi-oracle
    status table.
  - [docs/event-sweep.md](docs/event-sweep.md) — the RESET / INT / NMI / BUSREQ /
    WAIT sweep against perfectz80: how a case works, the groups, the recorded
    perfectz80 traces.
  - [docs/oracles.md](docs/oracles.md) — the four Z80 emulators
    (floooh/chips, superzazu, suzukiplan, redcode) used as oracles, their
    authorship, lineage, strengths, and how we triangulate against
    them. Includes the pass/fail matrix on Patrik Rak's z80test and
    the FUSE corpus.
  - [docs/research-notes.md](docs/research-notes.md) — catalog of sources that informed
    this core, with trust levels and source-conflict precedence.
  - [docs/real-silicon-traces.md](docs/real-silicon-traces.md) — how the sigrok KC85
    logic-analyzer captures are decoded and turned into a real-silicon T-state /
    sub-T-state-timing oracle.
  - [asic/timing/README.md](asic/timing/README.md) — the chips' pins against the
    NMOS Z80's datasheet (Zilog PS0178), at every process corner;
    [asic/spice/README.md](asic/spice/README.md) — the pads, the NMI pulse catcher
    and the whole GF180 chip at transistor level.

Tests and ROMs:

  - **[tests/README.md](tests/README.md)** — comprehensive test-suite
    overview: every test type, what it covers, how to run it, current
    result, and links to the per-subdirectory READMEs (`tests/common/`,
    `tests/fuse/`, `tests/zex/`, `tests/z80test/`, `tests/basic/`,
    `tests/traces/`, `tests/sigrok/`).
  - [docs/simplifications.md](docs/simplifications.md) — silicon-faithfulness
    audit (A–F): where the model's structure is simpler than the die's, and
    what became of each item.
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


## License

The project's original **C model, support software, and documentation are
MIT-licensed**. Its original **RTL and hardware implementation sources in
`rtl/` and `asic/librelane/` use CERN-OHL-W-2.0**. These are licenses for separate
components, not a choice of either license for every file.

Commercial use is welcome under the applicable terms. Hardware manufacturers
may sell compliant products without paying a royalty to the project author;
retain the required notices and make the required source available. See
[LICENSE](LICENSE) and [HARDWARE_NOTICE.md](HARDWARE_NOTICE.md).

Third-party reference emulators, ROMs, tests, and netlists keep their upstream
terms. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), including its
unresolved provenance items, and [CONTRIBUTING.md](CONTRIBUTING.md).
