# RZ80 on IHP SG13G2 — RTL-to-GDS of the bare core

`z80_top` hardened as a stand-alone macro (no pad ring) with the LibreLane
3.0.14 Classic flow: synthesis, floorplan, placement, clock tree, routing,
multi-corner STA, DRC, LVS, antenna, GDS -- then a gate-level "signs of
life" run of the routed netlist. This is a tapeout-readiness check of the
core, independent of any particular shuttle wrapper.

| | |
|---|---|
| PDK | `ihp-sg13g2`, IHP-Open-PDK `c4b8b4e5` (the pin of LibreLane 3.0.5 / 3.0.14; TinyTapeout's ttihp26b shuttle uses LibreLane 3.0.5) |
| Cells | `sg13g2_stdcell` |
| Clock target | 50 ns (20 MHz) |
| Config | `config.yaml` (Classic flow, relative die sizing) |

## Run it

LibreLane via Nix (see `docs/librelane-flow.md` for installing Nix with the
fossi-foundation binary cache), then from the repository root:

    nix profile install github:librelane/librelane/3.0.14
    make -C asic/ihp-sg13g2                  # RTL -> GDS (~30 min); the PDK is fetched by ciel
    make -C asic/ihp-sg13g2 summary          # area, per-corner timing, sign-off; non-zero exit on a failure
    make -C asic/ihp-sg13g2 sign_of_life     # the routed netlist runs asic/tb_sign_of_life.v
    make -C asic/ihp-sg13g2 sign_of_life_rtl # same program on rtl/
    make -C asic/ihp-sg13g2 synth            # synthesis and its checks only (minutes)

Results land in `asic/ihp-sg13g2/runs/<tag>/` (`final/` holds GDS, netlists,
metrics). CI: `.github/workflows/asic-ihp-sg13g2.yml` runs all of the above on
every push to this branch (and on demand), and keeps the GDS, netlist and
metrics as artifacts.

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
flip-flops, one IDU, WZ operand latch):

| | |
|---|---|
| Std cells | 7,133 |
| Cell area | 86,061 µm² |
| Die | 414 × 433 µm = 0.18 mm² (53 % utilised) |
| Setup slack @ 20 MHz | +12.2 ns worst corner (slow, 1.08 V 125 °C: ~26 MHz); +13.1 ns typical |
| Hold slack | +0.12 ns |
| Max slew / cap violations | 0 / 0 |
| Route / Magic / KLayout DRC, LVS, antenna, GDS XOR | all 0 |
| Signs of life, routed netlist | PASS (HALT after 554 clocks) |
| Runtime | 36 min (8 cores, shared) |

Previous core (main before the restructure, same flow): Sign-off clean and met 20 MHz at every corner (+10.7 ns at the slow corner): 13,598 cells, 0.143 mm² of cells on a 0.30 mm² die; ~3 h (2.5 h of it detailed routing).

TinyTapeout IHP tiles are 202 × 155 µm (up to 8x2); at 60 % density the core needs ~0.14 mm², about a 3x2 tile area (637 × 314 µm).

## Notes

- Gate-level simulation of SG13G2 netlists needs iverilog >= 12: the cell models drive their flip-flops through `$setuphold` delayed nets, which iverilog 11 leaves undriven (every flop stays X).
