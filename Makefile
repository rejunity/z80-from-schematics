# =============================================================================
# Schematic-faithful Z80 core - top-level build
#
#   make cmodel     build the C99 hardware-like model (static lib)
#   make ctest      build + run all C unit/instruction tests
#   make rtl        syntax-check / elaborate the Verilog-2001 RTL
#   make iverilog   build the Icarus Verilog simulation
#   make verilator  build the Verilator simulation
#   make traces     run reference programs, emit shared-format traces
#   make compare    diff C vs Verilog traces phase-by-phase
#   make test       run the full C + RTL + compare suite
#   make zexdoc     run ZEXDOC through the C model
#   make zexall     run ZEXALL through the C model
#   make clean      remove build artifacts
# =============================================================================

# ---- toolchain ----
CC        ?= cc
CFLAGS    ?= -std=c99 -O2 -g -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
IVERILOG  ?= iverilog
VVP       ?= vvp
VERILATOR ?= verilator
PYTHON    ?= python3

# ---- layout ----
BUILD     := build
BIN       := $(BUILD)/bin
CMODEL    := cmodel
RTL       := rtl
TESTS     := tests
SCRIPTS   := scripts

# ---- C model sources (exclude any file with its own main) ----
CMODEL_SRCS := $(filter-out $(CMODEL)/z80_main.c,$(wildcard $(CMODEL)/*.c))
CMODEL_OBJS := $(patsubst $(CMODEL)/%.c,$(BUILD)/cmodel/%.o,$(CMODEL_SRCS))
CMODEL_HDRS := $(wildcard $(CMODEL)/*.h)
CMODEL_LIB  := $(BUILD)/libz80.a
INCLUDES    := -I$(CMODEL)

# ---- C tests: each tests/common/*.c is a standalone test binary ----
CTEST_SRCS := $(wildcard $(TESTS)/common/*.c)
CTEST_BINS := $(patsubst $(TESTS)/common/%.c,$(BIN)/%,$(CTEST_SRCS))

# ---- RTL sources ----
RTL_SRCS  := $(wildcard $(RTL)/*.v)

.PHONY: all cmodel ctest rtl iverilog verilator traces compare test zexdoc zexall clean dirs tracegen zexrunner prelim fuse fuse_runner fuse_rtl all-tests silicon_cycles silicon_async basicrunner basic tinybasic basic_rtl tinybasic_rtl

all: cmodel ctest

# ----------------------------------------------------------------------------
# directories
# ----------------------------------------------------------------------------
dirs:
	@mkdir -p $(BUILD)/cmodel $(BIN) $(TESTS)/traces

# ----------------------------------------------------------------------------
# C model
# ----------------------------------------------------------------------------
cmodel: dirs $(CMODEL_LIB) $(BIN)/zexrunner $(BIN)/tracegen $(BIN)/fuse_runner

$(BUILD)/cmodel/%.o: $(CMODEL)/%.c $(CMODEL_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(CMODEL_LIB): $(CMODEL_OBJS)
	@if [ -z "$(CMODEL_OBJS)" ]; then echo "No C model sources yet."; else ar rcs $@ $(CMODEL_OBJS); echo "Built $@"; fi

# ----------------------------------------------------------------------------
# C tests
# ----------------------------------------------------------------------------
ctest: cmodel $(CTEST_BINS)
	@echo "== running C tests =="
	@fail=0; for t in $(CTEST_BINS); do \
	  echo "---- $$t ----"; \
	  $$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "C TESTS FAILED"; exit 1; else echo "ALL C TESTS PASSED"; fi

$(BIN)/%: $(TESTS)/common/%.c $(CMODEL_LIB) $(CMODEL_HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(CMODEL_LIB) -o $@

# C-model trace generator
tracegen: cmodel $(BIN)/tracegen
$(BIN)/tracegen: $(SCRIPTS)/tracegen.c $(CMODEL_LIB) $(CMODEL_HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(CMODEL_LIB) -o $@

# ZEX CP/M harness runner
zexrunner: cmodel $(BIN)/zexrunner
$(BIN)/zexrunner: $(SCRIPTS)/zexrunner.c $(CMODEL_LIB) $(CMODEL_HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(CMODEL_LIB) -o $@

# FUSE z80-test harness (Frank D. Cringle test corpus, 1356 cases)
fuse_runner: cmodel $(BIN)/fuse_runner
$(BIN)/fuse_runner: $(SCRIPTS)/fuse_runner.c $(CMODEL_LIB) $(CMODEL_HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(CMODEL_LIB) -o $@

fuse: fuse_runner
	@$(BIN)/fuse_runner tests/fuse/tests.in tests/fuse/tests.expected

# FUSE through the RTL (iverilog). Generates tb_fuse.v from tests.in, builds
# with iverilog, runs vvp, diffs results against tests.expected.
fuse_rtl:
	@$(PYTHON) $(SCRIPTS)/compare_fuse_rtl.py

# Real-silicon T-state validation against the sigrok KC85/4 logic-analyzer
# captures: per opcode observed in the kc85 OS loop, check that our emulator
# produces the same T-state count (or one of the spec-allowed branches for
# conditionals).
silicon_cycles: tracegen
	@$(PYTHON) $(SCRIPTS)/sigrok_opcode_cycles.py tests/sigrok/kc85-cpuclk.sr

# Same idea on the asynchronous 20 MHz capture: derives the real CPU clock
# period and pin sub-T-state transition offsets, then re-samples at CLK
# falling edges to cross-check the per-opcode T-state counts independently
# from the cpuclk synchronous capture.
silicon_async: tracegen
	@$(PYTHON) $(SCRIPTS)/sigrok_async_timing.py tests/sigrok/kc85-20mhz.sr --silicon-check

# Z80 BASIC runner: emulates a 68B50 ACIA at ports 0x80/0x81 (NASCOM
# convention) and ports 0/1 (1K Tiny BASIC convention) wired to host
# stdin/stdout. Asserts /INT while a stdin byte is queued for ROMs
# whose RX path is interrupt-driven.
basicrunner: cmodel $(BIN)/basicrunner
$(BIN)/basicrunner: $(SCRIPTS)/basicrunner.c $(CMODEL_LIB) $(CMODEL_HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(CMODEL_LIB) -o $@

basic: basicrunner
	@$(BIN)/basicrunner --autostart tests/basic/nascom_basic_4_7_rc2014.hex

tinybasic: basicrunner
	@$(BIN)/basicrunner tests/basic/tinybasic_1k.hex

# Same BASIC ROMs driven through the synthesisable Verilog RTL via Verilator
# (~21x slower than the C model, otherwise bit-identical behaviour).
$(BIN)/sim_basic: tests/verilator/basic_main.cpp $(RTL_SRCS)
	@mkdir -p $(BIN) $(BUILD)/obj_basic_rtl
	$(VERILATOR) --cc --exe --build -j 0 -Wno-fatal -Wno-WIDTH -Wno-CASEINCOMPLETE \
	  -O3 -CFLAGS "-O3" \
	  --Mdir $(BUILD)/obj_basic_rtl --top-module z80_core \
	  -I$(RTL) $(RTL_SRCS) tests/verilator/basic_main.cpp -o sim_basic
	@cp $(BUILD)/obj_basic_rtl/sim_basic $@

basic_rtl: $(BIN)/sim_basic
	@$(BIN)/sim_basic --autostart tests/basic/nascom_basic_4_7_rc2014.hex

tinybasic_rtl: $(BIN)/sim_basic
	@$(BIN)/sim_basic tests/basic/tinybasic_1k.hex

# ----------------------------------------------------------------------------
# RTL elaboration / sims
# ----------------------------------------------------------------------------
rtl:
	@if [ -z "$(RTL_SRCS)" ]; then echo "No RTL sources yet."; else \
	  echo "== elaborating RTL with iverilog =="; \
	  $(IVERILOG) -g2001 -I$(RTL) -t null $(RTL_SRCS) && echo "RTL elaboration OK"; fi

iverilog: dirs
	@if [ -f $(TESTS)/iverilog/tb_z80.v ]; then \
	  echo "== building iverilog sim =="; \
	  $(IVERILOG) -g2001 -I$(RTL) -s tb_z80 -o $(BUILD)/tb_z80.vvp \
	    $(RTL_SRCS) $(TESTS)/iverilog/tb_z80.v && \
	  echo "Built $(BUILD)/tb_z80.vvp"; \
	else echo "iverilog testbench not present yet."; fi

verilator: dirs
	@if [ ! -f $(TESTS)/verilator/sim_main.cpp ]; then echo "verilator testbench not present yet."; exit 0; fi; \
	printf '#include <cstdio>\nint main(){return 0;}\n' > $(BUILD)/.cxxcheck.cpp; \
	if ! c++ -std=gnu++17 -c $(BUILD)/.cxxcheck.cpp -o $(BUILD)/.cxxcheck.o >/dev/null 2>&1; then \
	  echo "SKIP verilator: host C++17 toolchain cannot compile libc++ headers"; \
	  echo "  (Apple clang / macOS SDK mismatch). iverilog covers RTL verification."; \
	  echo "  The testbench tests/verilator/sim_main.cpp builds on a healthy C++ toolchain."; \
	  exit 0; \
	fi; \
	echo "== building verilator sim =="; \
	$(VERILATOR) --cc --exe --build -j 0 -Wall -Wno-fatal -Wno-WIDTH -Wno-CASEINCOMPLETE \
	  --Mdir $(BUILD)/obj_dir --top-module z80_core \
	  -I$(RTL) $(RTL_SRCS) $(TESTS)/verilator/sim_main.cpp -o sim_z80 && \
	echo "Built $(BUILD)/obj_dir/sim_z80"

# ----------------------------------------------------------------------------
# traces / comparison
# ----------------------------------------------------------------------------
traces: cmodel
	@if [ -f $(SCRIPTS)/gen_traces.py ]; then $(PYTHON) $(SCRIPTS)/gen_traces.py; else echo "trace generator not present yet."; fi

compare: tracegen iverilog
	@$(PYTHON) $(SCRIPTS)/compare_traces.py

# ----------------------------------------------------------------------------
# compatibility suites
# ----------------------------------------------------------------------------
# ZEX_MAX caps instructions so an unfinished feature can't loop forever.
ZEX_MAX ?= 4000000000

prelim: zexrunner
	@$(PYTHON) $(SCRIPTS)/run_zex.py prelim $(ZEX_MAX)

zexdoc: zexrunner
	@$(PYTHON) $(SCRIPTS)/run_zex.py zexdoc $(ZEX_MAX)

zexall: zexrunner
	@$(PYTHON) $(SCRIPTS)/run_zex.py zexall $(ZEX_MAX)

# ----------------------------------------------------------------------------
# full suite
# ----------------------------------------------------------------------------
test: ctest rtl compare
	@echo "== full test suite complete =="

# Run every verification gate. Heavy: ZEXDOC + ZEXALL take ~30 min combined.
all-tests: ctest rtl compare fuse fuse_rtl
	@echo "== running ZEXDOC (~1 min) =="
	@$(BIN)/zexrunner tests/zex/zexdoc.com 6000000000 2>&1 | tr -d '\r' | grep -E 'OK$$|ERROR|complete|elapsed' | tail -10
	@echo "== running ZEXALL (~16 min) =="
	@$(BIN)/zexrunner tests/zex/zexall.com 12000000000 2>&1 | tr -d '\r' | grep -E 'OK$$|ERROR|complete|elapsed' | tail -10
	@echo "== running 4-way lockstep on zexdoc3 (~2 s) =="
	@c++ -std=gnu++17 -O2 -w -Iscripts -Icmodel scripts/lockstep_quad.c $(CMODEL_LIB) -o $(BIN)/lockstep_quad
	@$(BIN)/lockstep_quad tests/zex/zexdoc3.com 20000000
	@echo "== real-silicon T-state oracle (sigrok kc85 cpuclk capture) =="
	@$(PYTHON) $(SCRIPTS)/sigrok_opcode_cycles.py tests/sigrok/kc85-cpuclk.sr | tail -3
	@echo "== real-silicon clock + sub-T-state (sigrok kc85 20 MHz async) =="
	@$(PYTHON) $(SCRIPTS)/sigrok_async_timing.py tests/sigrok/kc85-20mhz.sr --silicon-check 2>&1 | tail -6
	@echo "== all-tests complete =="

clean:
	rm -rf $(BUILD)
	@echo "cleaned."
