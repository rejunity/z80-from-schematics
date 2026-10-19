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
