# RZ80 on the IcePi Zero — BASIC SBC and Z80 chip

Two builds, one `top.v`: **sbc** (below) and **chip**, the Z80 itself on
the 40-pin header with an external clock (see [Chip build](#chip-build)).

The sbc build is a Z80 single-board computer in one FPGA: the rz80 core (`rtl/z80_top.v`),
64 KiB of RAM with **NASCOM BASIC 4.7** (RC2014 build) preloaded, and a
68B50-style ACIA on the board's FTDI USB-UART at **115200 8N1**. Target:
[IcePi Zero](https://github.com/cheyao/icepi-zero) (Lattice ECP5
**LFE5U-25F**, CABGA256, 50 MHz MEMS oscillator).

    make venv     # once: python venv + pip-installed YoWASP FPGA toolchain
    make sbc      # bitstream -> build/top_sbc.bit   (make = both builds)
    make prog     # load to FPGA SRAM   (openFPGALoader, volatile)
    make flash    # write to SPI flash  (persists across power cycles)

(`prog` / `flash` take `MODE=sbc` (default) or `MODE=chip`.)

Then connect a terminal to the board's USB serial port:

    screen /dev/cu.usbserial-* 115200      # macOS; Ctrl-A K to quit

Press Enter at the `Memory top?` cold-start prompt and you're at
`Z80 BASIC Ver 4.7c` / `Ok`. **Button 0** resets the Z80 (RAM survives,
so you get the ROM's warm-start `C|W` prompt).

Terminal notes: BASIC wants CR line endings (screen sends CR — good),
and Ctrl-C is BREAK just like `make basic`.


## What's in the SBC (`top.v`)

| Piece | Detail |
|---|---|
| Clock | ECP5 PLL: 50 MHz → **4 MHz Z80 clock** (`CORE_HZ`; `3_500_000` gives the 3.5 MHz Spectrum rate). That clock *is* the Z80's CLK: one T-state per period, core state on the rising edge, the chip's falling-edge pin behaviour (MREQ/RD/WR edges, WAIT and data sampling) on the falling edge — see `docs/clocking.md`. No 2× clock, no clock enable; RAM, UART and ACIA all run in this one domain |
| Memory | one 64 KiB block RAM, preloaded with the ROM image at configuration; writes below `ROM_PROTECT_TOP` dropped (ROM protection: the ROM image rounded up to 1 KiB, `0x2000` for NASCOM BASIC, `0x0400` for Tiny BASIC). Synchronous read = one T-state of latency, which the Z80 bus timing absorbs without wait states |
| Console | `uart.v` (8N1 TX/RX) + 32-byte RX FIFO behind a 68B50-ACIA shim: `IN (0x80)`=status RDRF/TDRE, `OUT (0x80)`=control, `IN/OUT (0x81)`=data — the same map as `scripts/basicrunner.c`. Tiny BASIC's ports `0x00/0x01` are decoded too. A TX write that finds the transmitter still busy holds the CPU with `/WAIT` until the previous byte is out, so a program that writes without polling TDRE (Tiny BASIC) loses nothing |
| Interrupts | real 68B50 IRQ semantics: `/INT` = (RIE & RX-byte-pending) \| (TIE & TX-idle), with RIE/TIE decoded from control-register writes (IM 1; INTA gets `0xFF` = RST 38h). TIE matters: the ROM's z88dk ACIA driver uses an interrupt-driven TX ring buffer, and a status-only shim deadlocks it at real baud (fast-baud sims never enter that path — hence `make sim_real`) |
| Reset | power-on counter + button 0 (debounced, ~5 ms stretch) on the 50 MHz clock, release synchronized to the Z80 clock; `wait/nmi/busreq` tied idle |
| LEDs | 0 heartbeat · 1 TX activity · 2 RX activity · 3 RX FIFO non-empty · 4 HALT |
| Header | the Z80 bus mirrored as **outputs** on the 40-pin header (below) |

### Header

`gpio[27:0]` (the vendored LPF's names for the 40-pin Pi-style header).
A Z80 DIP has 38 signals; the header has 28 user I/O, so both builds
carry the first 28 of the priority list shared by all our boards —
memory strobes first, then as much address bus as fits: CLK, D0–D7,
/MREQ, /RD, /WR, A0–A15. (The Tang Primer 25K port has all 38;
/IORQ, /M1, /RFSH, /HALT, /BUSACK, /WAIT, /INT, /NMI, /BUSREQ are the
ones left out here.)

| gpio | signal | gpio | signal |
|---|---|---|---|
| 0–15 | A0–A15 | 24 | /MREQ |
| 16–23 | D0–D7 | 25 | CLK (header pin 22) |
| | | 26 | /RD |
| | | 27 | /WR |

In the sbc build every pin is an **output**, so a logic analyser or an
external bus display can watch BASIC run: CLK is the internal 4 MHz Z80
clock, D the byte on the internal bus (the CPU's write data while it
drives, otherwise what memory / the ACIA returned). All pins are 3.3 V,
not 5 V tolerant. Header pin numbers per `gpio` index are in
`icepi-zero.lpf`.

## Chip build

    make chip                  # -> build/top_chip.bit
    make prog MODE=chip        # or: make flash MODE=chip

The Z80 as a chip on the header, same layout as above: **CLK** on
gpio 25 (header pin 22) is an *input* and clocks the core directly — one
T-state per CLK period, both edges used as on the real chip (no PLL, no
clock enable). D0–D7 are bidirectional (driven while the Z80 writes),
A0–A15, /MREQ, /RD, /WR are outputs. Not on the header: /IORQ (an I/O
cycle shows as /RD or /WR with /MREQ high), /M1, /RFSH, /HALT, /BUSACK;
/WAIT, /INT, /NMI, /BUSREQ are tied inactive; **/RESET is button 0**
(plus power-on reset), released on a CLK rising edge. The CLK input is
constrained at 8 MHz (`icepi-zero-chip.lpf`). LEDs: 0 heartbeat · 1
blinks while CLK runs · 2 memory cycle · 3 in reset · 4 HALT. 3.3 V I/O
— level-shift a 5 V bus.

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

Synthesis maps the core's async-reset flops directly onto ECP5's
async set/reset FF mode, so the netlist keeps the RTL's (and the ASIC's)
reset semantics. Earlier builds ran `proc; async2sync` first; that
rewrote every flop as sync-reset plus a Q-side `reset ? value : Q` mux,
which hid the hold paths from yosys' clock-enable inference and cost
about 3,000 LUT4 (10,244 -> 7,446 for the whole SoC). `z80_top`'s
negative-edge flops map onto ECP5's inverted-clock FF mode.

## Simulation

    make sim            # Verilator, ~seconds  (needs verilator in PATH)
    make sim_real       # same at true 115200 pacing (exercises the TIE path)
    make sim_iverilog   # same script through iverilog, much slower
    make sim_tiny       # Verilator, 1K Tiny BASIC (SIM_BAUD=115200: real pacing)
    make ci             # all of these, both bitstreams and the Tiny BASIC bitstream

All three boot the *real* ROM through the *real* top-level — PLL stand-in
divider (50 MHz / 12 = 4.17 MHz), POR, BRAM, UART bits on the pins,
interrupt-driven RX — and script a session: answer `Memory top?`, wait
for `Ok`, type `PRINT 123+456`, expect `579`; the Verilator script then
presses button 0 with a bouncy release and expects the warm-start prompt.
The fast variants speed the UART up to 16 Z80 clocks per bit
(`-GBAUD=260417`) so serial framing doesn't dominate sim time.

`make sim_tiny` runs the 1K Tiny BASIC ROM with RAM from `0x0400`. It
types a short session (arithmetic, a `FOR` loop printing multi-digit
numbers, a long string) one character per echo, the way a person types,
and compares the whole console transcript byte for byte with the C
model's (`scripts/basicrunner`, same ROM and input). Tiny BASIC writes
without polling TDRE, so a byte the UART dropped fails the test.

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

`make ci` is what GitHub Actions runs: on every push to this branch
(`.github/workflows/fpga-icepi-zero.yml`) and nightly with this branch
merged into main (`targets.yml`), so a core change on main that
breaks the board shows up within a day.

## Variants

Tiny BASIC instead of NASCOM:

    make sbc ROM_HEX=../../tests/basic/tinybasic_1k.hex

The Makefile write-protects the ROM image rounded up to 1 KiB
(`ROM_PROTECT_TOP`, from `hex2mem.py --top`; override it on the command
line), so Tiny BASIC's variables and stack at `0x0400`–`0x07FF` are RAM.
A different `ROM_HEX` rebuilds the image and the bitstreams. The port map
is already decoded, Tiny BASIC needs no `/INT`, and the `/WAIT` flow
control absorbs its unpolled output. Pasting a long program can still
overrun the 32-byte RX FIFO: basic1K needs ~430 Z80 clocks per input
character, more than a character takes at 115200 baud, so give the
terminal a per-character delay (1 ms is plenty).
