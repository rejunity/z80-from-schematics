# Patrik Rak z80test (raxoft)

The most thorough single-Z80 test suite still maintained today. Five
ZX Spectrum `.tap` files vendored from
[github.com/raxoft/z80test](https://github.com/raxoft/z80test) (MIT, see
`LICENSE.txt` in this directory). Each variant runs ~160 micro-tests that
each compute a CRC across the documented + undocumented post-state of
one instruction class, compare against a baked-in expected CRC, and
print a per-test `OK` / `FAIL` line ending with a `Result: NNN of MMM
tests failed.` summary.

Run them all with:

    make z80test

This builds [`scripts/z80test_runner.c`](../../scripts/z80test_runner.c)
and runs three variants in sequence with per-variant **allowed-failure
baselines**: any number of failures *at or below* the baseline is treated
as a PASS, any number *above* the baseline as a regression (non-zero exit).
This is how we gate against drift without flipping red on the
silicon-faithfulness gaps we already track in
[docs/audit-followups.md](../../docs/audit-followups.md).


## Variants (and their baselines)

| File              | Baseline | What it covers                                                                                                          |
|-------------------|---------:|-------------------------------------------------------------------------------------------------------------------------|
| `z80doc.tap`      | 2        | **Documented** flag behavior only — XF / YF masked. Our 2 documented failures are `INI` / `IND` and trace back to the audit's block-I/O sub-cycle item (`F-block-op-M-cycle`). |
| `z80memptr.tap`   | 2        | MEMPTR / WZ exposure via `BIT n,(HL)` and `BIT n,(IX+d)` style probes. Our 2 failures are `INIR -> NOP` / `INDR -> NOP` Q-leak interactions. |
| `z80full.tap`     | 10       | Full **undocumented** behaviour (XF, YF, MEMPTR, Q-flag carry-over into SCF/CCF). Our 10 failures break down as ~8 SCF/CCF ST-variant differences (we model Zilog NMOS Q, not Toshiba) + 2 LDIR/LDDR -> NOP Q-leaks. |
| `z80ccf.tap`      | n/a      | SCF/CCF Q-flag-only probes. Variant available for reproducing individual ST-variant cases; not in the `make z80test` rotation today (the same gaps are covered by `z80full`).         |
| `z80flags.tap`    | n/a      | Per-instruction flag exhaustion. Also not in the `make z80test` rotation; subsumed by `z80doc` + `z80full`.                                                                          |


## Runner details

The Rak suite was written for the ZX Spectrum ROM environment. The
[`scripts/z80test_runner.c`](../../scripts/z80test_runner.c) harness
reconstructs just enough of that environment to run the tests headlessly
against our C model:

  - **TAP loader** — walks the `.tap` blocks looking for a `CODE` header
    (type=3, supplies the load address — always `0x8000` for the
    `z80test` variants) followed by its `DATA` block, then memcpys the
    payload at that address.
  - **`PC` start** — `PC = 0x8000`, `SP = 0xFFEE`, and bytes `0x0000-0x0001`
    are zeroed so the test's outer `RET` lands at `PC=0`, which we treat
    as the exit sentinel.
  - **ROM stubs** — two single-byte `RET` (`0xC9`) patches turn the
    Spectrum print path into no-ops the runner can intercept:
      - `mem[0x0010] = 0xC9` — `RST 0x10` (print register `A`). The
        runner trap fires *before* executing the `RET`, so it writes `A`
        to stdout, then lets the RET return to the caller.
      - `mem[0x1601] = 0xC9` — `CHAN-OPEN`. The test's `printinit` does
        `JP 0x1601`; the stub returns immediately so printing works
        without a real Spectrum ROM.
  - **Port `0xFE`** — every `IN A,(0xFE)` (any high byte) returns
    `0xBF`, which is what the Spectrum ULA returns when no keys are
    pressed and EAR is high. The Rak test that touches the IN path
    expects exactly that value. All other I/O reads return `0xFF`
    (open-bus convention).

The runner exits 0 when observed failures `<= max_allowed`; non-zero
otherwise. Argv: `z80test_runner <variant.tap> [max_instr] [max_allowed]`.


## Why we keep failures rather than fixing them in-place

The remaining `z80test` failures are **documented divergences**, not
bugs:

  - INI / IND / INIR / INDR sub-cycle ordering — listed as `F-block-op-M-cycle`
    in [docs/audit-followups.md](../../docs/audit-followups.md). Fixing
    requires rewriting the block-I/O M-cycle, which has cascading effects
    on the gate-level perfectz80 oracle diff.
  - SCF / CCF Q-flag behaviour — Zilog NMOS Q-leak vs. Toshiba's
    suppressed-leak variant. We model NMOS as the canonical choice; the
    Toshiba behaviour is a separate strap.
  - LDIR / LDDR / INIR / INDR -> NOP' Q-flag carry — same root cause as
    SCF / CCF.

These are listed in [docs/known-differences.md](../../docs/known-differences.md)
as well, with cross-references to the audit followups and to which Rak
test cases surface them. The baselines above let CI catch *new* drift
in any of these classes without the suite turning red on day one.


## See also

  - [`scripts/z80test_runner.c`](../../scripts/z80test_runner.c) — the
    headless runner: TAP loader, ROM stubs, port-FE wiring, pass/fail
    parser.
  - [`../../docs/audit-followups.md`](../../docs/audit-followups.md) —
    silicon-faithfulness items, including the block-I/O sub-cycle.
  - [`../../docs/known-differences.md`](../../docs/known-differences.md)
    — running list of deliberate / watched divergences.
  - [github.com/raxoft/z80test](https://github.com/raxoft/z80test) —
    upstream source (Patrik Rak, MIT).
  - `LICENSE.txt` — bundled MIT license from upstream.
