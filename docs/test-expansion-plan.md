# Test set expansion — plan

The `tests` branch hosts the work in this document. The plan is in
three concentric rings:

  1. **Layer in best-in-class external test suites** that exist as
     finished artifacts (.tap / .com / .asm / C sources).
  2. **Expand the trace-program set** that runs against perfectz80 (and
     iverilog + Verilator + the C model in parallel) so every external
     pin and corner-case is covered, not just opcode arithmetic.
  3. **Add a second gate-level oracle** alongside perfectz80 so we
     cross-check the Visual-Z80 netlist itself.

Already in place on this branch (just landed):

- **BASIC ROM regression** (`tests/basic/scripts/*`, `make basic_tests`,
  CI job `basic-tests`). Pipes canned BASIC scripts through
  `basicrunner` for NASCOM BASIC 4.7c (RC2014 build) and the 1K
  Tiny BASIC ROM; greps for expected substrings. 4 subtests today
  (arithmetic, FOR/NEXT + string accumulator, strings, Tiny arith).
  Catches gross ROM-boot regressions and tens-of-millions of cycles
  of "real software" execution that ZEX/FUSE never exercise.

## Ring 1 — external test suites to integrate

| Suite | Author | License | What it adds | Status |
|---|---|---|---|---|
| **z80test** (`z80full`, `z80doc`, `z80flags`, `z80memptr`, `z80ccf`) | Patrik Rak (raxoft) | MIT | Documented + undoc behaviour, MEMPTR/WZ, X/Y/H undoc flags, SCF/CCF Q-leak — **all things ZEXALL misses**. Per-subtest OK/CRC output like Cringle. | **to integrate** |
| **prelim.com** | Bartholomew 1981 | public domain | 899 instr / 8721 cycles — sanity-checks instructions ZEXALL itself depends on. Bundled in many emulators. | already shipped (`tests/zex/prelim.com`) |
| **FUSE 1356-case** | F. Cringle / fuse-emulator | MIT-style | Per-T-state event list + final state. | already integrated (`make fuse`, `make fuse_rtl`) |
| **ZEXDOC + ZEXALL** | F. Cringle | GPL-2.0 | Documented + undocumented opcode flag check. | already integrated (`make zexdoc`, `make zexall`, both CI jobs) |
| **floooh chips-test / z80-timing.c** | Andre Weissflog | MIT | Per-T-state pin assertions (`M1|MREQ|RD`, `MREQ|RFSH`, `IORQ|RD/WR`, etc.) for every opcode. Closest available analogue to a pin-level exerciser. | **to integrate** |
| **Woodster `Timing_Tests-48k_v1.0`** | (cited by MAME PR #11522) | unknown | M-cycle-shape regression — useful if license permits. | to evaluate |

### Concrete integration tasks

- **`tests/z80test/`**: download Rak's `z80doc.tap` + `z80full.tap` +
  `z80memptr.tap` + `z80ccfscr.tap` from
  https://github.com/raxoft/z80test (MIT). Wrap each `.tap` in a runner
  similar to `zexrunner`: load the tape into RAM, simulate ZX Spectrum
  ROM-call exits via a BDOS-style PC trap on the print routine. New
  Makefile target `make z80test` + CI job `z80test`. Estimated runtime
  ~10 min for `z80full` (similar to ZEXALL) — gate to the same
  always-run tier as the existing ZEX C jobs.

- **`tests/timing/`**: lift `z80-timing.c` from
  https://github.com/floooh/chips-test (MIT). It compiles against
  `chips/z80.h` today; we'd port to drive *our* `z80_core` instead and
  cross-check the asserted T-state pin patterns. This gives per-opcode
  PIN-LEVEL regression that ZEX/FUSE never deliver. Goes into the
  `c-tests` job (fast, deterministic).

## Ring 2 — expand perfectz80's program set

`scripts/compare_signal_timing.py` currently runs `prog1` + `prog2` +
`prog3_cb` for 200 phases each. That window covers `M1`/`MREQ`/`RD`/
`RFSH`/`HALT` for a handful of opcodes, but **never exercises**: INTA
acknowledge, NMI acceptance (only one pulse instant), IM 0/1/2 vector
fetch, WAIT-state insertion (mem and I/O), BUSREQ release / BUSACK
assertion, HALT exit on NMI vs INT, EI delay window, RESET behaviour,
I/O write strobes vs read strobes timing.

### Cross-harness pin-event format

All four harnesses (iverilog `tb_z80.v`, Verilator `sim_main.cpp`, C
`tracegen.c`, gate-level `perfectz80_runner.c`) today accept only one
optional `[nmi_phase]` argv. Extend them to accept an **events
sidecar**: `tests/traces/<prog>.events`

```
# format: <phase> <pin> <value>
# pin: nmi int wait busreq reset
60   int    0
80   int    1
150  busreq 0
220  busreq 1
```

Each harness reads the sidecar at startup, applies the events at the
matching phase. perfectz80_runner already has `cpu_writeINT` /
`cpu_writeWAIT` / `cpu_writeNMI` / `cpu_writeRESET` exported — plumbing
is one-liner per pin per harness.

### New programs (each with `.events` sidecar)

| Program             | Drives                | Tests                                        |
|---------------------|----------------------|----------------------------------------------|
| `prog9_inta_im1`    | `int 0@N`            | INTA ack, M1 + IORQ combo, IM 1 vector       |
| `prog10_inta_im2`   | `int 0@N`            | IM 2 vector read via {I,n}, push PC          |
| `prog11_halt_nmi`   | `nmi 0@N`            | HALT entry → NMI exit → push PC → jump 0066h |
| `prog12_halt_int`   | `int 0@N`            | HALT exit on INT (vs NMI), IFF1 behaviour    |
| `prog13_wait_mem`   | `wait 0@N..N+M`      | WAIT during MRD/MWR T2.N → Tw insertion      |
| `prog14_wait_io`    | `wait 0@N..N+M`      | WAIT during IORD/IOWR Tw.N → extra Tw        |
| `prog15_busreq_m1`  | `busreq 0@N`         | BUSREQ sampled at last M-cycle phase, BUSACK |
| `prog16_ei_delay`   | `int 0@N`            | INT pulse during EI window — postponed by 1  |
| `prog17_reset`      | `reset 0@N..N+M`     | RESET mid-execution, PC/I/R/IFF reset        |
| `prog18_di_then_int`| `int 0@N`            | INT held while DI set — ignored              |
| `prog19_nmi_in_int` | `nmi 0@N`            | NMI during INTA cycle, queueing              |
| `prog20_block_int`  | `int 0@N`            | INT during LDIR/CPIR — interrupted block-op  |

Twelve new programs, each ≤ 64 bytes, each driving one specific
external-pin scenario. perfectz80 compared per-half-cycle on the same 7
control pins it does today, plus optionally extend the comparison to
address-bus / data-bus columns during the valid windows.

### Bus-data + bus-address comparison

`scripts/compare_signal_timing.py` today compares only the 7 control
pins (`mreq, iorq, rd, wr, m1, rfsh, halt`) and treats `addr` / `data_o`
as don't-care. Extend it to **also** compare `addr` and `data_o` during
known-valid intervals — easy to define: `addr` valid when `mreq` or
`iorq` is low; `data_o` valid when `wr` is low.

That turns perfectz80 from a control-pin oracle into a full external
bus oracle, no Verilog or C model changes needed — just the Python diff.

## Ring 3 — second gate-level oracle

perfectz80 is derived from the visualz80 netlist (Z8400-era die). Any
mistake in the netlist itself shows up in our test as "agreement" with
a wrong reference. A second oracle from a different lineage shrinks
that risk.

Best candidate per research: **A-Z80** by Goran Devic
(https://github.com/gdevic/A-Z80, GPL-2.0) — schematic-driven Verilog,
not a netlist port, "cycle and bus accurate including nWAIT and
nBUSRQ", passes ZEXDOC/ZEXALL.

License caveat: A-Z80 is GPL-2.0; our repo is currently unlicensed (we
treat it as proprietary). To use A-Z80 as a CI-only oracle we'd:
- check it into `scripts/refs/az80/` *only* (not into the synthesizable
  RTL tree); or
- pull it at CI time from upstream and not redistribute.

The latter is cleaner. Implementation: a CI step that does
`git clone --depth=1 https://github.com/gdevic/A-Z80` into a scratch
dir, builds it via Verilator (it ships a self-test harness), runs the
same trace programs, diffs against our existing model. Failure case
just means we discover a divergence — investigation, not a CI red.

Defer until Ring-1 and Ring-2 are landed; A-Z80 is a polish step.

## CI footprint

After Ring-1 + Ring-2 are integrated:

- `c-tests` job adds: `make timing_tests` (chips-test/z80-timing port).
- New `z80test` CI job (mirrors `zexdoc` / `zexall` shape).
- `parity-tests` job: same `make compare` but with the bigger trace set.
- `make perfectz80` exercises 3 → 15 programs (still per-half-cycle but
  with shorter windows on the new pin-scenario programs, ~50–80 phases
  each so total wall clock stays under 1 minute).

Ring-3 (A-Z80) optionally adds one more CI job.

## Order of work

1. Ring 1: **`z80test`** integration (Rak's suite). One CI job, ~10 min.
2. Ring 2: pin-event sidecar format + harness extensions.
3. Ring 2: new trace programs `prog9..prog20`, register with perfectz80
   compare.
4. Ring 2: extend `compare_signal_timing.py` with bus-valid intervals.
5. Ring 1: port `z80-timing.c` from floooh/chips-test (per-T-state pin
   regression for every opcode).
6. Ring 3: A-Z80 as a second oracle.

The `unsimplify` audit followups (`docs/audit-followups.md`) are
deliberately deferred behind this test work — broader test coverage
will make the silicon-faithful refactors safer.
