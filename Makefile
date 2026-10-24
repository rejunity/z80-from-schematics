# =============================================================================
# Schematic-faithful Z80 core - top-level build
#
#   make cmodel     build the C99 hardware-like model (static lib)
#   make ctest      build + run all C unit/instruction tests
#   make rtl        syntax-check / elaborate the Verilog-2001 RTL
#   make iverilog   build the Icarus Verilog simulation
#   make verilator  build the Verilator simulation
#   make compare    C model vs iverilog (and Verilator, if built), every phase
#   make test       ctest + rtl + bus_controls + compare (quick)
#   make all-tests  every gate that needs only a C compiler, iverilog and
#                   Verilator, as CI runs them (~1.5 h)
#
#   instruction suites, C model: prelim zexdoc zexall z80test fuse
#     (z80test_rtl, fuse_rtl, zexall_subset_rtl: the same through the RTL)
#   against the die (perfectz80): perfectz80 pin_scenarios event_sweep fuse_pz80
#     (perfectz80_rtl, pin_scenarios_rtl: the RTL as the trace source)
#   make lockstep   our model and four other emulators, instruction by
#                   instruction over ZEXDOC3
#   make phase_identity  every pin of every phase vs PHASE_REF=<rev>
#   make cfast      the phase engine generated from the timing tables
#   make cfast_identity  cfast phase by phase vs PHASE_REF=<rev>
#   make cfast_bench  cfast against the other C emulators (cfast/BENCHMARK.md)
#   make clean      remove build artifacts
#
# No target tolerates a failure: every suite passes in full.
# =============================================================================

# ---- toolchain ----
CC        ?= cc
CFLAGS    ?= -std=c99 -O2 -g -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
IVERILOG  ?= iverilog
VVP       ?= vvp
VERILATOR ?= verilator
PYTHON    ?= python3

# Link-time optimisation (default on): the per-phase engine, the timing
# generator and the runners' bus loops (z80_sys_phase) inline across
# translation units -- ZEXDOC 2.8 -> 4.2 Minstr/s. Fat objects keep
# build/libz80.a usable by links without -flto, where the compiler makes
# them; the probe asks with -flto and -Werror, since Apple clang takes
# -ffat-lto-objects alone but refuses it next to -flto, and older clang
# ignores it with a warning. LTO=0 for a plain build (make clean when
# switching).
LTO ?= 1
ifeq ($(LTO),1)
FAT_LTO := $(shell $(CC) -flto -ffat-lto-objects -Werror -x c -c /dev/null -o /dev/null 2>/dev/null && echo -ffat-lto-objects)
CFLAGS += -flto $(FAT_LTO)
endif

# Jumps padded off 32-byte boundaries (x86): the microcode for Intel's JCC
# erratum (Skylake-derived cores: 6th to 10th generation Core, Xeon
# Scalable up to Cascade Lake) keeps a 32-byte block with a jump that
# crosses or ends on its boundary out of the decoded-uop cache, and
# cfast's steps are mostly jumps (ZEXDOC on an i7-7700: z80f_run 121 ->
# 111 cycles an instruction). The probe takes clang's option, else GNU
# as's, else none (arm64); JCC_PAD= for none. The benchmark builds every
# emulator with it.
ifeq ($(origin JCC_PAD),undefined)
JCC_PAD := $(shell $(CC) -mbranches-within-32B-boundaries -Werror -x c -c /dev/null -o /dev/null 2>/dev/null \
  && echo -mbranches-within-32B-boundaries \
  || ($(CC) -Wa,-mbranches-within-32B-boundaries -Werror -x c -c /dev/null -o /dev/null 2>/dev/null \
  && echo -Wa,-mbranches-within-32B-boundaries))
endif
CFLAGS += $(JCC_PAD)

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

.PHONY: xyflags xyflags_rtl xyflags_pz80 xy_study cfast cfast_bench cfast_identity cfast_single cfast_single_identity pinhash phase_identity zexrunner_tick timing_sdc timing_sdc_check equiv all cmodel ctest rtl iverilog verilator verilator_zex zexall_rtl zexall_subset_c zexall_subset_rtl verilator_basic verilator_basic_netlist compare test zexdoc zexall clean dirs tracegen zexrunner prelim fuse fuse_runner fuse_rtl fuse_pz80 all-tests silicon_cycles silicon_async perfectz80 perfectz80_rtl perfectz80_netlist synth iverilog_netlist pin_scenarios pin_scenarios_rtl pin_scenarios_netlist event_sweep basicrunner basic tinybasic basic_tests basic_c_tests basic_rtl_tests basic_netlist_tests z80test_runner z80test verilator_z80test z80test_rtl halt2int halt2int_probe lockstep

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

# ZEX CP/M harness runner; zexrunner_tick runs its bus through z80_tick
zexrunner: cmodel $(BIN)/zexrunner
$(BIN)/zexrunner: $(SCRIPTS)/zexrunner.c $(CMODEL_LIB) $(CMODEL_HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(CMODEL_LIB) -o $@

zexrunner_tick: cmodel $(BIN)/zexrunner_tick
$(BIN)/zexrunner_tick: $(SCRIPTS)/zexrunner.c $(CMODEL_LIB) $(CMODEL_HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -DZEX_TICK $(INCLUDES) $< $(CMODEL_LIB) -o $@

# Patrik Rak z80test (raxoft) harness — ZX Spectrum .tap loader + ROM stubs
# at 0x10 (RST 10 print) and 0x1601 (CHAN-OPEN no-op). Tests/z80test/*.tap
# bundled from https://github.com/raxoft/z80test (MIT).
z80test_runner: cmodel $(BIN)/z80test_runner
$(BIN)/z80test_runner: $(SCRIPTS)/z80test_runner.c $(CMODEL_LIB) $(CMODEL_HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(CMODEL_LIB) -o $@

# Patrik Rak's z80test variants through the C model, one after another;
# any failed test fails the target. z80doc: documented flags; z80flags: all
# flags, X and Y included; z80memptr: MEMPTR (WZ) through BIT n,(HL);
# z80full: all of it. Each variant prints 160 results (the NEC and ST
# SCF/CCF tests report "Skipped" on a Zilog part).
# Z80TEST_VARIANTS="z80full" runs a subset (CI's z80test-rtl matrix does).
Z80TEST_VARIANTS ?= z80doc z80flags z80memptr z80full
z80test: z80test_runner
	@set -e; for v in $(Z80TEST_VARIANTS); do \
	  stdbuf -oL $(BIN)/z80test_runner $(TESTS)/z80test/$$v.tap 8000000000; \
	done

# FUSE's Z80 tests (Philip Kendall's corpus from the FUSE emulator, 1356
# cases): every case's final state through the C model
fuse_runner: cmodel $(BIN)/fuse_runner
$(BIN)/fuse_runner: $(SCRIPTS)/fuse_runner.c $(CMODEL_LIB) $(CMODEL_HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(CMODEL_LIB) -o $@

fuse: fuse_runner
	@$(BIN)/fuse_runner tests/fuse/tests.in tests/fuse/tests.expected tests/fuse/silicon.expected

# The same corpus on the die: every case of tests.in runs on perfectz80
# behind a preamble that loads its registers, and the die's registers, WZ,
# PC, R, T-states and memory must match tests.expected, or
# tests/fuse/silicon.expected where the die differs from FUSE's values (six
# cases). IFF1, IFF2 and IM are not read back. ~1.5 min.
fuse_pz80: $(BIN)/fuse_pz80
	@$(BIN)/fuse_pz80 tests/fuse/tests.in tests/fuse/tests.expected tests/fuse/silicon.expected

$(BIN)/fuse_pz80: $(SCRIPTS)/fuse_pz80.c $(SCRIPTS)/refs/perfectz80/perfectz80.c $(SCRIPTS)/refs/perfectz80/netlist_sim.c
	@mkdir -p $(BIN)
	$(CC) -std=c99 -O2 -I$(SCRIPTS) $^ -o $@

# F's X and Y (bits 5, 3), and the rest of F, as the die has them:
# tests/xyflags/ is every case scripts/xy_study.py found where the reference
# emulators (ours, floooh/chips, superzazu, suzukiplan, redcode/Z80) do not
# all agree about F, or where one of them differs from perfectz80, with the
# die's values in tests.expected (FUSE's format; docs/oracles.md "X and Y").
# xyflags runs it through the C model, xyflags_rtl through the RTL (iverilog),
# xyflags_pz80 on the die. make xy_study runs the study again
# (XY_STUDY_ARGS="--seed 2 --corpus tests/xyflags" writes a new corpus).
xyflags: fuse_runner
	@$(BIN)/fuse_runner tests/xyflags/tests.in tests/xyflags/tests.expected /dev/null

xyflags_rtl: dirs
	@FUSE_IN=tests/xyflags/tests.in FUSE_EXPECTED=tests/xyflags/tests.expected FUSE_FIX=/dev/null \
	  FUSE_TB=$(BUILD)/tb_xyflags.v FUSE_VVP=$(BUILD)/tb_xyflags.vvp FUSE_LABEL="X / Y corpus" \
	  $(PYTHON) $(SCRIPTS)/compare_fuse_rtl.py

xyflags_pz80: $(BIN)/fuse_pz80
	@$(BIN)/fuse_pz80 tests/xyflags/tests.in tests/xyflags/tests.expected

XY_STUDY_ARGS ?=
xy_study: cmodel $(BIN)/fuse_pz80
	@mkdir -p $(BUILD)/xy
	$(CC) -std=gnu11 -O2 -w $(REDCODE_FLAGS) -c $(BENCH_REFS)/redcode_z80/Z80.c -o $(BUILD)/xy/redcode_Z80.o
	$(CXX) -std=gnu++17 -O2 -w -I$(SCRIPTS) -I$(CMODEL) $(REDCODE_FLAGS) \
	  $(SCRIPTS)/xy_oracles.cpp $(BUILD)/xy/redcode_Z80.o $(CMODEL_LIB) -o $(BUILD)/xy/xy_oracles
	@$(PYTHON) $(SCRIPTS)/xy_study.py $(XY_STUDY_ARGS)

# FUSE through the RTL (iverilog). Generates tb_fuse.v from tests.in, builds
# with iverilog, runs vvp, diffs results against tests.expected and
# tests/fuse/silicon.expected.
# Depends on `dirs` so the build/ output directory exists on a fresh checkout
# (the compare_fuse_rtl.py driver writes build/tb_fuse.vvp).
fuse_rtl: dirs
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

# HALT-to-INT acceptance T-state timing probe inspired by Mark Woodmass's
# HALT2INT v3 (2021): INT asserted at ten points of the halted NOP cycle,
# and each INT-to-INTA delay must equal the die's (perfectz80 gives 6 5 4 7
# 6 5 4 7 6 5 T-states for the same program). HALT2INT v3 itself needs a
# Spectrum (ROM and ULA contention); the pin sweep (make event_sweep)
# checks INT around HALT at every half-cycle against the die.
halt2int_probe: cmodel $(BIN)/halt2int_probe
$(BIN)/halt2int_probe: $(SCRIPTS)/halt2int_probe.c $(CMODEL_LIB) $(CMODEL_HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(CMODEL_LIB) -o $@

halt2int: halt2int_probe
	@$(BIN)/halt2int_probe

# Signal-timing diff vs perfectz80 (Brian Silverman et al's transistor-
# level netlist of the Visual Z80 die scan). Compares, every half-cycle, the
# control pins (mreq/iorq/rd/wr/m1/rfsh/halt), the address and the data
# under WR between our C model and the die across the trace programs (10
# hand-written + 4 seeded-random from scripts/gen_random_trace_progs.py).
# Any difference fails.
perfectz80: tracegen $(BIN)/perfectz80_runner
	@$(PYTHON) $(SCRIPTS)/compare_signal_timing.py 200

# Same gate but driving the iverilog RTL testbench (build/tb_z80.vvp) as
# the trace source instead of the C model. The pin-scenario programs with
# their .events go through the RTL too (pin_scenarios_rtl).
perfectz80_rtl: iverilog $(BIN)/perfectz80_runner
	@$(PYTHON) $(SCRIPTS)/compare_signal_timing.py --rtl=iverilog 200

# === NMOS pin timing: the chip flows' SDC =================================
# asic/timing/z80_chip_nmos4.sdc constrains a chip's pins to the NMOS Z80's
# AC characteristics at 4 MHz (Zilog PS0178, Z8400 table): generated from
# asic/timing/z8400_nmos.json by asic/timing/pin_timing.py. `make
# timing_sdc` regenerates it; `make timing_sdc_check` fails if the committed
# one is out of date. The chips themselves are checked by `make pin_timing`
# on the asic-<pdk> branches.
timing_sdc:
	$(PYTHON) asic/timing/pin_timing.py --write-sdc asic/timing/z80_chip_nmos4.sdc

timing_sdc_check: dirs
	@$(PYTHON) asic/timing/pin_timing.py --write-sdc $(BUILD)/z80_chip_nmos4.sdc
	@diff -u asic/timing/z80_chip_nmos4.sdc $(BUILD)/z80_chip_nmos4.sdc \
	  && echo "asic/timing/z80_chip_nmos4.sdc: up to date with z8400_nmos.json"

# === Formal equivalence of z80_core against a git revision ===============
# For pure restructurings (same registers, moved or renamed): proves the
# work tree's z80_core identical to REF's for every state and input --
# next-state, reset values and every pin image (scripts/equiv_rtl.py).
# Needs a native yosys on PATH (or YOSYS=...). `make equiv REF=main`.
REF ?= main
equiv:
	@$(PYTHON) $(SCRIPTS)/equiv_rtl.py $(REF)

# === LibreLane gate-level flow — "ultimate test" =============================
# Synthesise rtl/*.v through LibreLane (Yosys.Synthesis step only — no
# floorplan/PnR/STA) into a sky130 gate-level netlist, then diff its
# per-half-cycle pin behavior against the perfectz80 Visual-Z80
# gate-level netlist. The first gate that catches synthesis-introduced
# bugs (latches inferred where DFFs were intended, reset domain
# crossings folded by the synthesizer, etc.) — see docs/librelane-flow.md.
#
# `make synth` runs LibreLane via the asic/librelane/run_synth.sh wrapper,
# which expects `librelane` on PATH (nix profile install). It writes:
#   build/synth/z80_top.nl.v   — synthesized netlist (symlink)
#   build/synth/pdk_root.path   — resolved PDK_ROOT for downstream rules
synth: $(BUILD)/synth/z80_top.nl.v

# Synthesis produces both the netlist .v and a small pdk_root.path file
# (read lazily by the iverilog rule's recipe). One rule, two outputs.
# Only the netlist is declared here; pdk_root.path is a side effect.
# (Grouped-target syntax `&:` would be cleaner but is GNU Make 4.3+
# only; macOS ships 3.81.)
$(BUILD)/synth/z80_top.nl.v: $(RTL_SRCS) asic/librelane/config.json asic/librelane/run_synth.sh
	@mkdir -p $(BUILD)/synth
	asic/librelane/run_synth.sh

# Build the gate-level iverilog testbench. The sky130_fd_sc_hd Verilog
# cell models live under $(PDK_ROOT)/sky130A/libs.ref/sky130_fd_sc_hd/verilog/
# (PDK_ROOT resolved at synth time by run_synth.sh and persisted to
# build/synth/pdk_root.path, which this recipe reads).
#
# `-DFUNCTIONAL` picks the zero-delay functional cell models — much
# faster than spec-block timing models and sufficient for the logical
# correctness diff against perfectz80 (which is also unit-delay).
iverilog_netlist: synth $(BUILD)/tb_z80_netlist.vvp

$(BUILD)/tb_z80_netlist.vvp: $(TESTS)/iverilog/tb_z80_netlist.v $(BUILD)/synth/z80_top.nl.v
	@if [ ! -f $(BUILD)/synth/pdk_root.path ]; then \
	    echo "iverilog_netlist: $(BUILD)/synth/pdk_root.path missing — run \`make synth\` first"; exit 1; \
	  fi; \
	  PDK_ROOT=$$(cat $(BUILD)/synth/pdk_root.path); \
	  SKY130_V_DIR=$$PDK_ROOT/sky130A/libs.ref/sky130_fd_sc_hd/verilog; \
	  if [ ! -d "$$SKY130_V_DIR" ]; then \
	    echo "iverilog_netlist: sky130 verilog models not found at $$SKY130_V_DIR"; exit 1; \
	  fi; \
	  echo "== building gate-level iverilog sim (sky130 cells) =="; \
	  $(IVERILOG) -g2012 -DFUNCTIONAL -s tb_z80 -o $@ \
	    $(TESTS)/iverilog/tb_z80_netlist.v \
	    $(BUILD)/synth/z80_top.nl.v \
	    $$SKY130_V_DIR/primitives.v \
	    $$SKY130_V_DIR/sky130_fd_sc_hd.v && \
	  echo "Built $@"

# Gate-level netlist vs perfectz80. Default program set: 10 hand + 4
# random = 14 programs (the script's default when called without
# explicit paths); pin_scenarios_netlist runs the pin scenarios.
perfectz80_netlist: iverilog_netlist $(BIN)/perfectz80_runner
	@$(PYTHON) $(SCRIPTS)/compare_signal_timing.py --rtl=netlist 200

# Gate-level BASIC. Same sim_basic.cpp testbench (68B50 ACIA, NASCOM /INT
# wiring, --exit-on sentinel, etc.) but Verilator builds it against the
# LibreLane-synthesised netlist + sky130 cells instead of source RTL.
#
# "Real software" running across millions of cycles of synthesised gates —
# any ROM-boot regression that the rtl/ -> synth/ transformation
# introduces (latches, glitches, async-reset folding) shows up here long
# before it would on the trace programs.
#
# Wall clock: gate-level Verilator is 10-50× slower than source-RTL
# Verilator. With the --exit-on sentinel terminating each subtest soon
# after its DONE marker prints, expect ~5-15 min total for the 4 canned
# subtests vs ~2 s for the source-RTL leg.
verilator_basic_netlist: synth dirs
	@if [ ! -f $(TESTS)/verilator/sim_basic.cpp ]; then echo "sim_basic.cpp not present."; exit 0; fi; \
	if [ ! -f $(BUILD)/synth/pdk_root.path ]; then \
	  echo "verilator_basic_netlist: $(BUILD)/synth/pdk_root.path missing — run \`make synth\`"; exit 1; \
	fi; \
	printf '#include <cstdio>\nint main(){return 0;}\n' > $(BUILD)/.cxxcheck.cpp; \
	if ! c++ -std=gnu++17 -c $(BUILD)/.cxxcheck.cpp -o $(BUILD)/.cxxcheck.o >/dev/null 2>&1; then \
	  echo "SKIP verilator_basic_netlist: host C++17 toolchain cannot compile libc++ headers."; \
	  exit 0; \
	fi; \
	PDK_ROOT=$$(cat $(BUILD)/synth/pdk_root.path); \
	SKY130_V=$$PDK_ROOT/sky130A/libs.ref/sky130_fd_sc_hd/verilog; \
	if [ ! -d "$$SKY130_V" ]; then \
	  echo "verilator_basic_netlist: sky130 verilog not found at $$SKY130_V"; exit 1; \
	fi; \
	echo "== building verilator gate-level sim_basic_netlist (sky130) =="; \
	$(VERILATOR) --cc --exe --build -j 0 -O3 -Wall \
	  -Wno-fatal -Wno-WIDTH -Wno-CASEINCOMPLETE -Wno-UNUSEDSIGNAL \
	  -Wno-MULTITOP -Wno-MODDUP -Wno-PINMISSING -Wno-TIMESCALEMOD \
	  --Mdir $(BUILD)/obj_dir_basic_netlist --top-module z80_top \
	  +define+FUNCTIONAL +define+UNIT_DELAY \
	  -CFLAGS -DNETLIST_BUILD \
	  -I$$SKY130_V \
	  $$SKY130_V/primitives.v $$SKY130_V/sky130_fd_sc_hd.v \
	  $(BUILD)/synth/z80_top.nl.v \
	  $(abspath $(TESTS)/verilator/sim_basic.cpp) -o sim_basic_netlist && \
	echo "Built $(BUILD)/obj_dir_basic_netlist/sim_basic_netlist"

# Run the canned BASIC subtests through the gate-level Verilator
# binary. Same scripts as basic_rtl_tests, same sentinel-driven early
# exit; just a different backend.
basic_netlist_tests: verilator_basic_netlist
	@$(TESTS)/basic/run_basic_tests.sh netlist
# =============================================================================


# Pin-scenario programs (prog9..prog23). Each drives a specific external-
# pin event sequence via the .events sidecar (INT, NMI, WAIT, BUSREQ,
# RESET, ...) and must match perfectz80 in every phase: the control pins,
# the address and the data under WR. A difference fails the target.
PIN_SCENARIOS = \
	  $(TESTS)/traces/pin_scenarios/prog9_inta_im1.hex \
	  $(TESTS)/traces/pin_scenarios/prog10_halt_nmi.hex \
	  $(TESTS)/traces/pin_scenarios/prog11_wait_mem.hex \
	  $(TESTS)/traces/pin_scenarios/prog12_inta_im2.hex \
	  $(TESTS)/traces/pin_scenarios/prog13_halt_int.hex \
	  $(TESTS)/traces/pin_scenarios/prog14_wait_io.hex \
	  $(TESTS)/traces/pin_scenarios/prog15_busreq_m1.hex \
	  $(TESTS)/traces/pin_scenarios/prog16_ei_delay.hex \
	  $(TESTS)/traces/pin_scenarios/prog17_reset.hex \
	  $(TESTS)/traces/pin_scenarios/prog18_di_then_int.hex \
	  $(TESTS)/traces/pin_scenarios/prog19_nmi_in_int.hex \
	  $(TESTS)/traces/pin_scenarios/prog20_block_int.hex \
	  $(TESTS)/traces/pin_scenarios/prog21_wait_ldir.hex \
	  $(TESTS)/traces/pin_scenarios/prog22_wait_cpir.hex \
	  $(TESTS)/traces/pin_scenarios/prog23_wb_irq.hex

pin_scenarios: tracegen $(BIN)/perfectz80_runner
	@$(PYTHON) $(SCRIPTS)/compare_signal_timing.py 200 $(PIN_SCENARIOS)

# Same pin-scenarios but driving the iverilog RTL testbench (.events is
# wired into tb_z80.v via per-pin plusargs -- see the tests/iverilog/tb_z80.v
# initial block).
pin_scenarios_rtl: iverilog $(BIN)/perfectz80_runner
	@$(PYTHON) $(SCRIPTS)/compare_signal_timing.py --rtl=iverilog 200 $(PIN_SCENARIOS)

# Same pin-scenarios through the LibreLane-synthesised sky130 gate-level
# netlist (tb_z80_netlist.v consumes the same plusargs).
pin_scenarios_netlist: iverilog_netlist $(BIN)/perfectz80_runner
	@$(PYTHON) $(SCRIPTS)/compare_signal_timing.py --rtl=netlist 200 $(PIN_SCENARIOS)

# The pin sweep: the iverilog RTL and the C model against perfectz80, half-
# cycle by half-cycle, pins and pad drivers: RESET / INT / NMI pulses at
# every T-state of a set of instructions (one position per clock half; sub-T
# pulses at every 1/16 T), BUSREQ / WAIT walks, every opcode, the timing
# classes, the data latch, combinations and random fuzz; the die's traces
# are recorded in tests/event_sweep/pz80/. Fails on any difference from the
# die, and on any half-cycle where the C model differs from the RTL; see
# docs/event-sweep.md.
event_sweep: $(BUILD)/tb_event_sweep.vvp $(BIN)/event_sweep_pz80 $(BIN)/event_sweep_c
	@$(PYTHON) $(SCRIPTS)/event_sweep.py --c-model $(EVENT_SWEEP_ARGS)

$(BIN)/event_sweep_c: $(SCRIPTS)/event_sweep_c.c $(CMODEL_LIB) $(CMODEL_HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(INCLUDES) $< $(CMODEL_LIB) -o $@

$(BUILD)/tb_event_sweep.vvp: $(RTL_SRCS) $(TESTS)/iverilog/tb_event_sweep.v
	@mkdir -p $(BUILD)
	$(IVERILOG) -g2001 -I$(RTL) -s tb_event_sweep -o $@ $(RTL_SRCS) $(TESTS)/iverilog/tb_event_sweep.v

$(BIN)/event_sweep_pz80: $(SCRIPTS)/event_sweep_pz80.c $(SCRIPTS)/pz80_pads.c $(SCRIPTS)/pz80_pads.h \
                         $(SCRIPTS)/refs/perfectz80/perfectz80.c $(SCRIPTS)/refs/perfectz80/netlist_sim.c
	@mkdir -p $(BIN)
	$(CC) -std=c99 -O2 -I$(SCRIPTS) $(filter %.c,$^) -o $@

$(BIN)/perfectz80_runner: $(SCRIPTS)/perfectz80_runner.c $(SCRIPTS)/refs/perfectz80/perfectz80.c $(SCRIPTS)/refs/perfectz80/netlist_sim.c
	@mkdir -p $(BIN)
	$(CC) -std=c99 -O2 -I$(SCRIPTS) $^ -o $@

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

# Canned-input BASIC ROM regression via the C model. Pipes each
# tests/basic/scripts/<name>.in through basicrunner, greps for the
# substrings in <name>.expect. NASCOM scripts use --autostart; Tiny
# scripts run as-is. See tests/basic/run_basic_tests.sh.
basic_c_tests: basicrunner
	@$(TESTS)/basic/run_basic_tests.sh c

# Same subtests as basic_c_tests but driven through the Verilated RTL.
# Verilator is ~20x slower per cycle than the C model, but the harness's
# --exit-on sentinel handling keeps each subtest's actual sim work to
# ~100-150K instructions — the suite finishes in seconds, not minutes.
basic_rtl_tests: verilator_basic
	@$(TESTS)/basic/run_basic_tests.sh rtl

# Back-compat alias kept until external callers migrate.
basic_tests: basic_c_tests

# ----------------------------------------------------------------------------
# RTL elaboration / sims
# ----------------------------------------------------------------------------
rtl:
	@if [ -z "$(RTL_SRCS)" ]; then echo "No RTL sources yet."; else \
	  echo "== elaborating RTL with iverilog =="; \
	  $(IVERILOG) -g2001 -I$(RTL) -t null $(RTL_SRCS) && echo "RTL elaboration OK"; fi

.PHONY: bus_controls
bus_controls: dirs
	@set -e; for tb in tb_bus_controls tb_wait_edge tb_wait_write tb_int_ack; do \
	  $(IVERILOG) -g2012 -I$(RTL) -s $$tb -o $(BUILD)/$$tb.vvp \
	    $(RTL_SRCS) $(TESTS)/iverilog/$$tb.v; \
	  $(VVP) $(BUILD)/$$tb.vvp; \
	done

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
	$(VERILATOR) --cc --exe --build -j 0 -Wall \
	  -Wno-fatal -Wno-WIDTH -Wno-CASEINCOMPLETE -Wno-UNUSEDSIGNAL \
	  --Mdir $(BUILD)/obj_dir --top-module z80_top \
	  -I$(RTL) $(RTL_SRCS) $(abspath $(TESTS)/verilator/sim_main.cpp) -o sim_z80 && \
	echo "Built $(BUILD)/obj_dir/sim_z80"

# -Wno-UNUSEDSIGNAL above silences a recurring "wide compute, only carry
# bit consumed" pattern (e.g. rtl/z80_alu.v lo_add/lo_sub, whose low four
# bits go unused) -- those wires are declared at their full width as the
# die's adders produce both sum and carry; only the carry feeds HF.

# ZEX runner driven through the Verilated RTL — apples-to-apples with the C
# zexrunner but every cycle simulated at gate-level-equivalent fidelity. Same
# C++17 self-check as `verilator:` so the macOS dev box skips gracefully.
# The .cpp source MUST be an absolute path: Verilator 5.020 (Ubuntu 24.04 apt)
# leaves the cpp path as-given in the inner Makefile rule, which then fails
# because the inner make runs from inside --Mdir. Verilator 5.042 (Homebrew)
# rewrites it to "../../..." automatically. abspath sidesteps the difference.
verilator_zex: dirs
	@if [ ! -f $(TESTS)/verilator/sim_zex.cpp ]; then echo "sim_zex.cpp not present."; exit 0; fi; \
	printf '#include <cstdio>\nint main(){return 0;}\n' > $(BUILD)/.cxxcheck.cpp; \
	if ! c++ -std=gnu++17 -c $(BUILD)/.cxxcheck.cpp -o $(BUILD)/.cxxcheck.o >/dev/null 2>&1; then \
	  echo "SKIP verilator_zex: host C++17 toolchain cannot compile libc++ headers."; \
	  exit 0; \
	fi; \
	echo "== building verilator sim_zex =="; \
	$(VERILATOR) --cc --exe --build -j 0 -O3 -Wall \
	  -Wno-fatal -Wno-WIDTH -Wno-CASEINCOMPLETE -Wno-UNUSEDSIGNAL \
	  --Mdir $(BUILD)/obj_dir_zex --top-module z80_top \
	  -I$(RTL) $(RTL_SRCS) $(abspath $(TESTS)/verilator/sim_zex.cpp) -o sim_zex && \
	echo "Built $(BUILD)/obj_dir_zex/sim_zex"

# Run ZEXALL through the Verilated RTL. sim_zex prints the same Cringle
# OK/ERROR per-subtest lines as the C zexrunner (via BDOS console out).
# Verilator is ~20x slower than the C model; ZEXALL takes several hours
# of wall clock, so CI runs the 14-test subset below instead.
zexall_rtl: verilator_zex
	@$(BUILD)/obj_dir_zex/sim_zex tests/zex/zexall.com 12000000000

# Curated 14-test subset of ZEXALL chosen to keep the Verilator wall clock
# under an hour while exercising the instructions most prone to error
# (LDIR/LDDR + variants, CPIR/CPDR, DDCB BIT/SET/RES/INC/DEC/SHF, RRD/RLD,
# NEG, DAA/CPL/SCF/CCF with their X / Y flags, 16-bit ADC/SBC HL,rp).
# The C model runs the subset in ~2 min / ~550 M instructions; Verilator
# on ubuntu-latest takes 25-55 min wall clock, depending on the runner.
#
# The .com is generated from tests/zex/zexall.com by patching the
# 67-entry test-pointer table at file offset 0x3A; everything else
# (driver loop, BDOS shim, CRC tables, all 67 test data blocks)
# stays in place. See scripts/zex_make_subset.py.
tests/zex/zexall_subset.com: tests/zex/zexall.com scripts/zex_make_subset.py
	@$(PYTHON) $(SCRIPTS)/zex_make_subset.py $< $@ \
	  12 0 56 57 52 53 54 55 10 11 8 27 59 62

zexall_subset_c: zexrunner tests/zex/zexall_subset.com
	@$(BIN)/zexrunner tests/zex/zexall_subset.com 1000000000

zexall_subset_rtl: verilator_zex tests/zex/zexall_subset.com
	@$(BUILD)/obj_dir_zex/sim_zex tests/zex/zexall_subset.com 1000000000

# Patrik Rak z80test through the Verilated RTL. Mirrors verilator_zex /
# zexall_subset_rtl shape: a single sim_z80test binary loads any z80test
# .tap variant and reports pass/fail. Verilator is ~20x slower than the C
# model -- locally each variant runs in 6-15 min; CI runs one variant per
# job (z80test-rtl).
verilator_z80test: dirs
	@if [ ! -f $(TESTS)/verilator/sim_z80test.cpp ]; then echo "sim_z80test.cpp not present."; exit 0; fi; \
	printf '#include <cstdio>\nint main(){return 0;}\n' > $(BUILD)/.cxxcheck.cpp; \
	if ! c++ -std=gnu++17 -c $(BUILD)/.cxxcheck.cpp -o $(BUILD)/.cxxcheck.o >/dev/null 2>&1; then \
	  echo "SKIP verilator_z80test: host C++17 toolchain cannot compile libc++ headers."; \
	  exit 0; \
	fi; \
	echo "== building verilator sim_z80test =="; \
	$(VERILATOR) --cc --exe --build -j 0 -O3 -Wall \
	  -Wno-fatal -Wno-WIDTH -Wno-CASEINCOMPLETE -Wno-UNUSEDSIGNAL \
	  --Mdir $(BUILD)/obj_dir_z80test --top-module z80_top \
	  -I$(RTL) $(RTL_SRCS) $(abspath $(TESTS)/verilator/sim_z80test.cpp) -o sim_z80test && \
	echo "Built $(BUILD)/obj_dir_z80test/sim_z80test"

# The same z80test variants through the RTL (Verilator); any failed test
# fails the target, as in `z80test`.
z80test_rtl: verilator_z80test
	@set -e; for v in $(Z80TEST_VARIANTS); do \
	  stdbuf -oL $(BUILD)/obj_dir_z80test/sim_z80test $(TESTS)/z80test/$$v.tap 8000000000; \
	done

# BASIC ROM driver through the Verilated RTL (sim_basic). Builds the
# Verilator harness analogously to verilator_zex. Same C++17 self-check
# pattern as verilator: so macOS dev box skips gracefully.
verilator_basic: dirs
	@if [ ! -f $(TESTS)/verilator/sim_basic.cpp ]; then echo "sim_basic.cpp not present."; exit 0; fi; \
	printf '#include <cstdio>\nint main(){return 0;}\n' > $(BUILD)/.cxxcheck.cpp; \
	if ! c++ -std=gnu++17 -c $(BUILD)/.cxxcheck.cpp -o $(BUILD)/.cxxcheck.o >/dev/null 2>&1; then \
	  echo "SKIP verilator_basic: host C++17 toolchain cannot compile libc++ headers."; \
	  exit 0; \
	fi; \
	echo "== building verilator sim_basic =="; \
	$(VERILATOR) --cc --exe --build -j 0 -O3 -Wall \
	  -Wno-fatal -Wno-WIDTH -Wno-CASEINCOMPLETE -Wno-UNUSEDSIGNAL \
	  --Mdir $(BUILD)/obj_dir_basic --top-module z80_top \
	  -I$(RTL) $(RTL_SRCS) $(abspath $(TESTS)/verilator/sim_basic.cpp) -o sim_basic && \
	echo "Built $(BUILD)/obj_dir_basic/sim_basic"

# ----------------------------------------------------------------------------
# C model vs the RTL, phase by phase (tests/traces programs; Verilator joins
# when build/obj_dir/sim_z80 exists)
# ----------------------------------------------------------------------------
compare: tracegen iverilog
	@$(PYTHON) $(SCRIPTS)/compare_traces.py

# ----------------------------------------------------------------------------
# compatibility suites
# ----------------------------------------------------------------------------
# ZEX_MAX caps instructions so a broken build can't loop forever (ZEXDOC and
# ZEXALL run 5.76 G instructions; the C model takes ~20 min for each).
ZEX_MAX ?= 8000000000

prelim: zexrunner
	@$(PYTHON) $(SCRIPTS)/run_zex.py prelim $(ZEX_MAX)

zexdoc: zexrunner
	@$(PYTHON) $(SCRIPTS)/run_zex.py zexdoc $(ZEX_MAX)

zexall: zexrunner
	@$(PYTHON) $(SCRIPTS)/run_zex.py zexall $(ZEX_MAX)

# ----------------------------------------------------------------------------
# phase identity: the work tree's model against a reference revision
# ----------------------------------------------------------------------------
# Every pin of every phase (the 14 trace columns + data_drive, as a rolling
# hash) and the register file every 1M phases, through ZEXDOC, prelim,
# z80test, all trace programs and pin scenarios, random memory soups and a
# random data bus under random INT/NMI/WAIT/BUSREQ/RESET activity
# (scripts/phase_identity.py, scripts/pinhash.c). PHASE_REF=<rev> picks the
# reference (default HEAD: proves uncommitted work changes no pin);
# PHASE_ARGS=--long runs ZEXDOC and z80test to the end.
PHASE_REF  ?= HEAD
PHASE_ARGS ?=
# pinhash also hashes the die's pad drivers when the model has them
# (z80_pins_t addr_drive / ctl_drive), and reads the registers as the
# program sees them when it has z80_arch; phase_identity.py decides both
# the same way for the reference
PINHASH_DEFS := $(if $(shell grep -l addr_drive $(CMODEL)/z80.h 2>/dev/null),-DPINHASH_DRIVES) \
                $(if $(shell grep -l z80_arch $(CMODEL)/z80.h 2>/dev/null),-DPINHASH_ARCH)
pinhash: cmodel $(BIN)/pinhash
$(BIN)/pinhash: $(SCRIPTS)/pinhash.c $(CMODEL_LIB) $(CMODEL_HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(PINHASH_DEFS) $(INCLUDES) $< $(CMODEL_LIB) -o $@

phase_identity: pinhash
	@$(PYTHON) $(SCRIPTS)/phase_identity.py --ref $(PHASE_REF) --new-exe $(BIN)/pinhash $(PHASE_ARGS)

# ----------------------------------------------------------------------------
# cfast: the phase engine generated from the model's timing tables
# ----------------------------------------------------------------------------
# cfast/dump_tables.c prints cmodel/'s timing ROM and timing points;
# cfast/gen_fast.py turns them into straight-line phase steps that
# cfast/z80_fast.c runs around the model's own sequencer, with
# z80_phase_step for what the steps don't cover: z80f_tick, one phase per
# call (zexrunner_fast), and z80f_run, the steps back to back with the
# bus inlined (cfast/z80_fast_run.h, zexfast). make cfast_identity checks
# both phase by phase against PHASE_REF.
CFAST     := cfast
CFAST_GEN := $(BUILD)/cfast
CFAST_INC := -I$(CMODEL) -I$(CFAST) -I$(CFAST_GEN)
CFAST_DEP := $(CFAST)/z80_fast.c $(CFAST)/z80_fast.h $(CFAST_GEN)/z80_fast_steps.h \
             $(CMODEL_SRCS) $(CMODEL_HDRS)

cfast: $(BIN)/zexrunner_fast $(BIN)/pinhash_fast $(BIN)/zexfast $(BIN)/pinhash_run

$(CFAST_GEN)/dump_tables: $(CFAST)/dump_tables.c $(CMODEL_SRCS) $(CMODEL_HDRS)
	@mkdir -p $(CFAST_GEN)
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@

$(CFAST_GEN)/tables.json: $(CFAST_GEN)/dump_tables
	$< > $@

$(CFAST_GEN)/z80_fast_steps.h: $(CFAST)/gen_fast.py $(CFAST_GEN)/tables.json
	$(PYTHON) $(CFAST)/gen_fast.py $(CFAST_GEN)/tables.json $(CFAST_GEN)

$(BIN)/zexrunner_fast: $(SCRIPTS)/zexrunner.c $(CFAST_DEP)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -DZ80_FAST $(CFAST_INC) $< $(CFAST)/z80_fast.c $(CMODEL)/z80_sim.c -o $@

$(BIN)/pinhash_fast: $(SCRIPTS)/pinhash.c $(CFAST_DEP)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(PINHASH_DEFS) -DZ80_FAST $(CFAST_INC) $< $(CFAST)/z80_fast.c $(CMODEL)/z80_sim.c -o $@

# the run frame: cfast/zexfast.c, and pinhash compiled around z80f_run
$(BIN)/zexfast: $(CFAST)/zexfast.c $(CFAST)/z80_fast_run.h $(CFAST_DEP)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(CFAST_INC) $< -o $@

$(BIN)/pinhash_run: $(SCRIPTS)/pinhash.c $(CFAST)/z80_fast_run.h $(CFAST_DEP)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(PINHASH_DEFS) -DZ80_FAST_RUN $(CFAST_INC) $< $(CMODEL)/z80_sim.c -o $@

cfast_identity: cfast pinhash
	@$(PYTHON) $(SCRIPTS)/phase_identity.py --ref $(PHASE_REF) --new-exe $(BIN)/pinhash \
	  --fast-exe $(BIN)/pinhash_fast --run-exe $(BIN)/pinhash_run $(PHASE_ARGS)

# cfast as one header (stb style, cfast/amalgamate.py): the model, the
# generated steps, z80f_tick and z80f_run. cfast_single_identity checks the
# header's two frames like cfast_identity.
CFAST_SINGLE := $(CFAST_GEN)/z80_cfast.h
cfast_single: $(CFAST_SINGLE) $(BIN)/zexfast_single $(BIN)/pinhash_single

$(CFAST_SINGLE): $(CFAST)/amalgamate.py $(CFAST)/z80_fast_run.h $(CFAST_DEP)
	$(PYTHON) $(CFAST)/amalgamate.py $(CFAST_GEN) $@

$(BIN)/zexfast_single: $(CFAST)/zexfast.c $(CFAST_SINGLE)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -DZEX_SINGLE -I$(CFAST_GEN) $< -o $@

$(BIN)/pinhash_single: $(SCRIPTS)/pinhash.c $(CFAST_SINGLE)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $(PINHASH_DEFS) -DZ80_FAST_RUN -DZ80_SINGLE -I$(CMODEL) -I$(CFAST_GEN) $< $(CMODEL)/z80_sim.c -o $@

cfast_single_identity: cfast_single pinhash
	@$(PYTHON) $(SCRIPTS)/phase_identity.py --ref $(PHASE_REF) --new-exe $(BIN)/pinhash \
	  --fast-exe $(BIN)/pinhash_single --run-exe $(BIN)/pinhash_single $(PHASE_ARGS)

# cfast_bench: cfast's two frames, the reference model and the four other C
# emulators in scripts/refs/ on the same ZEXDOC instructions, each in the
# harness of cfast/bench/ (the oracles at -O2, redcode with the features the
# lockstep CI job builds it with); scripts/cfast_bench.py writes
# build/bench/bench.{md,json,svg}. CI runs it on every push (job cfast-bench).
# e.g. make cfast_bench CFAST_BENCH_ARGS="--cpu 3 --rounds 5"
CFAST_BENCH_ARGS ?=
BENCH_REFS := $(SCRIPTS)/refs
BENCH_BINS := $(BIN)/bench_chips $(BIN)/bench_superzazu $(BIN)/bench_redcode $(BIN)/bench_suzukiplan
REDCODE_FLAGS := -I$(BENCH_REFS)/redcode_z80 -I$(BENCH_REFS)/redcode_z80/Zeta \
  -DZ80_STATIC -DZ80_WITH_LOCAL_HEADER -DZ80_WITH_EXECUTE -DZ80_WITH_Q \
  -DZ80_WITH_PARITY_COMPUTATION -DZ80_WITH_SPECIAL_RESET -DZ80_WITH_FULL_IM0 \
  -DZ80_WITH_UNOFFICIAL_RETI

cfast_bench: cfast $(BIN)/zexrunner $(BENCH_BINS)
	@$(PYTHON) $(SCRIPTS)/cfast_bench.py --bin $(BIN) --out $(BUILD)/bench \
	  $(if $(JCC_PAD),--jcc-pad) $(CFAST_BENCH_ARGS)

$(BIN)/bench_chips: $(CFAST)/bench/bench_chips.c $(CFAST)/bench/bench_common.h
	@mkdir -p $(BIN)
	$(CC) -std=gnu11 -O2 $(JCC_PAD) -w -I$(BENCH_REFS) $< -o $@

$(BIN)/bench_superzazu: $(CFAST)/bench/bench_superzazu.c $(CFAST)/bench/bench_common.h
	@mkdir -p $(BIN)
	$(CC) -std=gnu11 -O2 $(JCC_PAD) -w -I$(BENCH_REFS) $< -o $@

$(BIN)/bench_redcode: $(CFAST)/bench/bench_redcode.c $(CFAST)/bench/bench_common.h
	@mkdir -p $(BIN)
	$(CC) -std=gnu11 -O2 $(JCC_PAD) -w $(REDCODE_FLAGS) $< $(BENCH_REFS)/redcode_z80/Z80.c -o $@

$(BIN)/bench_suzukiplan: $(CFAST)/bench/bench_suzukiplan.cpp $(CFAST)/bench/bench_common.h
	@mkdir -p $(BIN)
	$(CXX) -std=gnu++17 -O2 $(JCC_PAD) -w -I$(BENCH_REFS) $< -o $@

# ----------------------------------------------------------------------------
# full suite
# ----------------------------------------------------------------------------
test: ctest rtl bus_controls compare
	@echo "== quick test suite complete =="

# Every gate that needs only a C compiler, iverilog and Verilator, as CI
# runs them (CI adds the LibreLane netlist jobs and cfast_bench). ZEXDOC and
# ZEXALL take ~20 min each, the pin sweep ~15 min on 8 cores, the Verilator
# z80test variants 6-15 min each: about an hour and a half in all.
all-tests: ctest fuse fuse_rtl fuse_pz80 prelim z80test z80test_rtl halt2int lockstep \
           silicon_cycles silicon_async rtl bus_controls verilator compare perfectz80 perfectz80_rtl \
           pin_scenarios pin_scenarios_rtl basic_c_tests basic_rtl_tests event_sweep \
           cfast_identity cfast_single_identity zexdoc zexall zexall_subset_rtl
	@echo "== all-tests complete =="

# Our C model and four other emulators (superzazu, floooh/chips, suzukiplan,
# redcode/Z80) instruction by instruction over ZEXDOC3, 7.0 M instructions:
# PC, AF, BC, DE, HL, IX, IY and SP after each must agree
# (scripts/lockstep_quint.c; see there for the one masked difference).
lockstep: cmodel
	@mkdir -p $(BIN)
	$(CC) -std=gnu11 -O2 -w $(REDCODE_FLAGS) -c $(BENCH_REFS)/redcode_z80/Z80.c -o $(BUILD)/redcode_Z80.o
	$(CXX) -std=gnu++17 -O2 -w -I$(SCRIPTS) -I$(CMODEL) $(REDCODE_FLAGS) \
	  $(SCRIPTS)/lockstep_quint.c $(BUILD)/redcode_Z80.o $(CMODEL_LIB) -o $(BIN)/lockstep_quint
	@$(BIN)/lockstep_quint tests/zex/zexdoc3.com 20000000

# === FPGA LUT budget ========================================================
# `make luts` synthesizes z80_top alone for iCE40 (synth_ice40 -abc9 -dsp),
# ECP5 and Gowin GW5A with the same yosys recipe as the fpga-* board builds
# and prints LUT / carry / mux / FF counts (scripts/fpga_luts.sh,
# scripts/fpga_luts_table.py). `make luts_netlist` then runs each mapped
# netlist (yosys' own cell models flattened in) through the gate-level
# testbench against perfectz80 (the 14 trace programs, as perfectz80_netlist), so
# an area change is checked at the gates it produced.
#
# Toolchain: yowasp-yosys (WebAssembly yosys, pip) in .venv-fpga/, created
# on first use. Override with YOSYS=<path> to use a native yosys >= 0.50.
LUTS_FAMILIES := ice40 ecp5 gowin
LUTS_VENV     := .venv-fpga
YOSYS         ?= $(abspath $(LUTS_VENV))/bin/yowasp-yosys
.PHONY: luts luts_netlist luts_venv
luts_venv: $(LUTS_VENV)/.ok
$(LUTS_VENV)/.ok:
	$(PYTHON) -m venv $(LUTS_VENV)
	$(LUTS_VENV)/bin/pip install -q yowasp-yosys
	@touch $@

luts: $(foreach f,$(LUTS_FAMILIES),$(BUILD)/luts/$(f)/stat.json)
	@$(PYTHON) $(SCRIPTS)/fpga_luts_table.py $(LUTS_FAMILIES)

$(BUILD)/luts/%/stat.json: $(RTL_SRCS) $(wildcard $(RTL)/*.vh) $(SCRIPTS)/fpga_luts.sh | $(LUTS_VENV)/.ok
	@echo "== yosys: z80_top for $* =="
	@YOSYS=$(YOSYS) $(SCRIPTS)/fpga_luts.sh $* $(BUILD)/luts/$*

$(BUILD)/luts/%/tb.vvp: $(BUILD)/luts/%/stat.json $(TESTS)/iverilog/tb_z80_netlist.v
	$(IVERILOG) -g2012 -s tb_z80 -o $@ $(TESTS)/iverilog/tb_z80_netlist.v \
	  $(BUILD)/luts/$*/z80_top_sim.v

luts_netlist: $(foreach f,$(LUTS_FAMILIES),$(BUILD)/luts/$(f)/tb.vvp) $(BIN)/perfectz80_runner
	@for f in $(LUTS_FAMILIES); do \
	  echo "== $$f-mapped netlist vs perfectz80 =="; \
	  TB_NETLIST_VVP=$(abspath $(BUILD))/luts/$$f/tb.vvp NETLIST_LABEL="$$f-mapped" \
	    $(PYTHON) $(SCRIPTS)/compare_signal_timing.py --rtl=netlist 200 || exit 1; \
	done

clean:
	rm -rf $(BUILD)
	@echo "cleaned."
