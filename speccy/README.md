# speccy — a 48K ZX Spectrum in a terminal

speccy runs a 48K Spectrum on this repository's Z80. It uses **cfast**,
the generated phase engine for the C model, so the CPU is exact to the
half T-state. The ULA is modelled from the die and makes the CPU's clock,
so contention comes out of the CPU's pins with no tables. The picture is
drawn as text in any terminal that has Unicode block characters.

```
make speccy speccy_rom        # build/bin/speccy, and the ROM beside it
./build/bin/speccy            # the 48K BASIC
./build/bin/speccy game.tap   # types LOAD "", plays the tape at full speed
```

`make speccy_rom` downloads FUSE 1.6.0's copy of the 48K ROM and checks
its SHA-1. Amstrad allow the ROM to be distributed but not sold, so it is
not kept in this tree. speccy finds the ROM through `--rom`, then
`$SPECCY_ROM`, then `48.rom` next to the binary, then Debian's
`/usr/share/spectrum-roms/48.rom`, then FUSE's `share/fuse/48.rom`.

## The machine

After each CPU phase the machine serves the bus from cfast's pin word
(ROM, RAM, and the interrupt acknowledge's idle 0xFF). The ULA then runs
its 7 MHz pixel clocks up to the CPU's next phase:
- normally one;
- more while it holds the CPU clock.

The next phase then gets its inputs:
- an I/O read's byte: the keyboard and EAR from port 0xFE, or the
  floating bus;
- INT;
- RESET.

With GCC or Clang the CPU runs in cfast's run frame (`z80f_run`). The bus
and the ULA's step are inlined into each generated step, as `ZF_BUS`.
Other compilers get a `z80f_tick` loop with the same phases.

## The ULA

`ula.c` is written from the die: the CC0 gate-level reconstruction of a
6C001E-7 by [emu-russia/ula](https://github.com/emu-russia/ula), checked
against the published timings below.

**Counters.** C counts 0..447 pixel clocks a line and V counts 0..311
lines. The paper area, where the ULA fetches and contends, is C < 256,
V < 192.

**The CPU clock** is `CLK = C0 | HOLD`. It rises as C turns odd, when
the CPU enters phi 0 and a T-state begins. It falls as C turns even,
unless HOLD keeps it high:

```
HOLD = CLK & (C2|C3) & paper & !IOREQT2 & ((A14 & !A15 & !MREQT2) | (IORQ & !A0))
```

MREQT2 and IOREQT2 are transparent latches: they follow MREQ and
IORQ & !A0 while the clock is low and hold while it is high. The board
gates the ULA's /IOREQ with A0.

So the ULA only ever stretches the high half of a T-state that has a
lower-16K address on the bus and no MREQ yet:
- a T1;
- each internal T-state (`hl:1`, `ir:1`, ...);
- a T2 for its own port.

A T1 rising into C[3:0] = 3, 5, 7, 9, 11, 13, 15, 1 of a paper line waits
6, 5, 4, 3, 2, 1, 0, 0 T-states. That is FUSE's pattern from T-state
14335.

The step is per CPU phase, not per pixel clock. While a hold lasts,
nothing it reads changes except the C2|C3 window, so it lasts to the next
C that is a multiple of 16.

**Video:**
- in each 16-pixel group the ULA fetches bitmap, attribute, bitmap,
  attribute at C[3:0] = 9, 11, 13, 15;
- the shift register loads at C[2:0] = 4, and the attribute/border latch
  at 5;
- paper column c shows at C = 13 + 8c, so the border changes in 4-T-state
  cells aligned with the paper columns;
- the port latch is written from T2-low of an `OUT`;
- FLASH flips every 16 frames.

**INT.** The die pulls INT on line 248 for C 0..63, 32 T-states. A Z80
that samples INT at the rising edge of an instruction's last T-state, as
this one does, sees FUSE's "early" timing with it on for C 2..65. That is
the default; `--late` uses C 0..63.

**Floating bus.** An IN that no device answers (A0 = 1) reads what the
ULA fetched for the period that ends on its T3 falling edge, or 0xFF.

**Port 0xFE** reads `0xA0 | EAR << 6 | keys`, and bits 5 and 7 are never
driven. With no tape, EAR in is EAR out (bit 4), as on issue 3.

**Not modelled:**
- the snow effect (I in 0x40-0x7F);
- issue 2's EAR (bit 3 | bit 4);
- the analogue lag of EAR read-back after an OUT.

`make speccy_test` runs the machine itself, cfast and the ULA, against
the published 48K timings. Everything matches:

| Check | Result |
|---|---|
| FUSE's whole-frame contention checksum, early / late | 2308862976 / 2308927488 |
| FUSE's floating-bus checksum, early / late | 3427723200 / 3426156480 |
| The four I/O contention patterns | N:4, N:1 C:3, C:1 C:3, C:1 C:1 C:1 C:1 |
| Twelve instructions' patterns at every T-state of the frame | NOP, LD A,(HL), INC (HL), LD (HL),n, PUSH, POP, JR, INC BC and LD A,I with I = 0x40, LDI, CALL, EX (SP),HL |
| INT accepted at boundaries | 0..31 only |
| Border changes | at the first 4-T-state cell after the OUT's T2 |

## The tape and fast-forward

`tape.c` plays a `.tap` the way the ROM's SA-BYTES writes one:

| Part | Length of each half-wave |
|---|---|
| Pilot | 2168 T: 8063 half-waves before a header, 3223 before data |
| Sync | 667 T, then 735 T |
| Bits | two half-waves of 855 T for a 0, 1710 T for a 1 |
| After each block | one second of silence |

Any loader that reads standard blocks works, the ROM's or the program's
own.

The tape motor runs while a program reads the tape. That is detected two
ways: port 0xFE is read thousands of times a frame, or the PC is inside
the ROM's tape routines. The motor stops 3 s after the reading stops,
because the ROM sits in a one-second delay loop when it first hears a
tone. Multi-load games therefore pause and resume by themselves.

While the tape plays, the machine runs flat out and the picture updates
ten times a second, so the loading stripes show. The same applies while
the ROM starts up and `LOAD ""` is typed. Speed is about 17-19× real time
here (z80test's tape loads in about 5 s), so a 4-minute tape takes about
15 s. **F12** runs flat out at any time.

## The picture

`term.c` ports scr2term, a Python prototype, to C. A terminal cell is one glyph in
two colours, and the Spectrum gives each 8×8 character two colours (ink
and paper). So a cell that stays inside one character loses nothing.

| `--mode` | Cells for the paper | How |
|---|---|---|
| 128 | 128×48 | Octants (Unicode 16's 2×4 block characters): one block per pixel. Exact. |
| 120 | 120×48 | The same, but the 4th of every 4 character columns is 6 pixels wide, without its pixel columns 0 and 7. |
| 80 | 64×24 | Two cells per character. A character whose 8×8 bitmap matches a glyph of the ROM font, or its inverse, is printed as that character. The rest is octants at half resolution (`--crisp`: no blended shades). |
| 256 | 256×96 | Half blocks (▀, in every font): a cell is two pixels, one above the other, each in its own colour. Exact, with square pixels, but it needs a small font. |

`--quadrants` draws modes 128 and 120 with 2×2 quadrant blocks (▘▝▀▖▌▞▛▗▚▐▜▄▙▟█,
in every font) instead of octants: 128×96 cells, every pixel exact, but
twice as tall as wide, since a terminal cell is about 1:2.

- **Mode 120 and text:** columns 0 and 7 are blank in the ROM font's
  letters and digits, except the top-left pixel of T and Y.
- **Choosing a mode:** without `--mode`, speccy picks the widest of 128,
  120 and 80 that fits the terminal. Where the terminal has no octants
  (below), it picks 256 instead when the window has 256×96 cells.
- **The border:** shown as far as there is room.
- **Changed attributes or border:** where the picture has more than two
  colours in one cell, the cell keeps the two most common.
- **Updates:** each frame sends only the cells that changed, inside a
  synchronized update (mode 2026).
- **Braille:** octants need a terminal that draws them itself (Ghostty
  and kitty do) or a font that has them. Terminal.app and VS Code's
  terminal have neither and show boxes, so there (by `TERM_PROGRAM`)
  speccy draws braille dots instead; `--braille` and `--octants` choose
  either way. Braille shows only its dots, so the dots are the ink
  pixels, in their colour, on the paper's colour, however much of a cell
  the ink covers: letters, sprites and solid shapes are all dots. The
  ULA passes each pixel's bitmap bit and its attribute's other colour
  along for this. The exception is a character that is mostly ink of the
  colour of the paper around it (Horace's road: black ink with white
  dashes left as paper, beside black paper); there the paper is the dots.
- **Colours:** `--colors 256` (the default without `COLORTERM=truecolor`)
  is exact, because the xterm colour cube's levels 215 and 255 are the
  Spectrum's normal and bright intensities.

## Keys

A Spectrum program reads the keyboard as a matrix of keys held down, so it
needs key releases. A terminal normally sends characters only: a press,
then autorepeat, and no release. These are the ways to get presses and
releases without third-party libraries, and when speccy uses each
(`--keys` forces one):

| `--keys` | Where | How | Press and release | Needs |
|---|---|---|---|---|
| `console` | Linux virtual console | `KDSKBMODE` `K_MEDIUMRAW`: keycodes with a release bit | yes | being on a VT; restored on exit and on a crash |
| `kitty` | terminals with the kitty keyboard protocol (kitty, foot, Ghostty, Alacritty, WezTerm with `enable_kitty_keyboard`, recent iTerm2, others) | `CSI > 15 u`: press, repeat and release as escape codes, found with `CSI ? u` | yes | the terminal; works over SSH |
| `evdev` | Linux, any terminal | the kernel's keyboards, `/dev/input/event*` | yes | read access (the `input` group); local only |
| `macos` | macOS, any terminal | CoreGraphics `CGEventSourceKeyState`, polled; loaded at run time, no build flags | yes | Input Monitoring permission for the terminal app; local only |
| `windows` | Windows console (conhost, Windows Terminal) | `ReadConsoleInput` key records | yes | nothing |
| `legacy` | anything else | the characters, each a tap held 4 frames, extended while it autorepeats; the same key again waits 6 frames, since the ROM takes a key that comes back within 5 as one press | no | nothing |

`auto` tries them in that order:
1. `console`;
2. `kitty`;
3. `evdev`;
4. `macos`;
5. `legacy`.

Windows always uses `windows`.

**Focus:** evdev and CoreGraphics read the whole keyboard, whatever window
has focus. speccy therefore asks for the terminal's focus reports
(`CSI ? 1004 h`), lets go of every key when focus leaves, and ignores the
keyboard until focus returns. X11's `XQueryKeymap` would also work, but it
needs libX11, and Wayland has no equivalent.

**The layout** follows the Spectrum's own:
- letters, digits, Space and Enter are the keys in the same places;
- Shift is CAPS SHIFT, and Ctrl or Alt is SYMBOL SHIFT;
- the host's punctuation keys type their symbol (SYMBOL SHIFT plus the
  key it is on);
- Backspace is DELETE, the arrows are CAPS SHIFT 5-8, Esc is BREAK, and
  Tab is EXTENDED MODE.

F5 resets, F12 toggles full speed, and F10 or Ctrl-C quits.

Tested here:
- evdev with a uinput keyboard;
- the kitty and legacy parsers on byte streams;
- legacy mode in a pty.

Not tested here: the macOS key paths, the Linux console and Windows.

## Testing

`--frames N` runs headless for N frames, then prints the screen's text by
matching the ROM font. `--save-scr` also writes the screen as a `.scr`.
Patrik Rak's z80test tapes load through the ROM and run:

```
./build/bin/speccy tests/z80test/z80doc.tap --frames 8000
Z80 doc test        2012 RAXOFT
000 SELF TEST                 OK
001 SCF                       OK
...
```

On macOS (Apple clang, the system's make 3.81 and Python 3.9), the
`macOS` workflow builds speccy, runs `make speccy_test`, fetches the ROM
and boots it headless; this branch runs it by hand,
`gh workflow run macos.yml --ref speccy`.
