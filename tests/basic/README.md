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
