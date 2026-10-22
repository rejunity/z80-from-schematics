# speccy — a 48K ZX Spectrum in a terminal

speccy runs a 48K Spectrum on this repository's Z80. It uses **cfast**,
the generated phase engine for the C model, so the CPU is exact to the
half T-state. The ULA is stepped once per pixel clock, and the picture is
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

Each step of `zx_frame()` is one 7 MHz pixel clock. The ULA draws a pixel
and decides whether the CPU's clock moves on. If it does, cfast produces
the next CPU phase and the machine serves the bus from the pin word: ROM,
RAM, port 0xFE (keyboard, EAR, border), and INT for the first 32 T-states
of each frame.

With GCC or Clang the CPU runs in cfast's run frame (`z80f_run`). The bus
and the ULA's tick are inlined into each generated step, as `ZF_BUS`.
Other compilers get a `z80f_tick` loop with the same phases.

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
ten times a second. The same applies while the ROM starts up and
`LOAD ""` is typed. Speed reaches 25× real time headless and about 11×
while drawing, so a 4-minute tape loads in 10-20 s. **F12** runs flat out
at any time.

## The picture

`term.c` ports scr2term, a Python prototype, to C. A terminal cell is one glyph in
two colours, and the Spectrum gives each 8×8 character two colours (ink
and paper). So a cell that stays inside one character loses nothing.

| `--mode` | Cells for the paper | How |
|---|---|---|
| 128 | 128×48 | Octants (Unicode 16's 2×4 block characters): one block per pixel. Exact. |
| 120 | 120×48 | The same, but the 4th of every 4 character columns is 6 pixels wide, without its pixel columns 0 and 7. |
| 80 | 64×24 | Two cells per character. A character whose 8×8 bitmap matches a glyph of the ROM font, or its inverse, is printed as that character. The rest is octants at half resolution (`--crisp`: no blended shades). |

- **Mode 120 and text:** columns 0 and 7 are blank in the ROM font's
  letters and digits, except the top-left pixel of T and Y.
- **Choosing a mode:** without `--mode`, speccy picks the widest that
  fits the terminal.
- **The border:** shown as far as there is room.
- **Changed attributes or border:** where the picture has more than two
  colours in one cell, the cell keeps the two most common.
- **Updates:** each frame sends only the cells that changed, inside a
  synchronized update (mode 2026).
- **Fallbacks:** `--braille` uses braille dots for terminals without
  octants. `--colors 256` (the default without `COLORTERM=truecolor`)
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

Not tested here: macOS, the Linux console and Windows.

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
