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
PDK          := ihp-sg13g2
SCL          := sg13g2_stdcell
# The PDK version of IHP's LibreLane chip template
# (ihp-sg13-librelane-template, 2026-10-16), newer than LibreLane 3.0.14's
# own (c4b8b4e5, January 2026): its I/O cells abut without DRC errors
# (IHP-Open-PDK issue 835), which the chip's pad ring needs. (The template
# moved on the next day to 89d42ee6, whose LibreLane settings need
# LibreLane's development branch; the I/O cells are the same.) LibreLane's
# own ciel fetches it into ~/.ciel, beside the versions LibreLane fetches.
PDK_VERSION  := 7bbb4b5a5b64c446fedc64eacadc33fa65ade276
CIEL_HOME    ?= $(HOME)/.ciel
PDK_ROOT     := $(CIEL_HOME)/ciel/$(PDK)/versions/$(PDK_VERSION)
CIEL         ?= nix run github:librelane/librelane/3.0.14\#python3.pkgs.ciel --
FLOW_PDK      = --manual-pdk --pdk-root $(PDK_ROOT) --pdk $(PDK) --scl $(SCL)
CONFIG       := config.yaml
LIBRELANE    ?= librelane
LIBRELANE_ARGS ?=

LATEST = $(shell ls -dt runs/*/ 2>/dev/null | head -1)
CHIP_LATEST = $(shell ls -dt chip/runs/*/ 2>/dev/null | head -1)
# z80_top's timing views for the chip: the nominal-parasitics corner of
# each process corner
MACRO_CORNERS := typ slow fast

.PHONY: harden synth summary sign_of_life sign_of_life_rtl chip chip_summary chip_sign_of_life pin_timing clean
harden: | $(PDK_ROOT)
	$(LIBRELANE) $(FLOW_PDK) $(LIBRELANE_ARGS) $(CONFIG)

synth: | $(PDK_ROOT)
	$(LIBRELANE) $(FLOW_PDK) --to Checker.YosysSynthChecks $(LIBRELANE_ARGS) $(CONFIG)

$(PDK_ROOT):
	$(CIEL) fetch --pdk-root $(CIEL_HOME) --pdk-family $(PDK) $(PDK_VERSION)

summary:
	@../summary.py $(LATEST)

sign_of_life:
	@../sign_of_life.py $(LATEST)

sign_of_life_rtl:
	@../sign_of_life.py --rtl

chip: | $(PDK_ROOT)
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
