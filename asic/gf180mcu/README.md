# RZ80 on GlobalFoundries gf180mcuD — RTL-to-GDS of the bare core

`z80_top` hardened as a stand-alone macro (no pad ring) with the LibreLane
3.0.14 Classic flow: synthesis, floorplan, placement, clock tree, routing,
multi-corner STA, DRC, LVS, antenna, GDS -- then a gate-level "signs of
life" run of the routed netlist. This is a tapeout-readiness check of the
core, independent of any particular shuttle wrapper.

| | |
|---|---|
| PDK | `gf180mcuD`, `54435919` (the pin of LibreLane 3.0.14, used by TinyTapeout's ttgf26d shuttle). wafer.space's project template pins `f6eeac7d` of the same gf180mcuD variant and the same 7-track 5 V cell library (`gf180mcu_fd_sc_mcu7t5v0`, with `gf180mcu_fd_io` pads). |
| Cells | `gf180mcu_fd_sc_mcu7t5v0` |
| Clock target | 50 ns (20 MHz), met at the typical corners LibreLane signs off (+7.5 ns); the 5 V cells at the slow corner (ss, 125 °C, 4.5 V) reach ~15.5 MHz |
| Config | `config.yaml` (Classic flow, relative die sizing) |

## Run it

LibreLane via Nix (see `docs/librelane-flow.md` for installing Nix with the
fossi-foundation binary cache), then from the repository root:

    nix profile install github:librelane/librelane/3.0.14
    make -C asic/gf180mcu                  # RTL -> GDS (~20 min); the PDK is fetched by ciel
    make -C asic/gf180mcu summary          # area, per-corner timing, sign-off; non-zero exit on a failure (*)
    make -C asic/gf180mcu sign_of_life     # the routed netlist runs asic/tb_sign_of_life.v
    make -C asic/gf180mcu sign_of_life_rtl # same program on rtl/
    make -C asic/gf180mcu synth            # synthesis and its checks only (minutes)
    make -C asic/gf180mcu pla_check        # the decoder's AND / OR planes against rtl/z80_pla.v, all 2048 inputs
    make -C asic/gf180mcu floorplan_check  # the die floorplan's anchors in the latest run
    make -C asic/gf180mcu netlist_traces   # trace programs and pin scenarios on the routed netlist vs perfectz80

(*) Sign-off as LibreLane counts it: DRC / LVS / antenna / XOR clean,
no setup violations at the corners in TIMING_VIOLATION_CORNERS (typical),
no hold violations at any corner; every corner is printed.

Results land in `asic/gf180mcu/runs/<tag>/` (`final/` holds GDS, netlists,
metrics). CI: `.github/workflows/asic-gf180mcu.yml` runs all of the above on
every push to this branch (and on demand), and keeps the GDS, netlist and
metrics as artifacts. The job itself is `asic-pdk.yml`, which main's
`targets.yml` also runs nightly with this branch merged into main, so a
core change on main that breaks the flow shows up within a day.

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

Restructured core (`optimise-luts`: control word + shared datapath, exchange
flip-flops, one IDU, WZ operand latch), synthesized per RTL block
(`SYNTH_HIERARCHY_MODE: deferred_flatten`, which keeps the block names for
the block map) and laid out as the chip's macro: pins on the sides of
their pads (`pin_order.cfg`), power straps on Metal4 only, routing up to
Metal4. Measured with a 20 MHz target:

| | |
|---|---|
| Std cells | 8,595 |
| Cell area | 155,903 µm² |
| Die | 544 × 562 µm = 0.31 mm² (56 % utilised) |
| Setup slack @ 20 MHz | +7.5 ns typical (signed off; ~29 MHz); -14.7 ns at ss 125 °C 4.5 V (~15.5 MHz) |
| Hold slack | +0.26 ns |
| Route / Magic DRC, LVS, antenna, GDS XOR | all 0 (KLayout DRC is not part of LibreLane's GF180 Classic flow) |
| Signs of life, routed netlist | PASS (HALT after 554 clocks) |
| Runtime | 19 min (8 cores, shared) |

Previous core (main before the restructure, same flow): Sign-off clean (DRC / LVS / antenna / XOR 0), 16,225 cells, 0.29 mm² of cells on a 0.50 mm² die, but at 20 MHz the slow corner missed by 26.7 ns (typical passed with +3.9 ns); 12,112 max-slew violations. ~1 h 50 min.

TinyTapeout gf180 tiles are 347 × 161 µm (up to 4x4); at 60 % density the core needs ~0.26 mm², about a 3x2 tile area. wafer.space's smallest slot (0p5x0p5) has a 1.05 × 1.65 mm core area -- the Z80 takes under a fifth of it, pad ring included separately.

## The die floorplan

`z80_top` is laid out the way the Z80 die is, as Ken Shirriff's die analysis
describes it (righto.com: the labelled die photo of "The Z-80's 16-bit
increment/decrement circuit reverse engineered", "Down to the silicon: how
the Z80's registers are implemented", "Why the Z-80's data pins are
scrambled"). The pins already sit on the die's sides (`pin_order.cfg`), so
the die's arrangement faces the right pins. A LibreLane step of RZ80's own,
`RZ80.DieFloorplan` (`asic/librelane_plugin_rz80/`, found on `PYTHONPATH`,
inserted by `config.yaml`'s `substituting_steps`), runs after the pins are
placed and before global placement; it fixes the anchor cells (FIRM)
and global placement places everything else around them. A second step,
`RZ80.ReleaseAnchors`, frees them right after, so the timing repairs may
size them (a fixed cell that grows in place would overlap its neighbour)
and detailed placement shifts them the few sites that needs. The
arrangement is a short table written by hand from those descriptions
(`arrangement.py`); no die data is read.

- The register file: the 224 flip-flops in the die's 14 columns of 16-bit
  pairs, west to east PC IR (a gap) WZ SP IY IX DE DE' HL HL' BC BC' AF'
  AF, each column's bits in order from the top, the low byte above the
  high byte. `p_*0` / `p_*1` are RZ80's physical banks, which EXX, EX
  AF,AF' and EX DE,HL swap as the die does; their flip-flops sit in a row
  above the columns they swap. Tap cells come every 70 sites in each row,
  half a gap apart from row to row, and a set flip-flop takes 36 sites, so
  the columns of a pair share a gap between taps -- the first in the bit's
  row, the second in the row above -- and stand a uniform 19.6 µm apart.
- Beside it, bit for bit: the address latch on the west, and west of it
  the address unit (the incrementer/decrementer and the address muxes) as
  16 bit planes along the A pins, as the die's incrementer strip -- a cell
  belongs to bit i when the address outputs it reaches are all bit i, or
  else when bit i is the highest input bit in its cone; the ALU's operand
  latch on the east. The write-data latch sits at the data pins, each bit
  with its pin's latches, as the die's data latches line its east edge.
- The decoder as a PLA: `asic/pla/planes.py` turns `rtl/z80_pla.v`'s truth
  table (all 2048 inputs) into an AND plane of product terms and an OR
  plane of one line per output bit, three modules so synthesis keeps the
  terms (`asic/pla/z80_pla_planes.v`, used here instead of
  `rtl/z80_pla.v`; `make pla_check` proves them equal on every input). The
  terms' gates stand in a grid, the OR lines below them, the instruction
  register with each input line's true and complement drivers at the east
  end, as on the die.
- The ALU as 8 bit planes east of its operand latches, as the die stacks
  its ALU's bit slices beside the register file: bit i's cells in the
  four placement rows of the register file's bit row i, so the bus lines
  line up. A cell belongs to bit i when it reaches a result bit and bit i
  is the highest operand bit in its input cone (a carry chain counts to the
  bit it ends in), or when it feeds only the flags and reads only bit i;
  the logic no bit owns (zero, parity, overflow, half-carry, the flags'
  assembly, the operation decode) fills a strip below the planes, level
  with the high byte. RZ80's ALU is one 8-bit pass; the Z80's is a 4-bit
  core used twice, so the planes stand for the die's 8-bit slices around it.
- The state flip-flops in a row on top, and `z80_timing` (the pin images
  of the T-states) in a band below them, above the PLA and under the
  control pins, where the die keeps its state logic; every pin's
  flip-flops at its pin.

`make floorplan_check` reads the step's `anchors.json` and the final DEF:
every anchor within 10 µm of where it was put, the register columns in the
die's order west to east and each column's bits in order, the ALU's and
the address unit's bit planes in bit order.

Measured (RUN_2026-10-29_01-02-51, against the automatic layout above):
the core is 718 x 630 µm (0.45 mm², 42 % utilised; automatic 0.32 mm²,
56 %), sign-off clean, +5.6 ns setup slack at the typical corner (+8.1)
and -17.0 ns at the slow one (~15 MHz; -9.4, ~17 MHz) -- the die's long
bit and PLA lines cost the slow corner. On an idle 8-core machine the flow
takes 73 min (93 min of CPU) against the automatic layout's 18 min (27),
most of the difference the resizer's repairs after global routing on the
longer wires; the floorplan step itself takes ~1.5 s. The chip die grows 50 µm each way (1712 x 1562 µm); its pins
still meet the NMOS Z80's 4 MHz datasheet timing (`make pin_timing`).
Four placement rows a bit are what routes: with three, Metal3 -- the one
horizontal layer under the chip's grid -- overflows.

## Power: grid or tree (`PDN_STYLE`)

By default pdngen draws the usual power grid: Metal1 rails under the
cells, Metal4 stripes across the core (the block's power pins), and in
the chip Metal5 stripes over the macro. `PDN_STYLE=tree` draws the NMOS
die's kind instead, two interdigitated combs fed from the edges where the
Z80 has its supply pads -- +5 V east, GND west:

    make -C asic/gf180mcu PDN_STYLE=tree          # the core with the tree
    make -C asic/gf180mcu chip PDN_STYLE=tree     # its chip

- core (`config.tree.yaml` on top of `config.yaml`, `pdn_tree.tcl`):
  pdngen draws only the rails; `RZ80.PowerTree` then draws a VSS trunk
  down the west edge and a VDD trunk down the east edge (Metal4, 10 µm,
  the block's power pins), 5 µm Metal5 fingers off each trunk in turn
  every 8 rows, each stopping 5 µm short of the other trunk, and short
  Metal4 twigs every 60 µm along each finger, tying it to the rails of
  its net nearest to it -- every rail on one finger, so fingers meet
  only at their trunk -- and every rail's end on its own trunk;
- chip (`chip/config.tree.yaml`, `chip/pdn_tree.tcl`): the chip's grid
  (`pdn.tcl`, without its macro grid) stays off the macro, whose Metal5
  carries the fingers (`RZ80.PowerTreeChipObstruct`); then Metal5 straps
  over each trunk's ends, where the macro has no Metal5, feed the VDD
  trunk from the core ring's east side and the VSS trunk from its west
  side (`RZ80.PowerTreeChip`). The core ring and the pad ring stay rings.

Both steps re-run PSM's connectivity check once the tree is drawn (pdngen's
own check runs before the trunks, the power pins, exist).

Measured against the grid (core RUN_2026-10-29_08-19-51 vs
RUN_2026-10-29_01-02-51, both on an idle machine): sign-off clean, the
chip's pins still meet the 4 MHz datasheet; worst static IR drop 7-9 mV on
VDD and VSS at tt / ss / ff against the grid's 1.5-2 mV (OpenROAD PSM, the
block's pins as the sources: the grid's are all its stripes, the tree's
its two trunks); peak current 0.9 mA in a Metal1 rail (grid 0.26 mA),
2.4 mA in a Metal5 finger. The tree's via stacks take Metal2 and Metal3
tracks the grid leaves free: global-routing overflow 427 against 174,
1.07x the wire length, -22.2 ns setup slack at the slow corner (grid
-17.0) and +4.5 ns at the typical one (+5.6), and the flow takes 99 min
against 73.

Without `PDN_STYLE` (or with `grid`) the flows read only `config.yaml` /
`chip/config.yaml`, exactly as before.

## The chip: z80_top in a pad ring

`chip/` builds `z80_chip`: the hardened `z80_top` as a hard macro in a ring
of 40 `gf180mcu_fd_io` pads, one per pin of the 40-pin Z80, each on the
side of the die where the original's bond pad is (Visual Z80 die scan) and
in the same order:

    south  A5 A6 A7 A8 A9 A10 A11 A12 A13 A14 A15 CLK   (left to right)
    east   D4 D3 D5 D6 VCC D2 D7 D0 D1                  (bottom to top)
    north  INT NMI HALT MREQ IORQ RD WR BUSAK WAIT       (right to left)
    west   BUSRQ RESET M1 RFSH GND A0 A1 A2 A3 A4       (top to bottom)

The spacing is the pads' own, not the Z80's: the die is as small as the
pads allow, the 12 pads along the bottom and the 10 up the left side abut
at their 75 µm pitch, and LibreLane's pad ring spreads the other two sides'
pads evenly. `z80_top`'s own pins sit on the same sides in the same order
(`pin_order.cfg`), so the pad wires are short. The foundry pad ring shorts
the core supply to the 5 V I/O supply, so VCC and GND are the only supply
pads -- exactly the Z80's two. As on the Z80, the address bus floats during
RESET and BUSAK, MREQ IORQ RD WR during BUSAK, the data bus whenever the
CPU does not drive it (`chip/z80_chip.v`; `z80_top` itself is unchanged).

    make -C asic/gf180mcu                     # z80_top first: the chip uses its latest run as its macro
    make -C asic/gf180mcu chip                # the chip (LibreLane Chip flow, into chip/runs/)
    make -C asic/gf180mcu chip_summary        # sign-off of the chip, as `summary`
    make -C asic/gf180mcu chip_sign_of_life   # the signs-of-life program through the pads

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
`make -C asic/gf180mcu pin_timing` measures the routed chip with OpenSTA
at 100 pF and checks every parameter the replica has to meet -- PASS at
the tt, ss and ff corners. Output delays take 13-43 ns against 65-150 ns
limits; the tightest margins are the opcode's setup TsD(Cr) at the slow
corner (+8.9 ns of 35 ns) and the input holds at the fast corner (+0.6
to +1.4 ns, after the flow's hold repair; the falling clock edge reaches
the core late through the Schmitt CLK pad). The faster NMOS grades, 6 and
8 MHz (Z80B, Z80H), pass as well at every corner, the tightest by +0.5 ns
(an input hold at the fast corner; the opcode's setup keeps +3.8 and
+5.0 ns at the slow one).

The pad ring settings follow open-pdks' LibreLane config for
`gf180mcu_fd_io` (the PDK version LibreLane 3.0.14 pins predates it) and
the wafer.space gf180mcu project template; `chip/pads_bb.v` are blackbox
views of the pad cells for synthesis, whose own models do not lint.

## Notes

- Two kinds of path set the slow corner's clock (~15.5 MHz), about equally: full Z80 clock cycles (rising to rising edge) through decode, the execute block, the register file and the address unit into the address bus's pin former, and half cycles from read data sampled on the falling edge (`data_in_neg`) through the ALU and the register file into the same former, on the next rising edge. The half-cycle ones are also the typical corner's critical paths, which the resizer sizes until the typical corner is met. The 9-track cells (`asic-gf180mcu-9t`) speed up the full-cycle paths a lot but leave the half-cycle ones where they are: ~15.6 MHz at the slow corner, about the same.
- A looser target does not buy slow-corner margin: at 80 ns the typical corner has 14 ns spare, the resizer stops early, and the slow corner ends at ~89 ns (-9 ns). Closing 20 MHz at 125 °C / 4.5 V would take shorter half-cycle paths (RTL) or a flow that sizes for the slow corner.
- Post-route repair (`RUN_POST_GRT_RESIZER_TIMING`, `RUN_POST_GRT_DESIGN_REPAIR`; LibreLane leaves both off): the resizer works on estimated wiring and had judged the slow corner nearly done, then routing cost it 10-20 ns. One more pass on the routed wires' parasitics wins back about a third: at the slow corner the full-cycle slack went from -20.6 to -14.7 ns and the half-cycle from -10.1 to -6.2 ns (~14.2 -> ~15.5 MHz; typical ~25.6 -> ~29.5 MHz), and the chip's opcode setup from +5.9 to +8.9 ns, for 2 % more cell area and ~5 minutes.
- Delay cells: left to itself, LibreLane buffers z80_top's ports and repairs slew with this library's delay cells (521 of them; `dlyb_1` on a port is 3 ns at the slow corner). `EXTRA_EXCLUDED_CELLS` keeps them out: the chip's opcode setup gained 2 ns at the slow corner and the 6 and 8 MHz grades went from failing it (-1.3 / -0.0 ns) to passing; the cells take 7 % less area and the typical corner gained 1.4 ns. The slow corner's single worst path came out 3.8 ns longer, with its total negative slack unchanged -- run-to-run scatter of a corner the resizer cannot close.
