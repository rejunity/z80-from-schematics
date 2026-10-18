# RZ80 -> iCEBreaker (Lattice iCE40UP5K-SG48)
#
# Two bitstreams from one top.v (EXTERNAL_BUS parameter):
#   sbc   BASIC single-board computer, PLL 4 MHz Z80 clock, UART console on
#         the board's USB serial port, PMODs mirror the bus as outputs
#                                                  -> build/top_sbc.bin
#   chip  Z80 chip replacement: the PMODs are the live bus, CLK comes in on
#         P1A10                                    -> build/top_chip.bin
#
#   make venv    create .venv and pip-install the YoWASP toolchain
#   make         both bitstreams;  make sbc / make chip for one
#   make sim     Verilator smoke test of the sbc build (fast serial)
#   make sim_real  same at true 115200 pacing (exercises the TIE path)
#   make sim_tiny  Verilator: the 1K Tiny BASIC ROM, console transcript
#                compared byte for byte with the C model's (add
#                SIM_BAUD=115200 for real pacing)
#   make sim_iverilog  the boot script through iverilog (slow cross-check)
#   make sim_gl  the boot script on the synthesized sbc netlist (iCE40 cell models)
#   make sim_chip  chip build on an external RAM: RTL and synthesized netlist
#   make prog MODE=sbc|chip    write the bitstream to the SPI flash (iceprog)
#   make timing  nextpnr's Fmax and utilisation summary for both builds
#   make pins    PMOD pin directions as placed (reads the routed netlists)
#   make clean   remove build/;  make distclean also removes .venv
#
#   make sbc ROM_HEX=../../tests/basic/tinybasic_1k.hex   Tiny BASIC instead
#
# Synthesis and place & route are pip-installed into .venv: yowasp-yosys
# (synth_ice40) and yowasp-nextpnr-ice40, which also ships yowasp-icepack
# (and yowasp-icepll, used for the PLL settings in top.v). Programming
# needs a native iceprog (IceStorm) or openFPGALoader, see README.md.
#
# Sources are staged into build/ and the tools run from there: the YoWASP
# sandbox only sees the current directory tree, and it also makes top.v's
# ROM_FILE="rom.mem" resolve for yosys, Verilator and vvp alike.

ROOT     := ../..
BUILD    := build
VENV     := .venv
PYTHON   ?= python3

ROM_HEX  ?= $(ROOT)/tests/basic/nascom_basic_4_7_rc2014.hex
# First writable address in the 8 KiB ROM window (sbc). Default: the end
# of the ROM image rounded up to 1 KiB (hex2mem.py --top) -- 8192 = none
# for NASCOM BASIC; 1024 for the 1K Tiny BASIC makes 0x0400-0x1FFF RAM
# (its variables and stack live at 0x0400-0x07FF).
ifeq ($(origin ROM_PROTECT_TOP),undefined)
ROM_PROTECT_TOP := $(shell $(PYTHON) hex2mem.py --top $(ROM_HEX))
endif

# The core is several files: take whatever rtl/ has.
CORE_SRCS := $(wildcard $(ROOT)/rtl/*.v)
CORE_INCS := $(wildcard $(ROOT)/rtl/*.vh)
SOC_SRCS  := top.v uart.v

YOSYS    := $(abspath $(VENV))/bin/yowasp-yosys
NEXTPNR  := $(abspath $(VENV))/bin/yowasp-nextpnr-ice40
ICEPACK  := $(abspath $(VENV))/bin/yowasp-icepack
ICEPROG  ?= iceprog
IVERILOG ?= iverilog
VVP      ?= vvp

MODES := sbc chip
EXTBUS_sbc  := 0
EXTBUS_chip := 1
# Z80 clock constraint per build (MHz): the sbc's own 4 MHz, and the
# fastest external CLK the chip build is checked for.
Z80_MHZ_sbc  := 4
Z80_MHZ_chip ?= 8
MODE ?= sbc
# keep the per-build synth/pnr products (make would delete chained intermediates)
.SECONDARY:
# stem of the pattern rules below without any directory part
STEM = $(notdir $*)

.PHONY: all venv bin sbc chip sim sim_real sim_tiny sim_iverilog sim_gl sim_chip prog timing pins clean distclean FORCE
all: bin

# ---- toolchain venv (created before any tools are installed/used) ----
$(VENV)/.ok:
	$(PYTHON) -m venv $(VENV)
	$(VENV)/bin/pip install --upgrade pip
	$(VENV)/bin/pip install yowasp-yosys yowasp-nextpnr-ice40
	touch $@

venv: $(VENV)/.ok

# ---- staging ----
# make tracks files, not variables: rebuild when the ROM variant changes.
$(BUILD)/variant: FORCE
	@mkdir -p $(BUILD)
	@echo '$(ROM_HEX) $(ROM_PROTECT_TOP)' | cmp -s - $@ || echo '$(ROM_HEX) $(ROM_PROTECT_TOP)' > $@

$(BUILD)/rom.mem: $(ROM_HEX) hex2mem.py $(BUILD)/variant
	@mkdir -p $(BUILD)
	$(PYTHON) hex2mem.py $(ROM_HEX) $@ 8192

$(BUILD)/src: $(CORE_SRCS) $(CORE_INCS) $(SOC_SRCS)
	@rm -rf $(BUILD)/src && mkdir -p $(BUILD)/src
	cp $(CORE_SRCS) $(CORE_INCS) $(SOC_SRCS) $(BUILD)/src/
	@touch $@

# Shared pin constraints + this build's Z80 clock constraint (rewritten
# only when it changes, e.g. `make chip Z80_MHZ_chip=10`).
$(BUILD)/icebreaker_%.pcf: icebreaker.pcf FORCE
	@mkdir -p $(BUILD)
	@{ cat $<; echo; echo "# $(STEM) build: Z80 clock (MHz)"; \
	  echo "set_frequency clk_z80 $(Z80_MHZ_$(STEM))"; } > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@

# ---- synth / pnr / pack ----
# synth_ice40 straight on the RTL (never `proc; async2sync` first: that
# turns every async-reset flop into sync reset + a Q-side mux and hides the
# hold paths from clock-enable inference; on this core it costs ~400 LUT4,
# sbc 3935 -> 4338).
# The core's async-reset flops map onto SB_DFFR/SB_DFFS(E), z80_top's
# falling-edge flops onto SB_DFFN*.
# Console shows pass headers two levels deep (-v 2) plus warnings; the full
# log goes to build/yosys_<build>.log (-l).
YOSYS_QUIET := -v 2 -Q -T
# Known-benign warnings dropped from the console only (the -l log keeps them):
# the core's trace counters and memories, tri-state notes, BRAM map ranges.
YOSYS_FILTER := grep --line-buffered -vE 'port Q\[|Drivers conflicting|Replacing memory|limited support for tri-state|out of bounds on signal|Latch inferred for signal .\\z80_core'

$(BUILD)/top_%.json: $(BUILD)/src $(BUILD)/rom.mem $(BUILD)/variant $(VENV)/.ok
	@rm -f $@
	cd $(BUILD) && $(YOSYS) $(YOSYS_QUIET) -l yosys_$(STEM).log -p \
	  'read_verilog -Isrc src/*.v; hierarchy -top top -chparam EXTERNAL_BUS $(EXTBUS_$(STEM)) -chparam ROM_PROTECT_TOP $(ROM_PROTECT_TOP); synth_ice40 -top top -abc9 -dsp -json top_$(STEM).json' \
	  2>&1 | $(YOSYS_FILTER)
	@test -f $@ || { echo "yosys failed, see $(BUILD)/yosys_$(STEM).log"; exit 1; }

# nextpnr: full log in build/nextpnr_<build>.log, console shows the stage
# lines, placer/router progress (one router row per 5000 arcs) and the
# timing summary. --report writes utilisation + Fmax as JSON, --write the
# routed netlist (`make pins` reads the IO configuration from it).
# nextpnr's own exit status is saved ahead of the console filter, which
# would otherwise replace it: any nextpnr ERROR fails the build and
# removes the .asc.
NEXTPNR_PROGRESS := grep --line-buffered -E '^(Info: (Pack|Promot|Running|Placed|Creating|HeAP|SA |Routing|Router|Max frequency|Program finished|Device util)|Info: +[0-9]*[05]000 \||ERROR|Warning)'

$(BUILD)/top_%.asc: $(BUILD)/top_%.json $(BUILD)/icebreaker_%.pcf
	@rm -f $@
	cd $(BUILD) && { $(NEXTPNR) --up5k --package sg48 --json top_$(STEM).json \
	  --pcf icebreaker_$(STEM).pcf --asc top_$(STEM).asc \
	  --report report_$(STEM).json --write top_$(STEM).pnr.json 2>&1; \
	  echo $$? > nextpnr_$(STEM).rc; } | tee nextpnr_$(STEM).log | $(NEXTPNR_PROGRESS); \
	  rc=$$(cat nextpnr_$(STEM).rc); [ "$$rc" = 0 ] || { rm -f top_$(STEM).asc; \
	  echo "nextpnr failed (exit $$rc), see $(BUILD)/nextpnr_$(STEM).log"; exit 1; }
	@test -f $@

$(BUILD)/top_%.bin: $(BUILD)/top_%.asc
	cd $(BUILD) && $(ICEPACK) top_$(STEM).asc top_$(STEM).bin
	@test -f $@

sbc:  $(BUILD)/top_sbc.bin
chip: $(BUILD)/top_chip.bin
bin:  sbc chip

# Post-route Fmax per clock, then the resources used.
timing: $(BUILD)/top_sbc.asc $(BUILD)/top_chip.asc
	@for m in $(MODES); do echo "== $$m"; \
	  sed -n '/Routing complete/,$$p' $(BUILD)/nextpnr_$$m.log | grep -E 'Max frequency'; \
	  grep -A16 'Device utilisation' $(BUILD)/nextpnr_$$m.log \
	    | grep -E 'ICESTORM_(LC|RAM|SPRAM|PLL):|SB_(IO|GB):'; done

pins: $(BUILD)/top_sbc.asc $(BUILD)/top_chip.asc
	@for m in $(MODES); do echo "== $$m"; \
	  $(PYTHON) pins.py $(BUILD)/top_$$m.pnr.json icebreaker.pcf; done

# ---- simulation ----
# `make sim` (Verilator, fast -- seconds) is the default; the iverilog
# path is the slow cross-check (`make sim_iverilog`).
VERILATOR ?= verilator
# In simulation the PLL is a black box and the testbench clock stands in
# for its 48 MHz output: top.v's real /12 divider makes the 4 MHz Z80
# clock, so CORE_HZ keeps its default and only the baud rate changes.
# Default: fast serial (16 Z80 clks/bit) so framing doesn't dominate sim
# time. `make sim_real` runs at true 115200 -- that pacing is what
# exercises the ROM's interrupt-driven TX (TIE) path.
SIM_BAUD ?= 250000
# One object dir per baud: SIM_BAUD is baked into the model (-G/-D), so
# `make sim_real` after `make sim` must not reuse the fast binary.
OBJ_DIR := $(BUILD)/obj_dir_$(SIM_BAUD)

$(OBJ_DIR)/Vtop: $(BUILD)/src $(BUILD)/variant sim_top.cpp
	rm -rf $(OBJ_DIR)
	cd $(BUILD) && $(VERILATOR) --cc --exe --build -j 4 -O2 \
	  -Wno-fatal -Isrc --Mdir obj_dir_$(SIM_BAUD) --top-module top \
	  -GBAUD=$(SIM_BAUD) -GEXTERNAL_BUS=0 -GROM_PROTECT_TOP=$(ROM_PROTECT_TOP) \
	  -CFLAGS -DSIM_BAUD=$(SIM_BAUD) \
	  src/*.v ../sim_top.cpp > verilator.log 2>&1 || (tail -20 verilator.log; exit 1)

sim: $(OBJ_DIR)/Vtop $(BUILD)/rom.mem
	cd $(BUILD) && ./obj_dir_$(SIM_BAUD)/Vtop

sim_real:
	$(MAKE) sim SIM_BAUD=115200

# Tiny BASIC: its own ROM image and model (RAM from 0x0400), whatever
# ROM_HEX says; sim_top.cpp's `tiny` script.
TINY_HEX := $(ROOT)/tests/basic/tinybasic_1k.hex
TINY_DIR := obj_dir_tiny_$(SIM_BAUD)

$(BUILD)/rom_tiny.mem: $(TINY_HEX) hex2mem.py
	@mkdir -p $(BUILD)
	$(PYTHON) hex2mem.py $< $@ 8192

$(BUILD)/$(TINY_DIR)/Vtop: $(BUILD)/src sim_top.cpp hex2mem.py
	rm -rf $(BUILD)/$(TINY_DIR)
	cd $(BUILD) && $(VERILATOR) --cc --exe --build -j 4 -O2 \
	  -Wno-fatal -Isrc --Mdir $(TINY_DIR) --top-module top \
	  -GBAUD=$(SIM_BAUD) -GEXTERNAL_BUS=0 -GROM_FILE='"rom_tiny.mem"' \
	  -GROM_PROTECT_TOP=$$($(PYTHON) ../hex2mem.py --top ../$(TINY_HEX)) \
	  -CFLAGS -DSIM_BAUD=$(SIM_BAUD) \
	  src/*.v ../sim_top.cpp > verilator_tiny.log 2>&1 || (tail -20 verilator_tiny.log; exit 1)

sim_tiny: $(BUILD)/$(TINY_DIR)/Vtop $(BUILD)/rom_tiny.mem
	cd $(BUILD) && ./$(TINY_DIR)/Vtop tiny

$(BUILD)/tb_top.vvp: $(BUILD)/src $(BUILD)/rom.mem tb_top.v
	$(IVERILOG) -g2012 -I$(BUILD)/src -s tb_top -o $@ $(BUILD)/src/*.v tb_top.v

sim_iverilog: $(BUILD)/tb_top.vvp
	cd $(BUILD) && $(VVP) tb_top.vvp

# ---- gate-level simulation: the synthesized netlists ----
# What synth_ice40 made of the design -- the EBR ROM image, SPRAM byte
# lanes, negedge flops, the chip build's SB_IO / SB_GB_IO pads -- run on
# yosys' own iCE40 cell models (cells_sim.v from the venv). Those have no
# behavioural PLL; sim_pll.v stands in (testbench clock = PLL output, as
# in the RTL sims).
YOSYS_SHARE = $(shell $(VENV)/bin/python -c 'import os, yowasp_yosys; print(os.path.dirname(yowasp_yosys.__file__))')/share

$(BUILD)/gl/cells_sim.v: $(VENV)/.ok
	@mkdir -p $(BUILD)/gl
	awk '/^module SB_PLL40_2_PAD/{skip=1} !skip{print} /^endmodule/{skip=0}' \
	  $(YOSYS_SHARE)/ice40/cells_sim.v > $@

$(BUILD)/gl/top_%_gl.v: $(BUILD)/top_%.json
	@mkdir -p $(BUILD)/gl
	cd $(BUILD) && $(YOSYS) -q -p 'read_json top_$(STEM).json; write_verilog -noattr gl/top_$(STEM)_gl.v'

# sbc: the BASIC boot script on the netlist (Verilator). The netlist has
# BAUD = 115200 baked in, so this runs at real serial pacing.
$(BUILD)/gl/obj/Vtop: $(BUILD)/gl/top_sbc_gl.v $(BUILD)/gl/cells_sim.v sim_pll.v sim_top.cpp
	rm -rf $(BUILD)/gl/obj
	cd $(BUILD)/gl && $(VERILATOR) --cc --exe --build -j 4 -O1 \
	  -Wno-fatal -Wno-lint -Wno-style --no-timing -DNO_ICE40_DEFAULT_ASSIGNMENTS \
	  --top-module top --Mdir obj -CFLAGS -DSIM_BAUD=115200 \
	  cells_sim.v ../../sim_pll.v top_sbc_gl.v ../../sim_top.cpp \
	  > verilator.log 2>&1 || (tail -20 verilator.log; exit 1)

sim_gl: $(BUILD)/gl/obj/Vtop
	cd $(BUILD)/gl && ./obj/Vtop

# chip: tb_chip.v (external 4 KiB RAM + CLK on the PMOD pins), on the RTL
# and on the netlist (iverilog: the pads are real tristates there).
$(BUILD)/tb_chip_rtl.vvp: $(BUILD)/src tb_chip.v
	$(IVERILOG) -g2012 -I$(BUILD)/src -s tb_chip -o $@ $(BUILD)/src/*.v tb_chip.v

$(BUILD)/gl/tb_chip_gl.vvp: $(BUILD)/gl/top_chip_gl.v $(BUILD)/gl/cells_sim.v tb_chip.v
	$(IVERILOG) -g2012 -DNO_ICE40_DEFAULT_ASSIGNMENTS -s tb_chip -o $@ \
	  $(BUILD)/gl/cells_sim.v $(BUILD)/gl/top_chip_gl.v tb_chip.v

sim_chip: $(BUILD)/tb_chip_rtl.vvp $(BUILD)/gl/tb_chip_gl.vvp
	@echo "== chip build, RTL"
	@$(VVP) -n $(BUILD)/tb_chip_rtl.vvp
	@echo "== chip build, synthesized netlist"
	@$(VVP) -n $(BUILD)/gl/tb_chip_gl.vvp

# ---- board ----
# iceprog (native IceStorm) writes the SPI flash over the FT2232H's
# channel A; the FPGA reconfigures from it straight away and on every
# power-up. openFPGALoader can be used instead:
#   make prog ICEPROG="openFPGALoader -b ice40_generic -f"
prog: $(BUILD)/top_$(MODE).bin
	$(ICEPROG) $(BUILD)/top_$(MODE).bin

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(VENV)
