# Z80 BASIC ROMs run on our emulator

Two ROMs vendored here for the `basic` branch. Driven by
[`scripts/basicrunner.c`](../../scripts/basicrunner.c):

  - `make basic`     — runs NASCOM BASIC 4.7 interactively (C model)
  - `make tinybasic` — runs the 1 KiB Tiny BASIC interactively (C model)

Plus two non-interactive regression suites that feed canned scripts in and
assert against expected substrings in the BASIC output:

  - `make basic_c_tests`   — canned-script regression via C model     (~0.5 s)
  - `make basic_rtl_tests` — same canned-script regression via Verilator RTL
    (`~1 s` sim + Verilator build time)

## NASCOM BASIC 4.7 (RC2014 build)
File: `nascom_basic_4_7_rc2014.hex`  (Intel HEX, 8154 bytes of ROM 0x0000-0x1FD9)
Source: https://github.com/feilipu/NASCOM_BASIC_4.7 (Phillip Stevens'
RC2014 / NascomBasic32k 8 kB ROM build).
Provenance: Microsoft Basic 4.7 (1978 NASCOM port), adapted for Z80 +
RC2014's 68B50 ACIA at I/O ports 0x80 (status/control) and 0x81 (data).
License: MS Basic 4.7 has been freely redistributable since the late
1970s "give-it-away" letter; the RC2014 wrapper code is Phillip Stevens'
non-commercial license.

I/O conventions the runner emulates:
  - `IN  (0x80)` -> ACIA status: bit 0 RDRF, bit 1 TDRE (always 1 here)
  - `IN  (0x81)` -> read pending stdin byte (consumed)
  - `OUT (0x81),A` -> write byte to stdout
  - `OUT (0x80),A` -> ACIA control reset / baud / parity (ignored)
  - /INT pin asserted while a stdin byte is queued — NASCOM RX is fully
    interrupt-driven, so this is required, not just an optimisation.

Use CR (\r, 0x0D) as line terminator. LF alone is ignored.

### Interactive controls

| Key                 | Byte after translation | Effect                                                  |
|---------------------|------------------------|---------------------------------------------------------|
| Enter               | CR (0x0D)              | submit line to BASIC                                    |
| Backspace / Delete  | 0x7F → 0x08 (BS)       | erase last character                                    |
| **Ctrl-C**          | 0x03                   | BREAK / interrupt a running BASIC program               |
| **Ctrl-Space**      | 0x00 → 0x03            | equivalent to Ctrl-C (BREAK), translated by the runner  |
| **Ctrl-\\**         | 0x1C                   | **exit the runner cleanly** (restores tty)             |

`ISIG` is off in the tty setup so Ctrl-C reaches BASIC instead of being
captured by the kernel as SIGINT. Ctrl-Space is provided as an
alternative BREAK key for users who'd rather keep their shell-level
Ctrl-C habits or whose terminal has Ctrl-C bound to something else.
To leave the emulator, press **Ctrl-\\**.

#### Caveat: BREAK in Tiny BASIC during `RUN`

The 1K Tiny BASIC ROM has no interrupt-driven RX path and the BASIC
bytecode loop never checks stdin between statements — so Ctrl-C is
only seen when Tiny BASIC's `NextCharLoop` is actively running (i.e.
when the interpreter is waiting at a `>` prompt for the next program
line). **A running Tiny BASIC program cannot be interrupted with
Ctrl-C; use Ctrl-\\ to exit the runner.**

NASCOM BASIC does have an interrupt-driven RX (the runner wires
ACIA RX-ready to the Z80 /INT pin), so Ctrl-C will BREAK a running
program at the next statement boundary.

The runner itself stays responsive to Ctrl-\\ in either case: every
phase it drains all currently-readable bytes from the kernel into a
256-byte ring buffer, intercepting Ctrl-\\ at that boundary. So even
mid-`RUN` of a tight Tiny BASIC loop, Ctrl-\\ exits immediately.

### About the "Memory top?" prompt

On cold power-up, NASCOM BASIC's init code at `0x0148` checks the
`basicStarted` byte at RAM `0x802F`. If it's NOT `'Y'` (which is the
case in our emulator since `z80_sys_init` zeros all RAM), the loader
**skips** its own `Cold | Warm start (C|W) ?` prompt and goes directly
to BASIC's cold-start path. BASIC then prints `Memory top?` to ask the
user for the top of usable RAM; a blank reply (just CR) accepts the
default (top-of-RAM minus work-space, about 32 KB free here).

For interactive use, just press Enter at the prompt. For non-interactive
runs, pass `--autostart` to `basicrunner` (the default for `make
basic`); it injects one CR before stdin is consulted so BASIC starts
straight at the `Ok` prompt. You can also use `--prefeed "STRING"` to
queue arbitrary bytes ahead of stdin.

On a subsequent reset of the same in-memory emulator (warm restart),
the `basicStarted` flag will already be `'Y'` and the loader's
`Cold | Warm start (C|W) ?` prompt will appear instead.

## 1K Tiny BASIC (Will Stevens' basic1K)
File: `tinybasic_1k.hex`  (Intel HEX, **1018 bytes** of ROM 0x0000-0x03F9 — fits in 1 K)
Source: https://github.com/WillStevens/basic1K  (1K Tiny BASIC for 8080/Z80,
runs unmodified on Z80 too).
Provenance: Will Stevens, 2023, MIT license. About 60% the size of Palo
Alto Tiny BASIC; supports GOSUB/RETURN, FOR/NEXT, RND, ABS, USR, and a
single array `@`.

I/O conventions:
  - `IN  (0)`     -> 0x00 if no byte queued, 0xFF if a byte is ready
  - `IN  (1)`     -> consume one stdin byte
  - `OUT (1),A`   -> write byte to stdout

Both port mappings (NASCOM 0x80 / 0x81 and Tiny BASIC 0 / 1) are recognised
simultaneously by `basicrunner`, so the same binary handles both ROMs.


## Canned-script regression: `make basic_c_tests` / `basic_rtl_tests`

The regression driver lives at [`run_basic_tests.sh`](run_basic_tests.sh)
and is shared by both the C and RTL paths — same scripts, same assertions,
two harnesses.

Each subtest is a pair of files in `scripts/`:

  - `<name>.in`     — lines fed to BASIC stdin (CR-LF terminated, with a
    one-space prefix on lines after the first to compensate for the
    NASCOM "drop first char after Ok\n" quirk).
  - `<name>.expect` — substrings that must appear in BASIC's stdout, in
    file order. Lines beginning with `#` are comments. The **last
    non-comment line** is treated specially (see "sentinel" below).

Filename prefix selects ROM + autostart mode:

  - `nascom_*`      — NASCOM BASIC 4.7c, runner started with `--autostart`.
  - `tiny_*`        — 1 KiB Tiny BASIC, no autostart.

Current subtest set:

  - `nascom_arith.in`   — integer + floating point arithmetic, mixed types
  - `nascom_loops.in`   — FOR / NEXT / GOTO control flow + nested loops
  - `nascom_strings.in` — string functions (LEFT$, RIGHT$, MID$, LEN, CHR$, ASC)
  - `tiny_arith.in`     — Tiny BASIC arithmetic + GOSUB / RETURN

### `--exit-on` sentinel

Both `basicrunner` (C) and `sim_basic` (Verilator) accept `--exit-on
<substring>`. When the substring shows up in the program's stdout, the
harness runs for a small grace window (`50_000` instructions) and then
terminates cleanly, instead of spinning out to its `--max-instr` cap.
`run_basic_tests.sh` derives the sentinel automatically from each
`.expect` file as the last non-comment, non-blank line (typically a
`DONE-ARITH` / `DONE-STRINGS` / `DONE-LOOPS` marker printed by the
script's final `PRINT`). This makes the RTL run fast: instead of spending
~10 minutes per subtest grinding through dead loops at Verilator speed,
each subtest exits ~50K instructions after its last `PRINT`.

The grace window matters: BASIC's prompt-print path executes hundreds of
instructions per character, so cutting too early can clip the trailing
`Ok` line and falsely-fail the next assertion. 50K is empirically clear
across both ROMs.


## See also

  - [scripts/basicrunner.c](../../scripts/basicrunner.c) — the runner source
    (Intel HEX loader, ACIA emulation, ring-buffered stdin, termios setup).
  - [`../verilator/sim_basic.cpp`](../verilator/sim_basic.cpp) — Verilator
    harness that wraps the same ACIA + `--exit-on` plumbing around the RTL.
  - [`run_basic_tests.sh`](run_basic_tests.sh) — the shared canned-script
    driver invoked by `make basic_c_tests` / `make basic_rtl_tests`.
  - [../../README.md](../../README.md) — top-level project overview.
  - [../../docs/architecture.md](../../docs/architecture.md) — the design
    contract for the Z80 core these ROMs run on.
