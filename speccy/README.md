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
./build/bin/speccy game.tzx   # the same for a TZX: turbo and custom loaders
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

It also runs the tape player (`speccy/test_tape.c`), which needs the ROM, so
`make speccy_test` fetches it first:

| Check | Result |
|---|---|
| The pulses of every kind of TZX block: lengths, levels, pauses, flow control, stops, Z-RLE (stored, fixed and dynamic codes), malformed files | 34 checks |
| A `.tap` and the same blocks eight TZX ways (`speccy/tzxgen.py`): standard, turbo, pure tone + pulses + pure data, generalized, direct recording, CSW RLE and Z-RLE, and flow control with two stops, loaded through the ROM by `LOAD ""` | the same 700 bytes at 32768 and `0 OK` from each; the flow tape stops twice and starts again |

## The tape and fast-forward

`tape.c` plays a `.tap` the way the ROM's SA-BYTES writes one:

| Part | Length of each pulse |
|---|---|
| Pilot | 2168 T: 8063 pulses before a header, 3223 before data |
| Sync | 667 T, then 735 T |
| Bits | two pulses of 855 T for a 0, 1710 T for a 1 |
| After each block | one second of silence |

A `.tzx` (version 1.20) describes the signal itself, and `tape.c` plays
every block that makes one, so turbo and custom loaders load as the
cassette did:

| Block | What it plays |
|---|---|
| 0x10 standard data | as a `.tap` block, with the block's own pause |
| 0x11 turbo data | the same shape at the block's pilot, sync and bit lengths, pilot count and bits in the last byte |
| 0x12 pure tone, 0x13 pulse sequence, 0x14 pure data | a custom loader's parts, pulse by pulse |
| 0x15 direct recording | the samples as levels, a run of equal samples one pulse |
| 0x18 CSW recording | RLE, or Z-RLE inflated by a small built-in inflate |
| 0x19 generalized data | the pilot/sync and data symbol tables, each symbol's first pulse as its flags say (edge, no edge, low, high) |
| 0x20 pause, 0x2A stop if 48K | silence; a 0 ms pause or 0x2A stops the tape (see below) |
| 0x2B signal level | sets the level |
| 0x21/0x22 groups, 0x23 jump, 0x24/0x25 loop, 0x26/0x27 call and return, 0x28 select | followed; a selection takes its first entry |
| 0x30-0x35, 0x5A, and blocks it does not know | nothing; unknown ones are skipped by their length, as the format's extension rule allows. In a headless run the archive info (0x32) is printed: title, publisher, year, loader |

A pulse toggles the level at its start, or leaves it, or sets it, where a
0x19 symbol or a direct recording says so. A pause starts with the edge that
ends the last pulse, holds that level for 1 ms, then goes low (TZX 1.20, as
FUSE plays it).

The tape motor runs while a program reads the tape. That is detected two
ways: port 0xFE is read thousands of times a frame, or the PC is inside
the ROM's tape routines. The motor stops 3 s after the reading stops,
because the ROM sits in a one-second delay loop when it first hears a
tone. Multi-load games therefore pause and resume by themselves.

A stop block ("stop the tape": a 0 ms pause, or 0x2A on this 48K machine)
stops the tape where it is. It starts again once a program has read the
tape for half a second without a break: a loader waiting for the next
part, which is when someone would press PLAY. A loader that polls straight
through the stop starts it half a second later. A game that runs after
the stop leaves it stopped until it loads its next part. The title bar,
and a headless run's last line, say "stopped by the tape" meanwhile.

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
in every font) instead of octants, in one of two ways:
- `--quadrants=96` (the default when the window has 96 rows): 128×96
  cells, every pixel exact, but twice as tall as wide, since a terminal
  cell is about 1:2.
- `--quadrants=48`, half-height: the octants' 128×48 cells, each quadrant
  two pixels tall. Where the two differ, the quadrant blends them in
  linear light (`--crisp`: takes the ink's colour, bolder text but
  blobbier sprites).

`--bars` draws modes 128 and 120 as half-width bars: the octants' 128×48
cells, each four bars one above the other, a bar two pixels wide and one
tall, so the pixels stay square and the columns are halved instead of the
rows. Where a bar's two pixels differ, it blends them in linear light
(`--crisp`: takes the ink's colour). The glyphs are ▀ ▄ ▂ ▆ (in every
font), with the colours either way round: the 8 patterns whose colour
changes at most once down the cell. The other 8 (a bar alone in the
middle, bars that alternate) become the nearest of those, with the least
squared error in linear light; on a tie, the colour with fewer bars keeps
them, so a stroke one pixel tall is drawn two tall rather than dropped.

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

## A slow link

Each frame sends only the cells that changed, but a game that scrolls
changes most of them every frame. Measured in 24-bit colour, every frame
drawn (`--measure`, below), bytes a frame and what 50 frames a second
need:

| Game (Spectrum Computing, "Available") | What moves | Octants 128×48 | Braille 128×48 | Half blocks 256×96 | Text 64×24 |
|---|---|---|---|---|---|
| Zynaps | the game, scrolling sideways | 4,528 B · 227 KB/s | 1,867 · 94 | 9,397 · 471 | 3,249 · 163 |
| Stop the Express | its demo, scrolling sideways | 3,872 · 194 | 1,243 · 62 | 8,208 · 411 | 1,908 · 96 |
| Moon Cresta | the game, a starfield | 2,365 · 118 | 1,331 · 67 | 4,737 · 237 | 1,859 · 93 |
| Uridium | its demo, the dreadnought scrolling past | 1,125 · 56 | 287 · 14 | 2,666 · 134 | 483 · 24 |
| Deathchase | the game, trees rushing at the bike | 419 · 21 | 217 · 11 | 861 · 43 | 244 · 12 |
| Exolon | the game, flip-screen (for contrast) | 323 · 16 | 204 · 10 | 822 · 41 | 309 · 16 |

The biggest single frames, where the whole screen changes colour, reach
58 KB in octants and 133 KB in half blocks (Uridium); a frame where the
whole picture moves is 14-15 KB in octants. 256 colours (`--colors 256`)
take about a fifth off (Zynaps in octants: 3,627 B a frame). Over a serial line at 230,400 baud (23,040 bytes a second, 10
bits a byte) or 115,200 (11,520), the frames a second each gets, octants:

| Game | 115,200: game slowed / skipping | 230,400: game slowed / skipping |
|---|---|---|
| Zynaps | 2.5 / 1.1 | 5.1 / 2.0 |
| Stop the Express | 3.0 / 1.0 | 6.0 / 2.0 |
| Moon Cresta | 4.9 / 2.0 | 9.7 / 6.3 |
| Uridium | 10.2 / 2.0 | 20.5 / 3.9 |
| Deathchase | 27.5 / 10.0 | 50 / 50 |
| Exolon | 35.7 / 16.7 | 50 / 50 |

"Slowed" draws every frame and the game runs at that many frames a
second; "skipping" keeps the game at the Spectrum's 50.08 Hz and draws
every n-th frame, each carrying n frames' changes -- for scrolling, nearly
a whole screen, so the line carries one every half second or so. (Means
over a minute or so of each game; the bursts are larger.)

Measured: each game through a pty whose far end reads 23,040 or 11,520
bytes a second and answers `CSI 5 n`, 20 s of play straight after
`--fast-until`, octants in 24-bit colour. In every run the game ran 19.95
s of the Spectrum's time in 19.95 s, and its last screen was the same,
cell for cell, as an unthrottled run's:

| Game | 230,400 baud: frames drawn a second | 115,200 baud |
|---|---|---|
| Deathchase | 32.2 | 23.9 |
| Exolon | 17.9 | 6.3 |
| Uridium | 12.6 | 2.2 |
| Moon Cresta | 6.3 | 2.7 |
| Zynaps | 4.5 | 1.1 |
| Stop the Express | 1.8 | 0.8 |

speccy keeps the game at its own speed and skips drawing when the link is
behind:

- **Even steps.** It draws every frame, every 2nd (25 a second), every
  3rd (16.7) and so on. Every half second it looks back: if the link was
  behind when a frame was due, the step goes up one; once the link has
  kept up for a second, it comes down to the lowest step that would keep
  the link at most 85 % busy (from how busy it was at this step). A step
  down that does not hold makes the next try wait longer (up to 8 s).
- **Knowing the link is behind**, through whatever lies between:
  - after each frame speccy sends `CSI 5 n`, and the terminal answers `CSI
    0 n` once it has read everything before it, in order with the keys:
    the time to the answer is how long the frame took to get there,
    through a pty, ssh's buffers, a serial line or the terminal's own
    parser. A frame still unanswered a whole drawing period (n frames)
    after the quickest recent answer means the link is behind at this
    step, and the frames' time on the way (beyond the quickest answer) is
    how busy it was. When nothing is on its way, a question alone, at
    most once a second (4 bytes), gives the quickest answer without a
    frame in it. The answers are
    drained before speccy hands the terminal back, so none reaches the
    shell. Terminals that do not answer within 2 s are not asked again;
    the console's raw keycodes and Windows get no questions.
  - the output goes out without blocking, through a second open of the
    terminal device (stdout's file flags, shared with the shell, are left
    alone): what the device does not take waits in speccy, and nothing
    new is drawn until it has gone. A big frame never stops the game.
  - `TIOCOUTQ`, where the device reports its queue (serial lines; Linux
    ptys always say 0).
  - `--baud N`: a budget of N/10 bytes a second, for a serial line at N
    baud (or to try one), with a 200 ms burst; there the bytes sent are
    how busy it is. Without answers or a budget, the bytes the device took
    while the queue was full stand for what the link takes.
- **The picture stays right.** A frame is only ever sent whole, and the
  next one is the difference from it, so whatever is skipped, the screen
  ends up exactly as drawn without skipping (tested below).
- `--min-fps F` draws at least F frames a second, the game waiting for the
  link when it cannot keep up; `--no-skip` draws every frame and waits
  (the game slows, as before).
- `--stats` puts the frames drawn and emulated a second, bytes a frame,
  KB/s and frames skipped in the title, and a summary on exit.

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
For programs that want keys: `--type F:TEXT` types from frame F (`\n` is
ENTER), `--type-on S:TEXT` types whenever the screen shows S (and prints
that screen), `--exit-on S` stops when it shows S, and `--shot F:NAME`
writes `NAME.scr` and `NAME.txt` after frame F. `--type` works without
`--frames` too, and `--fast-until N` runs flat out to frame N, so a game
reaches the same frame with the same keys every time.

`--measure FILE` (with `--frames`) draws every frame into a terminal of
its own (`--size CxR`, default the mode's) and writes the bytes each one
would send; `--measure-from F` starts at frame F, `--measure-every D`
draws every D-th frame, and on POSIX `--measure-set GLYPHS:D,...` forks
one measurer per mode and step at frame F, so a game loads once. The
tables above come from it; the skipping was tested through a pty whose
far end reads 23,040 or 11,520 bytes a second and answers `CSI 5 n` like
a terminal.
Patrik Rak's z80test tapes load through the ROM and run:

```
./build/bin/speccy tests/z80test/z80doc.tap --frames 8000
Z80 doc test        2012 RAXOFT
000 SELF TEST                 OK
001 SCF                       OK
...
```

`make speccy_woodmass` runs Mark Woodmass's Z80 Test Suite (2008,
`tests/woodmass/`), whose results are a real NEC D780C-1's: it picks the
Flags test from the suite's menu, then the MEMPTR test, answers every
`scroll?`, and fails unless all 47 and all 58 tests pass. They do, in 14
minutes of the Spectrum's time (nearly all of it the Flags test), about a
minute of the host's:

```
./build/bin/speccy tests/woodmass/z80-test-suite-2008.tap --frames 60000 \
    --type 3100:1 --type-on 'scroll?:y' --exit-on 'press a key'
== frame 10250: scroll?
      Testing Z80 flags...
SCF                : 0EBF passed
CCF                : 3CED passed
...
CB (00-FF) 5+3 RAM : 1B66 passed
All tests finished - press a key
```

On macOS (Apple clang, the system's make 3.81 and Python 3.9), the
`macOS` workflow builds speccy, runs `make speccy_test`, fetches the ROM
and boots it headless; this branch runs it by hand,
`gh workflow run macos.yml --ref speccy`.
