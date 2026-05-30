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

.PHONY: all cmodel ctest rtl iverilog verilator traces compare test zexdoc zexall clean dirs tracegen zexrunner prelim fuse fuse_runner fuse_rtl

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

clean:
	rm -rf $(BUILD)
	@echo "cleaned."
