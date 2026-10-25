# Mark Woodmass Z80 Test Suite (2008)

Mark Woodmass ("Woody", the author of the Spectaculator emulator) wrote a
ZX Spectrum test suite whose expected values were captured on real
hardware: the program's menu says on a NEC D780C-1, an NMOS Z80 second
source. Two of its menu entries test the CPU: **1 Flags** (47 tests of
the flags instructions leave) and **2 MEMPTR** (58 tests of MEMPTR, the
internal WZ register).

## Vendored file

| File                                                | Source | License |
|-----------------------------------------------------|--------|---------|
| [`z80-test-suite-2008.tap`](z80-test-suite-2008.tap) | [`zxe.io`](https://zxe.io/depot/software/ZX%20Spectrum/Z80%20Test%20Suite%20%282008%29%28Woodmass%2C%20Mark%29%5B%21%5D.tap) | not established; see [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) |

Original distribution was on Woodmass's (now-defunct)
`homepage.ntlworld.com/mark.woodmass/` page; the binary is widely
mirrored. redcode/Z80's `sources/test-Z80.c` lists the same file.

## Where it runs: the speccy branch

The suite is a Spectrum program: it loads through the 48K ROM, draws a
menu read from the keyboard, prints through the ROM and pauses at
`scroll?` prompts, so it needs the ROM, the ULA's keyboard and its
interrupts. It runs on branch `speccy`, the 48K Spectrum built on cfast
with a die-exact ULA:

    make speccy_woodmass        # on branch speccy; the 48K ROM comes from make speccy_rom

`make speccy_woodmass` loads this `.tap` through the real 48K ROM, runs
menu 1 (Flags) and menu 2 (MEMPTR), answers the `scroll?` prompts, and
fails unless Flags reports 47 / 47 and MEMPTR 58 / 58. Both pass (speccy
5ec332e, which has main 780418a merged). It takes about 2.6 min including
the build: Flags runs 43,850 frames (45-55 s), MEMPTR 4 s.

It is not part of main's regular test set, because the Spectrum (ROM,
ULA, tape loader) lives on the speccy side branch, which has no CI. What
it checks is checked on main by z80test (`z80flags`, `z80memptr`; see
[../z80test/README.md](../z80test/README.md)) and, for MEMPTR, by FUSE's
WZ values ([../fuse/README.md](../fuse/README.md)), both against values
from Zilog parts or the Zilog die.

Two limits worth knowing:

  - Its reference is a NEC part, so where NEC and Zilog NMOS differ it
    would expect NEC's values; every one of its tests passes on our
    core, which follows the Zilog die.
  - Its SCF / CCF tests always follow an instruction that writes the
    flags, so it cannot see Q (the flags the previous instruction wrote,
    which SCF / CCF read X and Y from) after `EX AF,AF'`, a DD / FD prefix
    or a RESET cut; `z80ccf` can.

Earlier, main carried `scripts/woodmass_runner.c`, a CPU-only driver that
loaded the `.tap` at 0x8000 and stubbed the ROM's print routine. It could
not drive the menu (that needs the ROM's keyboard routine and the frame
interrupt), so it never ran the tests; no CI job ran it either. It was
removed in favour of the speccy run.

## HALT2INT v3

HALT2INT v3 is Mark Woodmass's 2021 follow-up that tests HALT-to-INT
acceptance timing on a real 48K Spectrum
([zxe.io](https://zxe.io/depot/software/ZX%20Spectrum/HALT2INT%20v3%20%282022-01-04%29%28Woodmass%2C%20Mark%29%20%5B%21%5D.zip),
GPLv2, source `.asm` included; not vendored). It calls the ROM's CLS and
OPEN-CHAN and times itself against ULA contended memory, so it too is a
Spectrum program, not a CPU-only test. The CPU behaviour it verifies --
the HALT M1 leaves PC past the HALT byte, the halted M1s fetch at that
PC, INT and NMI push it unchanged, and INT is sampled at the last
T-state of each halted M-cycle -- is checked on main against the die:

  - `make event_sweep` pulses INT around `HALT` at every half-cycle
    against perfectz80 and compares every pin;
  - `tests/traces/pin_scenarios/prog13_halt_int.hex` (`make
    pin_scenarios`) exits HALT on INT, every phase compared with the die;
  - FUSE case `76` (HALT) is checked on the die by `make fuse_pz80`;
  - `make halt2int` ([`scripts/halt2int_probe.c`](../../scripts/halt2int_probe.c))
    asserts INT at ten points of the halted NOP cycle and requires each
    INT-to-INTA delay to equal the die's (6 5 4 7 6 5 4 7 6 5 T-states).
