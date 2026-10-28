# Harden z80_top on this branch's PDK with LibreLane (Nix install, see
# README.md). `make` runs the full Classic flow (RTL -> GDS, no pad ring);
# `make summary` prints area / timing / sign-off numbers of the latest run
# and fails if a sign-off check does; `make sign_of_life` runs the
# signs-of-life program on the latest run's routed netlist (RTL version:
# `make sign_of_life_rtl`).
#
# The chip: `make chip` builds z80_chip -- z80_top in a ring of I/O pads
# on the Z80's sides (chip/z80_chip.v, chip/config.yaml, LibreLane Chip
# flow) -- into chip/runs/, apart from z80_top's runs. z80_top goes in as
# the hard macro of the latest `make` run (its views copied to
# chip/macro/), so run `make` first. `make chip_summary` and
# `make chip_sign_of_life` (the same program through the pads) read the
# chip's latest run.
# RZ80's LibreLane plugin (asic/librelane_plugin_rz80): the step that lays
# z80_top out after the Z80 die (config.yaml's substituting_steps)
export PYTHONPATH := $(abspath ..)$(if $(PYTHONPATH),:$(PYTHONPATH))

PDK          := gf180mcuD
SCL          := gf180mcu_fd_sc_mcu7t5v0
CONFIG       := config.yaml
LIBRELANE    ?= librelane
LIBRELANE_ARGS ?=

LATEST = $(shell ls -dt runs/*/ 2>/dev/null | head -1)
CHIP_LATEST = $(shell ls -dt chip/runs/*/ 2>/dev/null | head -1)
# z80_top's timing views for the chip: the nominal-parasitics corner of
# each process corner
MACRO_CORNERS := tt ss ff

.PHONY: harden synth summary sign_of_life sign_of_life_rtl chip chip_summary chip_sign_of_life pin_timing clean pla pla_check pla_check_gl floorplan_check netlist_traces
harden:
	$(LIBRELANE) --pdk $(PDK) --scl $(SCL) $(LIBRELANE_ARGS) $(CONFIG)

synth:
	$(LIBRELANE) --pdk $(PDK) --scl $(SCL) --to Checker.YosysSynthChecks $(LIBRELANE_ARGS) $(CONFIG)

summary:
	@../summary.py $(LATEST)

# the decoder's AND and OR planes (asic/pla/): regenerate them from
# rtl/z80_pla.v, or check them against it on all 2048 inputs (iverilog)
pla:
	python3 ../pla/planes.py
pla_check:
	python3 ../pla/planes.py --check ../pla/z80_pla_planes.v
# ... and as the latest run synthesized them, gate by gate on the PDK's cell models
pla_check_gl:
	python3 ../pla/planes.py --check-run $(LATEST)

# the latest run's die floorplan: every anchor where RZ80.DieFloorplan put it
floorplan_check:
	@../floorplan_check.py $(LATEST)

# the trace programs and the pin scenarios on the latest run's routed
# netlist, half-cycle by half-cycle against perfectz80 -- what main's CI
# does with the synthesized netlist (perfectz80_netlist,
# pin_scenarios_netlist), here on the layout's own gates
NL_VVP = $(abspath ../../build/tb_z80_netlist_$(PDK).vvp)
NL_MODELS = $(shell python3 -c "import json,glob,os; m=json.load(open('$(LATEST)resolved.json'))['CELL_VERILOG_MODELS']; print(' '.join(m + [u for x in m for u in sorted(glob.glob(os.path.join(os.path.dirname(x), '*_udp.v'))) if u not in m]))")
netlist_traces:
	@test -n "$(LATEST)" || { echo "no z80_top run: run make first"; exit 1; }
	$(MAKE) -C ../.. build/bin/perfectz80_runner
	iverilog -g2012 -DFUNCTIONAL -DUNIT_DELAY=#1 -s tb_z80 -o $(NL_VVP) ../../tests/iverilog/tb_z80_netlist.v $(LATEST)final/nl/z80_top.nl.v $(NL_MODELS)
	cd ../.. && TB_NETLIST_VVP=$(NL_VVP) NETLIST_LABEL="$(PDK), routed" python3 scripts/compare_signal_timing.py --rtl=netlist 200
	cd ../.. && TB_NETLIST_VVP=$(NL_VVP) NETLIST_LABEL="$(PDK), routed" python3 scripts/compare_signal_timing.py --rtl=netlist 200 $$(ls tests/traces/pin_scenarios/*.hex)

sign_of_life:
	@../sign_of_life.py $(LATEST)

sign_of_life_rtl:
	@../sign_of_life.py --rtl

chip:
	@test -n "$(LATEST)" || { echo "no z80_top run to take the macro from: run make first"; exit 1; }
	rm -rf chip/macro && mkdir -p chip/macro/lib
	cp $(addprefix $(LATEST)final/,gds/z80_top.gds lef/z80_top.lef vh/z80_top.vh nl/z80_top.nl.v pnl/z80_top.pnl.v) chip/macro/
	for c in $(MACRO_CORNERS); do cp $(LATEST)final/lib/nom_$${c}_*/z80_top__nom_$${c}_*.lib chip/macro/lib/$$c.lib; done
	$(LIBRELANE) --pdk $(PDK) --scl $(SCL) $(LIBRELANE_ARGS) chip/config.yaml

chip_summary:
	@../summary.py $(CHIP_LATEST)

chip_sign_of_life:
	@../sign_of_life.py $(CHIP_LATEST)

# the chip's pins against the NMOS Z80's AC characteristics (Zilog PS0178,
# Z8400), measured by OpenSTA on the chip's latest run: GRADE=4 (Z80A,
# the default), 6 or 8 MHz
GRADE ?= 4
STA   ?= $(shell command -v sta || echo "nix run github:librelane/librelane/3.0.14\#opensta --")
pin_timing:
	@STA="$(STA)" ../timing/pin_timing.py $(CHIP_LATEST) --grade $(GRADE)

clean:
	rm -rf runs chip/runs chip/macro
