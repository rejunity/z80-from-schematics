# Z80 emulator oracles

We use four independent open-source Z80 emulators as oracles in the
verification stack:

| Oracle      | Author / source                                    | Style                | Vendor path                                |
|-------------|----------------------------------------------------|----------------------|--------------------------------------------|
| **superzazu**   | Nicolas Allemand, `github.com/superzazu/z80`     | clean C, instruction-stepped | `scripts/refs/superzazu_z80.{h,c}` |
| **chips**       | Andre Weissflog (floooh), `github.com/floooh/chips` | tick-based single-header C, deeply silicon-faithful | `scripts/refs/chips_z80.h`     |
| **suzukiplan** | Yoji Suzuki (suzukiplan), `github.com/suzukiplan/z80` | MAME-derived single-header C++ | `scripts/refs/suzukiplan_z80.hpp` |
| **redcode**     | Manuel Sainz de Baranda, `github.com/redcode/Z80` | meticulously cited C, configurable per-variant (NMOS/CMOS/NEC/ST/Sharp) | `scripts/refs/redcode_z80/` + the Zeta C utility library |

This document explains what each oracle covers, how they compare on
Patrik Rak's `z80test` + the FUSE corpus, and which one we treat as
the silicon-truth tie-breaker.

## 1. Why four oracles?

A Z80 model can pass any single test suite by coincidence. A bug that
agrees with one suite's expected often masks itself behind that very
agreement. Triangulating against four independent implementations
makes it cheap to spot the moment our model behaves like an outlier —
and equally cheap to spot when an entire community of emulators
behaves the same way (which usually points to either an unsolved
silicon corner OR a test suite that's stale vs. silicon).

Real example, found via this process (2026-06-18): all four oracles
plus our model produced **bit-identical** CRCs for Rak's z80doc 098 INI
and 099 IND. Rak's expected differed. That ruled out a per-emulator
bug and refocused the investigation on the runner's IO callback —
which turned out to be the issue (`(port & 1) ? 0xFF : 0xBF` ULA
parity, not "0xBF when low byte is 0xFE"). Fix landed in commit
[`07b257f`](#).

## 2. Lineage

  - **superzazu** (2018+) — written from the Zilog UM0080 datasheet
    plus Sean Young's "Undocumented Z80 Documented." Clean C99,
    single-stepping at the instruction boundary. Optimized for
    readability; less silicon-faithful on undocumented corners.

  - **chips/z80.h** (2019+) — extracted from Andre Weissflog's `chips`
    8-bit emulator collection (used in his Z80 / KC85 / CPC tools). One
    of the first FOSS Z80 cores to model the per-tick bus state
    (rather than instruction-boundary), which is what makes it silicon-
    accurate on block instructions, MEMPTR/WZ, and timing-sensitive
    bugs. `floooh/chips-test` includes a `z80-fuse.c` harness that
    already blacklists FUSE's `BIT n,(HL)` Y/X expectations as
    "FUSE handles those wrong" — Andre and his collaborators were the
    first FOSS contributors to publicly call out FUSE's stale corners.

  - **suzukiplan/z80** (2020+) — derived from MAME's Z80 core
    (originally Juergen Buchmueller's). C++17 single-header. Slightly
    different lineage from the WoS / Sinclair-community pedigree;
    valuable as an outside-Spectrum-community sanity check.

  - **redcode/Z80** (2023+) — Manuel Sainz de Baranda y Goñi.
    Tracks every documented and undocumented behavior with line-level
    citations (Banks 2018, Helcmanovsky 2021/2022, rofl0r 2022/2023,
    Brewer 2014 HALT, Woodmass 2008/2021). Supports per-variant
    presets: `Z80_MODEL_ZILOG_NMOS`, `Z80_MODEL_ZILOG_CMOS`,
    `Z80_MODEL_NEC_NMOS`, `Z80_MODEL_ST_CMOS`, `Z80_MODEL_SHARP_LR35902`.
    The `XQ` + `YQ` option bits enable the SCF/CCF Q-leak fully under
    the Sean Young §4.1 formula. Comes with `sources/test-Z80.c`, a
    very thorough harness that drives Rak `.tap` files + Mark
    Woodmass's Z80 Test Suite + Frank D. Cringle's `zexall.com` /
    `zexdoc.com` and compares cycle counts + line/column counts +
    FNV-1 hashes. This is the most rigorous third-party harness in
    the FOSS Z80 emulation space at time of writing.

## 3. Test-suite pass-fail matrix (current scoreboard)

  Last refreshed: 2026-06-18 (commit `9f1acb2`).

  | Suite              | Ours     | superzazu | chips    | suzukiplan | redcode  |
  |--------------------|---------:|----------:|---------:|-----------:|---------:|
  | **Rak z80doc**     | **0**    | 3         | 2        | n/a *      | **0**    |
  | **Rak z80memptr**  | **0**    | 16        | **0**    | n/a *      | **0**    |
  | **Rak z80full**    | **0**    | 20        | 10       | n/a *      | **0**    |
  | FUSE corpus †      | 1348 + 8 | n/a       | n/a      | n/a        | 1348 ‡   |
  | ZEXDOC             | PASS     | (oracle)  | (oracle) | (oracle)   | (oracle) |
  | ZEXALL             | PASS     | (oracle)  | (oracle) | (oracle)   | (oracle) |

  \* suzukiplan crashes on a few undefined ED opcodes during the Rak
  test sequence; we run it only through the 5-way lockstep
  (`scripts/lockstep_quint.c`) rather than the Rak harness.

  † FUSE corpus: 1356 cases. "1348 + 8" means 1348 PASS, 8 of the
  remaining listed in `tests/fuse/known-fuse-wrong.txt` as cases
  where FUSE's expected output is incorrect vs. real silicon (see §5
  below). 0 unexpected fails. Our model and redcode now both produce
  PC = HALT_addr + 1 after a HALT (Brewer 2014); test `76` is the
  newest addition to the known-FUSE-wrong list.

  ‡ redcode end-to-end on FUSE (via `scripts/redcode_fuse_runner.c`)
  produces the same 1348 PASS, 8 FAIL pattern (same cases on both
  sides).

## 4. Distinctive strengths

  - **chips** — first FOSS core to publish per-tick bus modeling.
    Strongest reference for instruction-level timing pin sequences.

  - **redcode** — by far the most thorough citations + per-variant
    options. The Banks 2018 / Helcmanovsky 2021 / rofl0r 2022 / Sainz
    de Baranda 2023 block-instruction Y/X/H/P fold-in (which fixed our
    final 4 z80full failures) traces directly to comments in
    `scripts/refs/redcode_z80/Z80.c` lines 1199-1231.

  - **superzazu** — clean reference for "what does the datasheet say,"
    valuable for sanity-checking that our deviations from superzazu
    are deliberate silicon-faithfulness rather than accidental.

  - **suzukiplan** — gives us an out-of-WoS-pedigree third opinion;
    catches cases where the entire ZX Spectrum community might share a
    blind spot.

## 5. Silicon-truth tie-breaker

When oracles disagree we side with the source backed by real-hardware
testing, in this priority order:

  1. **Direct silicon traces** — Visual Z80 Remix, perfectz80 gate-level,
     sigrok captures, our own scope traces. Authoritative.

  2. **Patrik Rak's z80test** (validated against a real 48K Spectrum
     by Rak, refined by Chandler on NEC and Visual Z80 Remix) +
     **Mark Woodmass's Z80 Test Suite** (HALT/EI/IFF2 timing on real
     Spectrum hardware) + **boo-boo et al. 2006 MEMPTR paper** (real
     Zilog Z80 testing, multiple chip samples). All three are explicit
     real-hardware references; we treat them as the next-best evidence.

  3. **redcode/Z80** — Manuel Sainz de Baranda cites real-hardware
     papers and reverse-engineering work line by line. When he
     deviates from documentation, it's because he believed silicon
     does. Strong third-party evidence.

  4. **chips/z80.h** — Andre Weissflog's silicon-faithful tick-based
     model. Where chips and redcode agree but FUSE disagrees, we
     side against FUSE.

  5. **Sean Young's "Undocumented Z80 Documented"** — early systematic
     reverse-engineering work (2005 era). Authoritative for the
     documented behavior, occasionally outdated on corners decoded
     after 2018 (Banks et al.). We cite Sean Young's §4.1, §6, etc.
     in code comments but mark when a redcode/Banks finding supersedes
     it.

  6. **FUSE** — useful per-cycle bus-event corpus but the
     `tests.expected` was authored by Philip Kendall in 2006 from
     FUSE's own Z80 core, never silicon-validated. Stale on at least
     7 cases (see `tests/fuse/known-fuse-wrong.txt`). Treated as a
     secondary cross-check, not authoritative.

## 6. How we use the oracles in practice

  - **`scripts/lockstep_quint.c`** — runs our model + all four
    oracles in lockstep on ZEXDOC3 with a 5-way register-by-register
    diff at every instruction boundary. Catches the first instruction
    where our model diverges from any oracle. Sub-second on ~7M
    instructions.

  - **`scripts/{chips,superzazu,suzukiplan,redcode}_z80test_runner.c`**
    — each oracle gets its own Rak runner, so we can answer "what
    does oracle X say for this specific Rak test" cleanly. This was
    how we proved the SCF/CCF Q-leak was reversed in our model
    (commit `a5f316e`) and the INI/IND CRC mystery was an IO-callback
    bug (commit `07b257f`).

  - **`scripts/redcode_fuse_runner.c`** — runs all 1356 FUSE cases
    through redcode end-to-end. The 8 redcode-FUSE failures (vs our
    7) confirm FUSE-vs-silicon disagreements rather than per-emulator
    bugs.

  - The lockstep is fast (sub-second per 7M instructions) so it runs
    as part of every `ctest` run; the per-runner oracles are run on
    demand when investigating a specific bug.

## 7. Adding a new oracle

If you want to add a fifth oracle (e.g. mz80, z80ex, zexall-cpm-z80):

  1. Vendor under `scripts/refs/<oracle_name>/`.
  2. Add a `scripts/<oracle>_z80test_runner.c` mirroring the existing
     runner pattern (TAP loader, RST 0x10 hook at 0x0010 = `0xC9`,
     ULA-idle port-parity).
  3. Extend `scripts/lockstep_quint.c` — rename to `lockstep_hex` (six-
     way) or add as needed. The pattern with macro renames to avoid
     symbol clashes is documented at the top of `lockstep_quint.c`.
  4. Watch for include-guard collisions (we hit `Z80_H` collisions
     between redcode and our own `cmodel/z80.h` — see lockstep_quint.c
     header comment).
  5. Add a row to the matrix in §3 above.

## 8. References

  - Banks 2018: <https://github.com/hoglet67/Z80Decoder/wiki/Undocumented-Flags>
  - Helcmanovsky test: <https://github.com/MrKWatkins/ZXSpectrumNextTests>
  - boo-boo MEMPTR 2006: <https://raw.githubusercontent.com/floooh/emu-info/master/z80/memptr_eng.txt>
  - rofl0r MEMPTR finding: <https://github.com/hoglet67/Z80Decoder/issues/2>
  - Sainz de Baranda's MEMPTR rediscovery: <https://spectrumcomputing.co.uk/forums/viewtopic.php?t=10555>
  - Sean Young's "Undocumented Z80 Documented": <https://www.z80.info/zip/z80-documented.pdf>
  - Patrik Rak's z80test: <https://github.com/raxoft/z80test>
  - FUSE: <https://fuse-emulator.sourceforge.net/>
  - Frank D. Cringle's `zexall.com` / `zexdoc.com`: <https://mdfs.net/Software/Z80/Exerciser/>

See `docs/simplifications.md` §F1 for the canonical writeup of how each
oracle disagreement was traced, and `tests/fuse/known-fuse-wrong.txt`
for the case-by-case FUSE-vs-silicon list.
