# Harden z80_top on this branch's PDK with LibreLane (Nix install, see
# README.md). `make` runs the full Classic flow (RTL -> GDS, no pad ring);
# `make summary` prints area / timing / sign-off numbers of the latest run
# and fails if a sign-off check does; `make sign_of_life` runs the
# signs-of-life program on the latest run's routed netlist (RTL version:
# `make sign_of_life_rtl`).
PDK          := sky130A
SCL          := sky130_fd_sc_hd
CONFIG       := config.yaml
LIBRELANE    ?= librelane
LIBRELANE_ARGS ?=

LATEST = $(shell ls -dt runs/*/ 2>/dev/null | head -1)

.PHONY: harden synth summary sign_of_life sign_of_life_rtl clean
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

clean:
	rm -rf runs
