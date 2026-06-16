# Ring 3 — A-Z80 second gate-level oracle (draft)

## Status

**Spec-only.** No code lands yet on the `tests` branch. The integration
follows the same pattern as `make perfectz80` once one of the open items
below is resolved.

## Goal

`scripts/refs/perfectz80/` is a port of the **Visual Z80** transistor-
level netlist; any reverse-engineering mistake in that netlist surfaces
as "our model agrees with the wrong reference." A second gate-level (or
schematic-level) oracle from a different lineage shrinks that risk to
something testable.

Candidate identified by the research agent: **A-Z80** by Goran Devic
(<https://github.com/gdevic/A-Z80>) — Verilog RTL extracted from the
reverse-engineered Z80 schematics + die-extracted PLA. Cycle and bus
accurate, including `nWAIT` and `nBUSRQ` (the two we currently can't
cross-check against perfectz80 because the perfectz80 C API doesn't
expose BUSREQ at all). GPL-2.0 licensed.

## Why not landed today

1. **License surface.** Our repo carries no SPDX header today; we treat
   it as proprietary. GPL-2.0 inside `scripts/refs/` would force the
   whole project to GPL-2.0 by the linking clause. Acceptable answer:
   keep A-Z80 out of the repo tree and pull it at CI time only, never
   redistribute. The CI pull is the host's clone, not our distribution.

2. **Harness shape.** A-Z80 ships its own testbenches (TestBench.v
   etc.) targeting Modelsim/Quartus/ISE. To use it as an oracle for
   our 8-program trace set we'd need a small Verilator harness that
   matches `tracegen.c`'s per-half-cycle pin-trace output. Mechanical
   work, ~150-200 LOC.

3. **PLA difference.** A-Z80 uses a die-extracted PLA table, which
   matches the Z80 mask at the level of which (op,M,T) → (mux signal)
   maps. Our model uses a behaviourally-decoded PLA (cmodel/z80_pla.c
   + rtl/z80_pla.v) that produces the same control word for documented
   opcodes but might diverge on undocumented prefixes (ED-page,
   DDCB-page, certain RST sequences). If A-Z80 surfaces those as
   differences, that's the *whole point* of having the second oracle.

## Sketch of the integration when it lands

Add to `.github/workflows/ci.yml`:

```yaml
az80-oracle:
  name: A-Z80 cross-check (gate-equivalent, second lineage)
  runs-on: ubuntu-latest
  if: github.event_name != 'pull_request'   # informational
  timeout-minutes: 30
  steps:
    - uses: actions/checkout@v5
    - name: Install C + Verilator
      run: sudo apt-get update && sudo apt-get install -y \
             build-essential python3 verilator git
    - name: Clone A-Z80 (GPL-2.0; not redistributed by us)
      run: git clone --depth=1 https://github.com/gdevic/A-Z80.git /tmp/az80
    - name: Build A-Z80 + small tracegen-shaped Verilator harness
      run: |
        cp scripts/refs/az80_harness/sim_az80.cpp /tmp/az80/   # NEW
        cd /tmp/az80
        verilator --cc --exe --build -j 0 -Wall -Wno-fatal \
          --Mdir obj_dir --top-module z80_top \
          host/cpu_tb.v /* + the rest of A-Z80's source list */ \
          sim_az80.cpp -o sim_az80
    - name: Cross-check our 8 trace programs against A-Z80
      run: make az80_check
```

And a new Makefile target `az80_check` that runs each trace program
through both `tracegen` and the A-Z80 sim_az80, diffs the pin traces,
and reports phase divergences (informational only — `|| true`).

## Hard prerequisites

- `scripts/refs/az80_harness/sim_az80.cpp` (NEW, ~200 LOC, the harness
  that mirrors `tests/verilator/sim_main.cpp`'s output shape so the
  existing `compare_signal_timing.py` diff just works against it).
- A list of which A-Z80 source files to feed Verilator (the project
  is non-trivial; needs the rtl/cpu/ + rtl/decoder/ subtree).
- Trace-program preload mechanism (A-Z80's testbench uses `$readmemh`
  with a fixed path — our harness will need to accept the program path
  on the command line).

## Acceptance

A green CI run shows our 8 control pins agree with A-Z80 phase-by-phase
just like they do with perfectz80. Any divergence is a finding; the
job is informational at first, gated once each divergence is either
attributed to A-Z80 (and added to a "known A-Z80 quirks" list) or to
our model (and fixed).

## Why this is "Ring 3 minimal"

Pulling A-Z80 in is roughly the same lift as adding perfectz80 was —
one Makefile target, one harness, one Python diff that accepts a third
input column set. Splitting it out of this session's work keeps the
review surface small. The infrastructure that Rings 1 and 2 put in
place (`compare_signal_timing.py` with bus-valid intervals, the
`<prog>.events` sidecar format) is what makes the addition cheap when
it lands.
