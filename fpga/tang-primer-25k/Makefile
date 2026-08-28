# RZ80 -> Sipeed Tang Primer 25K (Gowin GW5A-LV25MG121NES)
#
#   make venv    create .venv and pip-install the toolchain
#   make fs      synthesize + place&route + pack -> build/top.fs  (default)
#   make sim     Verilator smoke test (fast serial)
#   make sim_real  same at true 115200 pacing (exercises TIE path)
#   make prog    load bitstream to SRAM (lost on power-cycle)
#   make flash   write bitstream to SPI flash (persistent)
#   make timing  show nextpnr's max-frequency summary
#   make clean   remove build/;  make distclean also removes .venv
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

ROM_HEX  ?= $(ROOT)/tests/basic/nascom_basic_4_7_rc2014.hex

CORE_SRCS := $(ROOT)/rtl/z80_core.v $(ROOT)/rtl/z80_pla.v \
             $(ROOT)/rtl/z80_alu.v  $(ROOT)/rtl/z80_timing.v
CORE_INCS := $(ROOT)/rtl/z80_defs.vh
SOC_SRCS  := top.v $(ICEPI)/uart.v

YOSYS      := $(abspath $(VENV))/bin/yowasp-yosys
NEXTPNR    := $(abspath $(VENV))/bin/yowasp-nextpnr-himbaechel-gowin
GOWIN_PACK := $(abspath $(VENV))/bin/gowin_pack
OPENFPGALOADER ?= openFPGALoader

DEVICE   := GW5A-LV25MG121NES
PACK_DEV := GW5A-25A
BOARD    := tangprimer25k

.PHONY: all venv fs sim sim_real prog flash timing clean distclean
all: fs

# ---- toolchain venv (created before any tools are installed/used) ----
$(VENV)/.ok:
	$(PYTHON) -m venv $(VENV)
	$(VENV)/bin/pip install --upgrade pip
	$(VENV)/bin/pip install yowasp-yosys yowasp-nextpnr-himbaechel-gowin apycula
	touch $@

venv: $(VENV)/.ok

# ---- staging ----
$(BUILD)/rom.mem: $(ROM_HEX) $(ICEPI)/hex2mem.py
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
# proc+async2sync before synth for the same reason as the ECP5 port:
# the core's async-assert reset constants conflict with FF init values,
# and the POR holds reset for thousands of clocks so sync semantics
# are equivalent.
$(BUILD)/top.json: $(BUILD)/src $(BUILD)/rom.mem $(VENV)/.ok
	cd $(BUILD) && $(YOSYS) -q -p \
	  'read_verilog -Isrc src/z80_core.v src/z80_pla.v src/z80_alu.v src/z80_timing.v src/top.v src/uart.v; hierarchy -top top; proc; async2sync; synth_gowin -top top -family gw5a -nolutram -json top.json' \
	  2>&1 | tee yosys.log | tail -20

# sspi_as_gpio is mandatory on GW5A-25 (apycula enforces it), but NOT
# cpu_as_gpio: apicula's examples pass it, we use no CPU-group pins,
# and repurposing config-interface pins risks locking out the BL616's
# JTAG access until the next power cycle.
$(BUILD)/top_pnr.json: $(BUILD)/top.json $(BUILD)/tang_primer_25k.cst
	cd $(BUILD) && $(NEXTPNR) --top top --json top.json --write top_pnr.json \
	  --device $(DEVICE) --vopt cst=tang_primer_25k.cst --vopt sspi_as_gpio \
	  2>&1 | tee nextpnr.log | grep -E "Max frequency|ERROR|error" || true
	@test -f $(BUILD)/top_pnr.json

$(BUILD)/top.fs: $(BUILD)/top_pnr.json
	cd $(BUILD) && $(GOWIN_PACK) --sspi_as_gpio -d $(PACK_DEV) -o top.fs top_pnr.json

fs: $(BUILD)/top.fs

timing: $(BUILD)/top_pnr.json
	@grep -E "Max frequency" $(BUILD)/nextpnr.log | tail -4

# ---- simulation (native Verilator) ----
VERILATOR ?= verilator
SIM_BAUD  ?= 781250          # core clk / 16; sim_real uses true 115200

$(BUILD)/obj_dir/Vtop: $(BUILD)/src sim_top.cpp
	rm -rf $(BUILD)/obj_dir
	cd $(BUILD) && $(VERILATOR) --cc --exe --build -j 4 -O2 \
	  -Wno-fatal -Isrc --Mdir obj_dir --top-module top \
	  -GCORE_HZ=12500000 -GBAUD=$(SIM_BAUD) -GEXTERNAL_BUS=0 \
	  -CFLAGS -DSIM_BAUD=$(SIM_BAUD) \
	  src/top.v src/uart.v src/z80_core.v src/z80_pla.v src/z80_alu.v src/z80_timing.v \
	  ../sim_top.cpp > verilator.log 2>&1 || (tail -20 verilator.log; exit 1)

sim: $(BUILD)/obj_dir/Vtop $(BUILD)/rom.mem
	cd $(BUILD) && ./obj_dir/Vtop

sim_real:
	$(MAKE) sim SIM_BAUD=115200

# ---- board ----
prog: $(BUILD)/top.fs
	$(OPENFPGALOADER) -b $(BOARD) $(BUILD)/top.fs

flash: $(BUILD)/top.fs
	$(OPENFPGALOADER) -b $(BOARD) -f $(BUILD)/top.fs

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(VENV)
