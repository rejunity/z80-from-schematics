# Test suite overview

Comprehensive inventory of every test gate in the repository. The
top-level [README.md](../README.md) status table summarises pass/fail;
this document explains **what each test is**, **where it lives**, **how
to run it**, and **what it covers**. Per-subdirectory READMEs go into
the format and provenance details.

The test set follows a "concentric verification" strategy: unit tests
catch decode/flag bugs in milliseconds; full instruction exercisers
(FUSE, ZEX, z80test) catch every-bit-of-the-flag-mask kind of bug in
seconds-to-minutes; trace + gate-level diffs catch silicon-faithfulness
bugs at sub-T-state resolution; real-silicon traces ground the whole
stack.


## Quick reference

| Test                                              | Where                          | Make target                              | Wall clock      | What it covers |
|---------------------------------------------------|--------------------------------|------------------------------------------|-----------------|----------------|
| C unit tests (11 binaries)                        | `tests/common/`                | `make ctest`                             | ~2 s            | Per-instruction state + per-T-state pin sequence |
| FUSE 1356-case (Kendall 2006, **not silicon-validated**) | `tests/fuse/`                  | `make fuse`, `make fuse_rtl`             | ~5 s / ~30 s    | Per-T-state event list + final state. 1348 PASS + 8 known-FUSE-wrong (Banks fold-in + Q-leak SCF/CCF + INxR/OTxR WZ + HALT-PC convention — silicon-faithful vs FUSE expected). |
| Cringle ZEX (prelim / zexdoc / zexall / subset)   | `tests/zex/`                   | `make prelim` / `zexdoc` / `zexall` / `zexall_subset_*` | ~1 s / ~1 min / ~16 min / ~17 min RTL | CRC-based exhaustive flag-exact instruction exerciser |
| Patrik Rak z80test (doc / memptr / full)          | `tests/z80test/`               | `make z80test`                           | ~2 min          | Documented + undocumented behaviour, MEMPTR / WZ, SCF / CCF Q-leak. **160 / 160 / 160 PASS** since the 2026-06-18 Banks / Q-leak / ULA-port-parity work. |
| BASIC ROM canned-script regression                | `tests/basic/`                 | `make basic_c_tests`, `make basic_rtl_tests` | ~0.5 s / ~2 s | NASCOM 4.7c + 1 KiB Tiny BASIC running "real software" via stdin scripts |
| C ↔ iverilog ↔ Verilator phase parity             | `tests/traces/`                | `make compare`                           | ~3 s            | Per-half-cycle bus-cycle trace identity across all three harnesses |
| Gate-level vs perfectz80 (C model path)           | `tests/traces/`                | `make perfectz80`                        | ~10 s           | Per-half-cycle 7-pin parity + bus addr/data informational findings |
| Gate-level vs perfectz80 (iverilog RTL path)      | `tests/traces/`                | `make perfectz80_rtl`                    | ~15 s           | Same diff but driving the iverilog RTL testbench (silicon-faithful leg) |
| Gate-level vs perfectz80 (LibreLane synth path)   | `librelane/` + `tests/iverilog/tb_z80_netlist.v` | `make perfectz80_netlist` | ~5 min cold / ~1 min warm | yosys-synthesised sky130 gate-level netlist diffed against the Visual-Z80 gate-level netlist over all 12 trace programs (8 hand + 4 random) — the "ultimate test" |
| Gate-level BASIC ROM (LibreLane synth path)       | `librelane/` + `tests/verilator/sim_basic.cpp`   | `make basic_netlist_tests` | ~9 min (CI)         | "Real software" — NASCOM BASIC 4.7c + Tiny BASIC running on the synthesised gates (Verilator + sky130 cells). CI runs on every push. |
| Pin-scenario programs vs perfectz80 — C path      | `tests/traces/pin_scenarios/`  | `make pin_scenarios`                     | ~15 s           | INT / NMI / WAIT / BUSREQ / RESET event-driven scenarios (informational) |
| Pin-scenario programs vs perfectz80 — iverilog RTL| `tests/traces/pin_scenarios/`  | `make pin_scenarios_rtl`                 | ~15 s           | Same 12 scenarios through tb_z80.v's `.events` plusarg support (informational) |
| Pin-scenario programs vs perfectz80 — sky130 netlist | `tests/traces/pin_scenarios/` | `make pin_scenarios_netlist`           | ~30 s           | Same 12 scenarios through the LibreLane gate-level netlist (informational) |
| Real KC85 silicon sync capture                    | `tests/sigrok/`                | `make silicon_cycles`                    | ~1 s            | Per-opcode T-state count vs a real Z80 logic-analyzer capture |
| Real KC85 silicon 20 MHz capture                  | `tests/sigrok/`                | `make silicon_async`                     | ~3 s            | Real CPU clock + sub-T-state pin-edge offsets |
| Lockstep 5-way oracle on ZEXDOC3                  | `scripts/lockstep_quint.c`     | (inline, see CI)                         | ~3 s            | Instruction-by-instruction regs + memory match across all 5 emulators (mine + superzazu + chips + suzukiplan + redcode). See [`docs/oracles.md`](../docs/oracles.md). |

`make all-tests` runs every gate above in sequence.


## Categories

### 1. C unit tests — `tests/common/`

11 small C programs that each exercise one slice of the C model and
print `N/N checks passed`. The runner is just `for f in build/bin/test_*; do $f; done`.

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
| `test_pla.c`          | The PLA decode table — every entry's decoded control word |
| `test_timing.c`       | **Per-T-state external-pin sequence assertions** for every distinct M-cycle shape. 135 checks across 21 opcodes — M1 (4 / 5 T), MREAD (3 / 4 / 5 T), MWRITE, IORD (4 T with Tw), IOWR, silent-T-state cycles. Inspired by [floooh/chips-test/z80-timing.c](https://github.com/floooh/chips-test) (MIT); predicates adapted to our per-half-cycle pin conventions ([docs/timing.md](../docs/timing.md)). |

Total: **267 checks** across the 11 binaries. Run with `make ctest`.


### 2. FUSE / Frank D. Cringle — `tests/fuse/`

The canonical opcode regression test from the FUSE ZX Spectrum emulator:
1356 per-instruction test cases, each with a starting state, a list of
expected per-T-state pin events, and an expected final state. See
[tests/fuse/README.md](fuse/README.md) for the format and `tests.in` /
`tests.expected` corpus.

  - `make fuse`     — drives the C model via `scripts/fuse_runner.c`.
    Result: **1356 / 1356 (100 %)**.
  - `make fuse_rtl` — same corpus through the iverilog RTL via
    `tests/iverilog/tb_fuse.v`. Result: **1356 / 1356 (100 %)**.


### 3. Cringle ZEX — `tests/zex/`

The classic CP/M-format CRC-based instruction exerciser. Each `.com`
program internally generates billions of random register / flag /
memory inputs, runs an instruction class on each, accumulates a CRC,
and compares against a known-good value baked into the binary. See
[tests/zex/README.md](zex/README.md) for provenance.

| Target                  | Workload                                       | Result          | Notes |
|-------------------------|------------------------------------------------|-----------------|-------|
| `make prelim`           | `prelim.com` — instruction sanity              | PASS            | ~1 s |
| `make zexdoc`           | `zexdoc.com` — 67 documented-flag CRCs         | **67 / 67**     | ~1 min |
| `make zexall`           | `zexall.com` — 67 full-flag CRCs (incl X/Y)    | **67 / 67**     | ~16 min |
| `make zexall_subset_c`  | 14-test curated slice via C model              | 14 / 14         | ~90 s |
| `make zexall_subset_rtl`| Same 14 tests via Verilator RTL                | 14 / 14         | ~17 min (CI: main + nightly only) |
| `make zexall_rtl`       | Full ZEXALL via Verilator RTL (local-only)     | known fails     | not in CI — see `docs/known-differences.md` #12 / #13 |

The 14-test subset is generated by `scripts/zex_make_subset.py` from
`zexall.com` and exercises the audit-focused instruction families
(LDIR/LDDR, DDCB variants, RRD/RLD, NEG, DAA/SCF/CCF, 16-bit ADC/SBC).


### 4. Patrik Rak z80test — `tests/z80test/`

The strictest single-Z80 test suite available, distributed as ZX Spectrum
`.tap` files. ~160 micro-tests per variant, CRC-checked against known
good values. Covers things ZEX misses: MEMPTR / WZ exposure, the SCF /
CCF Q-leak, INI / IND undocumented flag bits.

`make z80test` runs three variants in sequence with per-variant
**allowed-failure baselines** so CI catches *new* drift without going red
on the gaps we already track:

| Variant         | Baseline failures allowed | Current | Reason for the gap |
|-----------------|--------------------------:|--------:|--------------------|
| `z80doc.tap`    | 2                         | 2       | `INI` / `IND` flag mask — block-I/O M-cycle ordering ([docs/simplifications.md](../docs/simplifications.md) §F) |
| `z80memptr.tap` | 2                         | 2       | `INIR` / `INDR` → NOP' Q-leak (same root cause) |
| `z80full.tap`   | 10                        | 10      | SCF / CCF Q-leak variants (NMOS vs Toshiba) + block-op chains |

See [tests/z80test/README.md](z80test/README.md) for the runner shim and
how the ZX Spectrum print path is stubbed.


### 5. BASIC ROM — `tests/basic/`

Two real BASIC ROMs running on our emulator: NASCOM BASIC 4.7c (RC2014
build, 8 KiB) and 1 KiB Tiny BASIC. Driven interactively by
`scripts/basicrunner.c`; regression-driven by canned input scripts in
`tests/basic/scripts/` via `tests/basic/run_basic_tests.sh`.

  - `make basic`           — interactive NASCOM session (C model).
  - `make tinybasic`       — interactive 1 KiB Tiny BASIC session.
  - `make basic_c_tests`   — 4 canned-script subtests via the C model (~0.5 s).
  - `make basic_rtl_tests` — same 4 subtests through Verilator RTL
    (`tests/verilator/sim_basic.cpp`). The `--exit-on` sentinel mechanism
    terminates each subtest within ~50 K instructions of seeing its
    `DONE-XYZ` marker so the RTL leg runs in ~2 s rather than minutes.

Current subtests: `nascom_arith` (integer + float arithmetic),
`nascom_loops` (FOR / NEXT / GOTO + nested), `nascom_strings`
(LEFT$ / RIGHT$ / MID$ / LEN), `tiny_arith` (Tiny BASIC arithmetic +
GOSUB / RETURN). See [tests/basic/README.md](basic/README.md) for the
ACIA model, I/O conventions, and how to add a new subtest.


### 6. Bus-cycle traces — `tests/traces/`

The silicon-faithfulness backbone. Tiny Z80 programs run for a fixed
phase budget through every harness; their per-half-cycle traces are
diffed against each other and against the perfectz80 gate-level netlist.

Each trace row is 14 columns: `t phi m addr data_o data_i mreq iorq rd
wr m1 rfsh halt busack`. All four harnesses (C `tracegen.c`, iverilog
`tb_z80.v`, Verilator `sim_main.cpp`, perfectz80) emit the same format.

#### Programs

Hand-crafted (`prog1.hex` ... `prog8_nmi.hex`):

| File              | Bytes | What it covers |
|-------------------|------:|----------------|
| `prog1.hex`       | 30    | Core base set — LD, ALU, ADD HL,rp, RLCA, CPL, HALT |
| `prog2.hex`       | 18    | Branch / control — DJNZ, CALL, JR, RET, HALT |
| `prog3_cb.hex`    | 24    | CB-prefix rotates / shifts / BIT / RES / SET |
| `prog4_ed.hex`    | 56    | ED-prefix 16-bit ALU, IM, RETI / RETN, LD (nn),rp |
| `prog5_ddfd.hex`  | 43    | DD / FD (IX / IY) prefix — LD IX,nn, LD A,(IX+d), INC (IX+d) |
| `prog6_block.hex` | 33    | ED block ops — LDI / LDIR / CPI / CPIR (both termination paths) |
| `prog7_ddcb.hex`  | 25    | DDCB / FDCB `(IX+d)` bit ops + undocumented copy-into-r |
| `prog8_nmi.hex`   | 16    | NMI handling — ack M1, push, jump to 0066h |

Seeded-random (`prog_rnd_01.hex` ... `prog_rnd_04.hex`):

  - 48 bytes each, seeds 1..4. Generated by
    `scripts/gen_random_trace_progs.py` from a curated opcode pool that
    excludes JP / JR / CALL / RET / HALT (which would jump or stop in
    undefined ways inside the trace window). Catches patterns nobody
    thought to hand-write.
  - To add more: `scripts/gen_random_trace_progs.py 5 48 tests/traces/prog_rnd_05.hex`
    — the new program is picked up by all gates automatically (the
    compare scripts glob `prog_rnd_*.hex`).

Pin-scenario (`tests/traces/pin_scenarios/prog9` ... `prog20`):

  - 12 programs driving external input pins (`int`, `nmi`, `wait`,
    `busreq`, `reset`) on a deterministic phase schedule via a
    `<prog>.events` sidecar file.
  - Sidecar format: `<phase>  <pin>  <0|1>` per line.
  - Sidecar is now consumed by **all four** harnesses: C `tracegen`,
    `perfectz80_runner`, the iverilog testbench (`tb_z80.v`), and the
    Verilator harness (`sim_main.cpp`). The RTL paths take the sidecar
    via per-pin `+<pin>_lo=N +<pin>_hi=M` plusargs that
    `compare_signal_timing.py` derives from the sidecar.
  - Coverage: IM 1 / IM 2 INT-ack, HALT exit via NMI + via INT, WAIT
    insertion on mem + I/O, BUSREQ / BUSACK, EI shadow window, RESET
    mid-execution, DI masking, NMI during INTA, INT during LDIR.

See [tests/traces/README.md](traces/README.md) for the full program-by-program
breakdown.

#### Gates

| Make target              | What it diffs                                                     | Status |
|--------------------------|-------------------------------------------------------------------|--------|
| `make compare`           | C model ↔ iverilog ↔ Verilator (all three identical, every phase) | **PASS** on all 12 programs (8 hand + 4 random) |
| `make perfectz80`        | C model vs perfectz80 gate-level netlist (7 control pins + bus addr/data) | **12 / 12 PASS** (ctrl pins 100 %, addr 100 %, data_o 100 % across the board after Step 1 reset-init flip) |
| `make perfectz80_rtl`    | iverilog RTL vs perfectz80 (same 12 programs, RTL trace source)   | **12 / 12 PASS** — silicon-faithful leg |
| `make perfectz80_netlist`| LibreLane sky130 gate-level netlist vs perfectz80 (same 12 programs) | **12 / 12 PASS** — the "ultimate test" |
| `make pin_scenarios`     | C model vs perfectz80 on the 12 pin-scenario programs             | **8 / 12 PASS** clean (informational gate; the remaining 4 are documented per-program with root cause in [docs/perfect-branch.md](../docs/perfect-branch.md) "Remaining differences vs perfectz80") |
| `make pin_scenarios_rtl` | iverilog RTL vs perfectz80 on the 12 pin-scenarios                | **8 / 12 PASS** clean — now matches the C-path scoreboard since Step 4's reset filter was simplified (both detect immediately, harness masks the difference); remaining 4 same root causes as C path (informational) |
| `make pin_scenarios_netlist` | LibreLane sky130 gate-level netlist vs perfectz80 on the 12 pin-scenarios | **Informational**; identical RTL feeds synthesis, so the per-program counts match `pin_scenarios_rtl` modulo any sky130-cell mapping deltas (none observed currently) |

`compare_signal_timing.py` (the driver for `make perfectz80` / `_rtl` /
`_netlist`) reports informational bus-value parity — `data_o` (where wr
is low on both sides) and `addr` (compared with a **don't-care
tolerance**: phases where no data transfer is in progress on either
side — refresh windows, idle phases, T1.P setup — are treated as
matches regardless of value). Currently `data_o` is at **100 % match**
wherever it's defined; `addr` matches **100 %** on 10/12 programs, with
the residual 95-98 % on `prog_rnd_02` / `prog_rnd_03` being the
reset-register-init delta (our 0xFFFF vs perfectz80 netlist's 0x5555)
surfacing via PUSH instructions (same root cause as
[docs/known-differences.md](../docs/known-differences.md) row 1). Set
`BUS_STRICT=1` env var to promote bus diffs to gating.

`compare_signal_timing.py` also emits per-program VCD waveforms to
`build/vcd/<prog>.<source>.vcd` containing both our model's pins (top
scope) and perfectz80's pins (under `perfectz80` scope). Open with
`gtkwave` / `surfer` / similar to inspect both sides side-by-side from
a single file. Set `EMIT_VCD=0` to disable.


### 7. LibreLane gate-level — `librelane/` + `tests/iverilog/tb_z80_netlist.v`

The **ultimate test**. Pushes our Verilog RTL through LibreLane (Yosys
synthesis only — no floorplan/PnR/STA) into a sky130 gate-level netlist,
then runs that netlist through iverilog against the same trace programs
and diffs per-half-cycle pin behavior against perfectz80. Catches
synthesis-introduced bugs that pure source-RTL sim never sees: latches
inferred where DFFs were intended, async-reset domain crossings folded
into combinational paths, lint-suppressed glitches that gates expose.

```
make synth                # LibreLane synthesis → build/synth/z80_core.nl.v
make iverilog_netlist     # gate-level iverilog tb compiled w/ sky130 cell models
make perfectz80_netlist   # diff vs perfectz80 over all 12 trace programs
```

Install LibreLane via Nix (the project's first-class non-Docker path).
The fossi-foundation substituter MUST be configured at install time, or
nix tries to rebuild iverilog's pinned snapshot from source — its
self-test suite has 1 flaky case on x86_64-linux and the install fails.

    curl --proto '=https' --tlsv1.2 -fsSL https://install.determinate.systems/nix \
      | sh -s -- install --no-confirm --extra-conf "
          extra-substituters = https://nix-cache.fossi-foundation.org
          extra-trusted-public-keys = nix-cache.fossi-foundation.org:3+K59iFwXqKsL7BNu6Guy0v+uTlwsxYQxjspXzqLYQs=
          extra-experimental-features = nix-command flakes
        "
    nix profile install github:librelane/librelane

Program set: the same 12 programs the C and source-RTL legs run — 8
hand-crafted (`prog1.hex`..`prog8_nmi.hex`) + 4 seeded-random
(`prog_rnd_01.hex`..`prog_rnd_04.hex`). 200 phases each. Pin-scenarios
also run through the gate-level netlist via `make
pin_scenarios_netlist` now that `.events` is plumbed into
`tb_z80_netlist.v` (per-pin lo/hi plusargs derived from the sidecar).

**Gate-level BASIC** (`make basic_netlist_tests`) is a heavier
companion test. Same synthesised netlist, but instead of 200-phase
trace programs we run "real software" — NASCOM BASIC 4.7c cold-boots,
prints "Ok", and we feed canned input scripts through the 68B50 ACIA
ROM port and assert the expected output substrings appear, matching
the `basic_rtl_tests` pattern. Catches synthesis-introduced bugs that
take millions of cycles of ROM boot + interrupt-driven RX to manifest.
Uses Verilator (gate-level Verilator is ~10-50× slower than source-RTL
Verilator; ~9 min total CI wall clock — Verilator build ~4 min + sim
~3 min — with the --exit-on sentinel). Runs on every push.

Verilator gotcha: this needs **Verilator ≥ 5.030** for Verilog-1995
UDP-table parsing (sky130 FUNCTIONAL cells use UDPs like
`sky130_fd_sc_hd__udp_dff$P` as their DFF primitives). Ubuntu 24.04's
`apt install verilator` is 5.020 which lacks UDP support → black-boxed
DFFs → broken sim. CI installs Verilator via `nix profile install
nixpkgs#verilator`. Locally, a `nix profile install` of LibreLane gets
you a compatible Verilator too, but you may need to ensure it's on
PATH ahead of any system-installed Verilator.

See [../docs/librelane-flow.md](../docs/librelane-flow.md) for the full
plan, including the CI job, caching strategy, and risks/gotchas.

### 8. Real-silicon traces — `tests/sigrok/`

Two captures from a real KC85/2 (East-German Z80 clone running at
1.767 MHz) taken with a Saleae Logic + sigrok:

  - `kc85-cpuclk.sr` — synchronous logic-analyzer capture clocked off
    CPU CLK. Decoded by `scripts/sigrok_cycles.py` into per-opcode
    T-state counts (`make silicon_cycles`). **50 / 50 OK** classification,
    0 emulator mismatches.
  - `kc85-20mhz.sr` — 20 MHz async sample of MREQ / RD / WR / M1 +
    CPU CLK. Decoded by `scripts/sigrok_async_timing.py` to measure CPU
    clock period + each strobe's sub-T-state edge offset from the rising
    clock edge (`make silicon_async`). Confirms our model's per-half-cycle
    transitions match silicon-spec offsets.

See [docs/real-silicon-traces.md](../docs/real-silicon-traces.md) for the
full decode + interpretation notes.


### 9. 5-way oracle lockstep

Not a `make` target — runs inline in CI's `c-tests` job. The driver
`scripts/lockstep_quint.c` loads `tests/zex/zexdoc3.com` and steps
five emulators instruction-by-instruction:

  - our C model
  - superzazu's `z80.c`
  - floooh's `chips/z80.h`
  - suzukiplan's `z80.h`
  - redcode's `Z80` (Manuel Sainz de Baranda, 2023+) — most thorough
    silicon-citation FOSS Z80 emulator at time of writing

All five must agree on PC, AF, BC, DE, HL, IX, IY, SP after every
instruction. Result: **7,022,691 instructions identical** across all
five. Catches any architectural-correctness divergence independent of
Cringle's CRCs.

Per-oracle Rak runners are also available for targeted investigation:

  - `scripts/chips_z80test_runner.c`
  - `scripts/superzazu_z80test_runner.c`
  - `scripts/suzukiplan_z80test_runner.cpp`
  - `scripts/redcode_z80test_runner.c`
  - `scripts/redcode_fuse_runner.c` (drives all 1356 FUSE cases
    through redcode end-to-end)

See [`docs/oracles.md`](../docs/oracles.md) for the full per-oracle
pass/fail matrix and how we triangulate when they disagree.


## Testbench files (not directly invoked)

  - `tests/iverilog/tb_z80.v`   — iverilog testbench used by `make compare`,
    `make perfectz80_rtl`, and `make pin_scenarios_rtl`. Accepts
    `+prog=<hex>`, `+phases=<N>`, legacy `+nmi=<phase>` and per-pin
    `+<pin>_lo=N +<pin>_hi=M` plusargs (pins: `nmi`, `int`, `wait`,
    `busreq`, `reset`) for `.events` sidecar consumption.
  - `tests/iverilog/tb_fuse.v`  — testbench used by `make fuse_rtl`.
  - `tests/verilator/sim_main.cpp`  — main Verilator driver (mirror of `tb_z80.v`).
  - `tests/verilator/sim_zex.cpp`   — Verilator driver for `make zexall_subset_rtl`.
  - `tests/verilator/sim_basic.cpp` — Verilator driver for `make basic_rtl_tests`
    (68B50 ACIA emulation, `--exit-on` sentinel, NASCOM RX-interrupt
    wiring).


## CI footprint

The `.github/workflows/ci.yml` workflow has five parallel jobs that
together exercise everything above:

| Job                  | Triggers          | Includes |
|----------------------|-------------------|----------|
| `c-tests`            | every push        | `make ctest`, `make fuse`, `make silicon_cycles`, `make silicon_async`, 4-way lockstep |
| `rtl-tests`          | every push        | `make rtl` (elaboration), `make fuse_rtl` |
| `basic-c-tests`      | every push        | `make basic_c_tests` |
| `basic-rtl-tests`    | every push        | `make basic_rtl_tests` |
| `z80test`            | every push        | `make z80test` (~2 min) |
| `parity-tests`            | every push        | `make compare`, `make perfectz80`, `make perfectz80_rtl`, `make pin_scenarios`, `make pin_scenarios_rtl` |
| `librelane-netlist`       | every push        | `make synth` + `make perfectz80_netlist` + `make pin_scenarios_netlist` — 12 trace programs + 12 pin-scenarios through synthesised sky130 netlist vs perfectz80 (~3-5 min) |
| `librelane-basic-netlist` | every push        | `make synth` + `make verilator_basic_netlist` + `make basic_netlist_tests` — NASCOM BASIC 4.7c + Tiny BASIC running on the synthesised gates (~9 min) |
| `zexdoc`                  | every push        | full ZEXDOC via C model (~18 min) |
| `zexall`                  | every push        | full ZEXALL via C model (~19 min) |
| `zexall-subset-rtl`       | main + nightly    | 14-test ZEXALL slice via Verilator RTL (~17 min) |

Branch-push builds skip the RTL ZEXALL leg; PR-to-main + cron + manual
dispatch include it. Both LibreLane jobs cache the synthesised netlist
on `hashFiles('rtl/*.v', 'librelane/config.json', 'librelane/run_synth.sh')`,
so most pushes skip the ~3 min synthesis step.


## Forward-looking — what's not yet here

Most former gaps were closed in the 2026-06-18 silicon-faithfulness
sweep (see [`docs/simplifications.md`](../docs/simplifications.md)
§F1). The new HALT-PC convention flip (Brewer 2014 / Woodmass
HALT2INT 2021) and the Banks-2018 block-repeat fold-in are silicon-
faithful in both C and RTL. Functional coverage:

  - FUSE corpus C + RTL: **1348 PASS + 8 known-FUSE-wrong (silicon-
    faithful — see [`tests/fuse/known-fuse-wrong.txt`](fuse/known-fuse-wrong.txt))**
  - Patrik Rak z80doc / z80memptr / z80full C + RTL: **160 / 160 / 160**
  - ZEXDOC / ZEXALL: PASS
  - BASIC ROM on C / Verilator / sky130 gate-level: 4/4 subtests each
  - 5-way oracle lockstep over 7 M instructions: clean
  - `make halt2int`: HALT-to-INT timing probe PASS across full
    M-cycle sweep

Remaining items, sorted by effort × impact:

  - **C1 — reset state un-force** (small). Currently our model forces
    `rf=0xFFFF` on reset; perfectz80's netlist resets to 0x5555. The
    delta surfaces via PUSH on `prog_rnd_02` / `prog_rnd_03`
    (`make perfectz80` shows 95-98 % addr match instead of 100 %).
    Changing one constant closes
    [known-differences.md](../docs/known-differences.md) row 1.
  - **Pin-scenario HALT / WAIT / BUSREQ / RESET timing diffs vs
    perfectz80** (medium). 5/12 pin_scenarios PASS clean
    (`prog9_inta_im1`, `prog12_inta_im2`, `prog16_ei_delay`,
    `prog18_di_then_int`, `prog20_block_int`); 7/12 have ctrl-pin
    timing diffs informational. The PC convention bug that was
    `prog13_halt_int`'s biggest contributor (145/200 phases) is fixed
    by the Brewer 2014 HALT-PC flip; the residual is sub-T-state
    HALT pin and WAIT-insertion timing that needs case-by-case
    cleanup. Functional behavior on every one of these is verified
    by Rak + FUSE + the `make halt2int` probe.
  - **Mark Woodmass HALT2INT v3 end-to-end** (medium-large). v3 is
    silicon-validated for ZX Spectrum 48K's ULA timing, which we
    don't model. The CPU-only property it tests (HALT exit + INT
    accept T-state) is covered by `make halt2int`. Full v3 run
    requires either Spectrum ULA emulation or a ROM-hooking
    harness more elaborate than the current `scripts/woodmass_runner.c`.
    Notes in [`tests/woodmass/README.md`](woodmass/README.md).
  - **B2 / B3 — NMI / INT sample-point precision** (medium). Would
    tighten any residual pin-trace fidelity on the remaining 7/12
    pin_scenarios (`prog10_halt_nmi` etc.); see B2/B3 in
    [`docs/simplifications.md`](../docs/simplifications.md).

Other deferred items:

<!-- A-Z80 as a second gate-level oracle: dropped. With LibreLane's
sky130-synthesised netlist providing an independent gate-level
reference alongside perfectz80's Visual-Z80 port, the third oracle
is no longer worth the integration cost. -->

  - **NMOS vs Toshiba CMOS Q-leak switch**. The current C model + RTL
    implement Zilog NMOS Q-leak behaviour in SCF / CCF X / Y flags
    (well-documented silicon-faithful default). A Toshiba CMOS variant
    (no Q-leak) is a known divergence but isn't required by any test
    we currently fail — the z80test SCF/CCF baselines are already at
    the NMOS-correct number. Deferred until a use case demands it; see
    [docs/known-differences.md](../docs/known-differences.md) row 2.
  - **Woodster `Timing_Tests-48k_v1.0`** — third-party M-cycle-shape
    regression cited by MAME PR #11522. License unclear; to evaluate.

When any of those happen, add a row to the Quick Reference table at the
top of this file rather than starting a new planning doc.
