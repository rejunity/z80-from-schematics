# RZ80 -> IcePi Zero (Lattice ECP5 LFE5U-25F, CABGA256)
#
#   make venv    create .venv and pip-install the YoWASP FPGA toolchain
#   make bit     synthesize + place&route + pack -> build/top.bit  (default)
#   make sim     Verilator smoke test: boot BASIC, answer prompts over UART
#   make sim_real  same at true 115200 pacing (exercises the TIE path)
#   make sim_iverilog  the boot script through iverilog (slow cross-check)
#   make prog    load bitstream to SRAM (lost on power-cycle)
#   make flash   write bitstream to SPI flash (persistent)
#   make timing  show nextpnr's max-frequency summary
#   make clean   remove build/;  make distclean also removes .venv
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

CORE_SRCS := $(ROOT)/rtl/z80_top.v  $(ROOT)/rtl/z80_core.v $(ROOT)/rtl/z80_pla.v \
             $(ROOT)/rtl/z80_alu.v  $(ROOT)/rtl/z80_timing.v
CORE_INCS := $(ROOT)/rtl/z80_defs.vh
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

.PHONY: all venv bit sim sim_iverilog prog flash timing clean distclean
all: bit

# ---- toolchain venv (created before any tools are installed/used) ----
$(VENV)/.ok:
	$(PYTHON) -m venv $(VENV)
	$(VENV)/bin/pip install --upgrade pip
	$(VENV)/bin/pip install yowasp-yosys yowasp-nextpnr-ecp5
	touch $@

venv: $(VENV)/.ok

# ---- staging ----
$(BUILD)/rom.mem: $(ROM_HEX) hex2mem.py
	@mkdir -p $(BUILD)
	$(PYTHON) hex2mem.py $< $@

$(BUILD)/src: $(CORE_SRCS) $(CORE_INCS) $(SOC_SRCS)
	@mkdir -p $(BUILD)/src
	cp $(CORE_SRCS) $(CORE_INCS) $(SOC_SRCS) $(BUILD)/src/
	@touch $@

$(BUILD)/icepi-zero.lpf: icepi-zero.lpf
	@mkdir -p $(BUILD)
	cp $< $@

# ---- synth / pnr / pack ----
# Console shows pass headers two levels deep (-v 2) plus warnings; the full
# log goes to build/yosys.log (-l). Known-benign warnings are demoted (-w):
# the core's trace counters and memories, tri-state notes, BRAM map ranges.
YOSYS_QUIET := -v 2 -Q -T \
  -w 'Replacing memory|limited support for tri-state|Drivers conflicting|out of bounds on signal'

$(BUILD)/top.json: $(BUILD)/src $(BUILD)/rom.mem $(VENV)/.ok
	cd $(BUILD) && $(YOSYS) $(YOSYS_QUIET) -l yosys.log -p \
	  'read_verilog -Isrc src/z80_top.v src/z80_core.v src/z80_pla.v src/z80_alu.v src/z80_timing.v src/top.v src/uart.v; hierarchy -top top; proc; async2sync; synth_ecp5 -top top -json top.json'

# nextpnr: full log in build/nextpnr.log, console shows the stage lines,
# placer/router progress and the timing summary.
NEXTPNR_PROGRESS := grep --line-buffered -E '^(Info: (Pack|Running|Placed|Creating|HeAP|SA |Routing|Router|Max frequency|Program finished|Device util|promoting)|Info: +[0-9]+000 \||ERROR|Warning)'

$(BUILD)/top.config: $(BUILD)/top.json $(BUILD)/icepi-zero.lpf
	cd $(BUILD) && $(NEXTPNR) --$(FPGA_SIZE) --package $(PACKAGE) \
	  --lpf icepi-zero.lpf --json top.json --textcfg top.config \
	  2>&1 | tee nextpnr.log | $(NEXTPNR_PROGRESS) || true
	@test -f $(BUILD)/top.config

$(BUILD)/top.bit: $(BUILD)/top.config
	cd $(BUILD) && $(ECPPACK) --compress top.config top.bit

bit: $(BUILD)/top.bit

timing: $(BUILD)/top.config
	@grep -E "Max frequency" $(BUILD)/nextpnr.log | tail -4

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
	  src/top.v src/uart.v src/z80_top.v src/z80_core.v src/z80_pla.v src/z80_alu.v src/z80_timing.v \
	  ../sim_top.cpp > verilator.log 2>&1 || (tail -20 verilator.log; exit 1)

sim: $(OBJ_DIR)/Vtop $(BUILD)/rom.mem
	cd $(BUILD) && ./obj_dir_$(SIM_BAUD)/Vtop

sim_real:
	$(MAKE) sim SIM_BAUD=115200

$(BUILD)/tb_top.vvp: $(BUILD)/src $(BUILD)/rom.mem tb_top.v
	$(IVERILOG) -g2012 -I$(BUILD)/src -o $@ \
	  $(addprefix $(BUILD)/src/,$(notdir $(CORE_SRCS) $(SOC_SRCS))) tb_top.v

sim_iverilog: $(BUILD)/tb_top.vvp
	cd $(BUILD) && $(VVP) tb_top.vvp

# ---- board ----
prog: $(BUILD)/top.bit
	$(OPENFPGALOADER) -b icepi-zero $(BUILD)/top.bit

flash: $(BUILD)/top.bit
	$(OPENFPGALOADER) -b icepi-zero $(BUILD)/top.bit --write-flash

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(VENV)
