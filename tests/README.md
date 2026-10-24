# Test suite overview

Every test gate in the repository: what each one checks, against which
reference, where it lives, how to run it, and which CI job runs it.
Per-directory READMEs go into formats and provenance;
[docs/verification.md](../docs/verification.md) summarises the layers and
their current results.

The set follows a "concentric verification" strategy: unit tests catch
decode and flag bugs in milliseconds; instruction exercisers (FUSE, ZEX,
z80test) catch every bit of every flag in seconds to minutes; pin traces
against perfectz80 -- the transistor netlist of the Zilog NMOS die --
catch timing at half-cycle resolution; real-silicon captures ground the
whole stack.


## Nothing is tolerated

Every gate passes in full; one difference fails its target and its CI
job. What each harness accepts, and why:

| Gate | What it accepts | Cases accepted |
|------|-----------------|----------------|
| z80test (`make z80test`, `z80test_rtl`) | nothing: any failed test fails (the old allowed-failure argument is gone) | 0 |
| FUSE (`make fuse`, `fuse_rtl`, `fuse_pz80`) | nothing; six cases are checked against the die's values (`tests/fuse/silicon.expected`) instead of FUSE's, and all six are checked on the die | 0 skipped |
| Pin sweep (`make event_sweep`) | nothing: any difference from the die fails (the baseline file of known differences is gone) | 0 |
| perfectz80 compares (`make perfectz80`, `pin_scenarios`, `_rtl`, `_netlist`) | nothing: control pins, address and the data under WR every half-cycle, and trace lengths | 0 |
| ZEX (`make prelim`, `zexdoc`, `zexall`, the subset) | nothing: a run passes only with the completion banner and no `ERROR` | 0 |
| Lockstep (`make lockstep`) | F's X and Y (bits 5, 3) of the three emulators that predate Banks 2018 (superzazu, floooh/chips, suzukiplan): on ZEXDOC3 they differ only after 20 LDIR repeats. Against redcode/Z80 F is compared whole | X / Y of 3 oracles |
| HALT-to-INT probe (`make halt2int`) | nothing: each delay must equal the die's | 0 |
| sigrok captures (`make silicon_cycles`, `silicon_async`) | for conditional instructions either branch's T-state count (the capture carries no flags); prefix bytes are reported, not cross-checked | by design |
| BASIC (`make basic_*_tests`) | the expected substrings anywhere in the output | by design |
| I/O pads in SPICE (`spice-pads.yml`) | small numeric drift from the committed characterisation | by design |


## Quick reference

| Test | Where | Make target | CI job | Wall clock | Result |
|------|-------|-------------|--------|------------|--------|
| C unit tests (13 binaries) | `tests/common/` | `make ctest` | `c-tests` | ~2 s | 1069 / 1069 checks |
| FUSE, 1356 cases, C model | `tests/fuse/` | `make fuse` | `c-tests` | <1 s | 1356 / 1356 |
| FUSE through the RTL (iverilog) | `tests/fuse/` | `make fuse_rtl` | `rtl-tests` | ~1.5 min | 1356 / 1356 |
| FUSE on the die (perfectz80) | `tests/fuse/` | `make fuse_pz80` | `parity-tests` | ~1.5-3 min | 1356 / 1356 |
| Cringle prelim | `tests/zex/` | `make prelim` | `c-tests` | ~1 s | PASS |
| ZEXDOC, ZEXALL (C model) | `tests/zex/` | `make zexdoc`, `make zexall` | `zexdoc`, `zexall` | ~20 min each | 67 / 67 each |
| ZEXALL 14-test subset through the RTL | `tests/zex/` | `make zexall_subset_rtl` | `zexall-subset-rtl` (main, nightly) | 25-55 min | 14 / 14 |
| Patrik Rak z80test: z80doc, z80flags, z80memptr, z80full | `tests/z80test/` | `make z80test` | `z80test` | ~2 min | 160 / 160 each |
| The same through the RTL (Verilator) | `tests/z80test/` | `make z80test_rtl` | `z80test-rtl` (one job per variant) | 6-15 min a variant | 160 / 160 each |
| BASIC ROMs (NASCOM 4.7c, 1K Tiny BASIC) | `tests/basic/` | `make basic_c_tests`, `basic_rtl_tests` | `basic-c-tests`, `basic-rtl-tests` | ~1 s / ~1 min | 4 / 4 subtests |
| C ↔ iverilog ↔ Verilator, every phase | `tests/traces/` | `make compare` | `parity-tests` | ~10 s | 14 / 14 programs identical |
| Trace programs vs the die | `tests/traces/` | `make perfectz80`, `perfectz80_rtl` | `parity-tests` | ~20 s each | 14 / 14 |
| Pin scenarios vs the die | `tests/traces/pin_scenarios/` | `make pin_scenarios`, `pin_scenarios_rtl` | `parity-tests` | ~15 s each | 15 / 15 |
| The pin sweep vs the die (37,846 cases; C model = RTL) | `tests/event_sweep/`, `scripts/event_sweep.py` | `make event_sweep` | `event-sweep` (4 shards) | ~15 min on 8 cores | 37,846 / 37,846 |
| Bus-contract testbenches | `tests/iverilog/tb_{bus_controls,wait_edge,wait_write,int_ack}.v` | `make bus_controls` | `rtl-tests` | ~1 s | PASS |
| HALT-to-INT delays vs the die's | `scripts/halt2int_probe.c` | `make halt2int` | `c-tests` | <1 s | 10 / 10 |
| 5-way lockstep on ZEXDOC3 | `scripts/lockstep_quint.c` | `make lockstep` | `c-tests` | ~5 s | 7,022,691 instructions identical |
| Real KC85 captures | `tests/sigrok/` | `make silicon_cycles`, `silicon_async` | `c-tests` | ~5 s | 50 / 50, 9 / 9 opcodes |
| Synthesised netlist (sky130) vs the die | `asic/librelane/`, `tests/iverilog/tb_z80_netlist.v` | `make perfectz80_netlist`, `pin_scenarios_netlist` | `librelane-netlist` | ~5 min warm | 14 / 14, 15 / 15 |
| BASIC on the synthesised netlist | `tests/verilator/sim_basic.cpp` | `make basic_netlist_tests` | `librelane-basic-netlist` | ~9 min | 4 / 4 |
| Phase identity of a model change | `scripts/phase_identity.py`, `scripts/pinhash.c` | `make phase_identity PHASE_REF=<rev>` | — (run by hand before a model change lands) | ~2 min | identical |
| cfast vs the model (3.84 G phases) | `cfast/` | `make cfast_identity`, `cfast_single_identity` | `cfast-bench`, macOS | ~2-4 min each | identical |
| NMOS pin-timing SDC up to date | `asic/timing/` | `make timing_sdc_check` | `rtl-tests` | ~1 s | PASS |

`make all-tests` runs everything above except the LibreLane netlist gates
(about an hour and a half); `make test` is the quick subset (ctest, rtl,
bus_controls, compare).


## Categories

### 1. C unit tests — `tests/common/`

13 small C programs that each exercise one slice of the C model and print
`N/N checks passed`; `make ctest` runs them all.

| File                  | Coverage |
|-----------------------|----------|
| `test_alu.c`          | ALU operations + 4-bit nibble carry chain |
| `test_block.c`        | LDI / LDIR / CPI / CPIR / INI / OUTI etc. block ops |
| `test_cb.c`           | CB-prefix rotates / shifts / BIT / RES / SET |
| `test_ddcb.c`         | DDCB / FDCB `(IX+d)` / `(IY+d)` bit ops including undocumented copy-into-r |
| `test_ddfd.c`         | DD / FD (IX / IY) prefix instructions |
| `test_ed.c`           | ED prefix — block ops, 16-bit ADC / SBC, NEG, IM, LD I/R/A |
| `test_exec.c`         | The `z80_exec_step` dispatcher itself — every M-cycle shape |
| `test_flags.c`        | Documented + undocumented flag updates (X / Y / H / V / N) |
| `test_int.c`          | NMI / INT IM 0/1/2 acceptance, EI-delay, HALT loop |
| `test_int_ack.c`      | The interrupt acknowledge cycle pin by pin: M1 through INTA, the WAIT point, IM0 decode |
| `test_pla.c`          | The PLA decode table — every entry's decoded control word |
| `test_timing.c`       | **Per-T-state external-pin sequence assertions** for every distinct M-cycle shape (135 checks across 21 opcodes). Inspired by [floooh/chips-test/z80-timing.c](https://github.com/floooh/chips-test) (MIT); predicates adapted to our per-half-cycle pin conventions ([docs/timing.md](../docs/timing.md)). |
| `test_wait_write.c`   | WR held through wait states, write data timing |

Total: **1069 checks** across the 13 binaries.


### 2. FUSE — `tests/fuse/`

The per-opcode corpus of the FUSE Spectrum emulator: 1356 cases, each with
a starting state and an expected final state (registers, WZ, IFFs, IM,
HALTED, T-states, memory). Run through the C model (`make fuse`), the RTL
(`make fuse_rtl`) and, since FUSE's values came from FUSE's own core, on
the die (`make fuse_pz80`): the die agrees with `tests.expected` on 1350
cases; for the other six (HALT's PC, four INIR / OTIR / INDR / OTDR
repeats, a CPDR repeat) all three runners check the die's values,
`tests/fuse/silicon.expected`. Every case starts with Q = 0, as on the die
after its registers are loaded. See [fuse/README.md](fuse/README.md).


### 3. Cringle ZEX — `tests/zex/`

The classic CP/M CRC exercisers: `prelim`, `zexdoc` (documented flags) and
`zexall` (X and Y too), 67 tests each, through the C model; a 14-test
slice of ZEXALL through the Verilator RTL (the full one would take hours
there). A run passes only when it prints `Tests complete` with no
`ERROR`. See [zex/README.md](zex/README.md).


### 4. Patrik Rak z80test — `tests/z80test/`

The strictest single-Z80 suite, as ZX Spectrum `.tap` files: 160 tests per
variant, CRCs recorded on real hardware. `make z80test` runs z80doc,
z80flags, z80memptr and z80full through the C model, `make z80test_rtl`
through the RTL; all pass, and any failed test fails the target. The fifth
vendored variant, z80ccf (flags after a CCF following each instruction:
it exposes Q), fails test 129 (`EX AF,AF'`) on main, one of the three Q
differences listed in [docs/known-differences.md](../docs/known-differences.md);
branch `nmos-q` fixes them and adds z80ccf to the set. See
[z80test/README.md](z80test/README.md).


### 5. BASIC ROMs — `tests/basic/`

NASCOM BASIC 4.7c (RC2014 build, 8 KiB) and 1 KiB Tiny BASIC running
canned input scripts through `scripts/basicrunner.c` (C model),
`tests/verilator/sim_basic.cpp` (RTL) and the same harness built on the
synthesised netlist. Four subtests: `nascom_arith`, `nascom_loops`,
`nascom_strings`, `tiny_arith`. See [basic/README.md](basic/README.md).


### 6. Trace programs and pin scenarios — `tests/traces/`

Tiny Z80 programs run for a fixed number of phases (half T-states)
through every harness. Each trace row has 14 columns: `t phi m addr
data_o data_i mreq iorq rd wr m1 rfsh halt busack`; the C tracegen, the
iverilog and Verilator testbenches and `scripts/perfectz80_runner.c` emit
the same format.

  - **14 trace programs**: 10 hand-written (`prog1` ... `prog8_nmi`,
    `prog9_q_banks`, `prog10_exchange`) and 4 seeded-random
    (`prog_rnd_01` ... `prog_rnd_04`, from
    `scripts/gen_random_trace_progs.py`). 400 phases each unless a
    `<prog>.phases` file asks for more (`prog9_q_banks` 1200,
    `prog10_exchange` 800).
  - **15 pin scenarios** (`pin_scenarios/prog9_inta_im1` ...
    `prog23_wb_irq`): each drives INT / NMI / WAIT / BUSREQ / RESET from a
    `.events` sidecar (`<phase> <pin> <0|1>`), which every harness
    applies -- the RTL testbenches through per-pin `+<pin>_lo=N
    +<pin>_hi=M` plusargs.

| Make target | What it diffs |
|-------------|---------------|
| `make compare` | C model ↔ iverilog ↔ Verilator (Verilator when `make verilator` built it): every column of every phase of the trace programs |
| `make perfectz80` / `perfectz80_rtl` / `perfectz80_netlist` | the C model / the iverilog RTL / the synthesised netlist against perfectz80 on the trace programs: control pins and address every half-cycle, data wherever WR is low on both sides |
| `make pin_scenarios` / `pin_scenarios_rtl` / `pin_scenarios_netlist` | the same on the pin scenarios |

`compare_signal_timing.py` (behind the perfectz80 targets) also writes
`build/vcd/<prog>.<source>.vcd` with both sides' pins, our model at the
top scope and perfectz80 under `perfectz80` (`EMIT_VCD=0` to skip); CI
keeps them as artifacts. See [traces/README.md](traces/README.md).


### 7. The pin sweep — `scripts/event_sweep.py`

The RTL and the C model against perfectz80, half-cycle by half-cycle,
pins and pad drivers, in 37,846 cases: RESET / INT / NMI pulses at every
T-state of a set of instructions (sub-T pulses at every 1/16 T), BUSREQ
and WAIT walked through instructions, every opcode, one opcode of each
of 86 timing classes, interrupt modes 0 and 2, the data latch, combined
events and 2,000 random fuzz cases (plus 500 fresh ones every night).
perfectz80's side comes from recordings in `tests/event_sweep/pz80/`.
The C model must equal the RTL in every half-cycle. An extra group,
`opcodes_reset` (RESET at every half-cycle of every opcode, 27,768
cases, recorded), runs in the nightly CI run only (`--groups
opcodes_reset`; about 10 more minutes a shard). See
[docs/event-sweep.md](../docs/event-sweep.md).


### 8. LibreLane gate-level — `asic/librelane/` + `tests/iverilog/tb_z80_netlist.v`

The RTL through LibreLane's synthesis into a sky130 gate-level netlist,
then the same perfectz80 comparisons and the BASIC subtests on the gates:
catches what synthesis introduces (latches, folded resets, glitches).

```
make synth                    # LibreLane synthesis → build/synth/z80_top.nl.v
make iverilog_netlist         # gate-level iverilog tb compiled w/ sky130 cell models
make perfectz80_netlist       # the trace programs vs perfectz80
make pin_scenarios_netlist    # the pin scenarios vs perfectz80
make basic_netlist_tests      # BASIC on the gates (Verilator)
```

Install LibreLane via Nix. The fossi-foundation substituter MUST be
configured at install time, or nix tries to rebuild iverilog's pinned
snapshot from source — its self-test suite has 1 flaky case on
x86_64-linux and the install fails.

    curl --proto '=https' --tlsv1.2 -fsSL https://install.determinate.systems/nix \
      | sh -s -- install --no-confirm --extra-conf "
          extra-substituters = https://nix-cache.fossi-foundation.org
          extra-trusted-public-keys = nix-cache.fossi-foundation.org:3+K59iFwXqKsL7BNu6Guy0v+uTlwsxYQxjspXzqLYQs=
          extra-experimental-features = nix-command flakes
        "
    nix profile install github:librelane/librelane

Verilator for the gate-level BASIC must be **≥ 5.030** (UDP tables, used
by the sky130 FUNCTIONAL cells) and **≤ 5.048** (5.050 on mis-simulates
the same cells: NASCOM BASIC never reaches its prompt); CI pins
`nixpkgs/nixos-26.05#verilator` (5.048). Each BASIC subtest also runs
under a `--max-seconds` budget (`BASIC_MAX_SECONDS`, default 600) so a bad
simulator build fails in minutes. See
[../docs/librelane-flow.md](../docs/librelane-flow.md).


### 9. Real-silicon traces — `tests/sigrok/`

Two logic-analyser captures of a KC85 (U880, the East German Z80 clone)
running its OS:

  - `kc85-cpuclk.sr` — synchronous capture clocked off CPU CLK:
    per-opcode T-state counts and M-cycle shapes against the C model
    (`make silicon_cycles`): 50 opcodes, 50 OK.
  - `kc85-20mhz.sr` — 20 MHz asynchronous sample of MREQ / RD / WR / M1 +
    CLK: the clock period and each strobe's sub-T-state offset
    (`make silicon_async`): 9 opcodes, 9 OK.

The capture carries no register state, so a conditional instruction may
show either branch's length; prefix bytes are reported but not
cross-checked. See [docs/real-silicon-traces.md](../docs/real-silicon-traces.md).


### 10. Other emulators

`make lockstep` (`scripts/lockstep_quint.c`) loads `tests/zex/zexdoc3.com`
and steps five emulators instruction by instruction: our C model,
superzazu's `z80.c`, floooh/chips's `z80.h`, suzukiplan's `z80.h` and
redcode/Z80. PC, AF, BC, DE, HL, IX, IY and SP must agree after each of
the 7,022,691 instructions; F's X and Y are compared against redcode only
(the other three keep LDI's X / Y in a repeating LDIR). Per-oracle Rak
runners (`scripts/{chips,superzazu,suzukiplan,redcode}_z80test_runner.c`)
and `scripts/redcode_fuse_runner.c` are there for investigation; see
[docs/oracles.md](../docs/oracles.md).


### 11. Identity checks

  - `make phase_identity PHASE_REF=<rev>`: the work tree's model against
    a reference revision, every pin of every phase (and the register file
    every 1 M phases) through ZEXDOC, prelim, the four z80test variants,
    every trace program and pin scenario (at their `.phases` lengths),
    random memory soups and a random data bus under random pin events:
    3.84 G phases. Run it before a model change that should change no pin.
  - `make cfast_identity` / `cfast_single_identity`: cfast's two frames
    and the single header against the model on the same scenarios (CI:
    `cfast-bench`; macOS).


### 12. Smaller gates

  - `make bus_controls`: four iverilog testbenches for the bus contract
    (registered bus controls and decoder skew, the WAIT sampling edge, WR
    held through wait states, the INTA cycle).
  - `make halt2int` (`scripts/halt2int_probe.c`): INT asserted at ten
    points of the halted NOP cycle; each INT-to-INTA delay must equal the
    die's (6 5 4 7 6 5 4 7 6 5 T-states, from perfectz80).
  - `make timing_sdc_check`: the chips' NMOS pin-timing SDC is up to date
    with `asic/timing/z8400_nmos.json`.
  - `make equiv REF=<rev>` (by hand; needs yosys): formal equivalence of
    `z80_core` with a revision, for pure restructurings.
  - `make luts` / `luts_netlist` (by hand): LUT counts for iCE40, ECP5 and
    Gowin, and each mapped netlist against perfectz80.


## Not in the regular set

| What | Why not | Where it runs |
|------|---------|---------------|
| z80ccf (`tests/z80test/z80ccf.tap`) | test 129 fails on main until the Q fix | branch `nmos-q` adds it to `make z80test` |
| Mark Woodmass's Z80 Test Suite 2008 (`tests/woodmass/`) | a Spectrum program: needs the 48K ROM, the ULA's keyboard and interrupts | branch `speccy`: `make speccy_woodmass`, Flags 47 / 47, MEMPTR 58 / 58 ([woodmass/README.md](woodmass/README.md)) |
| Full ZEXALL through the RTL (`make zexall_rtl`) | several hours in Verilator | by hand; CI runs the 14-test subset, and ZEXALL through the C model |
| `make phase_identity` | needs a reference revision | by hand, for model changes |
| `make equiv`, `make luts_netlist` | need yosys | by hand |
| Transistor-level chip (`transistor.yml`) | half a day of ngspice | by hand ([asic/spice/README.md](../asic/spice/README.md)) |


## External suites we do not have

| Suite | What it adds | License, size | Recommendation |
|-------|--------------|---------------|----------------|
| [SingleStepTests/z80](https://github.com/SingleStepTests/z80) (JSMoo) | 1000 random cases per opcode: start and end state including WZ, Q (`q`) and the LD A,I/R state (`p`), and a per-cycle bus trace | MIT; ~1600 JSON files, ~0.7 MB each (git repo ~280 MB) | Worth adding as a downloaded, not vendored, final-state check (WZ, Q, P, every register): a nightly job or a `make` target that fetches a pinned commit. Generated from a fixed-up translation of the ares emulator's core, not from hardware, and its bus trace uses simplified 1-T strobes by default, so compare end states only, and let the die (`fuse_pz80`-style) settle disagreements |
| [ZXSpectrumNextTests](https://github.com/MrKWatkins/ZXSpectrumNextTests) (Peter Helcmanovsky): `Z80BlockInstructionFlags`, `Z80IntSkip`, `Z80CcfScfOutcomeStability` | interrupted block instructions' undocumented flags, interrupts held off by long EI / DD / FD runs, SCF / CCF X / Y stability (Q) | MIT; three small `.tap` files | Run on branch `speccy` like Woodmass's suite: all three are Spectrum programs |
| Woodmass HALT2INT v3 (2022) | HALT exit timing against the ULA | GPLv2, small | Run on `speccy`; the CPU part is covered here against the die (`make halt2int`, the sweep) |
| raxoft `z80docflags` | documented flags only, registers ignored | MIT, ~14 KB | Skip: `z80flags` checks a superset |
| Woodster `Timing_Tests-48k_v1.0` | M-cycle shapes through Spectrum contention | license unclear | Only with a license, and on `speccy` (it tests the ULA as much as the CPU) |


## Testbench files (not directly invoked)

  - `tests/iverilog/tb_z80.v`   — iverilog testbench used by `make compare`,
    `make perfectz80_rtl`, and `make pin_scenarios_rtl`. Accepts
    `+prog=<hex>`, `+phases=<N>`, legacy `+nmi=<phase>` and per-pin
    `+<pin>_lo=N +<pin>_hi=M` plusargs (pins: `nmi`, `int`, `wait`,
    `busreq`, `reset`) for `.events` sidecar consumption.
  - `tests/iverilog/tb_fuse.v`  — generated by `scripts/gen_fuse_tb.py` for `make fuse_rtl`.
  - `tests/iverilog/tb_event_sweep.v` — the pin sweep's RTL side (1/16-T event resolution).
  - `tests/iverilog/tb_z80_netlist.v` — the gate-level twin of `tb_z80.v`.
  - `tests/verilator/sim_main.cpp`  — main Verilator driver (mirror of `tb_z80.v`).
  - `tests/verilator/sim_zex.cpp`   — Verilator driver for `make zexall_subset_rtl`.
  - `tests/verilator/sim_z80test.cpp` — Verilator driver for `make z80test_rtl`.
  - `tests/verilator/sim_basic.cpp` — Verilator driver for `make basic_rtl_tests`
    (68B50 ACIA emulation, `--exit-on` sentinel, NASCOM RX-interrupt
    wiring).


## CI footprint

`.github/workflows/ci.yml` runs on every push, PR, nightly (04:00 UTC) and
on demand:

| Job | When | Runs |
|-----|------|------|
| `c-tests` | always | `make ctest`, `fuse`, `prelim`, `halt2int`, `silicon_cycles`, `silicon_async`, `lockstep` |
| `rtl-tests` | always | `make rtl`, `bus_controls`, `fuse_rtl`, `timing_sdc_check` |
| `basic-c-tests` | always | `make basic_c_tests` |
| `basic-rtl-tests` | always | `make basic_rtl_tests` |
| `z80test` | always | `make z80test` (~3 min) |
| `z80test-rtl` (4 jobs) | always | `make z80test_rtl` for one variant each |
| `parity-tests` | always | `make verilator`, `compare`, `perfectz80`, `perfectz80_rtl`, `pin_scenarios`, `pin_scenarios_rtl`, `fuse_pz80` |
| `event-sweep` (4 shards) | always; `opcodes_reset` and fresh fuzz nightly | `make event_sweep` (shard 0 also re-runs 50 recorded cases on perfectz80 live) |
| `librelane-netlist` | always | `make synth`, `iverilog_netlist`, `perfectz80_netlist`, `pin_scenarios_netlist` |
| `librelane-basic-netlist` | always | `make synth`, `verilator_basic_netlist`, `basic_netlist_tests` |
| `cfast-bench` | always | `make cfast_identity`, `cfast_bench` |
| `zexdoc`, `zexall` | always | ZEXDOC / ZEXALL through the C model (15-40 min each) |
| `zexall-subset-rtl` | main pushes, nightly, on demand | `make zexall_subset_rtl` (25-55 min) |

Both LibreLane jobs cache the synthesised netlist on
`hashFiles('rtl/*.v', 'asic/librelane/config.json', 'asic/librelane/run_synth.sh')`,
so most pushes skip the ~3 min synthesis step. `macos.yml` (main pushes)
runs `make ctest`, `fuse`, `cfast_identity`, `cfast_single_identity` and
`cfast_bench` on Apple silicon.

### Targets: FPGA boards and ASIC PDKs

`.github/workflows/targets.yml` builds main's core for every FPGA board
and ASIC PDK: on every push to main (docs-only pushes excepted), nightly
and on demand. Board and PDK sources live on their own branches, so each
job first merges its branch into main's tree.

| Job (matrix) | Branch merged | Runs |
|--------------|---------------|------|
| `fpga`: icepi-zero, tang-primer-25k, icebreaker, pico-ice, icesugar | `fpga-<board>-basic` | `make ci` in `fpga/<board>/`: sbc + chip bitstreams, the Tiny BASIC bitstream, and every board simulation (Verilator boot and Tiny BASIC at fast and real serial pacing, iverilog, gate-level where the board has one; on the iCE40 boards also both BASICs on nextpnr's routed netlist, `make sim_pnr`); timing summary; artifacts `fpga-<board>-bitstreams` (named downloads) and `fpga-<board>-logs` (`fpga-board.yml`) |
| `asic`: sky130, ihp-sg13g2, gf180mcu, gf180mcu-9t (9-track cells) | `asic-<pdk>` | signs of life on the RTL, LibreLane RTL-to-GDS, sign-off summary (fails on DRC / LVS / antenna / XOR / timing), signs of life on the routed netlist; where the branch has one (IHP, GF180, GF180 9-track), the chip: `z80_top` as a hard macro in a ring of pads on the Z80's sides (`make chip`, LibreLane Chip flow), its sign-off, the signs of life through the pads, and its pins against the NMOS Z80's datasheet at 4 MHz (`make pin_timing`); artifacts `asic-<pdk>-gds` (the GDS files, pictures and cell and block maps) and `asic-<pdk>-results` (`asic-pdk.yml`) |

The branches run the same jobs on their own pushes (`fpga-<board>.yml`,
`asic-<pdk>.yml` there). A branch that no longer merges cleanly into main
fails its job's first step: merge main into it. The FPGA tools are pip's
latest YoWASP builds, so the nightly run also catches toolchain drift.

When every board and PDK passed on main, `pages.yml` publishes that
build on GitHub Pages, <https://rejunity.github.io/z80-from-schematics/>
(it runs after Targets and after Transistor, the hand-run whole chip at
transistor level, and builds the site from both their latest runs on
main: Pages replaces the whole site on every deploy):
three pictures of each layout with its die size, cell counts and timing,
and the GDS and bitstream files under stable names (`rz80-<pdk>.gds`,
`rz80-<board>-basic|tinybasic|chip.<ext>`). The pictures also appear in
the Pages run's summary. `.github/scripts/dist.py` names the
files, draws the pictures with gdstk and Pillow -- the cell rows and the
metal stack as translucent layers (LibreLane's own KLayout render reads
badly at die scale), a cell map: every standard-cell instance in the
GDS coloured by kind, flip-flops, buffers and delays, other logic,
fill/decap/tap, and a block map: every placed cell (final DEF) coloured
by the RTL block it was synthesized in, ALU, register file, decoder PLA,
execute, address, timing, the core's sequencer, pin logic. The PDK
configs set `SYNTH_HIERARCHY_MODE: deferred_flatten` for that, so the
cell names keep their block -- and builds the page. Artifacts keep every run's files,
failed runs included, for 14 days.
