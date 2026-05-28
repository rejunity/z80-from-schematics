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

## Build & test

```sh
make cmodel     # build the C model static lib
make ctest      # build + run C unit/instruction tests
make rtl        # elaborate the Verilog RTL (iverilog -t null)
make iverilog   # build the Icarus Verilog simulation
make verilator  # build the Verilator simulation
make traces     # emit shared-format bus-cycle traces
make compare    # diff C vs Verilog traces phase-by-phase
make test       # full suite
make clean
```

Requires: a C99 compiler (clang/gcc), `iverilog`, `verilator`, `python3`, `make`.

## Status

Under active, incremental construction. See `docs/` for the design contract and
`docs/verification.md` for the current verification state. The repository is kept
buildable at every checkpoint.
