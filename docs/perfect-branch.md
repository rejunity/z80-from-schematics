# `perfect` branch — closing the last gaps

Direction: nail down the tests that aren't 100 % passing today, with
focus on the gaps we can close cleanly. Out of scope: deep silicon-
faithfulness refactors of the block-op M-cycle (those move to a future
branch alongside the `unsimplify` audit items already in
[docs/simplifications.md](simplifications.md)).

## Current "not 100 %" tests + this branch's plan for each

| Gate                          | Currently | This branch                                                                                   |
|-------------------------------|-----------|-----------------------------------------------------------------------------------------------|
| `make perfectz80` (`addr` parity) | 60–100 % match (refresh-phase one-cycle settle delta) | **Close** via addr-settle tolerance: allow our `addr` to lead perfectz80 by 1–2 cycles when MREQ/IORQ/RD/WR are all *inactive* on both sides (the bus is don't-care anyway). |
| `make pin_scenarios`          | Informational (C-only); ctrl-pin divergences | **Land `.events` in RTL testbenches** so pin-scenarios can also run through iverilog RTL + LibreLane netlist, not just C tracegen. Helps localise divergences (C-only vs all-three-paths). |
| `make z80test` (Rak) baselines 2/2/10 | 158/158/150 within baseline | **Keep baselines.** Root causes are block-op M-cycle ordering + SCF/CCF Q-leak chains on block ops — both substantial silicon-faithful refactors. Document gaps + NMOS/Toshiba choice clearly. |
| `make zexall_rtl` (full ZEXALL via Verilator RTL) | Known fails, not in CI | Unchanged — same root causes as the z80test gaps. |

## What ships in this branch

1. **Address-bus settle tolerance.** Extend
   `scripts/compare_signal_timing.py`'s bus-comparison logic with a
   look-ahead window: when our `addr` differs from perfectz80's at
   phase *i*, also check phases *i+1* and *i+2* on the perfectz80
   side — if our value matches one of those AND all four strobes
   (mreq, iorq, rd, wr) are inactive on both sides for that whole
   window, it's the well-known address-settle delta and should
   *not* count as a mismatch. Should bring `addr` parity to ~100 %
   on every prog1..prog8 + prog_rnd_*.

2. **VCD waveforms.** Emit `build/vcd/<prog>.vcd` per program during
   the perfectz80 comparison runs (C / iverilog / netlist legs).
   Our model's pin signals appear at top scope; perfectz80's pin
   signals appear under a `perfectz80.*` scope in the same VCD so
   GTKWave / Surfer / etc. can render both side-by-side from a
   single file. Implementation: ~80-line VCD writer in Python
   driven by the same row dictionaries `compare_signal_timing.py`
   already builds.

3. **`.events` sidecar in the RTL testbenches.** Today `tb_z80.v`,
   `tb_z80_netlist.v`, and `sim_main.cpp` only understand the legacy
   `+nmi=<phase>` shorthand. Approach: encode each `.events` sidecar
   into per-pin `+nmi_lo=N +nmi_hi=M`, `+int_lo=N +int_hi=M`,
   `+wait_lo=N +wait_hi=M`, `+busreq_lo=N +busreq_hi=M`,
   `+reset_lo=N +reset_hi=M` plusargs (parsed by
   `compare_signal_timing.py` from the existing sidecar). Each
   testbench drives the corresponding `*_n` line at the matching
   phases. Unlocks the next item.

4. **Pin-scenario RTL gates.** New `make pin_scenarios_rtl` (drives
   iverilog) and `make pin_scenarios_netlist` (drives LibreLane
   netlist) targets exercising the 12 prog9..prog20 pin-scenarios
   through the RTL paths, with `.events` plumbing from item 3. Both
   informational like the existing C-only `pin_scenarios`, but now
   we can see whether divergences are in the C model, the RTL, or
   the synthesised gates.

5. **NMOS/Toshiba switch — conditional.** Default stays NMOS (current
   Q-leak behaviour, matches Zilog NMOS silicon). If — and only if —
   a Toshiba CMOS variant is genuinely needed to close a currently-
   failing test, add a runtime flag (`s->cmos_q`) in the C model and a
   parameter in the RTL that suppresses the SCF/CCF Q-leak. Most
   failures we see today are NMOS-correct; the z80test SCF/CCF gaps
   are Q-leak-chain bugs on block ops, not NMOS-vs-Toshiba. So this
   item likely ships *not implemented* with a docs entry explaining
   why.

6. **Documentation.** Update `tests/README.md` so the "Forward-looking"
   bullet on `.events` in RTL testbenches gets demoted to a row in the
   Quick Reference. Update `docs/known-differences.md` row 14 (the
   `addr`-settle delta) to "resolved" once the tolerance lands.

## What this branch deliberately defers

  - **Block-op M-cycle ordering rewrite** — the root cause of the
    z80test INI/IND failures. Substantial work; tracked in
    `docs/simplifications.md` §F.
  - **Q-leak chain on block-op repeats** — z80memptr's INIR / INDR
    failures + z80full's LDIR/LDDR → NOP' chains. Same scale.
  - **A-Z80 as second gate-level oracle** — design sketch already at
    `docs/ring3-az80-oracle.md`; with LibreLane providing an
    independent synthesised gate-level reference now, A-Z80 is
    less critical.
  - **Full LibreLane PnR + STA** — out of scope; needs a tape-out
    target to justify the CI budget.

## Order of work

  1. Plan doc (this file).
  2. Address-bus settle tolerance in `compare_signal_timing.py` +
     known-differences.md row 14 update.
  3. VCD writer + per-program `.vcd` emission in `compare_signal_timing.py`.
  4. `.events` → plusargs encoder in `compare_signal_timing.py` +
     plusarg drivers in `tb_z80.v`, `tb_z80_netlist.v`,
     `sim_main.cpp`.
  5. `make pin_scenarios_rtl` + `make pin_scenarios_netlist` +
     CI wiring (both informational, every push).
  6. tests/README.md polish.
  7. Merge → main when each gate is green or explicitly accepted as
     deferred.
