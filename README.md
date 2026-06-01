# z80-from-schematics

A reverse-engineered, schematic-faithful Z80-compatible CPU core, implemented in
parallel as a **C99 hardware-like model** and a **Verilog-2001 RTL core**.

The design follows the original Zilog Z80's *internal organization* — PLA-style
instruction decode, an explicit M-cycle / T-state timing sequencer, named control
signals, an internal register/bus datapath, a Shirriff-informed ALU, an explicit
flags subsystem (including undocumented behavior), refresh, and interrupts — rather
than being a high-level instruction interpreter.

The C model is the fast executable reference; the Verilog RTL implements the *same
conceptual architecture* and is checked against the C model phase-by-phase.

## Layout

```
cmodel/    C99 hardware-like model
rtl/       Verilog-2001 synthesizable RTL
tests/     unit + instruction + timing-trace tests, ZEX suites, sim TBs
scripts/   Python test generators and trace comparators
docs/       research notes, architecture, PLA, timing, ALU, flags, verification
```

## Run BASIC!

The very first thing you can do on this faithul Z80 reconstruction is to run classical BASIC.

```sh
make basic
```

This will take you back in time to the BASIC prompt:
```
[basicrunner] loaded 8154 bytes from tests/basic/nascom_basic_4_7_rc2014.hex (intel hex)
[basicrunner] Ctrl-\ exits the runner; Ctrl-C or Ctrl-Space => BREAK in BASIC

RC2014 - MS Basic Loader
z88dk - feilipu

Memory top? 
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
```

## Build & test

```sh
make cmodel     # build the C model static lib + CLI binaries
make ctest      # C unit/instruction tests
make rtl        # elaborate the Verilog RTL (iverilog -t null)
make iverilog   # build the Icarus Verilog simulation
make verilator  # build the Verilator simulation
make traces     # emit shared-format bus-cycle traces
make compare    # diff C, iverilog, Verilator traces phase-by-phase
make fuse       # FUSE / Frank D. Cringle 1356-case suite on the C model
make fuse_rtl   # FUSE 1356-case suite driven through the RTL via iverilog
make prelim     # prelim.com CP/M instruction test
make zexdoc     # full ZEXDOC (~1 min on this host)
make zexall     # full ZEXALL (~16 min on this host)
make silicon_cycles # per-opcode T-state check vs real KC85 silicon (sigrok)
make silicon_async  # real CPU clock + sub-T-state pin offsets from 20 MHz capture
make all-tests  # every gate above in sequence
make clean
```

Requires: a C99 compiler, `iverilog`, `verilator`, `python3`, `make`. The Verilator
build needs a working C++17 toolchain (Apple clang 21+ or any modern gcc/clang).

## Status

| Gate | Result |
|------|--------|
| C unit tests | PASS |
| ZEXDOC | 67/67 |
| ZEXALL | 67/67 |
| FUSE / Cringle (C) | **1356/1356 (100%)** |
| FUSE / Cringle (through RTL) | **1342/1356 (98.97%)** — 14 testbench-init artifacts |
| C ↔ iverilog ↔ Verilator phase-by-phase | identical on all 8 trace programs |
| 4-way oracle lockstep (mine + superzazu + chips/z80 + suzukiplan/z80) | identical on 7,022,691 instructions of ZEXDOC3 |
| Gate-level signal trace vs perfectz80 (Visual Z80 netlist port) | 93–96% control-pin perfect |

The repository is kept buildable at every checkpoint. See `docs/architecture.md` for
the design contract and `docs/verification.md` for the current verification state;
`docs/known-differences.md` records every deliberate or watching divergence from
the reference behavior, with the four-oracle status table.
