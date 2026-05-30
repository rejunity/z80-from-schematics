# Architecture

This is the **shared design contract**. The C model (`cmodel/`) and the Verilog RTL
(`rtl/`) implement the *same* conceptual machine, in the *same* terms, so they can be
compared phase-by-phase. Naming is kept identical across both (see brief's coding
standards: `pin_*`, `ctl_*`, `pla_*`, `alu_*`, `rf_*`, `bus_*`, `*_n`, `*_next`, `*_q`).

## 1. Clocking & phases (single clock, DFF-only)

The original Z80 acts on **both** edges of its clock. We model that with a single
modern clock running at **2× the Z80 clock**, so each Z80 T-state is exactly two
modern-clock ticks. All state updates on `posedge clk`. No latches, no dual-edge.

```
Z80 T-state:   |   T1        |   T2        | ...
Z80 CLK:        __----____----____----____--
phase (phi):    0    1   0    1   0    1
modern clk tick:^    ^   ^    ^   ^    ^
```

- `phi == 0` ("**PHI_P**"): the first half of a T-state — corresponds to the Z80
  clock *rising-edge* region. Actions the real chip performs on the rising edge
  (drive address bus, raise/clear M1 at T1, latch refresh address at T3) take effect
  on the `posedge clk` that *enters* phi=0.
- `phi == 1` ("**PHI_N**"): the second half — corresponds to the Z80 clock
  *falling-edge* region. Actions on the real chip's falling edge (assert MREQ/RD,
  assert WR, sample WAIT and latch read data) take effect on the `posedge clk` that
  *enters* phi=1.

The canonical simulation step is **one phase** (`z80_phase_step()` in C; one `clk`
edge in RTL). Two phase-steps advance one T-state. Traces are emitted once per phase.

### Timing/sequencer registers
- `phi` (1 bit): current phase.
- `t_state` (3 bits): T-state index within the current M-cycle, 1-based (1..6).
- `m_cycle` (3 bits): machine-cycle index within the current instruction, 1-based.
- `bus_op` (enum): what kind of bus cycle the current M-cycle is
  (`M1`, `MRD`, `MWR`, `IORD`, `IOWR`, `INTA`, `INTERNAL`).
- WAIT extends an M-cycle by holding `t_state` (inserting `Tw`) — see `docs/timing.md`.

## 2. External pins (`pin_*`)

Active-low signals carry the `_n` suffix (asserted = 0).

| Pin | Dir | Meaning |
|-----|-----|---------|
| `pin_clk` | in | 2× modeled clock |
| `pin_reset_n` | in | async-assert/sync-deassert reset (active low) |
| `pin_addr[15:0]` | out | address bus |
| `pin_data_in[7:0]` | in | data bus, sampled side |
| `pin_data_out[7:0]` | out | data bus, driven side |
| `pin_data_drive` | out | 1 ⇒ core is driving the data bus (mux, not tri-state in RTL) |
| `pin_m1_n` | out | opcode fetch / interrupt ack |
| `pin_mreq_n` | out | memory request |
| `pin_iorq_n` | out | I/O request (and, with m1, interrupt ack) |
| `pin_rd_n` | out | read strobe |
| `pin_wr_n` | out | write strobe |
| `pin_rfsh_n` | out | refresh address valid |
| `pin_halt_n` | out | CPU halted |
| `pin_wait_n` | in | insert wait states (sampled on falling edge) |
| `pin_int_n` | in | maskable interrupt request |
| `pin_nmi_n` | in | non-maskable interrupt (edge) |
| `pin_busreq_n` | in | bus request |
| `pin_busack_n` | out | bus acknowledge |

The data bus is modeled as separate `pin_data_in`/`pin_data_out` + `pin_data_drive`
instead of a tri-state, matching "no internal tri-states; use explicit muxes."

## 3. Internal datapath

A single 8-bit internal data bus (`bus_db`) plus a 16-bit address path. No internal
tri-states; every bus is an explicit mux selected by control lines.

```
          +----------------------------------------------+
   ctl -->| PLA decode  ->  control word (ctl_*)         |
          +----------------------------------------------+
                 |                  |             |
                 v                  v             v
        +----------------+   +-----------+  +--------------+
        |  register file |<->|  ALU + F  |  | addr gen +/- |
        |  BC DE HL AF   |   | (nibble)  |  |  inc/dec     |
        |  BC'DE'HL'AF'  |   +-----------+  +--------------+
        |  IX IY SP PC   |        ^  |            |
        |  I R  WZ  IR   |        |  v            v
        +----------------+      bus_db        pin_addr
                 ^___________________|____________
                          internal 8-bit data bus
```

### Register file (`rf_*`)
16-bit-oriented file (mirrors the die): pairs `BC DE HL AF`, alternates
`BC' DE' HL' AF'`, plus `IX IY SP PC I R WZ` and the instruction register `IR`.
8-bit access selects the high/low half of a pair. The WZ ("MEMPTR") pair is an
internal temporary, not programmer-visible, but observable via `BIT n,(HL)` flags.
A 16-bit incrementer/decrementer sits on the address-read path (used for PC, SP, the
refresh `R` increment, and `(rp)±` block moves).

### ALU (`alu_*`)
Organized as a **4-bit nibble unit applied twice** (low nibble then high nibble) with
an explicit carry chain between passes — per Shirriff's die analysis (see
`docs/alu.md`). Operand latches `alu_op_a`, `alu_op_b`, op select `alu_op`, carry-in,
result `alu_res`, and the flag-source outputs. Logic ops, add/sub, DAA, and the
rotate/shift helpers share the unit.

### Flags (`alu_*`/`ctl_flag_*`)
Explicit, independently testable flags subsystem (`cmodel/z80_flags.c`,
`rtl/z80_flags.v`). Computes SF ZF YF HF XF PF NF CF including undocumented
X(bit5)/Y(bit3) behavior, DAA, SCF/CCF, BIT, and block-instruction flags. See
`docs/flags.md`.

## 4. Control word & decode

The PLA maps `(prefix_state, opcode)` → a **control word** of *named lines*
(`docs/pla.md`). It is compact and ROM-like: opcodes are decomposed into the standard
`x[7:6] y[5:3] z[2:0]` fields (with `p=y>>1`, `q=y&1`) and matched in a table, rather
than dispatched through an opaque per-opcode `switch`. The control word selects
register source/dest, ALU op, flag mode, bus source/dest, address source, memory/IO
control, the **M-cycle sequence template** (`seq_*`), prefix handling, interrupt and
refresh handling, and undocumented special-cases.

The **micro-sequencer** (`cmodel/z80_control.c`, `rtl/z80_control.v`) consumes the
control word + `m_cycle`/`t_state`/`phi` and drives the datapath and pins per phase. It
is a structured state machine keyed off the control word's sequence template — *not* a
giant behavioral function per opcode.

## 5. Module mapping (C ↔ Verilog, identical concepts)

| Concept | C file | Verilog file |
|---|---|---|
| Public types, pins, top step | `cmodel/z80.h`, `z80.c` | `rtl/z80_core.v` |
| Timing sequencer | `z80_timing.c` | `z80_timing.v` |
| PLA decode table | `z80_pla.c` | `z80_pla.v` |
| Control / micro-sequencer + prefix state | `z80_control.c`, `z80_internal.h` | `rtl/z80_core.v` (inlined) |
| Register file + IDU / WZ | `z80_regfile.c` | `rtl/z80_core.v` (inlined) |
| ALU | `z80_alu.c` | `z80_alu.v` |
| Flags | `z80_flags.c` | `rtl/z80_core.v` (inlined) |
| Bus muxing | `z80_bus.c` | `rtl/z80_core.v` (inlined) |
| Interrupts / refresh / HALT / WAIT / BUSREQ | inlined in `z80.c` top step | `rtl/z80_core.v` (inlined) |
| Trace | `z80_trace.c` | `rtl/z80_core.v` `$display` hooks |

The RTL is intentionally consolidated into `z80_core.v` plus four leaf files
(`z80_alu.v`, `z80_pla.v`, `z80_timing.v`, `z80_defs.vh`); concepts that have separate
files in the C model (regfile, bus, prefix, interrupts, refresh, flags) are inlined into
`z80_core.v` to keep cross-module wiring tractable in Verilog-2001. The C model is the
fast reference; the RTL implements the identical machine and is verified against it
phase-by-phase (`make compare`; see `docs/verification.md`).

### Building
`make cmodel` builds the static library and relinks the dependent CLI binaries
(`build/bin/zexrunner`, `build/bin/tracegen`); editing C sources requires only that one
target. `make ctest` rebuilds and runs unit tests; `make compare` rebuilds the iverilog
sim and diffs C↔RTL traces; `make zexdoc` / `make zexall` run the full exercisers.

## 6. Reset strategy

Reset is active-low (`pin_reset_n`). Asynchronous assertion is allowed (for ASIC/FPGA
reset trees) but **deassertion is synchronized**. On reset: `PC=0000`, `IR=0000`,
`I=00`, `R=00`, `IFF1=IFF2=0`, `IM=0`, prefix cleared, sequencer to M1/T1/phi0,
`SP` and the main registers are left undefined on real silicon but we force a known
state (`SP=FFFF`, `AF=FFFF`) for deterministic C↔RTL comparison and document it in
`docs/known-differences.md`.
