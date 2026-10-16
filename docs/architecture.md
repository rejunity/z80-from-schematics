# Architecture

This is the **shared design contract**. The C model (`cmodel/`) and the Verilog RTL
(`rtl/`) implement the *same* conceptual machine, in the *same* terms, so they can be
compared phase-by-phase. Naming is kept similar across both: the conceptual signals
described below use the prose-friendly `pin_*` / `ctl_*` style, while the actual
identifiers in code follow each language's idiom (`pins.mreq_n` etc. in C, bare
`mreq_n` ports + `rf_n` style next-state regs in Verilog).

## 1. Clocking & phases (one clock, one edge, DFF-only)

The original Z80 acts on **both** edges of its clock. The C model represents
that as two half-steps per T-state; the RTL runs on the Z80 clock itself, with
every register in the core on `posedge clk` (one tick per T-state), and a thin
top-level wrapper that re-creates the both-edges pin behaviour with a handful
of falling-edge flops. No latches, no 2× clock, one clock domain. The full
story — the retiming argument, the dual-edge output former, which input is
sampled on which edge, GF180 cell mapping — is in `docs/clocking.md`.

```
Z80 T-state:   |   T1        |   T2        | ...
Z80 CLK (clk):  __----____----____----____--
phase (phi):    0    1   0    1   0    1       <- C model half-steps / trace lines
core posedge:   ^        ^        ^            <- RTL: all internal state
wrapper negedge:     ^        ^        ^       <- RTL: pin former b-flops, input samplers
```

- `phi == 0` ("**PHI_P**"): the clock-high half of a T-state. Actions the
  real chip performs on the rising edge (drive address bus, raise/clear M1
  at T1, present the refresh address at T3) take effect here.
- `phi == 1` ("**PHI_N**"): the clock-low half. Actions on the real chip's
  falling edge (assert MREQ/RD, assert WR, deassert MREQ/RD/IORQ/WR at the
  last T-state, **latch MRD/IORD read data** at T3/T4) take effect here.

In the **C model** each half is one call of `z80_phase_step()`. In the **RTL**
one `posedge clk` evaluates both halves of a T-state back to back — the
clock-low-half work first (read-data latch, INT/NMI sample point), reading the
inputs the wrapper captured on the falling edge, then the clock-high-half work
(opcode latch, WAIT, sequencer), reading the live inputs. The core emits two
pin images per T-state (`*_pos` for the coming clock-high half, computed from
next-state; `*_neg` for the current clock-low half) and `rtl/z80_top.v` turns
them into the actual pin waveform with a glitch-free two-flop XOR former
(`a <= pos ^ b` on the rising edge, `b <= neg ^ a` on the falling edge,
`pin = a ^ b`).

Traces are still emitted once per **phase** (two lines per T-state), so the C
model, the RTL and the perfectz80 gate-level reference line up unchanged.

### Timing/sequencer registers
- `phi` (1 bit): current phase (C model only — the RTL evaluates both phases on one edge).
- `t_state` (4 bits in RTL, 8 in C — holds 1..N including WAIT extensions and the
   5-T INTERNAL/M1 cycle, 1-based).
- `m_cycle` (3 bits): machine-cycle index within the current instruction, 1-based.
- `bus_op` (enum): what kind of bus cycle the current M-cycle is
  (`BUSOP_M1`, `BUSOP_MRD`, `BUSOP_MWR`, `BUSOP_IORD`, `BUSOP_IOWR`, `BUSOP_INTA`,
  `BUSOP_INTERNAL`, `BUSOP_NONE`).
- WAIT extends an M-cycle by holding `t_state` (inserting `Tw`) — see `docs/timing.md`.

## 2. External pins (`pin_*`)

Active-low signals carry the `_n` suffix (asserted = 0).

| Pin                  | Dir | Meaning                                                         |
|----------------------|-----|-----------------------------------------------------------------|
| `pin_clk`            | in  | the Z80 clock (one period = one T-state)                        |
| `pin_reset_n`        | in  | async-assert / sync-deassert reset (active low)                 |
| `pin_addr[15:0]`     | out | address bus                                                     |
| `pin_data_in[7:0]`   | in  | data bus, sampled side                                          |
| `pin_data_out[7:0]`  | out | data bus, driven side                                           |
| `pin_data_drive`     | out | 1 ⇒ core is driving the data bus (mux, not tri-state in RTL)    |
| `pin_m1_n`           | out | opcode fetch / interrupt ack                                    |
| `pin_mreq_n`         | out | memory request                                                  |
| `pin_iorq_n`         | out | I/O request (and, with m1, interrupt ack)                       |
| `pin_rd_n`           | out | read strobe                                                     |
| `pin_wr_n`           | out | write strobe                                                    |
| `pin_rfsh_n`         | out | refresh address valid                                           |
| `pin_halt_n`         | out | CPU halted                                                      |
| `pin_wait_n`         | in  | insert wait states (sampled at the end of T2.N / Tw.N)          |
| `pin_int_n`          | in  | maskable interrupt request                                      |
| `pin_nmi_n`          | in  | non-maskable interrupt (edge)                                   |
| `pin_busreq_n`       | in  | bus request                                                     |
| `pin_busack_n`       | out | bus acknowledge                                                 |

The data bus is modeled as separate `pin_data_in`/`pin_data_out` + `pin_data_drive`
instead of a tri-state, matching "no internal tri-states; use explicit muxes."

In RTL, `z80_core` registers M1, MREQ, IORQ, RD, WR, RFSH, HALT and data-output
enable from the **next** timing state, on the same enabled edge as the sequencer.
This preserves phase timing without exposing intermediate `t_state`/`phi`/`bus_op`
decodes to external hardware. For example, T4.N → T1.P must not briefly decode
T1.N and assert RD/MREQ as M1 falls. M1 retains a reset-only output mask to
preserve its initial T1.P assertion on reset release. Address and data still use
the current-state decode; BUSACK comes from the registered bus-grant flag.
`make bus_controls` checks phase parity, injects decoder skew between edges,
and verifies falling-edge WAIT capture and continuous write strobes through Tw.
The stateless timing decoder describes initial T2.P; the core's WR register
retains the asserted strobe when a stalled memory write revisits that phase.

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
Computes SF ZF YF HF XF PF NF CF including undocumented X(bit5)/Y(bit3)
behavior, DAA, SCF/CCF, BIT, and block-instruction flags. Lives inside the ALU
module on both sides (`cmodel/z80_alu.c` and `rtl/z80_alu.v`) — the per-class
flag computers are exposed as helpers so they remain independently unit-testable.
See `docs/flags.md`.

## 4. Control word & decode

The PLA maps `(prefix_state, opcode)` → a **control word** of *named lines*
(`docs/pla.md`). It is compact and ROM-like: opcodes are decomposed into the standard
`x[7:6] y[5:3] z[2:0]` fields (with `p=y>>1`, `q=y&1`) and matched in a table, rather
than dispatched through an opaque per-opcode `switch`. The control word selects
register source/dest, ALU op, flag mode, bus source/dest, address source, memory/IO
control, the **M-cycle sequence template** (`seq_*`), prefix handling, interrupt and
refresh handling, and undocumented special-cases.

The **micro-sequencer** lives inside `cmodel/z80_core.c` (mirroring the sequencer
block of `rtl/z80_core.v`). It consumes the control word + `m_cycle`/`t_state`/`phi`
and drives the datapath and pins per phase — a structured state machine keyed off the
control word's `exec` line + step index, *not* a giant behavioral function per opcode.

## 5. Module mapping (C ↔ Verilog, one C file per Verilog module)

| Concept                                                                                                                       | C file                  | Verilog file        |
|-------------------------------------------------------------------------------------------------------------------------------|-------------------------|---------------------|
| Public types, pins, constants, module entry-point decls                                                                       | `cmodel/z80.h`          | `rtl/z80_defs.vh`   |
| Chip top: falling-edge input samplers + dual-edge pin former (the only negedge logic)                                        | (folded into the per-phase step) | `rtl/z80_top.v`     |
| Core: phase engine, micro-sequencer, bus-cycle setup, register file, interrupts / refresh / HALT / WAIT / BUSREQ              | `cmodel/z80_core.c`     | `rtl/z80_core.v`    |
| PLA decode (pure combinational)                                                                                               | `cmodel/z80_pla.c`      | `rtl/z80_pla.v`     |
| ALU + flag assembly (pure combinational)                                                                                      | `cmodel/z80_alu.c`      | `rtl/z80_alu.v`     |
| External pin timing (pure combinational)                                                                                      | `cmodel/z80_timing.c`   | `rtl/z80_timing.v`  |
| 8-bit register accessors                                                                                                      | `cmodel/z80_regfile.c`  | inlined in core     |
| Trace capture / emit                                                                                                          | `cmodel/z80_trace.c`    | core `$display` hooks |
| System wrapper (CPU + 64 K RAM/IO for runners)                                                                                | `cmodel/z80_sim.c/h`    | (test-bench only)   |

Both sides have **one .c per .v module** (plus `rtl/z80_top.v`, which has no
C counterpart: it only re-times pins to the two clock edges). The three combinational submodules
(`z80_pla`, `z80_alu`, `z80_timing`) are pure functions in C with one parameter per
Verilog port — by-value inputs first, by-pointer outputs after — so a C call site
reads like a Verilog instantiation. The stateful `z80_core` holds the registered
state (the `z80_t` struct = the Verilog module's internal regs) and is the only
file that mutates it; its public functions (`z80_init`, `z80_reset`,
`z80_phase_step`, `z80_set_pc`) are the only external entry points.

The C model is the fast reference; the RTL implements the identical machine and is
verified against it phase-by-phase (`make compare`; see `docs/verification.md`).

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
