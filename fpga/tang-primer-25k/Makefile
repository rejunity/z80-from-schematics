# RZ80 -> Sipeed Tang Primer 25K (Gowin GW5A-LV25MG121NES)
#
# Two bitstreams from one top.v (EXTERNAL_BUS parameter):
#   sbc   BASIC single-board computer, internal 4 MHz clock, UART console,
#         J3 mirrors the Z80 bus as outputs        -> build/top_sbc.fs
#   chip  Z80 chip replacement: J3 is the live bus, CLK comes in on J3-22
#                                                  -> build/top_chip.fs
#
#   make venv    create .venv and pip-install the toolchain
#   make         both bitstreams (= make fs);  make sbc / make chip for one
#   make sim     Verilator smoke test of the sbc build (fast serial)
#   make sim_real  same at the hardware's serial pacing (exercises TIE path)
#   make sim_tiny  Verilator: the 1K Tiny BASIC ROM, console transcript
#                compared byte for byte with the C model's (add
#                SIM_BAUD=240000 for the hardware's pacing)
#   make sim_uart  uart.v at 4 and 3.5 MHz against BAUD: TX edges, RX at +-3 %
#   make prog MODE=sbc|chip    load bitstream to SRAM (lost on power-cycle)
#   make flash MODE=sbc|chip   write bitstream to SPI flash (persistent)
#   make timing  show nextpnr's max-frequency summary for both builds
#   make ci      everything CI runs (fpga-board.yml): both bitstreams, the
#                Tiny BASIC bitstream in build_tiny/ and every simulation
#   make clean   remove build/ and build_tiny/;  make distclean also .venv
#
#   make sbc ROM_HEX=../../tests/basic/tinybasic_1k.hex   Tiny BASIC instead
#
# `make ci` also runs the top-level `make bus_controls` regressions
# (tb_wait_top / tb_reset_top drive the chip build's header).
#
# Whole flow is pip-installed into .venv: yowasp-yosys (synth_gowin),
# yowasp-nextpnr-himbaechel-gowin (place&route, WASM), and apycula
# (gowin_pack bitstream writer). Flashing needs native openFPGALoader
# (brew install openfpgaloader), board name `tangprimer25k`.
#
# Shares uart.v / hex2mem.py with fpga/icepi-zero (same SoC, different
# board top). Sources are staged into build/ and the tools run from
# there: the YoWASP sandbox only sees the current directory tree.

ROOT     := ../..
ICEPI    := ../icepi-zero
BUILD    := build
VENV     := .venv
PYTHON   ?= python3
IVERILOG ?= iverilog
VVP      ?= vvp

ROM_HEX  ?= $(ROOT)/tests/basic/nascom_basic_4_7_rc2014.hex
# Writes below ROM_PROTECT_TOP are dropped (the ROM). Default: the end of
# the ROM image rounded up to 1 KiB (hex2mem.py --top) -- 8192 for NASCOM
# BASIC, 1024 for the 1K Tiny BASIC, which keeps its variables and stack
# at 0x0400-0x07FF.
ifeq ($(origin ROM_PROTECT_TOP),undefined)
ROM_PROTECT_TOP := $(shell $(PYTHON) $(ICEPI)/hex2mem.py --top $(ROM_HEX))
endif

CORE_SRCS := $(wildcard $(ROOT)/rtl/*.v)
CORE_INCS := $(wildcard $(ROOT)/rtl/*.vh)
SOC_SRCS  := top.v $(ICEPI)/uart.v

YOSYS      := $(abspath $(VENV))/bin/yowasp-yosys
NEXTPNR    := $(abspath $(VENV))/bin/yowasp-nextpnr-himbaechel-gowin
GOWIN_PACK := $(abspath $(VENV))/bin/gowin_pack
OPENFPGALOADER ?= openFPGALoader

DEVICE   := GW5A-LV25MG121NES
PACK_DEV := GW5A-25A
BOARD    := tangprimer25k

MODES := sbc chip
EXTBUS_sbc  := 0
EXTBUS_chip := 1
MODE ?= sbc
# keep the per-build synth/pnr netlists (make would delete chained intermediates)
.SECONDARY:
# stem of the pattern rules below without any directory part
STEM = $(notdir $*)

.PHONY: all venv fs sbc chip sim sim_real sim_tiny prog flash timing sim_uart ci clean distclean FORCE
all: fs

# ---- toolchain venv (created before any tools are installed/used) ----
$(VENV)/.ok:
	$(PYTHON) -m venv $(VENV)
	$(VENV)/bin/pip install --upgrade pip
	$(VENV)/bin/pip install yowasp-yosys yowasp-nextpnr-himbaechel-gowin apycula
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

$(BUILD)/rom.mem: $(ROM_HEX) $(ICEPI)/hex2mem.py $(BUILD)/rom.cfg
	@mkdir -p $(BUILD)
	$(PYTHON) $(ICEPI)/hex2mem.py $< $@

$(BUILD)/src: $(CORE_SRCS) $(CORE_INCS) $(SOC_SRCS)
	@mkdir -p $(BUILD)/src
	cp $(CORE_SRCS) $(CORE_INCS) $(SOC_SRCS) $(BUILD)/src/
	@touch $@

$(BUILD)/tang_primer_25k.cst: tang_primer_25k.cst
	@mkdir -p $(BUILD)
	cp $< $@

# ---- synth / pnr / pack ----
# No async2sync: the core's async-reset flops map straight onto GW5A
# DFFC/DFFP(E). async2sync (used before) turned every flop into
# sync-reset plus a Q-side mux, which hid the hold paths from clock-enable
# inference: ~3,900 extra LUTs (11,922 -> 8,122 for the sbc build).
# Console shows pass headers two levels deep (-v 2) plus warnings; the full
# log goes to build/yosys_<build>.log (-l).
YOSYS_QUIET := -v 2 -Q -T
# Known-benign warnings dropped from the console only (the -l log keeps them):
# the core's trace counters and memories, tri-state notes, BRAM map ranges.
YOSYS_FILTER := grep --line-buffered -vE 'port Q\[|Drivers conflicting|Replacing memory|limited support for tri-state|out of bounds on signal|Latch inferred for signal .\\z80_core'

$(BUILD)/top_%.synth.json: $(BUILD)/src $(BUILD)/rom.mem $(VENV)/.ok
	@rm -f $@
	cd $(BUILD) && $(YOSYS) $(YOSYS_QUIET) -l yosys_$(STEM).log -p \
	  'read_verilog -Isrc src/*.v; hierarchy -top top -chparam EXTERNAL_BUS $(EXTBUS_$(STEM)) -chparam ROM_PROTECT_TOP $(ROM_PROTECT_TOP); synth_gowin -top top -family gw5a -nolutram -json top_$(STEM).synth.json' \
	  2>&1 | $(YOSYS_FILTER)
	@test -f $@ || { echo "yosys failed, see $(BUILD)/yosys_$(STEM).log"; exit 1; }

# sspi_as_gpio is mandatory on GW5A-25 (apycula enforces it), but NOT
# cpu_as_gpio: apicula's examples pass it, we use no CPU-group pins,
# and repurposing config-interface pins risks locking out the BL616's
# JTAG access until the next power cycle.
# nextpnr: full log in build/nextpnr_<build>.log, console shows the stage
# lines, placer/router progress (one router row per 5000 arcs) and the
# timing summary. nextpnr's own exit status is saved ahead of that console
# filter, which would otherwise replace it: any nextpnr ERROR fails the
# build and removes the netlist -- a hold violation included, after which
# nextpnr still writes one.
# Each build has one "Failed to route ... using dedicated routing"
# warning: the LUT that inverts the Z80 clock for the falling-edge flops
# takes it on a logic input. It is left off the console; more than one
# means CLK is off the global clock network (README, Clocking).
NEXTPNR_PROGRESS := grep --line-buffered -E '^(Info: (Pack|Running|Placed|Creating|HeAP|SA |Routing|Router|Max frequency|Program finished|Device util)|Info: +[0-9]*[05]000 \||ERROR|Warning)' \
  | grep --line-buffered -v 'Failed to route net'

$(BUILD)/top_%.pnr.json: $(BUILD)/top_%.synth.json $(BUILD)/tang_primer_25k.cst
	@rm -f $@
	cd $(BUILD) && { $(NEXTPNR) --top top --json top_$(STEM).synth.json --write top_$(STEM).pnr.json \
	  --device $(DEVICE) --vopt cst=tang_primer_25k.cst --vopt sspi_as_gpio 2>&1; \
	  echo $$? > nextpnr_$(STEM).rc; } | tee nextpnr_$(STEM).log | $(NEXTPNR_PROGRESS); \
	  rc=$$(cat nextpnr_$(STEM).rc); [ "$$rc" = 0 ] || { rm -f top_$(STEM).pnr.json; \
	  echo "nextpnr failed (exit $$rc), see $(BUILD)/nextpnr_$(STEM).log"; exit 1; }
	@n=$$(grep -c 'Failed to route net' $(BUILD)/nextpnr_$(STEM).log); \
	  [ "$$n" -le 1 ] || echo "nextpnr: $$n clock sinks on general routing, expected 1 (see README, Clocking)"
	@test -f $@

$(BUILD)/top_%.fs: $(BUILD)/top_%.pnr.json
	cd $(BUILD) && $(GOWIN_PACK) --sspi_as_gpio -d $(PACK_DEV) -o top_$(STEM).fs top_$(STEM).pnr.json

sbc:  $(BUILD)/top_sbc.fs
chip: $(BUILD)/top_chip.fs
fs:   sbc chip

timing: $(BUILD)/top_sbc.pnr.json $(BUILD)/top_chip.pnr.json
	@for m in $(MODES); do echo "== $$m"; grep -E "Max frequency" $(BUILD)/nextpnr_$$m.log | tail -4; done

# ---- simulation (native Verilator) ----
VERILATOR ?= verilator
# Sim Z80 clock is the /12 stand-in for the PLLA+CLKDIV: 4.1667 MHz, so
# -GCORE_HZ overrides the UART divisors to match. SIM_BAUD = Z80 clk / 16
# (make keeps trailing spaces, so no inline comment on that line).
# `make sim_real` runs at the hardware's pacing -- 17.36 Z80 clocks a
# bit, i.e. 230400 baud at 4 MHz, which is 240000 baud at the sim's
# 4.17 MHz -- the pacing that exercises the ROM's interrupt-driven TX.
SIM_CORE_HZ := 4166667
SIM_BAUD  ?= 260417
SIM_BAUD_REAL := 240000
# One object dir per baud: SIM_BAUD is baked into the model (-G/-D), so
# `make sim_real` after `make sim` must not reuse the fast binary.
OBJ_DIR   := $(BUILD)/obj_dir_$(SIM_BAUD)

$(OBJ_DIR)/Vtop: $(BUILD)/src sim_top.cpp
	rm -rf $(OBJ_DIR)
	cd $(BUILD) && $(VERILATOR) --cc --exe --build -j 4 -O2 \
	  -Wno-fatal -Isrc --Mdir obj_dir_$(SIM_BAUD) --top-module top \
	  -GCORE_HZ=$(SIM_CORE_HZ) -GBAUD=$(SIM_BAUD) -GEXTERNAL_BUS=0 \
	  -CFLAGS -DSIM_BAUD=$(SIM_BAUD) \
	  src/*.v \
	  ../sim_top.cpp > verilator.log 2>&1 || (tail -20 verilator.log; exit 1)

sim: $(OBJ_DIR)/Vtop $(BUILD)/rom.mem
	cd $(BUILD) && ./obj_dir_$(SIM_BAUD)/Vtop

sim_real:
	$(MAKE) sim SIM_BAUD=$(SIM_BAUD_REAL)

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
	    -o $(BUILD)/tb_uart_$$hz.vvp $(ICEPI)/uart.v $(ICEPI)/tb_uart.v || exit 1; \
	  $(VVP) -n $(BUILD)/tb_uart_$$hz.vvp > $(BUILD)/tb_uart_$$hz.log; cat $(BUILD)/tb_uart_$$hz.log; \
	  grep -q '^PASS' $(BUILD)/tb_uart_$$hz.log || exit 1; \
	done

# Tiny BASIC: its own ROM image and model (RAM from 0x0400), whatever
# ROM_HEX says; sim_top.cpp's `tiny` script.
TINY_HEX := $(ROOT)/tests/basic/tinybasic_1k.hex
TINY_DIR := obj_dir_tiny_$(SIM_BAUD)

$(BUILD)/rom_tiny.mem: $(TINY_HEX) $(ICEPI)/hex2mem.py
	@mkdir -p $(BUILD)
	$(PYTHON) $(ICEPI)/hex2mem.py $< $@

$(BUILD)/$(TINY_DIR)/Vtop: $(BUILD)/src sim_top.cpp $(ICEPI)/hex2mem.py
	rm -rf $(BUILD)/$(TINY_DIR)
	cd $(BUILD) && $(VERILATOR) --cc --exe --build -j 4 -O2 \
	  -Wno-fatal -Isrc --Mdir $(TINY_DIR) --top-module top \
	  -GCORE_HZ=$(SIM_CORE_HZ) -GBAUD=$(SIM_BAUD) -GEXTERNAL_BUS=0 \
	  -CFLAGS -DSIM_BAUD=$(SIM_BAUD) \
	  -GROM_FILE='"rom_tiny.mem"' \
	  -GROM_PROTECT_TOP=$$($(PYTHON) ../$(ICEPI)/hex2mem.py --top ../$(TINY_HEX)) \
	  src/*.v ../sim_top.cpp > verilator_tiny.log 2>&1 || (tail -20 verilator_tiny.log; exit 1)

sim_tiny: $(BUILD)/$(TINY_DIR)/Vtop $(BUILD)/rom_tiny.mem
	cd $(BUILD) && ./$(TINY_DIR)/Vtop tiny

# ---- board ----
prog: $(BUILD)/top_$(MODE).fs
	$(OPENFPGALOADER) -b $(BOARD) $(BUILD)/top_$(MODE).fs

flash: $(BUILD)/top_$(MODE).fs
	$(OPENFPGALOADER) -b $(BOARD) -f $(BUILD)/top_$(MODE).fs

# ---- CI (.github/workflows/fpga-board.yml) ----
# Both bitstreams, the Tiny BASIC bitstream (build_tiny/), every
# simulation (Tiny BASIC at both serial pacings) and the top-level
# bus_controls regressions, which drive the chip build's header.
ci: sim_uart sbc chip sim sim_real sim_tiny
	$(MAKE) sim_tiny SIM_BAUD=$(SIM_BAUD_REAL)
	$(MAKE) sbc BUILD=build_tiny ROM_HEX=$(TINY_HEX)
	$(MAKE) -C $(ROOT) bus_controls

clean:
	rm -rf $(BUILD) build_tiny

distclean: clean
	rm -rf $(VENV)
