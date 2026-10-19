# RZ80 on SkyWater sky130A — RTL-to-GDS of the bare core

`z80_top` hardened as a stand-alone macro (no pad ring) with the LibreLane
3.0.14 Classic flow: synthesis, floorplan, placement, clock tree, routing,
multi-corner STA, DRC, LVS, antenna, GDS -- then a gate-level "signs of
life" run of the routed netlist. This is a tapeout-readiness check of the
core, independent of any particular shuttle wrapper.

| | |
|---|---|
| PDK | `sky130A`, open_pdks `8afc8346` (the pin of LibreLane 3.0.14, which TinyTapeout's ttsky26d shuttle uses) |
| Cells | `sky130_fd_sc_hd` |
| Clock target | 50 ns (20 MHz) |
| Config | `config.yaml` (Classic flow, relative die sizing) |

## Run it

LibreLane via Nix (see `docs/librelane-flow.md` for installing Nix with the
fossi-foundation binary cache), then from the repository root:

    nix profile install github:librelane/librelane/3.0.14
    make -C asic/sky130                  # RTL -> GDS (~30 min); the PDK is fetched by ciel
    make -C asic/sky130 summary          # area, per-corner timing, sign-off; non-zero exit on a failure (*)
    make -C asic/sky130 sign_of_life     # the routed netlist runs asic/tb_sign_of_life.v
    make -C asic/sky130 sign_of_life_rtl # same program on rtl/
    make -C asic/sky130 synth            # synthesis and its checks only (minutes)

(*) Sign-off as LibreLane counts it: DRC / LVS / antenna / XOR clean,
no setup violations at the corners in TIMING_VIOLATION_CORNERS (typical),
no hold violations at any corner; every corner is printed.

Results land in `asic/sky130/runs/<tag>/` (`final/` holds GDS, netlists,
metrics). CI: `.github/workflows/asic-sky130.yml` runs all of the above on
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
flip-flops, one IDU, WZ operand latch), its pins on the sides where the
Z80 has its pads, in the Z80's order (`pin_order.cfg`, as on the gf180mcu
and IHP cores, whose chips put them in a pad ring):

| | |
|---|---|
| Std cells | 8,435 |
| Cell area | 66,141 µm² |
| Die | 432 × 443 µm = 0.19 mm² (38 % utilised) |
| Setup slack @ 20 MHz | +10.7 ns typical (signed off); -1.4 ns at ss 100 °C 1.60 V (~19.5 MHz) |
| Hold slack | +0.11 ns |
| Route / Magic / KLayout DRC, LVS, antenna, GDS XOR | all 0 |
| Signs of life, routed netlist | PASS (HALT after 554 clocks) |
| Runtime | 16 min (8 cores, shared) |

With the tool's own pin placement the slow corner had +3.8 ns (8,320
cells, 429 × 440 µm); fixing the pins to their sides costs it ~5 ns.

Previous core (main before the restructure, same flow): Did not finish: at 45 % core utilisation global routing failed (93 % of tracks used, 5,095 overflows); at 30 % it was still in detailed routing after 4.5 h on 8 cores. Synthesis: ~92,500 µm² of cells.

TinyTapeout sky130 tiles are 161 × 111.5 µm; at TinyTapeout's 60 % placement density the core needs ~0.11 mm², i.e. about a 4x2 tile area (683 × 226 µm).

## Notes

- sky130 has the least routing resource of the three PDKs; this core's next-state muxes are wire-heavy, so the config keeps placement density low (30 % core utilisation, 40 % target density).
