# LibreLane gate-level flow

The goal of the `tests-librelane` branch — the **ultimate test**: take
our Verilog RTL, push it through LibreLane to a real synthesized
**gate-level netlist** on an open PDK (sky130), then run the resulting
netlist through `iverilog` against the same trace programs our existing
C / RTL / perfectz80 tests use. If the synthesized gates produce the
same per-half-cycle pin behavior as our source RTL and the perfectz80
Visual-Z80 netlist, the design is silicon-faithful all the way down to
the cells.

End deliverable: `make perfectz80_netlist` — a third comparison path
alongside `make perfectz80` (C model) and `make perfectz80_rtl` (source
iverilog RTL), this one driving a yosys-synthesized sky130 netlist.


## Scope

### What we do

  - Run **LibreLane's synthesis stage only** (Yosys + sky130 cell
    mapping) on `rtl/*.v`.
  - Capture the post-synthesis netlist (`z80_core.nl.v`) — gates + flops
    expressed in `sky130_fd_sc_hd_*` cells.
  - Build a new iverilog testbench `tests/iverilog/tb_z80_netlist.v` that
    `\`include`s the sky130 cell-model Verilog library, instantiates the
    synthesized netlist module, and emits the same 14-column bus-cycle
    trace the existing harnesses use.
  - Wire `make perfectz80_netlist` into the comparison pipeline + a new
    CI job that runs on every push.

### What we don't do

  - **No floorplanning, placement, routing, CTS, or STA.** The full ASIC
    flow takes much longer and produces no signals our gate-level sim
    needs that synthesis alone doesn't already give us.
  - **No SDF back-annotation.** Gate-level sim uses unit-delay (or
    zero-delay) timing of the cell library models. This is enough for
    *logical* gate-level correctness; real timing closure is a separate
    project.
  - **No pin-scenario programs in v1.** The pin-scenarios under
    `tests/traces/pin_scenarios/` need the `.events` sidecar wired into
    the iverilog testbench — both `tb_z80.v` and `tb_z80_netlist.v`.
    Since `tb_z80.v` doesn't have it yet either, this is a separate
    followup ([tests/README.md](../tests/README.md) "Forward-looking").

### Why "ultimate test"

Each verification layer answers a different question:

  - `ctest` / FUSE / ZEX answer "does the C model behave like a Z80?"
  - `make compare` answers "does the C model = iverilog RTL = Verilator?"
  - `make perfectz80` / `_rtl` answer "does our source design = a known
    gate-level netlist?"
  - **`make perfectz80_netlist` answers "does our design *after a real
    synthesis tool has chewed on it* still behave like a Z80?"**

That last question is the one that bites first-time chip designers: RTL
that passes simulation but doesn't survive synthesis — latches inferred
where DFFs were intended, mis-encoded resets, lint-suppressed glitches
that gates expose, async-reset domain crossings the synthesiser folds
into combinational paths, etc.


## RTL pre-flight

(From a scout of `rtl/*.v`.)

  - 4 files, **1,625 lines** of synthesizable Verilog total.
  - Single clock (`clk`); single async reset (`reset_n`).
  - **No** `initial`, `$display`, `$readmem*`, `$finish`, `$random` in
    any RTL file.
  - **No** latches inferred — all `always @*` blocks are complete-case
    combinational; all storage is `always @(posedge clk or negedge
    reset_n)`.
  - **No** clock gating; no `negedge clk` in RTL (only in testbench).
  - **`dbg_t` / `dbg_phi` / `dbg_m` are already top-level OUTPUT ports**
    of `z80_core` (`rtl/z80_core.v:23-25`). Synthesis will preserve
    them, so the gate-level testbench can read traces via the port
    interface, no hierarchical reference needed.

No RTL changes anticipated. The design was written with synthesis in
mind ("Z80 ready for FPGA/ASIC").


## Tooling — native LibreLane via Nix

Install path: **Nix** (the LibreLane project's first-class non-Docker
option). Same install method on macOS (local dev) and Ubuntu (CI), so
no host-OS forking in our scripts.

**Critical:** the `nix-cache.fossi-foundation.org` substituter MUST be
configured (URL + trusted public key) before `nix profile install` —
otherwise nix tries to rebuild LibreLane's pinned iverilog snapshot
from source, whose self-test suite has 1 flaky case on x86_64-linux
("Ran 297, Failed 1") and the build fails. URL + public key (from the
[LibreLane Linux install docs](https://librelane.readthedocs.io/en/latest/installation/nix_installation/installation_linux.html)):

  - `https://nix-cache.fossi-foundation.org`
  - `nix-cache.fossi-foundation.org:3+K59iFwXqKsL7BNu6Guy0v+uTlwsxYQxjspXzqLYQs=`

| Concern             | Approach                                                              |
|---------------------|-----------------------------------------------------------------------|
| Local install (Mac) | Determinate Systems installer with `--extra-conf` for the fossi-foundation substituter, then `nix profile install github:librelane/librelane`. See the project's [Linux install docs](https://librelane.readthedocs.io/en/latest/installation/nix_installation/installation_linux.html) — the Mac install page documents the same flags. |
| CI install (Ubuntu) | `DeterminateSystems/nix-installer-action@main` with `extra-conf:` input containing the same substituter + public key + `extra-experimental-features = nix-command flakes`. Then `nix profile install github:librelane/librelane`. |
| First-time cost     | Local: ~5 min for the closure download. CI: same on cold runner; ~30 s on warm GHA cache. |
| Subsequent runs     | Local: instant (warm nix store). CI: ~30 s with `magic-nix-cache-action` on `/nix/store`. |

Why not pip? `pip install librelane` exists but expects `yosys` + `openroad`
already on PATH; Ubuntu's apt yosys is too old, and macOS doesn't have an
OpenROAD package, so we'd be back to a per-OS install regardless. Nix
solves both with one toolchain.

Why not Docker? User direction — local devs prefer not to maintain
Docker Desktop on their machines.

### PDK

`sky130A` — SkyWater 130 nm open PDK. Free, well-supported, the default
that LibreLane ships against. The sky130_fd_sc_hd standard cell library
(HD variant — "high density") covers everything our Z80 needs. The Nix
LibreLane install bundles the PDK as a dependency — no separate install
step.


## Concrete steps

### Step 1 — LibreLane config (`librelane/config.json`)

```json
{
  "DESIGN_NAME": "z80_core",
  "VERILOG_FILES": [
    "../rtl/z80_alu.v",
    "../rtl/z80_pla.v",
    "../rtl/z80_timing.v",
    "../rtl/z80_core.v"
  ],
  "CLOCK_PORT": "clk",
  "CLOCK_NET": "clk",
  "CLOCK_PERIOD": 50,
  "DESIGN_IS_CORE": true,
  "PDK": "sky130A",
  "STD_CELL_LIBRARY": "sky130_fd_sc_hd"
}
```

`CLOCK_PERIOD` 50 ns is conservative (20 MHz) — easily met by sky130 for
a small design. We don't care about the actual target frequency for a
sim-only flow; synthesis just needs a clock to constrain on.

### Step 2 — Synthesis-only runner (`librelane/run_synth.py`)

Invoke the LibreLane Classic flow stopping at `Yosys.Synthesis`:

```python
from librelane.flows import Flow
flow = Flow.factory.get("Classic")(
    config_file="config.json",
    design_dir=".",
)
flow.start(last_run="latest", to="Yosys.Synthesis")
```

Output netlist path:
`librelane/runs/<timestamp>/final/nl/z80_core.nl.v`. The Makefile
sym-links the latest run's netlist into `build/synth/z80_core.nl.v` for
stable paths.

(Exact LibreLane Python API may have moved between versions. The
fallback if the `flow.start(to=...)` form doesn't exist: run from the
CLI as `librelane --to Yosys.Synthesis config.json`. We'll resolve at
execution time.)

### Step 3 — Gate-level testbench (`tests/iverilog/tb_z80_netlist.v`)

Mirror of `tests/iverilog/tb_z80.v` (same memory + program loader +
14-column trace dump), but:

  - Includes the sky130 cell-model Verilog:
    ```verilog
    `include "<PDK>/sky130A/libs.ref/sky130_fd_sc_hd/verilog/primitives.v"
    `include "<PDK>/sky130A/libs.ref/sky130_fd_sc_hd/verilog/sky130_fd_sc_hd.v"
    ```
  - The `<PDK>` path comes from a `+pdk=` plusarg or environment variable
    (`PDK_ROOT`); the Makefile sets it from `nix-shell --run env`'s
    `PDK_ROOT`.
  - Instantiates `z80_core` from the synthesized netlist (same module
    name, same port list — synthesis preserves the port interface).
  - Does NOT define `USE_POWER_PINS` — the cell models auto-tieoff their
    `VPWR`/`VGND`/`VPB`/`VNB` pins in that mode.
  - Same `dump` task, same `+prog=` / `+phases=` plusargs.

### Step 4 — Make targets

```make
PDK_ROOT ?= $(shell nix-shell -p librelane --run 'echo $$PDK_ROOT' 2>/dev/null)
LIBRELANE = nix run github:librelane/librelane --

# Synthesis: librelane → sky130 netlist
synth: $(BUILD)/synth/z80_core.nl.v

$(BUILD)/synth/z80_core.nl.v: rtl/*.v librelane/config.json librelane/run_synth.py
	cd librelane && $(LIBRELANE) python3 run_synth.py
	mkdir -p $(BUILD)/synth
	ln -sf $$(ls -dt librelane/runs/*/final/nl/z80_core.nl.v | head -1) $@

# Gate-level iverilog testbench
iverilog_netlist: synth $(BUILD)/tb_z80_netlist.vvp

$(BUILD)/tb_z80_netlist.vvp: tests/iverilog/tb_z80_netlist.v $(BUILD)/synth/z80_core.nl.v
	iverilog -g2012 -DFUNCTIONAL -o $@ $^ \
	  -I $(PDK_ROOT)/sky130A/libs.ref/sky130_fd_sc_hd/verilog

# Gate-level vs perfectz80 — the gate of record for this branch
perfectz80_netlist: iverilog_netlist $(BIN)/perfectz80_runner
	@$(PYTHON) $(SCRIPTS)/compare_signal_timing.py --rtl=netlist 200 \
	  tests/traces/prog1.hex tests/traces/prog2.hex \
	  tests/traces/prog3_cb.hex tests/traces/prog4_ed.hex \
	  tests/traces/prog_rnd_01.hex
```

`-DFUNCTIONAL` is the sky130 cell-lib flag that picks the zero-delay
functional model — much faster than the spec-block timing model and
sufficient for logical correctness.

### Step 5 — Compare-script `--rtl=netlist` arm

`scripts/compare_signal_timing.py` already supports `--rtl=iverilog` and
`--rtl=verilator`. Adding `--rtl=netlist` is a one-line dispatch — it
runs `vvp build/tb_z80_netlist.vvp +prog=<prog> +phases=<N>` and parses
the same 14-column trace.

### Step 6 — Program set

All 12 programs that the C and source-RTL legs run: 8 hand-crafted
(`prog1.hex`..`prog8_nmi.hex`) + 4 seeded-random (`prog_rnd_01.hex`..
`prog_rnd_04.hex`). 200 phases each. Gate-level iverilog is ~5-10×
slower than RTL — so ~10-30 s per program. Total: under 7 min of sim
after synthesis.

The 5-program starter set (`prog1`, `prog2`, `prog3_cb`, `prog4_ed`,
`prog_rnd_01`) confirmed control-pin parity locally — synthesis-time
green — before expanding to the full 12. Pin-scenarios stay C-only
until `.events` lands in the iverilog testbenches (separate followup).

### Step 7 — CI job (`librelane-netlist`)

New job in `.github/workflows/ci.yml`. Decision: **every push** during
development (the user wants fast feedback while iterating; the cost is
~5 min/push assuming cache hits).

```yaml
librelane-netlist:
  name: LibreLane gate-level sim vs perfectz80
  runs-on: ubuntu-latest
  timeout-minutes: 45
  steps:
    - uses: actions/checkout@v5
    - uses: DeterminateSystems/nix-installer-action@main
    - uses: DeterminateSystems/magic-nix-cache-action@main   # GitHub Actions cache for /nix/store
    - name: Install LibreLane (nix profile)
      run: nix profile install github:librelane/librelane
    - name: Cache synthesized netlist on rtl/ + config hash
      id: synth-cache
      uses: actions/cache@v4
      with:
        path: build/synth/z80_core.nl.v
        key: synth-${{ hashFiles('rtl/*.v', 'librelane/config.json', 'librelane/run_synth.py') }}
    - name: Install C toolchain + iverilog
      run: |
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends \
          build-essential python3 iverilog
    - name: Run synthesis (skipped on cache hit)
      if: steps.synth-cache.outputs.cache-hit != 'true'
      run: make synth
    - name: Run gate-level sim vs perfectz80
      run: make perfectz80_netlist
    - uses: actions/upload-artifact@v6
      if: always()
      with:
        name: synth-netlist
        path: build/synth/z80_core.nl.v
        retention-days: 30
```

Cache key is hash of `rtl/*.v` + the LibreLane config + the runner
script. Any change to RTL or to the flow re-runs synthesis; most pushes
don't touch those, so most CI runs skip the slow synth step and just
do the ~30 s gate-level sim.

### Step 8 — `tests/README.md` row

Once green, add a row to the Quick Reference table:

| LibreLane gate-level vs perfectz80 | `librelane/` + `tests/iverilog/tb_z80_netlist.v` | `make perfectz80_netlist` | ~5 min cold / ~30 s warm | yosys-synthesized sky130 netlist diffed against the Visual-Z80 gate-level netlist |

Plus a new short category section in tests/README.md explaining the
flow.


## Risks / gotchas

1. **Nix install time on first CI run**: ~5–8 min. The
   `magic-nix-cache-action` caches the nix store across runs so
   subsequent jobs reuse the LibreLane closure. Worst case the cache
   key invalidates when LibreLane is updated and we eat one cold run.

2. **Cell-library `specify` blocks**: sky130 Verilog has pin-to-pin
   timing definitions. Solved by `-DFUNCTIONAL` (zero-delay functional
   model). If we ever want timing-annotated sim, we'd drop the flag and
   tolerate the slowdown.

3. **Power pins**: sky130 cells expose `VPWR`/`VGND`/`VPB`/`VNB`. We
   don't define `USE_POWER_PINS` so the cells auto-tieoff — saves
   boilerplate.

4. **Reset polarity**: We use `reset_n` (async, active-low). Yosys maps
   it to whichever sky130 cells implement async-reset DFFs (the
   `sky130_fd_sc_hd__dfrtp` family). The testbench drives `reset_n`
   identically pre- and post-synthesis.

5. **`dbg_*` survival**: Top-level output ports driven by simple
   register assigns. Synthesis preserves them. If for any reason yosys
   optimised them away we'd add `(* keep = "true" *)` attributes — but
   we don't expect to need to.

6. **macOS local nix install**: Determinate Systems' installer handles
   macOS cleanly; the LibreLane Nix flake supports darwin. If a user
   doesn't have nix, `make synth` errors out with a clear pointer to
   the installer command.

7. **Random programs at gate level**: They already surfaced two
   loader-parity bugs in the source-RTL flow. At gate level they may
   surface new audit items — that's the *point* of "ultimate test." Any
   divergence is a real finding, not a regression.

8. **First-time disk usage**: nix-store with the LibreLane closure is
   ~3 GB. Subsequent runs share it.


## What this branch ships when done

  - `docs/librelane-flow.md` — **this file**, kept up to date.
  - `librelane/config.json` + `librelane/run_synth.py` — flow config.
  - `tests/iverilog/tb_z80_netlist.v` — gate-level testbench.
  - `Makefile` — new targets `synth`, `iverilog_netlist`,
    `perfectz80_netlist`.
  - `scripts/compare_signal_timing.py` — `--rtl=netlist` arm.
  - `.github/workflows/ci.yml` — `librelane-netlist` job.
  - `tests/README.md` — new row + category section.

Merge to `main` when the gate is green on the 5-program starter set.
