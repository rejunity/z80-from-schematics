#!/usr/bin/env bash
# Synthesize z80_top alone for one FPGA family and report its size.
#
# usage: scripts/fpga_luts.sh <ice40|ecp5|gowin> <out dir>
#
# Writes <out>/stat.txt + stat.json (yosys stat), <out>/z80_top.v (the
# mapped netlist), <out>/z80_top_sim.v (the same with the cell models
# flattened in, for gate-level simulation) and <out>/yosys.log. Same recipe as the board
# builds on the fpga-* branches: synth_<family> straight on the RTL, so
# the core's async-reset flops map onto the families' async set/reset
# FFs (SB_DFFR/S, TRELLIS_FF, DFFC/DFFP) and keep the RTL semantics.
#
# Do NOT run `proc; async2sync` first (the fpga branches used to):
# async2sync rewrites each flop as sync-reset plus a Q-side
# `reset ? value : Q` mux, so the hold paths no longer read Q, opt_dff
# stops recognising them as clock enables and every hold becomes LUT
# muxes -- ~2,350 extra LUT4 on iCE40 (9,352 -> 6,999) and ~3,000 / ~3,900
# on the ECP5 / Gowin SoC builds.
#
# YOSYS defaults to yowasp-yosys (pip install yowasp-yosys); it only sees
# files under its working directory, so the sources are staged into
# <out>/src first.
set -euo pipefail
fam=${1:?family: ice40, ecp5 or gowin}
out=${2:?output directory}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
YOSYS=${YOSYS:-yowasp-yosys}

case $fam in
  ice40) synth="synth_ice40 -top z80_top -abc9 -dsp" ;;
  ecp5)  synth="synth_ecp5 -top z80_top" ;;
  gowin) synth="synth_gowin -top z80_top -family gw5a -nolutram" ;;
  *) echo "unknown family $fam" >&2; exit 2 ;;
esac

mkdir -p "$out/src"
cp "$ROOT"/rtl/*.v "$ROOT"/rtl/*.vh "$out/src/"
cd "$out"
"$YOSYS" -q -l yosys.log -p "
  read_verilog -Isrc src/*.v;
  hierarchy -top z80_top;
  $synth;
  tee -q -o stat.txt stat;
  tee -q -o stat.json stat -json;
  write_verilog -noattr z80_top.v" >/dev/null

# Gate-level simulation netlist: flatten yosys' own models of the
# family's cells into the mapped netlist, so plain iverilog can run it
# (the vendor cells_sim.v files use SystemVerilog iverilog rejects).
"$YOSYS" -q -l yosys_sim.log -p "
  read_verilog z80_top.v;
  read_verilog -overwrite -D NO_ICE40_DEFAULT_ASSIGNMENTS +/$fam/cells_sim.v;
  hierarchy -top z80_top;
  proc; flatten; opt_clean;
  write_verilog -noattr z80_top_sim.v" >/dev/null
