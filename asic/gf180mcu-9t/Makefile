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
SCL          := gf180mcu_fd_sc_mcu9t5v0
# LibreLane 3.0.14's own gf180mcu version, fetched with its own ciel and
# passed with --manual-pdk: its LibreLane settings for the 9-track library
# name a drc_exclude.cells (PNR_EXCLUDED_CELL_FILE) that only the 7-track
# library ships, and LibreLane stops on the missing file. pnr_exclude.cells
# goes there: the 7-track list's two cells (mux2_1, oai33_2), renamed.
PDK_VERSION  := 54435919abffb937387ec956209f9cf5fd2dfbee
CIEL_HOME    ?= $(HOME)/.ciel
PDK_ROOT     := $(CIEL_HOME)/ciel/gf180mcu/versions/$(PDK_VERSION)
SCL_EXCLUDE  := $(PDK_ROOT)/$(PDK)/libs.tech/openlane/$(SCL)/drc_exclude.cells
CIEL         ?= nix run github:librelane/librelane/3.0.14\#python3.pkgs.ciel --
FLOW_PDK      = --manual-pdk --pdk-root $(PDK_ROOT) --pdk $(PDK) --scl $(SCL)
CONFIG       := config.yaml
LIBRELANE    ?= librelane
LIBRELANE_ARGS ?=

LATEST = $(shell ls -dt runs/*/ 2>/dev/null | head -1)
CHIP_LATEST = $(shell ls -dt chip/runs/*/ 2>/dev/null | head -1)
# z80_top's timing views for the chip: the nominal-parasitics corner of
# each process corner
MACRO_CORNERS := tt ss ff

.PHONY: harden synth summary sign_of_life sign_of_life_rtl chip chip_summary chip_sign_of_life pin_timing clean
harden: | $(SCL_EXCLUDE)
	$(LIBRELANE) $(FLOW_PDK) $(LIBRELANE_ARGS) $(CONFIG)

synth: | $(SCL_EXCLUDE)
	$(LIBRELANE) $(FLOW_PDK) --to Checker.YosysSynthChecks $(LIBRELANE_ARGS) $(CONFIG)

$(PDK_ROOT):
	$(CIEL) fetch --pdk-root $(CIEL_HOME) --pdk-family gf180mcu $(PDK_VERSION)

$(SCL_EXCLUDE): | $(PDK_ROOT)
	cp pnr_exclude.cells $@

summary:
	@../summary.py $(LATEST)

sign_of_life:
	@../sign_of_life.py $(LATEST)

sign_of_life_rtl:
	@../sign_of_life.py --rtl

chip: | $(SCL_EXCLUDE)
	@test -n "$(LATEST)" || { echo "no z80_top run to take the macro from: run make first"; exit 1; }
	rm -rf chip/macro && mkdir -p chip/macro/lib
	cp $(addprefix $(LATEST)final/,gds/z80_top.gds lef/z80_top.lef vh/z80_top.vh nl/z80_top.nl.v pnl/z80_top.pnl.v) chip/macro/
	for c in $(MACRO_CORNERS); do cp $(LATEST)final/lib/nom_$${c}_*/z80_top__nom_$${c}_*.lib chip/macro/lib/$$c.lib; done
	$(LIBRELANE) $(FLOW_PDK) $(LIBRELANE_ARGS) chip/config.yaml

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
