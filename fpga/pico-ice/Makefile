# RZ80 -> pico-ice (tinyVision.ai: Lattice iCE40UP5K-SG48 + RP2040)
#
# Two bitstreams from one top.v (EXTERNAL_BUS parameter):
#   sbc   BASIC single-board computer, internal 4 MHz clock, UART console
#         through the RP2040's USB-UART, headers mirror the Z80 bus as
#         outputs                                  -> build/top_sbc.bin
#   chip  Z80 chip replacement: the headers are the live bus, CLK comes in
#         on Pmod 1 pin 10 (ICE44)                 -> build/top_chip.bin
#
#   make venv    create .venv and pip-install the toolchain
#   make         both bitstreams;  make sbc / make chip for one
#   make sim     Verilator smoke test of the sbc build (fast serial)
#   make sim_real  same at the hardware's serial pacing (exercises the TIE path)
#   make sim_tiny  Verilator: the 1K Tiny BASIC ROM, console transcript
#                compared byte for byte with the C model's (add
#                SIM_BAUD=115200 for the hardware's pacing)
#   make sim_iverilog  the boot script through iverilog (slow cross-check)
#   make sim_uart  uart.v at 4 and 3.5 MHz against BAUD: TX edges, RX at +-3 %
#   make sim_chip  iverilog smoke test of the chip build: an external clock,
#                memory and output port on the header pins run a program
#   make sim_pnr the boot script on the routed sbc netlist, as packed into
#                the bitstream (sim_pnr_tiny: Tiny BASIC on build_tiny/'s)
#   make prog MODE=sbc|chip    load into FPGA CRAM over USB DFU (volatile)
#   make flash MODE=sbc|chip   write the FPGA's config flash over USB DFU
#   make uf2 MODE=sbc|chip [UF2_DRIVE=/media/$USER/pico-ice]
#                build/top_<mode>.uf2 for the `pico-ice` USB drive (and copy it)
#   make timing  post-route Fmax per clock and I/O delays, both builds
#   make ci      everything CI runs (fpga-board.yml): both bitstreams, the
#                Tiny BASIC bitstream in build_tiny/ and every simulation
#   make clean   remove build/ and build_tiny/;  make distclean also .venv
#
#   make sbc ROM_HEX=../../tests/basic/tinybasic_1k.hex   Tiny BASIC instead
#
# Synthesis and P&R are pip-installed into .venv: yowasp-yosys
# (synth_ice40) and yowasp-nextpnr-ice40, which also ships the IceStorm
# tools (yowasp-icepack, yowasp-icepll). Programming goes through the
# pico-ice's stock RP2040 firmware: native dfu-util (`prog`, `flash`), or
# no tool at all -- copy the UF2 onto the board's USB drive (`uf2`).
#
# Sources are staged into build/ and the tools run from there: the YoWASP
# sandbox can only see the current directory tree, and it also makes
# top.v's ROM_FILE="rom.mem" resolve for yosys, Verilator and vvp alike.

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

# The core is several files (z80_top + the die's blocks + headers): take them all.
CORE_SRCS := $(wildcard $(ROOT)/rtl/*.v)
CORE_INCS := $(wildcard $(ROOT)/rtl/*.vh)
SOC_SRCS  := top.v uart.v
SIM_SRCS  := $(addprefix src/,$(notdir $(SOC_SRCS) $(CORE_SRCS)))

YOSYS    := $(abspath $(VENV))/bin/yowasp-yosys
NEXTPNR  := $(abspath $(VENV))/bin/yowasp-nextpnr-ice40
ICEPACK  := $(abspath $(VENV))/bin/yowasp-icepack
DFU_UTIL ?= dfu-util
IVERILOG ?= iverilog
VVP      ?= vvp

# pico-ice DFU device (stock firmware): alt 0 = config flash, alt 1 = CRAM
DFU_DEV  := 1209:b1c0

MODES := sbc chip
EXTBUS_sbc  := 0
EXTBUS_chip := 1
# Z80 clock constraint per build (MHz): sbc runs at 4; the chip build's
# external CLK is constrained at 8 to leave margin over a 4-6 MHz Z80 bus.
ZCLK_MHZ_sbc  := 4
ZCLK_MHZ_chip := 8
MODE ?= sbc
# keep the per-build synth/pnr netlists (make would delete chained intermediates)
.SECONDARY:
# stem of the pattern rules below without any directory part
STEM = $(notdir $*)

.PHONY: all venv bin sbc chip sim sim_real sim_tiny sim_iverilog sim_chip sim_pnr sim_pnr_tiny prog flash uf2 timing sim_uart ci clean distclean FORCE
all: bin

# ---- toolchain venv (created before any tools are installed/used) ----
$(VENV)/.ok:
	$(PYTHON) -m venv $(VENV)
	$(VENV)/bin/pip install --upgrade pip
	$(VENV)/bin/pip install yowasp-yosys yowasp-nextpnr-ice40
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

# ROM: 8 KiB block RAM at 0x0000-0x1FFF, so an 8192-line image.
$(BUILD)/rom.mem: $(ROM_HEX) hex2mem.py $(BUILD)/rom.cfg
	@mkdir -p $(BUILD)
	$(PYTHON) hex2mem.py $< $@ 8192

$(BUILD)/src: $(CORE_SRCS) $(CORE_INCS) $(SOC_SRCS)
	@mkdir -p $(BUILD)/src
	cp $(CORE_SRCS) $(CORE_INCS) $(SOC_SRCS) $(BUILD)/src/
	@touch $@

# pin constraints + this build's Z80 clock constraint
$(BUILD)/pico-ice_%.pcf: pico-ice.pcf Makefile
	@mkdir -p $(BUILD)
	{ cat $<; echo "set_frequency clk_z80 $(ZCLK_MHZ_$(STEM))"; } > $@

# ---- synth / pnr / pack ----
# No `proc; async2sync` before synth_ice40: the core's async-reset flops
# map straight onto SB_DFFR/SB_DFFS (and z80_top's falling-edge flops onto
# SB_DFFN*). async2sync turns every flop into sync-reset plus a Q-side mux,
# which hides the hold paths from clock-enable inference and costs
# thousands of LUTs -- the UP5K's 5,280 would not hold the core.
# Console shows pass headers two levels deep (-v 2) plus warnings; the full
# log goes to build/yosys_<build>.log (-l).
YOSYS_QUIET := -v 2 -Q -T
# Known-benign warnings dropped from the console only (the -l log keeps them):
# the core's trace counters and memories, tri-state notes, BRAM map ranges.
YOSYS_FILTER := grep --line-buffered -vE 'port Q\[|Drivers conflicting|Replacing memory|limited support for tri-state|out of bounds on signal|Latch inferred for signal .\\z80_core'

$(BUILD)/top_%.json: $(BUILD)/src $(BUILD)/rom.mem $(VENV)/.ok
	@rm -f $@
	cd $(BUILD) && $(YOSYS) $(YOSYS_QUIET) -l yosys_$(STEM).log -p \
	  'read_verilog -Isrc src/*.v; hierarchy -top top -chparam EXTERNAL_BUS $(EXTBUS_$(STEM)) -chparam ROM_PROTECT_TOP $(ROM_PROTECT_TOP); synth_ice40 -top top -abc9 -dsp -json top_$(STEM).json' \
	  2>&1 | $(YOSYS_FILTER)
	@test -f $@ || { echo "yosys failed, see $(BUILD)/yosys_$(STEM).log"; exit 1; }

# nextpnr: full log in build/nextpnr_<build>.log, console shows the clock
# constraints (set and PLL-derived), the main utilisation rows, placer and
# router progress (one router row per 5000 arcs) and the timing summary --
# about 30 lines. --write saves the routed netlist (`make sim_pnr`).
# nextpnr fails the build if a clock misses its constraint; its own exit
# status is saved ahead of the console filter, which would otherwise
# replace it, so any nextpnr ERROR fails the build and removes the .asc
# and the routed netlist.
NEXTPNR_PROGRESS := grep --line-buffered -E '^(Info: (constraining clock|HeAP Placer Time|SA placement time|Routing [0-9]|Routing complete|Router1 time|Max frequency|Program finished)|Info: +Derived frequency|Info:[[:space:]]+(ICESTORM_(LC|RAM|SPRAM|PLL)|SB_IO|SB_GB):|Info: +[0-9]*[05]000 \||ERROR|Warning)'

$(BUILD)/top_%.asc $(BUILD)/top_%.pnr.json: $(BUILD)/top_%.json $(BUILD)/pico-ice_%.pcf
	@rm -f $(BUILD)/top_$(STEM).asc $(BUILD)/top_$(STEM).pnr.json
	cd $(BUILD) && { $(NEXTPNR) --up5k --package sg48 --pcf pico-ice_$(STEM).pcf \
	  --json top_$(STEM).json --asc top_$(STEM).asc --report report_$(STEM).json \
	  --write top_$(STEM).pnr.json 2>&1; \
	  echo $$? > nextpnr_$(STEM).rc; } | tee nextpnr_$(STEM).log | $(NEXTPNR_PROGRESS); \
	  rc=$$(cat nextpnr_$(STEM).rc); [ "$$rc" = 0 ] || { rm -f top_$(STEM).asc top_$(STEM).pnr.json; \
	  echo "nextpnr failed (exit $$rc), see $(BUILD)/nextpnr_$(STEM).log"; exit 1; }
	@test -f $@

$(BUILD)/top_%.bin: $(BUILD)/top_%.asc
	cd $(BUILD) && $(ICEPACK) top_$(STEM).asc top_$(STEM).bin

$(BUILD)/top_%.uf2: $(BUILD)/top_%.bin bin2uf2.py
	$(PYTHON) bin2uf2.py $< $@

sbc:  $(BUILD)/top_sbc.bin
chip: $(BUILD)/top_chip.bin
bin:  sbc chip

# Post-route figures: Fmax per clock, then the unconstrained I/O and
# cross-domain delays (in the chip build these are the header timings:
# pad -> posedge/negedge clk_z80 = data setup, clk_z80 -> pad = clock to out).
timing: $(BUILD)/top_sbc.asc $(BUILD)/top_chip.asc
	@for m in $(MODES); do echo "== $$m"; sed -n '/Routing complete/,$$p' $(BUILD)/nextpnr_$$m.log | grep -E 'Max (frequency|delay)'; done

# ---- simulation ----
# `make sim` (Verilator, fast -- seconds) is the default; the iverilog
# path is kept as the slow cross-check (`make sim_iverilog`).
VERILATOR ?= verilator
# In simulation the PLL is a black box and its output is replaced by the
# 12 MHz input (x1), so the same /6 divider gives a 2 MHz Z80 clock;
# -GCORE_HZ overrides the UART divisors to match. Default: fast serial
# (16 Z80 clks/bit) so framing doesn't dominate sim time. `make sim_real`
# uses the hardware's pacing instead -- 17.36 Z80 clocks per bit, i.e.
# 230400 baud at 4 MHz, which is 115200 baud at the sim's 2 MHz -- and that pacing
# is what exercises the ROM's interrupt-driven TX (TIE) path. (make keeps
# trailing spaces, so no inline comments on these lines.)
SIM_CORE_HZ := 2000000
SIM_BAUD  ?= 125000
SIM_BAUD_REAL := 115200
# One object dir per baud: SIM_BAUD is baked into the model (-G/-D), so
# `make sim_real` after `make sim` must not reuse the fast binary.
OBJ_DIR   := $(BUILD)/obj_dir_$(SIM_BAUD)

$(OBJ_DIR)/Vtop: $(BUILD)/src sim_top.cpp
	rm -rf $(OBJ_DIR)
	cd $(BUILD) && $(VERILATOR) --cc --exe --build -j 4 -O2 \
	  -Wno-fatal -Isrc --Mdir obj_dir_$(SIM_BAUD) --top-module top \
	  -GCORE_HZ=$(SIM_CORE_HZ) -GBAUD=$(SIM_BAUD) -GEXTERNAL_BUS=0 \
	  -CFLAGS -DSIM_BAUD=$(SIM_BAUD) \
	  $(SIM_SRCS) ../sim_top.cpp > verilator.log 2>&1 || (tail -20 verilator.log; exit 1)

sim: $(OBJ_DIR)/Vtop $(BUILD)/rom.mem
	cd $(BUILD) && ./obj_dir_$(SIM_BAUD)/Vtop

sim_real:
	$(MAKE) sim SIM_BAUD=$(SIM_BAUD_REAL)

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
	  -GCORE_HZ=$(SIM_CORE_HZ) -GBAUD=$(SIM_BAUD) -GEXTERNAL_BUS=0 \
	  -GROM_FILE='"rom_tiny.mem"' \
	  -GROM_PROTECT_TOP=$$($(PYTHON) ../hex2mem.py --top ../$(TINY_HEX)) \
	  -CFLAGS -DSIM_BAUD=$(SIM_BAUD) \
	  $(SIM_SRCS) ../sim_top.cpp > verilator_tiny.log 2>&1 || (tail -20 verilator_tiny.log; exit 1)

sim_tiny: $(BUILD)/$(TINY_DIR)/Vtop $(BUILD)/rom_tiny.mem
	cd $(BUILD) && ./$(TINY_DIR)/Vtop tiny

$(BUILD)/tb_top.vvp: $(BUILD)/src $(BUILD)/rom.mem tb_top.v
	cd $(BUILD) && $(IVERILOG) -g2012 -Isrc -o tb_top.vvp $(SIM_SRCS) ../tb_top.v

sim_iverilog: $(BUILD)/tb_top.vvp
	cd $(BUILD) && $(VVP) tb_top.vvp

# chip build (EXTERNAL_BUS=1, behavioural pads): the testbench is the
# external Z80 system on the header pins (see tb_chip.v).
$(BUILD)/tb_chip.vvp: $(BUILD)/src tb_chip.v
	cd $(BUILD) && $(IVERILOG) -g2012 -Isrc -o tb_chip.vvp $(SIM_SRCS) ../tb_chip.v

sim_chip: $(BUILD)/tb_chip.vvp
	cd $(BUILD) && $(VVP) tb_chip.vvp

# ---- the UART at the hardware's clock-to-baud ratio ----
# tb_uart.v runs uart.v at the sbc's 4 MHz and at the 3.5 MHz option
# against top.v's BAUD: every TX bit edge against its ideal time and the
# bytes as an ideal receiver reads them, and RX from a sender off by up
# to 3 % either way, its start edge at 16 phases of the clock (each
# sample's distance from its bit's centre is printed). A few seconds.
UART_BAUD := $(shell sed -n 's/^ *parameter BAUD *= *\([0-9_]*\).*/\1/p' top.v | tr -d _)
sim_uart:
	@mkdir -p $(BUILD)
	@for hz in 4000000 3500000; do \
	  $(IVERILOG) -g2012 -P tb_uart.CLK_HZ=$$hz -P tb_uart.BAUD=$(UART_BAUD) \
	    -o $(BUILD)/tb_uart_$$hz.vvp uart.v tb_uart.v || exit 1; \
	  $(VVP) -n $(BUILD)/tb_uart_$$hz.vvp > $(BUILD)/tb_uart_$$hz.log; cat $(BUILD)/tb_uart_$$hz.log; \
	  grep -q '^PASS' $(BUILD)/tb_uart_$$hz.log || exit 1; \
	done

# ---- post-place-and-route simulation: the routed sbc netlist ----
# nextpnr's routed netlist (top_sbc.pnr.json) is the design as icepack
# packs it into the bitstream: every ICESTORM_LC with its LUT and flop as
# placed, the EBR ROM image, the SPRAM, global buffers, pads. Here it runs
# on yosys' iCE40 cell models (cells_sim.v from the venv), zero delay.
# sim_pnr.py ties the inputs nextpnr left unrouted to the level the
# hardware reads there and feeds the PLL's dedicated clock paths from the
# testbench clock -- the same x1 stand-in as the RTL sims, so the Z80 runs
# at 2 MHz; sim_pnr_io.v turns the pads into wires, sim_pnr_cells.v models
# the PLL and SPRAM cells. The netlist has CORE_HZ = 4 MHz and BAUD =
# 230400 baked in, 17.36 Z80 clocks per bit, which the testbench sees as
# SIM_BAUD_REAL at 2 MHz: `make sim_real`'s pacing, and each script ends
# on the same clock as the RTL at that pacing.
#   make sim_pnr        the boot script on build/'s routed netlist
#   make sim_pnr_tiny   Tiny BASIC on build_tiny/'s (built if needed)
YOSYS_SHARE = $(shell $(VENV)/bin/python -c 'import os, yowasp_yosys; print(os.path.dirname(yowasp_yosys.__file__))')/share
PNR_DIR := $(BUILD)/pnr
SIM_PNR_ARGS ?=
# A broken netlist mostly hangs the CPU instead of failing a check, and
# at this speed the testbench's own step limit is over an hour away: give
# up after SIM_PNR_SECONDS (a healthy run takes 1-2 minutes).
SIM_PNR_SECONDS ?= 900

$(PNR_DIR)/top_sbc_pnr.v: $(BUILD)/top_sbc.pnr.json sim_pnr.py sim_pnr_io.v $(VENV)/.ok
	@mkdir -p $(PNR_DIR)
	$(PYTHON) sim_pnr.py $(YOSYS_SHARE)/ice40/cells_sim.v $< \
	  $(PNR_DIR)/top_sbc_sim.json $(PNR_DIR)/cells_sim.v
	cp sim_pnr_io.v $(PNR_DIR)/
	cd $(PNR_DIR) && $(YOSYS) -q -p \
	  'read_json top_sbc_sim.json; techmap -map sim_pnr_io.v t:SB_IO; select -assert-none t:SB_IO; opt_clean; write_verilog -noattr top_sbc_pnr.v'

$(PNR_DIR)/obj/Vtop: $(PNR_DIR)/top_sbc_pnr.v sim_pnr_cells.v sim_top.cpp
	rm -rf $(PNR_DIR)/obj
	cd $(PNR_DIR) && $(VERILATOR) --cc --exe --build -j 4 -O1 \
	  -Wno-fatal -Wno-lint -Wno-style --no-timing -DNO_ICE40_DEFAULT_ASSIGNMENTS \
	  --top-module top --Mdir obj -CFLAGS -DSIM_BAUD=$(SIM_BAUD_REAL) \
	  cells_sim.v ../../sim_pnr_cells.v top_sbc_pnr.v ../../sim_top.cpp \
	  > verilator.log 2>&1 || (tail -20 verilator.log; exit 1)

sim_pnr: $(PNR_DIR)/obj/Vtop
	cd $(PNR_DIR) && timeout $(SIM_PNR_SECONDS) stdbuf -oL ./obj/Vtop $(SIM_PNR_ARGS) || { rc=$$?; \
	  [ $$rc != 124 ] || echo "sim_pnr: no result after $(SIM_PNR_SECONDS) s (a healthy run takes 1-2 min)"; exit $$rc; }

sim_pnr_tiny:
	$(MAKE) sim_pnr BUILD=build_tiny ROM_HEX=$(TINY_HEX) SIM_PNR_ARGS=tiny

# ---- board (stock pico-ice firmware on the RP2040) ----
# prog:  bitstream into the iCE40's configuration RAM -- lost on power-cycle.
# flash: bitstream into the iCE40's SPI flash, then the RP2040 reboots and
#        the FPGA configures from it (persistent).
# uf2:   the same flash write without dfu-util: copy the .uf2 onto the
#        `pico-ice` USB drive.
prog: $(BUILD)/top_$(MODE).bin
	$(DFU_UTIL) -d $(DFU_DEV) --alt 1 --download $<

flash: $(BUILD)/top_$(MODE).bin
	$(DFU_UTIL) -d $(DFU_DEV) --alt 0 --download $< --reset

# UF2_DRIVE=<mount point of the pico-ice drive> copies it there directly.
uf2: $(BUILD)/top_$(MODE).uf2
	@if [ -n "$(UF2_DRIVE)" ]; then cp $< "$(UF2_DRIVE)"/ && sync && echo "copied $< to $(UF2_DRIVE)"; \
	else echo "copy $< onto the pico-ice USB drive, or: make uf2 MODE=$(MODE) UF2_DRIVE=/media/\$$USER/pico-ice"; fi

# ---- CI (.github/workflows/fpga-board.yml) ----
# Both bitstreams and their UF2 images, the Tiny BASIC bitstream
# (build_tiny/) and every simulation, Tiny BASIC at both serial pacings
# and on its routed netlist.
ci: sim_uart sbc chip $(BUILD)/top_sbc.uf2 $(BUILD)/top_chip.uf2 sim sim_real sim_tiny sim_iverilog sim_chip sim_pnr
	$(MAKE) sim_tiny SIM_BAUD=$(SIM_BAUD_REAL)
	$(MAKE) sbc BUILD=build_tiny ROM_HEX=$(TINY_HEX)
	$(MAKE) sim_pnr_tiny

clean:
	rm -rf $(BUILD) build_tiny

distclean: clean
	rm -rf $(VENV)
