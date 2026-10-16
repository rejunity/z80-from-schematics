# Clocking: one clock, one edge, dual-edge pins

This note describes the `cmos-clock` restructuring: how a design that
originally needed a 2× clock (one tick per Z80 half-cycle) was collapsed onto
the Z80 clock itself, with every internal register on the rising edge, and
how the chip still produces the original Z80's both-edges pin behaviour.

## 1. Where we started

The original NMOS Z80 runs two non-overlapping internal clock phases from one
external clock and does real work on both edges. The first RTL modeled that
literally: `clk` ran at twice the Z80 rate, a `phi` register toggled every
tick, and a Z80 T-state was two ticks (`phi=0` = "P", clock-high half;
`phi=1` = "N", clock-low half). Rising-edge events (drive address, raise M1,
latch opcode …) happened on the tick entering `phi=0`; falling-edge events
(assert MREQ/RD, assert WR, latch read data, sample WAIT …) on the tick
entering `phi=1`. Pins were a combinational function of
`(bus_op, t_state, phi, …)` (`rtl/z80_timing.v`).

That is easy to simulate against the half-cycle C model, but as silicon it
means a clock pin at 2× the bus rate, i.e. a Z80 that is not a Z80.

## 2. The retiming

Look at what each of the two ticks actually did:

| tick | formerly | state it changes | inputs it reads |
|------|----------|------------------|-----------------|
| entering `phi=1` (the falling edge) | "B half" | `tmp8` (MRD data at T3, IORD data at T4), `nmi_sampled`/`int_sampled` (T_last.P sample), NMI edge detect, `phi` | `data_in`, `int_n`, `nmi_n` **as present during the clock-high half** |
| entering `phi=0` (the rising edge) | "A half" | everything else: T-state advance, micro-sequencer, register file, M1 opcode latch, WAIT decision, BUSREQ decision | `data_in`, `wait_n`, `busreq_n`, `nmi_n` **as present during the clock-low half** |

The B half is tiny and feeds the A half. So one rising-edge evaluation can
do `A(B(state, inputs_high_half), inputs_low_half)` — provided the
*high-half inputs are still available at the rising edge*. They are, if a
falling-edge flop holds them. That flop is the only thing in the design that
needs the falling edge, and it lives in the top-level wrapper.

Concretely, in `rtl/z80_core.v` the combinational next-state block now runs
once per T-state and reads, in order:

1. **clock-low-half work** (what the chip did on the falling edge of this
   T-state): NMI edge detect against `nmi_n_neg`; the T_last.P INT/NMI
   sample from `int_n_neg` / the NMI edge state; the MRD/IORD read-data
   latch from `data_in_neg`;
2. **clock-high-half work** (this rising edge): NMI edge detect against the
   live `nmi_n`; the M1/INTA latch from the live `data_in`; the WAIT sample
   from the live `wait_n`; BUSREQ; the T-state advance and the whole
   micro-sequencer.

`phi` is gone. `t_state`, `m_cycle`, `bus_op` and friends advance once per
rising edge. The register file, ALU, PLA, flags — untouched.

## 3. Pins: two images per T-state, formed in the wrapper

Each pin takes one value during the clock-high half of a T-state and one
during the clock-low half (MREQ in an M1: high in T1.P, low from T1.N, high
again at T3.P, low at T3.N, high at T4.N). The core therefore exports two
images of every pin, both computed by the same `z80_timing` function the C
model calls:

- `*_pos` — the clock-high half of the T-state being **entered** at this
  rising edge. It is computed from the *next-state* signals (`bus_op_n`,
  `t_n`, `m_addr_n`, …) so the wrapper can register it on the very edge the
  state commits.
- `*_neg` — the clock-low half of the **current** T-state, from registered
  state (a half-cycle path into the wrapper's falling-edge flops).

`rtl/z80_top.v` turns the pair into a real dual-edge waveform with the
two-flop XOR former:

```
always @(posedge clk) a <= pins_pos ^ b;     // a: rising-edge half
always @(negedge clk) b <= pins_neg ^ a;     // b: falling-edge half
assign pins = a ^ b;
```

After a rising edge `pins = (pos ^ b) ^ b = pos`; after a falling edge
`pins = a ^ (neg ^ a) = neg`. Exactly one XOR input changes per edge, so
every pin makes exactly one transition per edge — there is no clock-as-data
mux (which would glitch WR# at the rising edge of T3 when the high-half
value of T2 differs from the low-half value), no inverted clock net, and the
outputs are flop-driven rather than decoded from state. Reset forces
`a = idle pattern, b = 0`, so pins sit idle while `reset_n` is low.

Why can't the P half come straight from a core-registered copy? Because at
the rising edge the output must flip from `neg(k-1)` to `pos(k)` in a single
transition driven by rising-edge flops, while the falling-edge flop still
holds whatever it held: any such former needs `pos(k)` at the D input *before*
the edge. That is why `*_pos` is a next-state function.

## 4. Inputs: which edge samples what

| input | sample point (unchanged from the half-cycle model) | consumed as |
|-------|-----------------------------------------------------|-------------|
| `data_in` | M1 opcode / INTA vector at the rising edge ending T2 | live, in the core |
| `data_in` | MRD data at the falling edge of T3, IORD data at the falling edge of T4 | `data_in_neg` (wrapper negedge flop) |
| `wait_n` | rising edge ending T2.N (memory) / Tw.N (I/O) | live |
| `int_n` | level during T_last.P (UM0080: rising edge of the last T-state) | `int_n_neg` |
| `nmi_n` | falling-edge detect, both halves | live **and** `nmi_n_neg` |
| `busreq_n` | instruction boundary; every rising edge while granted | live |
| `reset_n` | async assert, sync release | — |

Ten falling-edge flops in total (8 data + INT + NMI). Active-low control
inputs reset to their *inactive* (high) level, so they want a negedge DFF
with async **set**, not reset.

## 5. What synthesis should produce (GF180 target, sky130 today)

- Both flop banks are sinks of the **same clock tree**. Do not describe
  `clk_n = ~clk` and clock from that; `always @(negedge clk …)` is enough.
- Native negative-edge cells exist in both GF180 5 V libraries:

  | function | 7-track | 9-track |
  |----------|---------|---------|
  | negedge DFF | `gf180mcu_fd_sc_mcu7t5v0__dffnq_{1,2,4}` | `…mcu9t5v0__dffnq_*` |
  | negedge DFF, async reset → 0 | `…__dffnrnq_*` | `…__dffnrnq_*` |
  | negedge DFF, async set → 1 | `…__dffnsnq_*` | `…__dffnsnq_*` |
  | negedge DFF, set and reset | `…__dffnrsnq_*` | `…__dffnrsnq_*` |

  sky130_fd_sc_hd has the equivalents (`dfrtn`, `dfbbn`). Yosys infers
  `$_DFF_N*` / `$_DFFSR_N*` cells for `negedge` processes and `dfflibmap`
  picks the native cell when the liberty offers one; if a flow has put the
  negedge cells on a don't-use list it will instead insert an inverter on
  the clock pin — **check the mapped netlist**. Expected count here:
  `PW + 10 = 43` negedge flops (33 former `b` bits + 10 input samplers).
- The negedge→posedge paths (`*_neg` sampler → core, former `a`↔`b`) are
  ordinary half-cycle paths in one clock domain. Time them; do not mark
  them false or asynchronous.
- SDC should describe the real duty cycle and constrain inputs that are
  sampled on the falling edge against it:

  ```
  create_clock -name clk -period 100.0 -waveform {0.0 50.0} [get_ports clk]
  set_input_delay -clock clk -clock_fall -max <d> [get_ports {data_in[*] int_n nmi_n}]
  set_input_delay -clock clk -clock_fall -min <d> [get_ports {data_in[*] int_n nmi_n}]
  ```

- Scan: GF180's documented scan flops are rising-edge only. The 43 negedge
  flops can be left out of automatic scan (inputs are controllable from
  pins, outputs observable on pins) or given an explicit test mux into a
  small falling-edge chain.

## 6. What changed observably

The retiming is behaviour-preserving against the half-cycle C model on every
trace program (`make compare`), FUSE through RTL, Rak z80test and BASIC
through Verilator. Three places where the old 2× RTL was *wrong* or
ill-defined were cleaned up along the way:

- **M1 opcode latch under WAIT.** The 2× RTL latched the opcode and bumped
  PC/R on the same tick it sampled WAIT low, then latched again after the
  Tw — the C model never did. The core now suppresses the latch while a
  WAIT sample is holding T2 (`latch_pos && !stall`).
- **BUSREQ release.** The 2× RTL re-sampled `busreq_n` on both ticks and
  could restart the M1 on a falling edge, leaving P/N swapped relative to
  the external clock for the rest of the run. Release is now sampled on the
  rising edge only, like silicon.
- **Pins during a bus grant.** The 2× RTL kept decoding strobes from the
  frozen `(bus_op, t_state)` while BUSACK was low (an M1 frozen at T4 drove
  refresh MREQ throughout the DMA). Strobes now idle while granted, as in
  the C model.

Still informational-only differences versus the C model (as before, see
`docs/known-differences.md`): the reset-assert filter / release settle and
the 2-phase BUSREQ release settle the C model carries from the perfectz80
analysis are not in the RTL.

## 7. Simulating it

Trace "phases" survive: even phase = after a rising edge, odd = after a
falling edge. The testbenches (`tests/iverilog/tb_z80*.v`,
`tests/verilator/sim_main.cpp`) toggle `clk` once per phase, apply the pin
events just after the edge that starts the phase, capture memory writes
from the settled pins, and print the phase bit themselves (the core no
longer has `dbg_phi`). Reset is released in the clock-low half; the first
rising edge after release enters T1.P of the M1 at PC=0 and is trace line 0,
which keeps the existing C↔RTL and perfectz80 alignment (`PZ_OFFSET`)
intact.
