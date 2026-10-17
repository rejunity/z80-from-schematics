# RZ80 BASIC SBC on the IcePi Zero

A Z80 single-board computer in one FPGA: the rz80 core (`rtl/z80_top.v`),
64 KiB of RAM with **NASCOM BASIC 4.7** (RC2014 build) preloaded, and a
68B50-style ACIA on the board's FTDI USB-UART at **115200 8N1**. Target:
[IcePi Zero](https://github.com/cheyao/icepi-zero) (Lattice ECP5
**LFE5U-25F**, CABGA256, 50 MHz MEMS oscillator).

    make venv     # once: python venv + pip-installed YoWASP FPGA toolchain
    make          # bitstream -> build/top.bit
    make prog     # load to FPGA SRAM   (openFPGALoader, volatile)
    make flash    # write to SPI flash  (persists across power cycles)

Then connect a terminal to the board's USB serial port:

    screen /dev/cu.usbserial-* 115200      # macOS; Ctrl-A K to quit

Press Enter at the `Memory top?` cold-start prompt and you're at
`Z80 BASIC Ver 4.7c` / `Ok`. **Button 0** resets the Z80 (RAM survives,
so you get the ROM's warm-start `C|W` prompt).

Terminal notes: BASIC wants CR line endings (screen sends CR — good),
and Ctrl-C is BREAK just like `make basic`.

This board has only the SBC build. A Z80 *chip-replacement* build (the
live bus on a header, clock from outside) needs 37 header pins; the
IcePi's 40-pin header has 28 user I/O. The Tang Primer 25K port
([`../tang-primer-25k`](../tang-primer-25k/README.md)) has both builds.

## What's in the SBC (`top.v`)

| Piece | Detail |
|---|---|
| Clock | ECP5 PLL: 50 MHz → **4 MHz Z80 clock** (`CORE_HZ`; `3_500_000` gives the 3.5 MHz Spectrum rate). That clock *is* the Z80's CLK: one T-state per period, core state on the rising edge, the chip's falling-edge pin behaviour (MREQ/RD/WR edges, WAIT and data sampling) on the falling edge — see `docs/clocking.md`. No 2× clock, no clock enable; RAM, UART and ACIA all run in this one domain |
| Memory | one 64 KiB block RAM, preloaded with the ROM image at configuration; writes below `0x2000` dropped (ROM protection). Synchronous read = one T-state of latency, which the Z80 bus timing absorbs without wait states |
| Console | `uart.v` (8N1 TX/RX) + 32-byte RX FIFO behind a 68B50-ACIA shim: `IN (0x80)`=status RDRF/TDRE, `OUT (0x80)`=control, `IN/OUT (0x81)`=data — the same map as `scripts/basicrunner.c`. Tiny BASIC's ports `0x00/0x01` are decoded too |
| Interrupts | real 68B50 IRQ semantics: `/INT` = (RIE & RX-byte-pending) \| (TIE & TX-idle), with RIE/TIE decoded from control-register writes (IM 1; INTA gets `0xFF` = RST 38h). TIE matters: the ROM's z88dk ACIA driver uses an interrupt-driven TX ring buffer, and a status-only shim deadlocks it at real baud (fast-baud sims never enter that path — hence `make sim_real`) |
| Reset | power-on counter + button 0 (debounced, ~5 ms stretch) on the 50 MHz clock, release synchronized to the Z80 clock; `wait/nmi/busreq` tied idle |
| LEDs | 0 heartbeat · 1 TX activity · 2 RX activity · 3 RX FIFO non-empty · 4 HALT |
| Header | the Z80 bus mirrored as **outputs** on the 40-pin header (below) |

### Header mirror

`gpio[27:0]` (the vendored LPF's names for the 40-pin Pi-style header)
carry the bus so a logic analyser or an external bus display can watch
BASIC run. 28 pins is a subset of the Tang port's full 37-pin chip pinout:

| gpio | signal | gpio | signal |
|---|---|---|---|
| 0–15 | A0–A15 | 24 | /MREQ |
| 16–23 | D0–D7 | 25 | /IORQ |
| | | 26 | /RD |
| | | 27 | /WR |

D shows the byte on the internal bus: the CPU's write data while it
drives, otherwise what memory / the ACIA returned. All pins are 3.3 V
outputs, not 5 V tolerant. Header pin numbers per `gpio` index are in
`icepi-zero.lpf`.

## Toolchain — venv, no system packages

`make venv` creates `.venv/` and pip-installs the
[YoWASP](https://yowasp.org/) WebAssembly builds of the open ECP5 flow:
`yowasp-yosys`, `yowasp-nextpnr-ecp5` (which ships `yowasp-ecppack` and
`yowasp-ecppll`). Sources are staged into `build/` and the tools run from
there because the YoWASP sandbox only sees the current directory tree.

Flashing is the one native tool: `openFPGALoader` (`brew install
openfpgaloader`), which knows the board as `-b icepi-zero`.

Constraints: `icepi-zero.lpf` is vendored from the board repo (v1.3,
zlib-style licence). The PLL output carries its own frequency attribute,
so nextpnr times the Z80 domain without an extra constraint.

Synthesis runs `proc; async2sync` first (the core's async-assert reset
constants conflict with FF init values; the POR holds reset for thousands
of clocks, so sync semantics are equivalent). `z80_top`'s negative-edge
flops map onto ECP5's inverted-clock FF mode.

## Simulation

    make sim            # Verilator, ~seconds  (needs verilator in PATH)
    make sim_real       # same at true 115200 pacing (exercises the TIE path)
    make sim_iverilog   # same script through iverilog, much slower

All three boot the *real* ROM through the *real* top-level — PLL stand-in
divider (50 MHz / 12 = 4.17 MHz), POR, BRAM, UART bits on the pins,
interrupt-driven RX — and script a session: answer `Memory top?`, wait
for `Ok`, type `PRINT 123+456`, expect `579`; the Verilator script then
presses button 0 with a bouncy release and expects the warm-start prompt.
The fast variants speed the UART up to 16 Z80 clocks per bit
(`-GBAUD=260417`) so serial framing doesn't dominate sim time.

Verilator result on this branch:

    Memory top?
    Z80 BASIC Ver 4.7c
    Copyright (C) 1978 by Microsoft
    31948 Bytes free
    Ok
    PRINT 123+456
     579
    [sim_top] 123+456=579 OK -- pressing button 0 (reset)
    ...
    Cold | Warm start (C|W) ? W
    Ok
    [sim_top] PASS: boot, arithmetic, button warm-restart all good

## Variants

Tiny BASIC instead of NASCOM:

    make ROM_HEX=../../tests/basic/tinybasic_1k.hex

(Port map already decoded; Tiny BASIC needs no `/INT`.)
