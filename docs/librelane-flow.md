# LibreLane gate-level flow — plan

The goal of this branch (`tests-librelane`) is the **ultimate test**:
take our Verilog RTL, push it through LibreLane to a real synthesized
**gate-level netlist** on an open PDK (sky130), then run the resulting
netlist through `iverilog` against the same trace programs and pin-event
sidecars our existing C / RTL / perfectz80 tests use. If the synthesized
gates produce the same per-half-cycle pin behavior as our source RTL
and the perfectz80 Visual-Z80 netlist, the design is silicon-faithful
all the way down to the cells.

The end deliverable: `make perfectz80_netlist` — a third comparison
path alongside `make perfectz80` (C model) and `make perfectz80_rtl`
(source iverilog RTL), this one driving a yosys-synthesized sky130
netlist.


## Scope

### What we do

  - Run LibreLane's **synthesis stage only** (Yosys + sky130 cell
    mapping) on `rtl/*.v`.
  - Capture the post-synthesis netlist (`z80_core.nl.v`) — gates + flops
    expressed in `sky130_fd_sc_hd_*` cells.
  - Build a new iverilog testbench `tests/iverilog/tb_z80_netlist.v` that
    `\`include`s the sky130 cell-model Verilog library, instantiates the
    synthesized netlist module, and emits the same 14-column bus-cycle
    trace format the existing harnesses use.
  - Wire a new `make perfectz80_netlist` target into the comparison
    pipeline and into a new CI job.

### What we don't do

  - **No floorplanning, placement, routing, CTS, or STA.** The full ASIC
    flow takes much longer and produces no signals our gate-level sim
    needs that synthesis alone doesn't already give us.
  - **No power-domain or DRC/LVS checks** — out of scope for a logic
    correctness test.
  - **No SDF back-annotation.** Gate-level sim uses unit-delay (or
    zero-delay) timing of the cell library models. This is enough for
    *logical* gate-level correctness; real timing closure is a separate
    project.

### Why "ultimate test"

Each verification layer answers a different question:

  - `ctest` / FUSE / ZEX answer "does the C model behave like a Z80?"
  - `make compare` answers "does the C model = iverilog RTL = Verilator?"
  - `make perfectz80` / `_rtl` answer "does our source design = a known
    gate-level netlist?"
  - **`make perfectz80_netlist` answers "does our design *after a real
    synthesis tool has chewed on it* still behave like a Z80?"**

That last question is the one that bites first-time chip designers: RTL
that passes simulation but doesn't survive synthesis (latches inferred
where DFFs were intended, mis-encoded resets, lint-suppressed glitches
that gates expose, etc.). Random + hand-written + pin-scenario trace
programs all flowing through the synthesized gates is the most stringent
correctness gate we can build without taping out.


## Status of the RTL as input

Pre-flight check (from a scout of `rtl/*.v`):

  - 4 files, **1,625 lines** of synthesizable Verilog total.
  - Single clock (`clk`); single async reset (`reset_n`).
  - **No** `initial`, `$display`, `$readmem*`, `$finish`, `$random` in any
    RTL file.
  - **No** latches inferred — all `always @*` blocks are complete-case
    combinational; all storage is `always @(posedge clk or negedge reset_n)`.
  - **No** clock gating; no `negedge clk` in RTL (only in testbench).
  - **dbg_t / dbg_phi / dbg_m are already top-level OUTPUT ports** of
    `z80_core` (`rtl/z80_core.v:23-25`). That's the critical
    synthesizability win — synthesis will preserve them, so the
    gate-level testbench can dump traces via the port interface, no
    hierarchical reference needed.

No RTL changes anticipated. The design was already written with synthesis
in mind ("Z80 ready for FPGA/ASIC").


## Tooling — LibreLane

LibreLane is the maintained successor to OpenLane (now archived);
written in Python, builds on Yosys + OpenROAD + Magic. Three install
options, in order of portability:

| Method  | Local                                    | CI                              | Footprint    |
|---------|------------------------------------------|---------------------------------|--------------|
| Docker  | `docker pull ghcr.io/librelane/librelane:latest` | First-class — direct image use | ~3–5 GB pull |
| Nix     | `nix profile install`                    | Works but rarely needed         | ~5–8 GB      |
| pip     | `pip install librelane`                  | Works; needs OpenROAD on PATH   | ~500 MB + OpenROAD ~1 GB |

**Decision: Docker.** Same image locally and in CI; no host-OS
specifics; LibreLane's documented "official" install path; works on
macOS (the only place we need it locally) via Docker Desktop. The
~5 GB pull is a one-time cost; CI caches the pulled layers.

Local users who don't have Docker get a clear error message and a
pointer to install it. The `make synth` target won't try to fall back
to a host yosys (which would diverge from CI in subtle ways).

### PDK

`sky130A` — SkyWater 130 nm open PDK. Free, well-supported, the default
that LibreLane ships against. The sky130_fd_sc_hd standard cell library
(HD variant — "high density") covers everything our Z80 needs (basic
combinational + DFFs + latches we won't use).

No special PDK config needed for synthesis-only. The Docker image
includes the PDK by default.


## Plan — concrete steps

### Step 1 — LibreLane config

Create `librelane/config.json`:

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

### Step 2 — Synthesis-only flow runner

`librelane/run_synth.py` invokes only the `Yosys.Synthesis` step:

```python
from librelane.flows import Flow
flow = Flow.factory.get("Classic")(config_file="config.json",
                                   design_dir=".",
                                   target_step="Yosys.Synthesis")
flow.start()
```

(Exact API may differ — to confirm at execution time. Worst case we run
the full Classic flow with `--to Yosys.Synthesis` on the CLI.)

Output netlist: `librelane/runs/<timestamp>/final/nl/z80_core.nl.v` —
sym-linked into `build/synth/z80_core.nl.v` by the make rule for stable
paths.

### Step 3 — Gate-level testbench `tests/iverilog/tb_z80_netlist.v`

Same structure as `tests/iverilog/tb_z80.v` (memory + program loader +
14-column trace dump) but:

  - Includes the sky130 cell-model Verilog library:
    `\`include "<PDK_ROOT>/sky130A/libs.ref/sky130_fd_sc_hd/verilog/primitives.v"`
    `\`include "<PDK_ROOT>/sky130A/libs.ref/sky130_fd_sc_hd/verilog/sky130_fd_sc_hd.v"`
  - Instantiates `z80_core` from the synthesized netlist (same module
    name; iverilog picks it up from the `-y` search path or the
    explicit netlist file we pass).
  - Drives `VPWR`/`VGND` cell-model pins implicitly via the cell's `\`define`
    `USE_POWER_PINS` handling — or just doesn't define it so the cells
    use their default tieoffs.
  - Same `dump` task, same `+prog=` / `+phases=` plusargs.

### Step 4 — Make targets

```makefile
synth: $(BUILD)/synth/z80_core.nl.v

$(BUILD)/synth/z80_core.nl.v: rtl/*.v librelane/config.json
	docker run --rm -v $(PWD):/work -w /work/librelane \
	  ghcr.io/librelane/librelane:latest \
	  python3 run_synth.py
	ln -sf librelane/runs/RUN_*/final/nl/z80_core.nl.v $(BUILD)/synth/

iverilog_netlist: synth $(BUILD)/tb_z80_netlist.vvp

$(BUILD)/tb_z80_netlist.vvp: tests/iverilog/tb_z80_netlist.v $(BUILD)/synth/z80_core.nl.v
	iverilog -g2012 -o $@ $^ -y $(PDK_ROOT)/sky130A/libs.ref/sky130_fd_sc_hd/verilog

perfectz80_netlist: iverilog_netlist $(BIN)/perfectz80_runner
	@$(PYTHON) $(SCRIPTS)/compare_signal_timing.py --rtl=netlist 200
```

Plus a `--rtl=netlist` arm in `compare_signal_timing.py` that runs
`vvp build/tb_z80_netlist.vvp` instead of `vvp build/tb_z80.vvp`.

### Step 5 — Starter program set

For the first run, restrict to the small hand-crafted programs to keep
synthesis + gate-sim wall clock low:

  - `prog1.hex` — base set
  - `prog2.hex` — branch/control
  - `prog3_cb.hex` — CB prefix
  - `prog4_ed.hex` — ED prefix
  - `prog_rnd_01.hex` — one random program for opcode-mix coverage

5 programs × 400 phases each. Gate-level iverilog typically runs 5-10×
slower than RTL — so ~30 s of wall clock per program is a reasonable
budget (vs ~2 s for RTL today). Total: ~3 min of sim after synthesis.

Once that gate is green, expand to all 12 (hand + random) and
eventually pin-scenarios (which need `.events` plumbing into tb_z80_netlist.v,
matching what tb_z80.v already lacks — see Forward-looking section of
[tests/README.md](../tests/README.md)).

### Step 6 — CI job

New job in `.github/workflows/ci.yml` — `librelane-netlist`:

```yaml
librelane-netlist:
  name: LibreLane gate-level sim vs perfectz80
  runs-on: ubuntu-latest
  # ~5 GB image pull + ~2 min synth + ~3 min gate sim. Gated to push-
  # to-main + nightly + manual dispatch (same gate as zexall-subset-rtl).
  if: >-
    (github.event_name == 'push' && (github.ref == 'refs/heads/main' || github.ref == 'refs/heads/master'))
    || github.event_name == 'schedule'
    || github.event_name == 'workflow_dispatch'
  timeout-minutes: 60
  steps:
    - uses: actions/checkout@v5
    - name: Cache synthesized netlist on rtl/ hash
      uses: actions/cache@v4
      with:
        path: build/synth/z80_core.nl.v
        key: synth-${{ hashFiles('rtl/*.v', 'librelane/config.json') }}
    - name: Pull LibreLane image (cached)
      uses: docker/setup-buildx-action@v3
    - name: Run synthesis (no-op on cache hit)
      run: make synth
    - name: Build + run gate-level sim, diff vs perfectz80
      run: make perfectz80_netlist
    - uses: actions/upload-artifact@v6
      with:
        name: synth-netlist
        path: build/synth/z80_core.nl.v
        retention-days: 30
```

The cache step is the key win: most CI runs won't touch `rtl/*.v`, so
the netlist is reused and we skip the slow synthesis. Only when RTL
genuinely changes does synth re-run.

### Step 7 — `tests/README.md` row

Once the gate is green, add a row to the Quick Reference table:

| LibreLane gate-level vs perfectz80 | `librelane/` + `tests/iverilog/tb_z80_netlist.v` | `make perfectz80_netlist` | ~5 min cold / ~3 min warm | yosys-synthesized sky130 netlist sim diffed against the gate-level Visual-Z80 netlist |

Plus a new category section explaining the flow.


## Risks / gotchas

1. **Docker on macOS**: Docker Desktop is required locally. If missing,
   `make synth` exits with a clear error pointing to install
   instructions. CI is unaffected.

2. **First image pull**: ~3–5 GB. GitHub Actions caches Docker layers on
   subsequent runs; first run on a fresh CI runner takes ~2 min just for
   the pull.

3. **Cell library `specify` blocks**: sky130 Verilog models have
   `specify` blocks defining pin-to-pin delays. `iverilog -g2012`
   handles them at sim time but is slow. If the slowdown is painful,
   pass `-D FUNCTIONAL` to the cell-lib include so it picks the
   functional (zero-delay) variant.

4. **Power pins**: sky130 cells have `VPWR` / `VGND` / `VPB` / `VNB`
   pins. If `USE_POWER_PINS` is defined, they must be tied; if not, the
   cells auto-tie. We don't define it — saves boilerplate.

5. **Reset polarity**: We use `reset_n` (async, active-low). Synthesis
   may map it to a sky130 cell with positive-reset; yosys handles the
   inversion. Pre-synthesis and post-synthesis sim should both work
   from the same `reset_n` testbench driver.

6. **`dbg_*` survival**: The scout already confirmed these are
   top-level output ports driven by simple assigns from registered
   state. Synthesis WILL preserve them. (If it didn't, we'd need to add
   `(* keep = "true" *)` attributes or just rely on the port being a
   keepalive.)

7. **Memory model timing**: The testbench `mem[]` array is unchanged
   from `tb_z80.v` — it's a sim-only memory, not part of synthesis.
   Memory accesses respond at the same per-phase cadence.

8. **Random program patterns**: The random programs already surfaced two
   loader-parity bugs on the existing flow. They may surface gate-level
   bugs too — that's the *point* of "ultimate test." Any divergence is
   a real audit item, not a regression in this branch.


## What this branch ships when done

  - `docs/librelane-flow.md` — **this file**, kept up to date.
  - `librelane/config.json` + `librelane/run_synth.py` — flow config.
  - `tests/iverilog/tb_z80_netlist.v` — gate-level testbench.
  - `Makefile` — new targets `synth`, `iverilog_netlist`,
    `perfectz80_netlist`.
  - `scripts/compare_signal_timing.py` — `--rtl=netlist` arm.
  - `.github/workflows/ci.yml` — `librelane-netlist` job.
  - `tests/README.md` — new row + category section.

When the gate is green, merge to `main` like the previous test-expansion
branches.


## Open questions for the reviewer

1. **Docker locally is OK on macOS?** Or do we want a fallback path
   (host yosys + a tarball'd sky130 PDK) for users without Docker?
   *Default proposal: Docker required, hard fail with pointer if absent.*

2. **CI gating: main + nightly + manual, like zexall-subset-rtl?** Or
   every branch push? *Default proposal: same as the RTL ZEXALL subset
   (heavy enough not to run on every branch).*

3. **Starter program set: 5 programs as listed?** Or all 12 right away?
   *Default proposal: start with 5; expand once green.*

4. **CI artifact retention: 30 days for the synthesized netlist?**
   *Default proposal: 30 days, matches other artifacts.*

5. **Forward to pin-scenarios?** That depends on `.events` getting wired
   into `tb_z80_netlist.v`, which depends on `.events` first landing in
   `tb_z80.v`. Track as a separate followup. *Default proposal: not in
   this branch; out of scope for "ultimate test" v1.*

Awaiting direction before executing any of the above.
