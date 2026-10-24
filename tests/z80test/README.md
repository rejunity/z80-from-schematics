# Patrik Rak z80test (raxoft)

The most thorough single-Z80 test suite still maintained today. Five
ZX Spectrum `.tap` files vendored from
[github.com/raxoft/z80test](https://github.com/raxoft/z80test) (MIT, see
`LICENSE.txt` in this directory). Each variant runs 160 tests; each test
runs one instruction class over many operands, computes a CRC over the
state it checks and compares it with a CRC Patrik Rak recorded on real
hardware. The suite prints `OK` or `FAILED` (with both CRCs) per test and
ends with `Result: all tests passed.` or `Result: N of 160 tests failed.`

    make z80test        # z80doc, z80flags, z80memptr, z80full through the C model
    make z80test_rtl    # the same through the RTL (Verilator, 6-15 min a variant)
    make z80test Z80TEST_VARIANTS=z80full     # one variant

Any failed test fails the target: there is no allowed-failure count. CI
runs `make z80test` in the `z80test` job and one variant per job of the
`z80test-rtl` matrix.


## Variants

| File              | What it checks (Rak's descriptions)                                        | C model and RTL |
|-------------------|------------------------------------------------------------------------------|-----------------|
| `z80full.tap`     | all flags and all registers                                                  | 160 / 160, in `make z80test` |
| `z80doc.tap`      | all registers, the documented flags only                                     | 160 / 160, in `make z80test` |
| `z80flags.tap`    | all flags, X and Y included; registers ignored                              | 160 / 160, in `make z80test` |
| `z80memptr.tap`   | all flags after a `BIT n,(HL)` that follows each tested instruction: MEMPTR (WZ) shows through X and Y | 160 / 160, in `make z80test` |
| `z80ccf.tap`      | all flags after a `CCF` that follows each tested instruction: Q shows through CCF's X and Y | 159 / 160: test 129 `EX AF,AF'` fails |

"160 / 160" counts the four tests of the NEC and ST variants of SCF / CCF
(003-006), which the suite reports as `Skipped` on a Zilog-type CPU.

`z80ccf` test 129 fails on main because the models leave Q, the "flags
written by the last instruction" state that SCF / CCF read their X and Y
from, equal to the swapped-in F after `EX AF,AF'`; the die clears it (CRC
3E5C2221, expected 4CFA32A0). It is one of three Q differences found on
perfectz80 in October 2026 (EX AF,AF', a DD / FD prefix, a RESET cut at
the next M1), listed in
[docs/known-differences.md](../../docs/known-differences.md). The fix is
on branch `nmos-q`, which also adds `z80ccf` to `make z80test`.

Upstream also has `z80docflags` (documented flags only, registers
ignored): `z80flags` checks a superset of it, so it is not vendored.


## Runner details

The Rak suite was written for the ZX Spectrum ROM environment. The
[`scripts/z80test_runner.c`](../../scripts/z80test_runner.c) harness (and
its Verilator twin, [`tests/verilator/sim_z80test.cpp`](../verilator/sim_z80test.cpp))
reconstructs just enough of that environment to run the tests headlessly:

  - **TAP loader** — walks the `.tap` blocks looking for a `CODE` header
    (type=3, supplies the load address — always `0x8000` for the
    `z80test` variants) followed by its `DATA` block, then copies the
    payload to that address.
  - **`PC` start** — `PC = 0x8000`, `SP = 0xFFEE`, and the word at
    `0xFFEE` is zero so the test's outer `RET` lands at `PC=0`, which the
    runner treats as the exit sentinel.
  - **ROM stubs** — two single-byte `RET` (`0xC9`) patches turn the
    Spectrum print path into no-ops the runner can intercept:
      - `mem[0x0010] = 0xC9` — `RST 0x10` (print register `A`). The
        runner traps the fetch at `0x0010`, writes `A` to stdout, then
        lets the `RET` return to the caller.
      - `mem[0x1601] = 0xC9` — `CHAN-OPEN`. The test's `printinit` does
        `JP 0x1601`; the stub returns immediately.
  - **I/O reads** — even ports return `0xBF` (the 48K ULA with no key
    pressed and EAR high), odd ports `0xFF` (floating bus), whatever was
    written to them: redcode/Z80's test harness does the same, and the
    `INI` / `IND` tests' CRCs depend on that parity.

`z80test_runner <variant.tap> [max_instr]` exits 0 when the suite prints
`all tests passed`, 1 when it reports failures, and 2 when it ends without
a result line (the instruction limit, or a crash).


## See also

  - [`../../docs/known-differences.md`](../../docs/known-differences.md)
    — the differences from the die, open and resolved.
  - [`../../docs/oracles.md`](../../docs/oracles.md) — the same `.tap`
    files through four other emulators
    (`scripts/{chips,superzazu,suzukiplan,redcode}_z80test_runner.c`).
  - [github.com/raxoft/z80test](https://github.com/raxoft/z80test) —
    upstream source (Patrik Rak, MIT).
  - `LICENSE.txt` — bundled MIT license from upstream.
