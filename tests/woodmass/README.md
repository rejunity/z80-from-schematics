# Mark Woodmass Z80 Test Suite

Mark Woodmass (AKA "Woody") authored a series of Z80 test programs
captured against real ZX Spectrum hardware. They cover behaviour that
Patrik Rak's `z80test` deliberately doesn't test — primarily HALT
exit timing on INT/NMI, EI-shadow, IFF2 restore, and the DD/FD
prefix-chaining and `ED 77/7F` undocumented opcodes. redcode/Z80
cites HALT2INT v3 in its source comments as the canonical real-
hardware reference for HALT-state exit (`scripts/refs/redcode_z80/
Z80.c:1633`).

## Vendored files

| File                                                | Source | License |
|-----------------------------------------------------|--------|---------|
| [`z80-test-suite-2008.tap`](z80-test-suite-2008.tap) | [`zxe.io`](https://zxe.io/depot/software/ZX%20Spectrum/Z80%20Test%20Suite%20%282008%29%28Woodmass%2C%20Mark%29%5B%21%5D.tap) | freely-distributed; credit Woodmass |

Original distribution was on Woodmass's (now-defunct) `homepage.ntlworld.com/mark.woodmass/`
personal page; binaries are widely mirrored. `redcode/Z80`'s own
`sources/test-Z80.c` lists this file with format `TEST_FORMAT_WOODMASS`
and expected FNV-1 hashes / cycle / line / column counts.

## Harness — `scripts/woodmass_runner.c`

The runner is a thin C driver: load `.tap` at 0x8000, run from
`0x8057` (one of the two Woodmass start addresses; the other is
`0x8049`), and stream RST 0x10 prints + an FNV-1 hash of the output.

**Caveat (2026-06-18):** the suite references several Spectrum
48K ROM addresses (e.g. `LD HL, 0x2758`). Without the ROM loaded at
`0x0000-0x3FFF` the test diverges from its silicon-captured
expected hash early. The runner supports `--rom <path>`. Amstrad
has permitted non-commercial redistribution of the original Sinclair
ROM since 1999, but we do not currently vendor it. CI runs the
runner in `--no-rom` smoke mode (verifies the test code reaches its
HALT exit, does not validate the hash).

## How to run locally

    cc -std=c99 -O2 -Icmodel scripts/woodmass_runner.c build/libz80.a \
       -o build/bin/woodmass_runner
    build/bin/woodmass_runner tests/woodmass/z80-test-suite-2008.tap
    # Add --rom <path-to-48.rom> for full validation.

## Why we keep this around

Adding HALT2INT-class coverage closes the one major gap our Rak +
FUSE matrix doesn't catch: interrupt acceptance timing relative to
the HALT internal-NOP loop. Our model added the HALT + IFF +
NMI/INT engine in commits `c9aaaf1` (C) and `f1f2100` (RTL); this
suite is the appropriate silicon-derived regression for that engine
once we have the ROM dependency settled.

## Next steps

  - Decide whether to vendor `48.rom` (Amstrad's distribution
    license allows it for non-commercial use; many open-source
    Spectrum emulators do).
  - Update the runner to compute and check FNV-1 against
    Woodmass's published expected hashes (see redcode's
    `sources/test-Z80.c` for the canonical table).
  - Wire as a separate CI job once the hash check is in place.

## HALT2INT v3 — investigated 2026-06-18

HALT2INT v3 is Mark Woodmass's 2021 follow-up that specifically
tests HALT-to-INT acceptance timing on a real 48K Spectrum.
Downloaded from
[zxe.io](https://zxe.io/depot/software/ZX%20Spectrum/HALT2INT%20v3%20%282022-01-04%29%28Woodmass%2C%20Mark%29%20%5B%21%5D.zip)
(GPLv2, source `.asm` included).

The v3 test is **not a clean CPU-only test**:
  - calls `0x0D6B` (CLS) and `0x1601` (OPEN-CHAN) → needs Spectrum ROM
  - reads ULA contended memory at addresses 14335, 14336, 22528, etc.
    → needs ULA contention timing modeling (board-level, not CPU)
  - distinguishes between early-issue and late-issue 48K models

The CORE silicon behavior HALT2INT verifies (HALT M1 commits with
PC past the HALT byte; HALT-NOP loop re-fetches at that PC; INT/NMI
accept that PC unchanged) **was missing from our model and the RTL
and is now fixed** (commits referenced in
`docs/known-differences.md` item 9). The verification ran through
two layers:
  1. `tests/traces/pin_scenarios/prog13_halt_int.hex` — a focused
     Z80-level HALT→INT scenario, now agreeing with perfectz80 on
     address bus throughout the HALT loop (was 107/200, now 110/200
     match after the convention flip).
  2. FUSE test `76` (HALT instruction) — now in `tests/fuse/
     known-fuse-wrong.txt` because FUSE's expected reflects the
     pre-Brewer 2014 convention.

That gives us the same silicon-faithful HALT-PC convention HALT2INT
v3 was designed to verify, without depending on the Spectrum
ULA / ROM. A future full HALT2INT v3 run remains a possible
follow-up if we wire the ULA contention model and ROM dependencies.

## `make halt2int` — focused HALT-INT timing probe

[`scripts/halt2int_probe.c`](../../scripts/halt2int_probe.c) is a
small, focused regression that exercises the CPU-only silicon
property HALT2INT v3 verifies: the T-state delay between an INT pin
going low during the HALT loop and the INTA M-cycle starting.

The probe sweeps INT-assert timing across an 8-phase (4 T-state) HALT
NOP window and verifies the delay stays in the silicon-faithful range
(3..8 T-states; the exact value depends on where in the M-cycle INT
goes low, per Z80 datasheet's "INT sampled at last T-state of current
M-cycle" rule). Output:

    INT @halt+ 2 phases (~T1): delta = 5 T-states OK
    INT @halt+ 4 phases (~T2): delta = 4 T-states OK
    INT @halt+ 6 phases (~T3): delta = 7 T-states OK
    ...
    range observed: 4..7 T-states (silicon range: 3..8)
    verdict: PASS (silicon-faithful)

The pattern repeats every 4 T-states (8 phases) — each step within
the M-cycle shifts the relative position of the sample window by one
T-state, sliding through the 4..7 range cyclically.
