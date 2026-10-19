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

.PHONY: harden synth summary sign_of_life sign_of_life_rtl chip chip_summary chip_sign_of_life clean
harden:
	$(LIBRELANE) --pdk $(PDK) --scl $(SCL) $(LIBRELANE_ARGS) $(CONFIG)

synth:
	$(LIBRELANE) --pdk $(PDK) --scl $(SCL) --to Checker.YosysSynthChecks $(LIBRELANE_ARGS) $(CONFIG)

summary:
	@../summary.py $(LATEST)

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

clean:
	rm -rf runs chip/runs chip/macro
