# prog9_q_banks — parity test for the Q register + Banks-2018 fold-in

A small Z80 program that captures the F-register state after every
flag-modifying operation by `PUSH AF`-ing it to memory. Because the
trace comparator (`scripts/compare_traces.py`) diffs `data_out` on
every phase, any RTL-vs-C divergence on F immediately surfaces as a
mismatched memory-write byte.

## What it specifically catches

The 2026-06-18 RTL regression that motivated this test had two
independent bugs that the existing trace programs (`prog1`..`prog8`,
`prog_rnd_01`..`04`) missed:

  1. **F-modification detector for the Q register**. The RTL was
     comparing `rf_n[AF][7:0] != rf[AF][7:0]` to decide whether an
     instruction modified F. That's wrong for instructions like SCF
     chained after an unrelated `LD r,n`: SCF writes F, but if F's
     value happens to match the pre-SCF value, the comparator says
     "no modification" and Q gets zeroed. The next SCF then leaks
     the WRONG bits via `Y/X = A | (F XOR Q)`. The C model uses a
     separate `c->f_modified` flag set by `z80_setF()`, so it doesn't
     have this bug.

  2. **Banks INXR/OTXR `pf_arg` formula**. The repeat-iteration
     formula is `parity((t & 7) ^ newB ^ ((newB ± 1) & 7))`. The RTL
     had `parity((t & 7) ^ ((newB ± 1) & 7))` — missing `^ newB`.

Both bugs slipped past `make compare` because no existing trace
program chained SCF/CCF or ran the block-repeat instructions far
enough to trigger the Banks fold-in. Both bugs were caught by Rak's
`z80full` 007 SCF+CCF + 102 INIR→NOP' + 103 INDR→NOP' through
`sim_z80test.cpp`, but only after the Verilator run completed
(~5 min). This program reproduces both bug families in ~1200 phases.

## Program flow

  - SCF / CCF / SCF chain — three F-modifiers separated by a
    non-F-modifier (`LD A, n`) to trip the `Q := 0 after non-F-mod`
    rule. Each `SCF`/`CCF` is followed by `PUSH AF`.
  - LDIR with BC=3 — runs 3 iterations; the Banks fold-in fires on
    the 2 internal NOP M-cycles between iterations.
  - CPIR with BC=3 + target 0xAA at the *third* slot — runs all 3
    iterations, same Banks structure.
  - INIR with BC=0x0303 — IO reads return whatever the bus stub
    presents; what matters is the Banks `pf_arg` formula running
    against the IO data byte + decremented B.

After each block the result `PUSH AF` lands on the bus. The trace
diff covers every F bit; any RTL-vs-C divergence on Y, X, H, P, N
shows up as a mismatched `data_out`.

## Verifying

  - `make compare` runs all `tests/traces/*.hex` through C, iverilog
    and Verilator and diffs every phase.
  - `python3 scripts/compare_traces.py tests/traces/prog9_q_banks.hex 1200`
    runs just this one. Expect `PASS (1200 phases identical)` on both
    iverilog and Verilator legs.

If a future change re-introduces either bug class, this will fire
inside the C-tests CI job (under 10 seconds wall-clock), instead of
the ~15-minute Rak-on-RTL job.

See also: `docs/oracles.md` for the rationale on why every silicon-
faithful change needs a parity oracle that runs in the C-tests
budget — not just the Rak/FUSE coverage which can take minutes.
