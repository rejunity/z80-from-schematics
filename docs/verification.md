# Verification

The verification state of the C model, the RTL and cfast, layer by layer:
what checks what, against which reference, with which `make` target and
CI job. [tests/README.md](../tests/README.md) has the per-suite detail;
[known-differences.md](known-differences.md) lists every difference from
the die found so far, open and resolved.

No check tolerates a failure: every suite below passes in full, and a
single difference fails its target and its CI job. The two lists that
could hold exceptions are empty or replaced by checked values: the pin
sweep's `tests/event_sweep/baseline.txt` is empty, and FUSE's six cases
where FUSE's expected values are not the die's are checked against the
die's values (`tests/fuse/silicon.expected`), not skipped.

## References

The die is the reference wherever it can be: **perfectz80**, the
transistor netlist of the Zilog NMOS Z80 from the Visual Z80 die scan
(Brian Silverman et al.; `scripts/refs/perfectz80/`), simulated
switch-level, ~10 k half-cycles a second. Then suites whose expected values
came from real chips: Patrik Rak's z80test (CRCs recorded on real
Spectrums), Cringle's ZEXDOC / ZEXALL (CRCs from a real Z80), the sigrok
logic-analyser captures of a KC85 (a U880, the East German Z80 clone),
and Mark Woodmass's 2008 suite (a NEC D780C-1; on branch `speccy`). FUSE's
corpus came from FUSE's own core, so it is checked on the die
(`make fuse_pz80`). Four other emulators are a cross-check, not a
reference.

## Layers

| # | Layer | Target | CI job | Result |
|---|-------|--------|--------|--------|
| 1 | C unit tests (`tests/common/`, 13 binaries) | `make ctest` | `c-tests` | 1069 / 1069 checks |
| 2 | FUSE corpus, final state of 1356 cases | `make fuse`, `make fuse_rtl` | `c-tests`, `rtl-tests` | 1356 / 1356 (C model and RTL) |
| 3 | FUSE corpus on the die | `make fuse_pz80` | `parity-tests` | 1356 / 1356 |
| 4 | Cringle's exercisers through the C model | `make prelim`, `make zexdoc`, `make zexall` | `c-tests`, `zexdoc`, `zexall` | PASS, 67 / 67, 67 / 67 |
| 5 | ZEXALL's 14 most error-prone tests through the RTL | `make zexall_subset_rtl` | `zexall-subset-rtl` (main, nightly) | 14 / 14 |
| 6 | Patrik Rak z80test: z80doc, z80flags, z80memptr, z80full | `make z80test`, `make z80test_rtl` | `z80test`, `z80test-rtl` (one variant per job) | 160 / 160 each, C model and RTL |
| 7 | C model ↔ iverilog ↔ Verilator, every phase of 14 trace programs | `make compare` | `parity-tests` | identical |
| 8 | Trace programs against the die: control pins, address and data under WR every half-cycle | `make perfectz80`, `make perfectz80_rtl` | `parity-tests` | 14 / 14, C model and RTL |
| 9 | Pin scenarios against the die (15 programs driving INT / NMI / WAIT / BUSREQ / RESET) | `make pin_scenarios`, `make pin_scenarios_rtl` | `parity-tests` | 15 / 15, C model and RTL |
| 10 | The pin sweep against the die: pins and pad drivers every half-cycle, 37,846 cases (RESET / INT / NMI / BUSREQ / WAIT at every T-state, every opcode, 86 timing classes, the data latch, combinations, fuzz); C model = RTL every half-cycle | `make event_sweep` | `event-sweep` (4 shards; nightly also RESET at every half-cycle of every opcode, 27,768 cases, and fresh fuzz) | 37,846 / 37,846 |
| 11 | Synthesised netlist (LibreLane, sky130) against the die: trace programs and pin scenarios | `make perfectz80_netlist`, `make pin_scenarios_netlist` | `librelane-netlist` | 14 / 14, 15 / 15 |
| 12 | BASIC ROMs (NASCOM 4.7c, 1K Tiny BASIC) on the C model, the RTL and the netlist | `make basic_c_tests`, `basic_rtl_tests`, `basic_netlist_tests` | `basic-c-tests`, `basic-rtl-tests`, `librelane-basic-netlist` | 4 / 4 subtests each |
| 13 | HALT-to-INT delays, ten INT positions, equal to the die's | `make halt2int` | `c-tests` | 10 / 10 |
| 14 | Lockstep with superzazu, floooh/chips, suzukiplan and redcode/Z80 over ZEXDOC3 | `make lockstep` | `c-tests` | 7,022,691 instructions identical |
| 15 | Real silicon: per-opcode T-states and pin-edge offsets from the KC85 captures | `make silicon_cycles`, `make silicon_async` | `c-tests` | PASS |
| 16 | Model changes: every pin of every phase against a reference revision (3.84 G phases: ZEXDOC, prelim, z80test, trace programs, pin scenarios, random soups and buses under random pin events) | `make phase_identity` | — (run by hand) | identical |
| 17 | cfast against the model, both frames and the single header, the same scenarios | `make cfast_identity`, `make cfast_single_identity` | `cfast-bench` (cfast_identity) | identical |
| 18 | Bus-contract testbenches: WAIT edges, writes through Tw, INTA, decoder skew | `make bus_controls` | `rtl-tests` | PASS |

Beyond `ci.yml`: `targets.yml` builds the core for every FPGA board and
ASIC PDK and runs their simulations (main pushes, nightly); `transistor.yml`
runs the whole GF180 chip at transistor level against the RTL and the
datasheet's AC limits (by hand); `spice-pads.yml` re-characterises the I/O
pads in ngspice and fails when they drift from the committed results
beyond small numeric tolerances; `macos.yml` builds and runs the cfast
identities on macOS (main pushes). Woodmass's 2008 suite (Flags 47 / 47,
MEMPTR 58 / 58) runs on branch `speccy` (`make speccy_woodmass`), outside
the regular set; see [tests/woodmass/README.md](../tests/woodmass/README.md).

`make all-tests` runs every layer except those on the LibreLane netlist
(11, and 12's netlist leg); `make test` is the quick subset (ctest, rtl,
bus_controls, compare).

## What each layer catches

  - **Instruction results** (layers 1-6, 12, 14): every documented and
    undocumented result: flags with X and Y, WZ (MEMPTR), the block
    instructions' repeat flags, Q (the flags SCF / CCF read their X and Y
    from), I / R, interrupts and HALT.
  - **Timing** (layers 7-11, 13, 15): every pin in every half-cycle,
    against the die, under every pin event the sweep places, plus the pad
    drivers (which pins the die drives or floats when).
  - **Equivalence of implementations** (layers 7, 10, 16, 17): C model =
    RTL = synthesised netlist = cfast, phase by phase.

What no layer reads directly: the die's internal state beyond what the
pins show and what FUSE's final state includes (registers, WZ, I, R);
three Q differences found in October 2026 by probing the die showed
that gap (see known-differences.md). Their fix, and Q, F and WZ recorded
from perfectz80 next to the pins, are on branch `nmos-q`.

## Notes from the record

FUSE through the RTL first passed 1342 of 1356; two issues, both fixed:
the testbench pokes `m_addr` as well as PC after reset (the in-flight
M1 otherwise read `mem[0]`), and the ALU operand mux reads `rf[hlp]`
through a continuous wire, as iverilog 12's `always @*` did not see the
array index through a function call.

The pin scenarios with INT once pulsed it between the CPU's sampling
edges, so no interrupt was ever taken and they passed without testing
it; since 2026-10-21 they hold INT across an instruction's end.

## See also

  - [architecture.md](architecture.md) — the shared C / RTL design contract.
  - [known-differences.md](known-differences.md) — every difference from
    the die, open and resolved, plus the multi-oracle status table.
  - [event-sweep.md](event-sweep.md) — the pin sweep.
  - [oracles.md](oracles.md) — the other emulators.
  - [real-silicon-traces.md](real-silicon-traces.md) — how the sigrok KC85
    captures are used as a real-silicon T-state / sub-T-state oracle.
  - [research-notes.md](research-notes.md) — catalog of sources informing
    this core and the source-conflict precedence we follow.
