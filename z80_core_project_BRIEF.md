# Project brief: reverse-engineered, schematic-faithful Z80 core in C99 and Verilog-2001

## Goal

Design and implement a new Z80-compatible CPU core using reverse-engineered knowledge of the original Zilog Z80 as the architectural guide. The goal is not merely to create a behavioral clone. The goal is to produce a modern, readable, synthesizable implementation that maps as closely as practical to the original Z80’s conceptual structure: PLA decode, timing sequencer, buses, control signals, ALU organization, register-file behavior, increment/decrement paths, refresh logic, interrupt behavior, and undocumented instruction behavior.

Treat available reverse-engineered schematics, PLA tables, transistor-level investigations, die analysis, and gate-level simulations as a kind of “disassembly” of the original chip. Convert that into a human-readable, maintainable, hardware-first implementation.

The target outputs are:

1. A C99 model for fast development and test iteration.
2. A Verilog-2001 RTL implementation for synthesis and simulation.
3. Shared tests and traces that make the C and Verilog implementations easy to compare.
4. A self-sufficient development loop that allows code generation, testing, debugging, and correction without requiring further human instructions.

## Primary design philosophy

Build the design around the Z80’s original internal organization rather than around a high-level instruction interpreter.

The implementation should preserve recognizable original structures where practical:

- PLA-style instruction decode table.
- Explicit control-signal lines.
- Timing sequencer corresponding to M-cycles and T-states.
- Positive-edge and negative-edge reference timing behavior.
- Internal buses and bus ownership.
- Register file and alternate register set behavior.
- WZ/MEMPTR behavior.
- Increment/decrement paths.
- ALU structure informed by Ken Shirriff’s reverse-engineering work.
- Flag generation and undocumented flag behavior.
- Refresh register and memory refresh timing.
- Interrupt flip-flops, interrupt modes, NMI, IRQ, HALT, WAIT, BUSREQ/BUSACK.
- Indexed instruction paths for IX/IY.
- Prefix decode structure for CB, ED, DD, FD, DDCB, and FDCB.
- Undocumented opcodes and documented traps/edge cases.

The PLA should remain close to a ROM-like structure. Do not collapse it into opaque behavioral `case` statements unless there is a clear reason. Prefer a compact decode table that emits named control signals.

## Research phase

First collect, inspect, and summarize all relevant sources before implementation.

Use at least these sources:

- Zilog Z80 official user manuals and timing documentation.
- Ken Shirriff’s Z80 reverse-engineering articles, especially ALU and instruction decode analysis.
- Visual6502 / Visual Z80 transistor-level or gate-level data and simulator resources.
- Known Z80 PLA decode tables and die-reconstruction work.
- Existing schematic or transistor-level reverse-engineering projects.
- MAME Z80 implementation.
- Other mature C-level Z80 cores.
- Existing Verilog/VHDL Z80 cores only as secondary behavioral references, not as the design authority.
- ZEXDOC, ZEXALL, and related undocumented-instruction test suites.
- Known Z80 “trap” and edge-case analyses.

During research, create a `docs/research-notes.md` file that records:

- Source name.
- URL or local path.
- What it contributes.
- How trustworthy it is.
- Whether it is primary documentation, reverse-engineering evidence, emulator behavior, or secondary interpretation.
- Any contradictions with other sources.
- Open questions.

When sources disagree, prefer in this order:

1. Original Zilog timing and programmer documentation for documented behavior.
2. Die-level, transistor-level, PLA-level, or gate-level reverse-engineering evidence.
3. Exhaustive hardware behavior tests from real Z80-family chips.
4. Mature emulator behavior with strong test coverage, especially MAME.
5. Other emulator cores or FPGA cores.
6. Informal forum posts, unless supported by tests or die evidence.

## Target implementation style

Use C99 and Verilog-2001.

For Verilog:

- Use `.v` files only.
- Use explicit module ports.
- Use `wire`, `reg`, `always @*`, `always @(posedge clk)`, and straightforward synthesizable RTL.
- Use `localparam` for constants.
- Use `generate` only where it improves clarity.
- Do not use SystemVerilog features.
- Do not use `initial` blocks in synthesizable logic.
- Do not infer latches.
- Do not use transparent latches.
- Use D flip-flops only.
- Make reset behavior explicit and ASIC-suitable.
- Keep the implementation FPGA-friendly.
- Prefer one clock domain.
- Build a modern single-clock synchronous implementation while respecting the original Z80’s positive-edge and negative-edge externally visible timing.

For C:

- Use C99.
- Make the C model hardware-like, not interpreter-like.
- Use explicit structs for registers, buses, control signals, sequencer state, PLA outputs, ALU inputs/outputs, and pins.
- Step the C model by half-cycle, T-state, or another timing unit that can map directly to Verilog.
- Keep naming aligned with the Verilog implementation.
- Make the C and Verilog easy to compare side-by-side.
- Avoid clever abstractions that hide hardware behavior.

The C model should act as a fast executable reference for the RTL, but both should share the same conceptual architecture.

## Timing requirement

The design must be sub-cycle accurate relative to Z80 reference timing.

It must respect:

- M1 cycle behavior.
- Opcode fetch timing.
- Memory read/write timing.
- I/O read/write timing.
- Refresh timing.
- WAIT sampling and insertion.
- Interrupt sampling.
- HALT timing.
- BUSREQ/BUSACK timing.
- Positive and negative edge relationships described in the Z80 documentation.
- Correct external pin transitions for MREQ, IORQ, RD, WR, RFSH, M1, HALT, WAIT, INT, NMI, BUSREQ, BUSACK, address bus, and data bus.

Because the implementation uses a single modern clock and DFFs only, model original positive/negative edge behavior using explicit phase state, enable strobes, and registered outputs rather than actual dual-edge clocking.

Suggested approach:

- Represent each Z80 T-state as two internal phases: `PHI_P` and `PHI_N`, or equivalent.
- Make all state update on the positive edge of the modern clock.
- Use phase registers and combinational next-state logic to emulate original half-cycle behavior.
- Ensure external pin timing matches the reference timing diagrams.
- Add trace checks for every externally visible bus cycle.

## Architectural modules

Preserve a modular structure similar to the original conceptual chip organization.

Suggested module layout:

```
rtl/
  z80_core.v
  z80_pins.v
  z80_timing.v
  z80_pla.v
  z80_control.v
  z80_regfile.v
  z80_alu.v
  z80_flags.v
  z80_addr.v
  z80_bus.v
  z80_interrupts.v
  z80_refresh.v
  z80_prefix.v
  z80_debug_trace.v
```

Suggested C layout:

```
cmodel/
  z80.h
  z80.c
  z80_pla.c
  z80_timing.c
  z80_control.c
  z80_regfile.c
  z80_alu.c
  z80_flags.c
  z80_bus.c
  z80_interrupts.c
  z80_trace.c
```

Test layout:

```
tests/
  common/
  zex/
  generated/
  traces/
  verilator/
  iverilog/
  mame_compare/
```

Documentation layout:

```
docs/
  research-notes.md
  architecture.md
  pla.md
  timing.md
  alu.md
  flags.md
  undocumented.md
  verification.md
  known-differences.md
```

## PLA and decode requirements

The instruction decoder should be built around a PLA-like table.

The PLA table should:

- Be compact and readable.
- Be close to a ROM-like structure.
- Emit named control signals.
- Preserve original-style signal groupings where known.
- Distinguish decode for unprefixed, CB, ED, DD, FD, DDCB, and FDCB instruction spaces.
- Make undocumented opcodes explicit.
- Avoid hiding decode behavior inside high-level instruction functions.

Suggested conceptual format:

```c id="wnybpo"
typedef struct {
    uint64_t match_mask;
    uint64_t match_value;
    z80_control_t ctrl;
} z80_pla_row_t;
```

For Verilog, prefer a structurally similar decode table using `localparam` control bit positions and explicit combinational matching logic.

The control word should include signals for:

- Register source/destination selection.
- ALU operation.
- Flag operation.
- Bus source/destination.
- Address source.
- Memory/I/O control.
- Sequencer action.
- Prefix handling.
- Interrupt handling.
- Refresh handling.
- Special-case undocumented behavior.

Document every control signal in `docs/pla.md`.

## ALU requirements

Base the ALU organization on Ken Shirriff’s reverse-engineering analysis and any available die/schematic evidence.

The ALU should preserve recognizable internal behavior where practical:

- 4-bit or nibble-based structure if supported by the reverse engineering.
- Carry chain behavior.
- Decimal adjust behavior.
- Rotate/shift behavior.
- Flag source paths.
- Undocumented X/Y flag derivation from result bits 3 and 5 where applicable.
- Correct behavior for `DAA`, `CPL`, `SCF`, `CCF`, block instructions, `BIT`, indexed `BIT`, and other flag-sensitive cases.

Do not implement flags as an afterthought. Create an explicit flags subsystem whose behavior is testable independently.

## Register and bus behavior

Model all programmer-visible and important internal registers:

- AF, BC, DE, HL.
- Alternate AF', BC', DE', HL'.
- IX, IY.
- SP, PC.
- I, R.
- WZ/MEMPTR or equivalent internal temporary pair.
- Instruction register.
- Prefix state.
- Interrupt flip-flops IFF1/IFF2.
- Interrupt mode.
- HALT state.

Preserve bus-like behavior internally:

- Register output bus.
- ALU input/output paths.
- Address-generation path.
- Increment/decrement path.
- Temporary latches replaced with DFF-based phase registers.
- Explicit tri-state replacement using muxes.

No internal tri-states in RTL. Use explicit muxes.

## C and Verilog co-development loop

Develop in this order:

1. Research notes and source hierarchy.
2. Public interface and pin model.
3. Timing sequencer skeleton.
4. PLA/control signal schema.
5. C model skeleton.
6. Verilog skeleton.
7. ALU and flags.
8. Register file.
9. Basic opcode fetch and simple 8-bit instructions.
10. Memory read/write instructions.
11. 16-bit instructions.
12. Branch/call/return/restart.
13. Prefix handling.
14. CB/ED/DD/FD/DDCB/FDCB instructions.
15. Interrupts, refresh, WAIT, HALT, BUSREQ/BUSACK.
16. Undocumented behavior.
17. Exhaustive verification.
18. Cleanup and documentation.

At every stage:

- Implement first in the C hardware-like model.
- Add tests.
- Compare against trusted references.
- Port the same structure to Verilog.
- Verify with Verilator.
- Verify with iverilog.
- Compare C and Verilog traces.
- Fix the model or RTL before moving on.

Do not allow the C model and RTL to diverge architecturally.

## Test strategy

Create a layered verification system.

### Unit tests

Add unit tests for:

- ALU operations.
- Flag generation.
- DAA.
- Rotates and shifts.
- BIT/SET/RES.
- Indexed addressing.
- WZ/MEMPTR behavior.
- Interrupt flip-flops.
- Refresh register.
- Timing sequencer.
- PLA row matching.

### Instruction tests

Use generated instruction tests that initialize CPU state, memory, pins, and expected outputs.

Test:

- All documented opcodes.
- All undocumented opcodes.
- All prefix combinations.
- All flag-relevant input classes.
- Boundary values: `00`, `01`, `7F`, `80`, `FE`, `FF`.
- Carry/half-carry/overflow-sensitive cases.
- PC/SP wraparound.
- Memory address wraparound.
- IX/IY displacement edge cases: `-128`, `-1`, `0`, `1`, `127`.

### Compatibility tests

Integrate:

- ZEXDOC.
- ZEXALL.
- Other undocumented Z80 test suites where available.
- MAME comparison traces.
- Comparison against at least one mature C Z80 core.
- Gate-level or transistor-level simulation comparison where possible.

### Timing tests

Create bus-cycle trace tests for:

- Opcode fetch.
- Memory read.
- Memory write.
- I/O read.
- I/O write.
- Refresh.
- Interrupt acknowledge.
- WAIT insertion.
- HALT.
- BUSREQ/BUSACK.
- NMI.
- INT modes 0, 1, and 2.

Trace format should include at least:

```
cycle
phase
t_state
m_cycle
pc
ir
prefix_state
addr
data_out
data_in
mreq
iorq
rd
wr
m1
rfsh
halt
busack
wait
int
nmi
internal_control_word
```

C and Verilog should emit comparable traces.

## Reference comparison

Build comparison tools that can run the same program against:

- The C model.
- The Verilog RTL through Verilator.
- The Verilog RTL through iverilog.
- MAME or another trusted emulator.
- Any available gate-level/transistor-level simulator.

When differences occur, classify them:

- Documented behavior mismatch.
- Undocumented behavior mismatch.
- Timing mismatch.
- Flag mismatch.
- MEMPTR/WZ mismatch.
- Interrupt/refresh mismatch.
- Test bug.
- Reference ambiguity.

Record unresolved differences in `docs/known-differences.md`.

## Build system

Create a self-contained build system.

Suggested tools:

- `make`
- `gcc` or `clang`
- `iverilog`
- `verilator`
- Python scripts for test generation and trace comparison

Suggested top-level commands:

```sh
make cmodel
make ctest
make rtl
make verilator
make iverilog
make test
make zexdoc
make zexall
make traces
make compare
make clean
```

The repository should be usable from a fresh checkout with clear instructions.

## Coding standards

Use consistent naming.

Suggested naming:

- External pins: `pin_mreq_n`, `pin_iorq_n`, `pin_rd_n`, etc.
- Internal active-low signals should end in `_n`.
- Control signals should use `ctl_`.
- Timing signals should use `ts_` or `t_`.
- PLA outputs should use `pla_`.
- ALU signals should use `alu_`.
- Register-file signals should use `rf_`.
- Bus signals should use `bus_`.
- Next-state signals should use `_next`.
- Registered state should use `_q` where helpful.

Avoid:

- Behavioral instruction mega-functions.
- Opaque opcode switch statements.
- Unexplained magic constants.
- Simulation-only constructs in synthesizable RTL.
- `initial` blocks in synthesizable RTL.
- Latches.
- Internal tri-states.
- SystemVerilog syntax.
- Asynchronous design assumptions unless explicitly required for reset or external pin synchronization.

## ASIC and FPGA considerations

The core should be ASIC-targetable while remaining FPGA-friendly.

Requirements:

- Explicit reset strategy.
- No reliance on power-up `initial` state.
- No inferred latches.
- No gated clocks unless there is a documented, isolated reason.
- Prefer clock enables.
- Single primary clock.
- Synchronize asynchronous external inputs where appropriate.
- Make reset polarity and behavior clear.
- Separate synthesizable RTL from simulation-only testbench code.
- Avoid vendor-specific primitives in core RTL.

## Deliverables

Produce:

1. `docs/research-notes.md`
2. `docs/architecture.md`
3. `docs/pla.md`
4. `docs/timing.md`
5. `docs/alu.md`
6. `docs/flags.md`
7. `docs/undocumented.md`
8. `docs/verification.md`
9. C99 hardware-like model.
10. Verilog-2001 RTL core.
11. Verilator testbench.
12. iverilog testbench.
13. Shared trace format.
14. Test generator.
15. ZEXDOC/ZEXALL integration.
16. Reference comparison scripts.
17. Known-differences log.
18. Final verification report.

## Success criteria

The project is successful when:

- The C model passes unit tests, generated instruction tests, ZEXDOC, and ZEXALL.
- The Verilog RTL matches the C model cycle-by-cycle or phase-by-phase.
- Verilator and iverilog simulations agree.
- External bus timing matches Z80 reference documentation at sub-cycle level.
- Documented and undocumented instruction behavior matches known test suites.
- Differences from MAME or other references are investigated and documented.
- The implementation remains recognizably organized around PLA decode, control signals, timing sequencer, ALU, buses, and register structures rather than becoming a generic behavioral emulator.
- The RTL is synthesizable Verilog-2001 without latches, `initial` blocks, or SystemVerilog-only features.
- The repository contains enough documentation, tests, scripts, and traces for continued autonomous development.

## Working method for Claude

Work incrementally and keep the repository always buildable.

For each stage:

1. State the local objective.
2. Identify relevant source material.
3. Implement the smallest useful piece.
4. Add or update tests.
5. Run the tests.
6. Inspect failures.
7. Fix issues.
8. Update documentation.
9. Commit or summarize the completed step.
10. Move to the next stage only after the current one is verified.

When uncertain, do not guess silently. Record the uncertainty, create a test or comparison case, and prefer evidence from die-level analysis, official timing documentation, or mature verified references.

Optimize for correctness, traceability, and structural faithfulness first. Optimize compactness and elegance second.
