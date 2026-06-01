# Z80 BASIC ROMs run on our emulator

Two ROMs vendored here for the `basic` branch. Driven by
`scripts/basicrunner.c` via `make basic` (NASCOM) and (TODO) `make
tinybasic`.

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

| Key | Byte after translation | Effect |
|---|---|---|
| Enter | CR (0x0D) | submit line to BASIC |
| Backspace / Delete | 0x7F → 0x08 (BS) | erase last character |
| **Ctrl-C** | 0x03 | BREAK / interrupt a running BASIC program |
| **Ctrl-Space** | 0x00 → 0x03 | equivalent to Ctrl-C (BREAK), translated by the runner |
| **Ctrl-\\** | 0x1C | **exit the runner cleanly** (restores tty, returns to shell) |

`ISIG` is off in the tty setup so Ctrl-C reaches BASIC instead of being
captured by the kernel as SIGINT. Ctrl-Space is provided as an
alternative BREAK key for users who'd rather keep their shell-level
Ctrl-C habits or whose terminal has Ctrl-C bound to something else.
To leave the emulator, press **Ctrl-\\**.

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

Both port mappings (NASCOM 0x80/0x81 and Tiny BASIC 0/1) are recognised
simultaneously by `basicrunner`, so the same binary handles both ROMs.
