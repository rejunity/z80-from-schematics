# RZ80 on GlobalFoundries gf180mcuD, 9-track cells — RTL-to-GDS of the bare core

`z80_top` hardened as a stand-alone macro (no pad ring) with the LibreLane
3.0.14 Classic flow -- synthesis, floorplan, placement, clock tree, routing,
multi-corner STA, DRC, LVS, antenna, GDS -- then a gate-level "signs of
life" run of the routed netlist; and the same core in the Z80 pad ring
(`chip/`). Everything is as on `asic-gf180mcu` -- RTL, PDK version, flow
settings, pads, chip -- except the standard cells: the 9-track 5 V library
`gf180mcu_fd_sc_mcu9t5v0`, taller (5.04 µm rows against 3.92 µm) and
stronger than the 7-track one, to see what it buys at the slow corner.

| | |
|---|---|
| PDK | `gf180mcuD`, `54435919` (the pin of LibreLane 3.0.14), fetched by the Makefile with LibreLane's own ciel and passed with `--manual-pdk`: this version's LibreLane settings for the 9-track library name a `drc_exclude.cells` (`PNR_EXCLUDED_CELL_FILE`) that only the 7-track library ships, and LibreLane stops on the missing file. The Makefile puts `pnr_exclude.cells` there: the 7-track list's two cells (`mux2_1`, `oai33_2`) under their 9-track names. |
| Cells | `gf180mcu_fd_sc_mcu9t5v0` |
| Clock target | 50 ns (20 MHz), met at the typical corners LibreLane signs off (+7.1 ns); at the slow corner (ss, 125 °C, 4.5 V) the core reaches ~15.6 MHz, about as with the 7-track cells (below) |
| Config | `config.yaml` (Classic flow, relative die sizing, wider than tall to fit the chip's pad ring) |

## Run it

LibreLane via Nix (see `docs/librelane-flow.md` for installing Nix with the
fossi-foundation binary cache), then from the repository root:

    nix profile install github:librelane/librelane/3.0.14
    make -C asic/gf180mcu-9t                  # RTL -> GDS (~10 min); the Makefile fetches the PDK
    make -C asic/gf180mcu-9t summary          # area, per-corner timing, sign-off; non-zero exit on a failure (*)
    make -C asic/gf180mcu-9t sign_of_life     # the routed netlist runs asic/tb_sign_of_life.v
    make -C asic/gf180mcu-9t sign_of_life_rtl # same program on rtl/
    make -C asic/gf180mcu-9t synth            # synthesis and its checks only (minutes)

(*) Sign-off as LibreLane counts it: DRC / LVS / antenna / XOR clean,
no setup violations at the corners in TIMING_VIOLATION_CORNERS (typical),
no hold violations at any corner; every corner is printed.

Results land in `asic/gf180mcu-9t/runs/<tag>/` (`final/` holds GDS,
netlists, metrics). CI: `.github/workflows/asic-gf180mcu-9t.yml` runs all
of the above on every push to this branch (and on demand), and keeps the
GDS, netlist and metrics as artifacts. The job itself is `asic-pdk.yml`,
which main's `targets.yml` also runs nightly with this branch merged into
main, so a core change on main that breaks the flow shows up within a day.

## 7-track against 9-track at the slow corner

The same RTL (main with the NMOS pin fixes), PDK version and flow settings
-- no delay cells, post-route repair on (see Notes) -- both dies square,
20 MHz target; the 7-track run is `asic-gf180mcu`'s, slack at the slow
corner with worst-case wires:

| | 7-track | 9-track |
|---|---|---|
| Worst setup slack, ss 125 °C 4.5 V: full-cycle paths (rising to rising edge) | -14.7 ns | -5.2 ns |
| ... half-cycle paths (falling to rising edge) | -6.2 ns | -6.9 ns |
| Fastest clock at ss | ~15.5 MHz | ~15.7 MHz |
| Setup TNS, ss | -1,617 ns | -472 ns |
| Setup slack, typical (signed off) / fast | +7.5 / +10.0 ns | +7.2 / +11.5 ns |
| Hold slack, fast | +0.26 ns | +0.25 ns |
| Cell area | 155,903 µm² | 189,310 µm² (+21 %) |
| Die | 544 × 562 µm = 0.31 mm² | 610 × 637 µm = 0.39 mm² (+27 %) |
| Max-slew violations (all corners) | 2,379 | 1,232 |
| Power, typical, 20 MHz | 0.145 W | 0.187 W (+29 %) |

The 9-track cells speed up the full-cycle paths -- the worst of them by
10 ns, the slow corner's total negative slack to under a third -- but not the
half-cycle ones, and those set the clock: read data sampled on the
falling edge (`data_in_neg`) going through the ALU and the register file
into the address bus's pin former on the next rising edge, half a clock
later (an operand just read becoming the next address). They are also
the critical paths at the typical corner, which LibreLane signs off: the
resizer sizes them until the typical corner is met, by about the same
margin with either library, and both libraries slow down alike from there
to the slow corner. So the slow corner's clock comes out about the same,
~15.5 MHz, for 27 % more die and 29 % more power -- and the chip's pins gain
nothing either (below). The NMOS Z80 is not at stake: at 4 MHz half a
clock is over 100 ns, against the ~35 ns these paths take at the slow
corner.

## Signs of life

`asic/tb_sign_of_life.v` is deliberately tiny: a 37-byte program writes
01 02 04 .. 80 to 8000h, reads it back and adds it up, OUTs the sum (FFh)
to port 42h, CALLs a subroutine that OUTs 5Ah to port 43h and RETurns,
and HALTs -- opcode fetch, immediate and memory reads, memory writes, the
ALU, a loop, I/O, the stack and HALT in ~570 T-states, a few seconds at
gate level. It checks the memory pattern, both I/O writes, the return
address on the stack, /HALT, and that /RD+/WR and /MREQ+/IORQ never assert
together. The cell models come from the run's `resolved.json`
(`CELL_VERILOG_MODELS`), so the same script serves every PDK.

## Results

The same core as `asic-gf180mcu` (`optimise-luts` restructure, synthesized
per RTL block with `SYNTH_HIERARCHY_MODE: deferred_flatten`), laid out as
the chip's macro: pins on the sides of their pads (`pin_order.cfg`), power
straps on Metal4 only, routing up to Metal4, and a die 0.75 as tall as it
is wide -- the shape of the room inside the chip's pad ring. Measured with
a 20 MHz target:

| | |
|---|---|
| Std cells | 8,570 |
| Cell area | 189,307 µm² |
| Die | 702 × 557 µm = 0.39 mm² (54 % utilised) |
| Setup slack @ 20 MHz | +7.1 ns typical (signed off; ~29 MHz); -10.7 ns at ss 125 °C 4.5 V (~15.6 MHz) |
| Hold slack | +0.25 ns |
| Route / Magic DRC, LVS, antenna, GDS XOR | all 0 (KLayout DRC is not part of LibreLane's GF180 Classic flow) |
| Signs of life, routed netlist | PASS (HALT after 554 clocks) |
| Runtime | 10 min (8 cores, shared) |

## The chip: z80_top in a pad ring

`chip/` builds `z80_chip` as on `asic-gf180mcu`: the hardened `z80_top` as
a hard macro in a ring of 40 `gf180mcu_fd_io` pads, one per pin of the
40-pin Z80, each on the side of the die where the original's bond pad is
(Visual Z80 die scan) and in the same order:

    south  A5 A6 A7 A8 A9 A10 A11 A12 A13 A14 A15 CLK   (left to right)
    east   D4 D3 D5 D6 VCC D2 D7 D0 D1                  (bottom to top)
    north  INT NMI HALT MREQ IORQ RD WR BUSAK WAIT       (right to left)
    west   BUSRQ RESET M1 RFSH GND A0 A1 A2 A3 A4       (top to bottom)

The spacing is the pads' own, not the Z80's: the die is as small as the
pads allow, the 12 pads along the bottom and the 10 up the left side abut
at their 75 µm pitch, and LibreLane's pad ring spreads the other two sides'
pads evenly. The die is the 7-track chip's: the 9-track core, square,
would be taller than the room inside the pad ring (778 × 628 µm), so it is
laid out wider than tall and centred in it. `z80_top`'s own pins sit on
the same sides in the same order (`pin_order.cfg`), so the pad wires are
short. The foundry pad ring shorts the core supply to the 5 V I/O supply,
so VCC and GND are the only supply pads -- exactly the Z80's two. As on
the Z80, the address bus floats during RESET and BUSAK, MREQ IORQ RD WR
during BUSAK, the data bus whenever the CPU does not drive it
(`chip/z80_chip.v`; `z80_top` itself is unchanged).

    make -C asic/gf180mcu-9t                     # z80_top first: the chip uses its latest run as its macro
    make -C asic/gf180mcu-9t chip                # the chip (LibreLane Chip flow, into chip/runs/)
    make -C asic/gf180mcu-9t chip_summary        # sign-off of the chip, as `summary`
    make -C asic/gf180mcu-9t chip_sign_of_life   # the signs-of-life program through the pads
    make -C asic/gf180mcu-9t pin_timing          # the pins against the NMOS Z80's datasheet (GRADE=4/6/8)

| | |
|---|---|
| Die | 1662 × 1512 µm = 2.5 mm², pad-limited (the bottom and left pad rows abut) |
| Pads | 40: `bi_t` at its weakest drive, 4 mA, with slow slew (address and control outputs with their enables, the data bus), `in_c` / `in_s` (inputs; Schmitt for CLK and RESET), `dvdd` / `dvss` (VCC, GND) |
| Sign-off | DRC, LVS, antenna, XOR all 0; the NMOS Z80's 4 MHz pin timing met at every corner (below); 7 unconnected outputs (`z80_top`'s debug pins, no pads) |
| Signs of life through the pads | PASS (HALT after 554 clocks) |
| Runtime | 7 min after `make` (the core is not re-synthesized or re-routed) |

The outputs drive like the NMOS Z80's as far as `gf180mcu_fd_io` allows:
the NMOS part sinks 2.0 mA at 0.4 V and sources 250 µA at 2.4 V (PS0178
DC characteristics); `bi_t`'s weakest setting is 4 mA, here with slow
slew (`Z80_PAD_DRIVE` in `chip/z80_chip.v`). Like the Z80 the chip runs
from 5 V. The data pads' input buffers are on only while the CPU does not
drive the bus, which is also the only case the pad's Liberty characterises
the output for.

The pins are constrained to, and checked against, the NMOS Z80's AC
characteristics at 4 MHz (Zilog PS0178, Z8400; `../timing/`): the chip
flow runs on `z80_chip_nmos4.sdc`, generated from the datasheet table, and
`make -C asic/gf180mcu-9t pin_timing` measures the routed chip with
OpenSTA at 100 pF and checks every parameter the replica has to meet --
PASS at the tt, ss and ff corners. Output delays take 12-43 ns against
65-150 ns limits; the tightest margins are the opcode's setup TsD(Cr) at
the slow corner (+7.1 ns of 35 ns) and the input holds at the fast corner
(+0.6 to +2.1 ns, after the flow's hold repair). The faster NMOS grades,
6 and 8 MHz, pass as well at every corner, the tightest by +0.5 ns (an
input hold at the fast corner; the opcode's setup keeps +2.0 and +3.2 ns
at the slow one). For the pins the 9-track cells buy nothing: the output
delays are the pads', and the pads are the same; the opcode's setup is
what the core's input paths and clock tree leave, and those come out
~2 ns worse here than on the 7-track chip.

The pad ring settings follow open-pdks' LibreLane config for
`gf180mcu_fd_io` (the PDK version LibreLane 3.0.14 pins predates it) and
the wafer.space gf180mcu project template; `chip/pads_bb.v` are blackbox
views of the pad cells for synthesis, whose own models do not lint.

## Notes

- Post-route repair (`RUN_POST_GRT_RESIZER_TIMING`, `RUN_POST_GRT_DESIGN_REPAIR`; LibreLane leaves both off), as on `asic-gf180mcu`: the resizer works on estimated wiring, and one more pass on the routed wires' parasitics took this core's slow-corner slack from -11.9 to -10.7 ns (full cycle) and from -11.8 to -7.1 ns (half cycle), ~13.6 -> ~15.6 MHz, the typical corner from +4.4 to +7.1 ns, and the chip's opcode setup at the slow corner from +3.1 to +7.1 ns -- the 6 and 8 MHz grades went from failing it (-2.1 / -0.8 ns) to passing.
- Delay cells: left to itself, LibreLane buffers z80_top's ports and repairs slew with this library's delay cells (471 of them; `dlyd_1` on a port is ~5 ns at the slow corner). `EXTRA_EXCLUDED_CELLS` keeps them out: before post-route repair, the chip's opcode setup at the slow corner went from +0.4 to +3.1 ns, the cells took 12 % less area, and the slow corner's total negative slack halved.
- The 9-track library's `drc_exclude.cells` is missing from this PDK version (see the PDK row above); the 7-track list's two cells are excluded instead.
