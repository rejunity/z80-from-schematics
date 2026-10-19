# RZ80 on IHP SG13G2 — RTL-to-GDS of the bare core

`z80_top` hardened as a stand-alone macro (no pad ring) with the LibreLane
3.0.14 Classic flow: synthesis, floorplan, placement, clock tree, routing,
multi-corner STA, DRC, LVS, antenna, GDS -- then a gate-level "signs of
life" run of the routed netlist. This is a tapeout-readiness check of the
core, independent of any particular shuttle wrapper.

| | |
|---|---|
| PDK | `ihp-sg13g2`, IHP-Open-PDK `7bbb4b5a` (2026-10-15), the version of IHP's LibreLane chip template -- the Makefile fetches it with LibreLane's own ciel. LibreLane 3.0.14 pins `c4b8b4e5` (January 2026; also TinyTapeout's ttihp26b), whose I/O cells fail DRC where they abut (IHP-Open-PDK issue 835) |
| Cells | `sg13g2_stdcell` |
| Clock target | 50 ns (20 MHz) |
| Config | `config.yaml` (Classic flow, relative die sizing) |

## Run it

LibreLane via Nix (see `docs/librelane-flow.md` for installing Nix with the
fossi-foundation binary cache), then from the repository root:

    nix profile install github:librelane/librelane/3.0.14
    make -C asic/ihp-sg13g2                  # RTL -> GDS (~30 min); the PDK is fetched by ciel
    make -C asic/ihp-sg13g2 summary          # area, per-corner timing, sign-off; non-zero exit on a failure (*)
    make -C asic/ihp-sg13g2 sign_of_life     # the routed netlist runs asic/tb_sign_of_life.v
    make -C asic/ihp-sg13g2 sign_of_life_rtl # same program on rtl/
    make -C asic/ihp-sg13g2 synth            # synthesis and its checks only (minutes)

(*) Sign-off as LibreLane counts it: DRC / LVS / antenna / XOR clean,
no setup violations at the corners in TIMING_VIOLATION_CORNERS (typical),
no hold violations at any corner; every corner is printed.

Results land in `asic/ihp-sg13g2/runs/<tag>/` (`final/` holds GDS, netlists,
metrics). CI: `.github/workflows/asic-ihp-sg13g2.yml` runs all of the above on
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
their pads (`pin_order.cfg`), power straps on TopMetal1 only, routing up
to Metal5:

| | |
|---|---|
| Std cells | 7,417 |
| Cell area | 89,040 µm² |
| Die | 422 × 440 µm = 0.19 mm² (53 % utilised) |
| Setup slack @ 20 MHz | +12.1 ns at the slow corner (1.08 V 125 °C: ~26 MHz), +13.1 ns typical, +13.7 ns fast -- every corner signed off (the PDK's own setting) |
| Hold slack | +0.13 ns |
| Max slew / cap violations | 0 / 0 |
| Route / Magic / KLayout DRC, LVS, antenna, GDS XOR | all 0 |
| Signs of life, routed netlist | PASS (HALT after 554 clocks) |
| Runtime | 24 min (8 cores, shared) |

Previous core (main before the restructure, same flow): Sign-off clean and met 20 MHz at every corner (+10.7 ns at the slow corner): 13,598 cells, 0.143 mm² of cells on a 0.30 mm² die; ~3 h (2.5 h of it detailed routing).

TinyTapeout IHP tiles are 202 × 155 µm (up to 8x2); at 60 % density the core needs ~0.14 mm², about a 3x2 tile area (637 × 314 µm).

## The chip: z80_top in a pad ring

`chip/` builds `z80_chip`: the hardened `z80_top` as a hard macro in a ring
of 48 `sg13g2_io` pads, 12 a side. Each pin of the 40-pin Z80 has its pad
on the side of the die where the original's bond pad is (Visual Z80 die
scan), in the same order; eight more pads are supplies. The IHP core runs
at 1.2 V, the pads at 3.3 V: pins 11 (VCC) and 29 (GND) are the I/O supply,
a core supply pad sits beside each, and six more supply pads fill the
sides to 12 -- three I/O and two core supply pairs in all.

    south  A5 A6 A7 A8 A9 A10 A11 A12 A13 A14 A15 CLK                  (left to right)
    east   D4 D3 D5 D6 VCC VDD_CORE VSS_CORE_E GND_E D2 D7 D0 D1      (bottom to top)
    north  INT NMI HALT MREQ IORQ VCC_N VDD_CORE_N GND_N RD WR BUSAK WAIT  (right to left)
    west   BUSRQ RESET M1 RFSH VCC_W GND VSS_CORE A0 A1 A2 A3 A4       (top to bottom)

The pads abut at their own 80 µm pitch (LibreLane's pad ring), which with
12 a side makes the die IHP's template's 1600 µm square. `z80_top`'s own
pins sit on the same sides in the same order (`pin_order.cfg`), so the pad
wires are short. As on the Z80, the address bus floats during RESET and
BUSAK, MREQ IORQ RD WR during BUSAK, the data bus whenever the CPU does not
drive it (`chip/z80_chip.v`; `z80_top` itself is unchanged).

    make -C asic/ihp-sg13g2                     # z80_top first: the chip uses its latest run as its macro
    make -C asic/ihp-sg13g2 chip                # the chip (LibreLane Chip flow, into chip/runs/)
    make -C asic/ihp-sg13g2 chip_summary        # sign-off of the chip, as `summary`
    make -C asic/ihp-sg13g2 chip_sign_of_life   # the signs-of-life program through the pads

| | |
|---|---|
| Die | 1600 × 1600 µm = 2.56 mm², pad-limited (12 pads a side abut; a multiple of the 400 µm step of the density rules' 800 µm windows) |
| Pads | 48: `IOPadTriOut4mA` (address bus, MREQ IORQ RD WR), `IOPadOut4mA` (M1 RFSH HALT BUSAK), `IOPadInOut4mA` (data bus), `IOPadIn` (inputs), `IOPadIOVdd` / `IOPadIOVss` (VCC, GND and two more pairs), `IOPadVdd` / `IOPadVss` (core supply, two pairs) |
| Sign-off | KLayout DRC (IHP's reference deck), LVS, antenna, XOR all 0; setup and hold met at every corner; 7 unconnected outputs (`z80_top`'s debug pins, no pads) |
| Signs of life through the pads | PASS (HALT after 554 clocks) |
| Runtime | ~20 min after `make` (the core is not re-synthesized or re-routed) |

DRC follows IHP's chip template (ihp-sg13-librelane-template, Apache-2.0):
its PDK version, whose I/O cells abut cleanly -- on LibreLane 3.0.14's pin
the supply pads' contacts sat 0.085 µm from the cell edge and KLayout
reported 210 Cnt.b / TV2.b errors -- and KLayout as the sign-off DRC, the
template's `make librelane-klayoutdrc`. Magic's IHP deck, run on this chip
for comparison, reports the seal ring (Seal.a / Seal.b, 1,494) and
overlaps between the abutted I/O cells (669), which KLayout passes; it
also takes 35 min here. The template's bond pad cell is carried in
`chip/ip/` (GDS, LEF, and its empty Verilog view for the gate-level run),
and `chip/pads_bb.v` are blackbox views of the pad cells for synthesis.

## Notes

- Gate-level simulation of SG13G2 netlists needs iverilog >= 12: the cell models drive their flip-flops through `$setuphold` delayed nets, which iverilog 11 leaves undriven (every flop stays X).
- Since the 2026-10 PDK the cell models keep their primitives in `sg13g2_udp.v`, beside `sg13g2_stdcell.v`; `sign_of_life.py` adds it, as IHP's template's own testbench does.
