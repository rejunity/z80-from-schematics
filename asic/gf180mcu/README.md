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
| Clock target | 80 ns (12.5 MHz) -- the 5 V cells at the slow corner (ss, 125 °C, 4.5 V) do ~14 MHz; typical ~22 MHz |
| Config | `config.yaml` (Classic flow, relative die sizing) |

## Run it

LibreLane via Nix (see `docs/librelane-flow.md` for installing Nix with the
fossi-foundation binary cache), then from the repository root:

    nix profile install github:librelane/librelane/3.0.14
    make -C asic/gf180mcu                  # RTL -> GDS (~30 min); the PDK is fetched by ciel
    make -C asic/gf180mcu summary          # area, per-corner timing, sign-off; non-zero exit on a failure
    make -C asic/gf180mcu sign_of_life     # the routed netlist runs asic/tb_sign_of_life.v
    make -C asic/gf180mcu sign_of_life_rtl # same program on rtl/
    make -C asic/gf180mcu synth            # synthesis and its checks only (minutes)

Results land in `asic/gf180mcu/runs/<tag>/` (`final/` holds GDS, netlists,
metrics). CI: `.github/workflows/asic-gf180mcu.yml` runs all of the above on
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
flip-flops, one IDU, WZ operand latch), measured with a 20 MHz target:

| | |
|---|---|
| Std cells | 8,391 |
| Cell area | 155,892 µm² |
| Die | 535 × 553 µm = 0.30 mm² (57 % utilised) |
| Setup @ 20 MHz | -18.5 ns at ss 125 °C 4.5 V (worst path 70 ns, full cycle: ~14 MHz); +5.1 ns typical (~22 MHz) -- hence the 80 ns target |
| Hold slack | +0.25 ns |
| Route / Magic DRC, LVS, antenna, GDS XOR | all 0 (KLayout DRC is not part of LibreLane's GF180 Classic flow) |
| Signs of life, routed netlist | PASS (HALT after 554 clocks) |
| Runtime | 26 min (8 cores, shared) |

Previous core (main before the restructure, same flow): Sign-off clean (DRC / LVS / antenna / XOR 0), 16,225 cells, 0.29 mm² of cells on a 0.50 mm² die, but at 20 MHz the slow corner missed by 26.7 ns (typical passed with +3.9 ns); 12,112 max-slew violations. ~1 h 50 min.

TinyTapeout gf180 tiles are 347 × 161 µm (up to 4x4); at 60 % density the core needs ~0.26 mm², about a 3x2 tile area. wafer.space's smallest slot (0p5x0p5) has a 1.05 × 1.65 mm core area -- the Z80 takes under a fifth of it, pad ring included separately.

## Notes

- The worst path at the slow corner is a full Z80 clock cycle (rising to rising edge) through the execute block's next-state logic, not one of the half-cycle paths into the falling-edge pin former.
