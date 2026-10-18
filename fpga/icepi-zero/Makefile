# RZ80 -> IcePi Zero (Lattice ECP5 LFE5U-25F, CABGA256)
#
# Two bitstreams from one top.v (EXTERNAL_BUS parameter):
#   sbc   BASIC single-board computer, internal 4 MHz clock, USB-UART
#         console, the header mirrors the Z80 bus      -> build/top_sbc.bit
#   chip  the Z80 as a chip on the header, CLK in on header pin 22
#                                                      -> build/top_chip.bit
#
#   make venv    create .venv and pip-install the YoWASP FPGA toolchain
#   make         both bitstreams (= make bit);  make sbc / make chip for one
#   make sim     Verilator smoke test of the sbc build: boot BASIC, answer
#                prompts over UART
#   make sim_real  same at true 115200 pacing (exercises the TIE path)
#   make sim_tiny  Verilator: the 1K Tiny BASIC ROM, console transcript
#                compared byte for byte with the C model's (add
#                SIM_BAUD=115200 for real pacing)
#   make sim_iverilog  the boot script through iverilog (slow cross-check)
#   make prog MODE=sbc|chip    load bitstream to SRAM (lost on power-cycle)
#   make flash MODE=sbc|chip   write bitstream to SPI flash (persistent)
#   make timing  show nextpnr's max-frequency summary for both builds
#   make clean   remove build/;  make distclean also removes .venv
#
#   make sbc ROM_HEX=../../tests/basic/tinybasic_1k.hex   Tiny BASIC instead
#
# Synthesis tools (yowasp-yosys / yowasp-nextpnr-ecp5 / yowasp-ecppack) are
# WASM builds installed by pip into .venv -- no system packages needed.
# Flashing needs a native openFPGALoader (brew install openfpgaloader).
#
# All sources are staged into build/ and the tools run from there: the
# YoWASP sandbox can only see the current directory tree, and it also
# makes top.v's ROM_FILE="rom.mem" resolve for yosys and vvp alike.

ROOT     := ../..
BUILD    := build
VENV     := .venv
PYTHON   ?= python3

ROM_HEX  ?= $(ROOT)/tests/basic/nascom_basic_4_7_rc2014.hex
# Writes below ROM_PROTECT_TOP are dropped (the ROM). Default: the end of
# the ROM image rounded up to 1 KiB (hex2mem.py --top) -- 8192 for NASCOM
# BASIC, 1024 for the 1K Tiny BASIC, which keeps its variables and stack
# at 0x0400-0x07FF.
ifeq ($(origin ROM_PROTECT_TOP),undefined)
ROM_PROTECT_TOP := $(shell $(PYTHON) hex2mem.py --top $(ROM_HEX))
endif

CORE_SRCS := $(wildcard $(ROOT)/rtl/*.v)
CORE_INCS := $(wildcard $(ROOT)/rtl/*.vh)
SOC_SRCS  := top.v uart.v

STAGED_SRCS := $(addprefix $(BUILD)/src/,$(notdir $(CORE_SRCS) $(SOC_SRCS)))

YOSYS    := $(abspath $(VENV))/bin/yowasp-yosys
NEXTPNR  := $(abspath $(VENV))/bin/yowasp-nextpnr-ecp5
ECPPACK  := $(abspath $(VENV))/bin/yowasp-ecppack
OPENFPGALOADER ?= openFPGALoader
IVERILOG ?= iverilog
VVP      ?= vvp

FPGA_SIZE := 25k
PACKAGE   := CABGA256

MODES := sbc chip
EXTBUS_sbc  := 0
EXTBUS_chip := 1
MODE ?= sbc
# keep the per-build synth / pnr results (make would delete chained intermediates)
.SECONDARY:
# stem of the pattern rules below without any directory part
STEM = $(notdir $*)

.PHONY: all venv bit sbc chip sim sim_real sim_tiny sim_iverilog prog flash timing clean distclean FORCE
all: bit

# ---- toolchain venv (created before any tools are installed/used) ----
$(VENV)/.ok:
	$(PYTHON) -m venv $(VENV)
	$(VENV)/bin/pip install --upgrade pip
	$(VENV)/bin/pip install yowasp-yosys yowasp-nextpnr-ecp5
	touch $@

venv: $(VENV)/.ok

# ---- staging ----
# build/rom.cfg changes only when ROM_HEX or ROM_PROTECT_TOP does, so a
# different ROM on the command line rebuilds the image and the bitstreams.
ROM_CFG := $(abspath $(ROM_HEX)) $(ROM_PROTECT_TOP)
$(BUILD)/rom.cfg: FORCE
	@mkdir -p $(BUILD)
	@echo '$(ROM_CFG)' | cmp -s - $@ || echo '$(ROM_CFG)' > $@
FORCE:

$(BUILD)/rom.mem: $(ROM_HEX) hex2mem.py $(BUILD)/rom.cfg
	@mkdir -p $(BUILD)
	$(PYTHON) hex2mem.py $< $@

$(BUILD)/src: $(CORE_SRCS) $(CORE_INCS) $(SOC_SRCS)
	@mkdir -p $(BUILD)/src
	cp $(CORE_SRCS) $(CORE_INCS) $(SOC_SRCS) $(BUILD)/src/
	@touch $@

$(BUILD)/%.lpf: %.lpf
	@mkdir -p $(BUILD)
	cp $< $@

# ---- synth / pnr / pack ----
# Console shows pass headers two levels deep (-v 2) plus warnings; the full
# log goes to build/yosys.log (-l).
YOSYS_QUIET := -v 2 -Q -T
# Known-benign warnings dropped from the console only (the -l log keeps them):
# the core's trace counters and memories, tri-state notes, BRAM map ranges.
YOSYS_FILTER := grep --line-buffered -vE 'port Q\[|Drivers conflicting|Replacing memory|limited support for tri-state|out of bounds on signal|Latch inferred for signal .\\z80_core'

$(BUILD)/top_%.json: $(BUILD)/src $(BUILD)/rom.mem $(VENV)/.ok
	@rm -f $@
	cd $(BUILD) && $(YOSYS) $(YOSYS_QUIET) -l yosys_$(STEM).log -p \
	  'read_verilog -Isrc src/*.v; hierarchy -top top -chparam EXTERNAL_BUS $(EXTBUS_$(STEM)) -chparam ROM_PROTECT_TOP $(ROM_PROTECT_TOP); synth_ecp5 -top top -json top_$(STEM).json' \
	  2>&1 | $(YOSYS_FILTER)
	@test -f $@ || { echo "yosys failed, see $(BUILD)/yosys_$(STEM).log"; exit 1; }

# nextpnr: full log in build/nextpnr_<build>.log, console shows the stage
# lines, placer/router progress (one router row per 5000 arcs) and the
# timing summary. nextpnr's own exit status is saved ahead of that console
# filter, which would otherwise replace it: any nextpnr ERROR fails the
# build and removes its output.
NEXTPNR_PROGRESS := grep --line-buffered -E '^(Info: (Pack|Running|Placed|Creating|HeAP|SA |Routing|Router|Max frequency|Program finished|Device util|promoting)|Info: +[0-9]*[05]000 \||ERROR|Warning)'

# the chip build adds the external-CLK constraint (icepi-zero-chip.lpf)
LPF_sbc  := icepi-zero.lpf
LPF_chip := icepi-zero.lpf icepi-zero-chip.lpf

$(BUILD)/top_%.config: $(BUILD)/top_%.json $(BUILD)/icepi-zero.lpf $(BUILD)/icepi-zero-chip.lpf
	@rm -f $@
	cd $(BUILD) && { $(NEXTPNR) --$(FPGA_SIZE) --package $(PACKAGE) \
	  $(addprefix --lpf ,$(LPF_$(STEM))) --json top_$(STEM).json --textcfg top_$(STEM).config 2>&1; \
	  echo $$? > nextpnr_$(STEM).rc; } | tee nextpnr_$(STEM).log | $(NEXTPNR_PROGRESS); \
	  rc=$$(cat nextpnr_$(STEM).rc); [ "$$rc" = 0 ] || { rm -f top_$(STEM).config; \
	  echo "nextpnr failed (exit $$rc), see $(BUILD)/nextpnr_$(STEM).log"; exit 1; }
	@test -f $@

$(BUILD)/top_%.bit: $(BUILD)/top_%.config
	cd $(BUILD) && $(ECPPACK) --compress top_$(STEM).config top_$(STEM).bit

sbc:  $(BUILD)/top_sbc.bit
chip: $(BUILD)/top_chip.bit
bit:  sbc chip

timing:
	@for m in $(MODES); do echo "== $$m"; grep -E "Max frequency" $(BUILD)/nextpnr_$$m.log | tail -3; done

# ---- simulation ----
# `make sim` (Verilator, fast -- seconds) is the default; the iverilog
# path is kept as the slow cross-check (`make sim_iverilog`).
VERILATOR ?= verilator
# Simulation Z80 clock is the /12 stand-in for the PLL: 4.1667 MHz, so
# -GCORE_HZ overrides the UART divisors to match.
# Default: fast serial (16 Z80 clks/bit) so framing doesn't dominate sim
# time. `make sim_real` runs at true 115200 -- that pacing is what
# exercises the ROM's interrupt-driven TX (TIE) path.
SIM_CORE_HZ := 4166667
SIM_BAUD  ?= 260417

# One object dir per baud: SIM_BAUD is baked into the model (-G/-D), so
# `make sim_real` after `make sim` must not reuse the fast binary.
OBJ_DIR := $(BUILD)/obj_dir_$(SIM_BAUD)

$(OBJ_DIR)/Vtop: $(BUILD)/src sim_top.cpp
	rm -rf $(OBJ_DIR)
	cd $(BUILD) && $(VERILATOR) --cc --exe --build -j 4 -O2 \
	  -Wno-fatal -Isrc --Mdir obj_dir_$(SIM_BAUD) --top-module top \
	  -GCORE_HZ=$(SIM_CORE_HZ) -GBAUD=$(SIM_BAUD) -CFLAGS -DSIM_BAUD=$(SIM_BAUD) \
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
	$(PYTHON) hex2mem.py $< $@

$(BUILD)/$(TINY_DIR)/Vtop: $(BUILD)/src sim_top.cpp hex2mem.py
	rm -rf $(BUILD)/$(TINY_DIR)
	cd $(BUILD) && $(VERILATOR) --cc --exe --build -j 4 -O2 \
	  -Wno-fatal -Isrc --Mdir $(TINY_DIR) --top-module top \
	  -GCORE_HZ=$(SIM_CORE_HZ) -GBAUD=$(SIM_BAUD) -CFLAGS -DSIM_BAUD=$(SIM_BAUD) \
	  -GROM_FILE='"rom_tiny.mem"' \
	  -GROM_PROTECT_TOP=$$($(PYTHON) ../hex2mem.py --top ../$(TINY_HEX)) \
	  src/*.v ../sim_top.cpp > verilator_tiny.log 2>&1 || (tail -20 verilator_tiny.log; exit 1)

sim_tiny: $(BUILD)/$(TINY_DIR)/Vtop $(BUILD)/rom_tiny.mem
	cd $(BUILD) && ./$(TINY_DIR)/Vtop tiny

$(BUILD)/tb_top.vvp: $(BUILD)/src $(BUILD)/rom.mem tb_top.v
	$(IVERILOG) -g2012 -I$(BUILD)/src -o $@ \
	  $(addprefix $(BUILD)/src/,$(notdir $(CORE_SRCS) $(SOC_SRCS))) tb_top.v

sim_iverilog: $(BUILD)/tb_top.vvp
	cd $(BUILD) && $(VVP) tb_top.vvp

# ---- board ----
prog: $(BUILD)/top_$(MODE).bit
	$(OPENFPGALOADER) -b icepi-zero $(BUILD)/top_$(MODE).bit

flash: $(BUILD)/top_$(MODE).bit
	$(OPENFPGALOADER) -b icepi-zero $(BUILD)/top_$(MODE).bit --write-flash

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(VENV)
