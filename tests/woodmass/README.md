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
