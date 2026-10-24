# cfast's speed

cfast is rz80's C model generated into a phase engine (`cfast/`). Every
half clock it shows the same pins as the model and the RTL (`make
cfast_identity` checks this phase by phase), for emulators of Z80 machines.
Here it runs next to the C Z80 emulators the project checks its model
against (`scripts/refs/`), on the same instructions.

![ZEXDOC, million instructions a second, per emulator](https://rejunity.github.io/z80-from-schematics/cfast-bench.svg)

The chart is the latest run of CI's `cfast-bench` job, which runs on every
push; the project's site shows it with its table
([the C model, for emulators](https://rejunity.github.io/z80-from-schematics/#cfast)).
The table below is one run of it, on GitHub's runner:

| Emulator | Sees every | ZEXDOC Minstr/s | Z80 clock | Against cfast |
|---|---|---:|---:|---:|
| **rz80 cfast, z80f_run** | half clock | 62.7 | 507 MHz | 1.00× |
| rz80 cfast, z80f_tick | half clock | 18.2 | 147 MHz | 0.29× |
| rz80 C model | half clock | 10.9 | 88 MHz | 0.17× |
| floooh/chips | clock | 45.9 | 371 MHz | 0.73× |
| suzukiplan | instruction | 96.4 | 779 MHz | 1.54× |
| superzazu | instruction | 188.0 | 1520 MHz | 3.00× |
| redcode | instruction | 185.1 | 1496 MHz | 2.95× |

The first 50,000,000 instructions of ZEXDOC, the best of 5 rounds of each emulator's CPU time (on CPU 1); Z80 clock: the T-states it emulates a second. AMD EPYC 9V45 96-Core Processor, cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0, -O2; GitHub Actions (ubuntu24), commit e64fd89, 2026-10-21 12:41 UTC.

- **rz80 cfast, z80f_run**: the phase engine generated from the C model's timing tables, steps back to back with the bus inlined (`cfast/`)
- **rz80 cfast, z80f_tick**: the same steps, one half clock per call, the pins in a word
- **rz80 C model**: the reference model the RTL is checked against (`cmodel/`)
- **floooh/chips**: [floooh/chips](https://github.com/floooh/chips) `z80.h`: `z80_tick`, the pins in a word
- **suzukiplan**: [suzukiplan/z80](https://github.com/suzukiplan/z80): `execute(1)`
- **superzazu**: [superzazu/z80](https://github.com/superzazu/z80): `z80_step`
- **redcode**: [redcode/Z80](https://github.com/redcode/Z80): `z80_execute(1)`

## The columns

- **Sees every**: how finely a machine around the emulator sees the bus.
  cfast and the rz80 C model run a half clock (one phase) at a time with the
  pins of each (`z80f_tick` one per call, `z80f_run` back to back with the
  machine's bus inlined into the steps), so a wait state, a bus request or
  an interrupt lands on the half clock it lands on in silicon.
  floooh/chips steps a clock (a T-state) with its pins; the other three
  run whole instructions and report the clocks they took.
- **ZEXDOC Minstr/s**: million instructions a second, over the first 50
  million of ZEXDOC.
- **Z80 clock**: the T-states it emulates a second, the clock of a Z80 it
  keeps up with (the original runs at 4 MHz).
- **Against cfast**: Minstr/s against `z80f_run`'s.

## How it is measured

`scripts/cfast_bench.py`, run by `make cfast_bench`:

- Every emulator runs in a harness of the same frame (`cfast/bench/`;
  rz80's own: `cfast/zexfast.c`, `scripts/zexrunner.c`): ZEXDOC at 0100,
  its BDOS calls caught at 0005. Each must run the same instructions: their
  T-states agree within 0.5 % (floooh/chips and redcode end a prefixed
  instruction their own way, which moves them 0.1 %), or the script fails.
- The time is each process's own CPU time, the best of 5 rounds, with the
  emulators interleaved within each round and pinned to one CPU.
- rz80's code builds with the project's flags (`-O2`, LTO), the others at
  `-O2`; redcode with the features the lockstep CI job builds it with.
- On x86 they all build with their jumps padded off 32-byte boundaries
  (the Makefile's `JCC_PAD`, probed; `JCC_PAD=` for none). Intel's
  microcode for the JCC erratum (Skylake-derived cores: 6th to 10th
  generation Core) keeps a 32-byte block whose jump crosses or ends on its
  boundary out of the decoded-uop cache, which costs each emulator what its
  code layout happens to cost it. On an i7-7700, best of 3, padding made
  `z80f_run` 7 % faster, floooh/chips 13 %, superzazu 14 %, redcode 8 %,
  suzukiplan 6 %, the C model 11 %, and `z80f_tick` 4 % slower.

Run it:

    make cfast_bench                                   # build/bench/bench.{md,json,svg}
    make cfast_bench CFAST_BENCH_ARGS="--cpu 3 --rounds 5 --instr 50000000"

## Kept up to date

CI's `cfast-bench` job runs it on every push, after `make cfast_identity`
has checked cfast against the model in every phase: the table is in the
job's summary and its artifact `cfast-bench` (90 days). `pages.yml` puts
main's latest one on the site with the chart above.
