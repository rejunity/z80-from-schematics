# RZ80 on the iCESugar — BASIC SBC and Z80 chip replacement

Two bitstreams from one `top.v` for MuseLab's
[iCESugar](https://github.com/wuxx/icesugar) v1.5 (Lattice
**iCE40UP5K-SG48**; its on-board debugger, the **iCELink** — an
STM32F103 with DAPLink firmware on the USB-C port — feeds the FPGA its
12 MHz clock, programs the SPI flash by drag and drop, and is a USB serial
port to the FPGA):

| build | what it is | Z80 clock | PMODs |
|---|---|---|---|
| **sbc** (`make sbc`) | BASIC single-board computer: rz80 + 8 KiB ROM with **NASCOM BASIC 4.7** + 32 KiB RAM + 68B50-style ACIA on the iCELink's USB serial port (**115200 8N1**) | internal, PLL → **4 MHz** | the Z80 bus as **outputs** (same pinout as the chip build) |
| **chip** (`make chip`) | Z80 chip replacement: the live CPU bus on the PMODs, memory is whatever you wire up | **external**, CLK in on P2_2 | a Z80's own pin directions |

    make venv              # once: python venv + pip-installed YoWASP toolchain
    make                   # both bitstreams -> build/top_sbc.bin, build/top_chip.bin
    make prog MODE=sbc     # copy it onto the iCELink USB drive; MODE=chip for the other

The board needs its jumpers on **J5** (the UART between the iCELink and
the FPGA) and **J6** (the RGB LED). For the sbc build, open the iCELink's
serial port (USB CDC):

    screen /dev/ttyACM0 115200             # Linux
    screen /dev/cu.usbmodem* 115200        # macOS; Ctrl-A K to quit

Press Enter at the `Memory top?` cold-start prompt and you're at
`Z80 BASIC Ver 4.7c` / `Ok`. The board has **no user push button** (S2 is
the FPGA's CRESET), so **DIP switch 1** (S1, position 1) is /RESET: ON holds
the Z80 in reset, OFF lets it run; flip it on and off and RAM survives, so
you get the ROM's warm-start `C|W` prompt. Keep it OFF otherwise. BASIC
wants CR line endings (screen sends CR), Ctrl-C is BREAK, and the
backspace key works as in `make basic`: the ACIA hands BASIC a DEL (7Fh,
what macOS terminals send for ⌫) as BS (08h). Tiny BASIC has no line
editing.

RGB LED: **green** blinks (~1.4 Hz) while the bitstream runs; **red**
shows serial activity (sbc) or a Z80 that is held in reset / halted (chip
— it stays lit until CLK edges arrive to release the reset); blue is off.

## PMOD pinout (both builds)

PMOD1–3 have 24 user I/O, but two of PMOD1's are the iCELink UART (P1_2,
P1_11, through J5) and PMOD4's four are the DIP switch, so 22 are free.
They carry the first 22 signals of the pinout order shared by all our
boards (strobes first, then as much address as fits): **CLK, D0–D7,
/MREQ, /RD, /WR, A0–A9**. Pin names are the board's own numbering
(`P<pmod>_<pin>`, as in wuxx/icesugar `src/common/io.pcf`: pins 1–4 and
9–12 are I/O, the middle ones 3.3 V and GND — check the silkscreen).

| PMOD pin | FPGA pin | signal | chip build | sbc build |
|---|---|---|---|---|
| P1_1, P1_3, P1_4 | 10, 3, 48 | A0–A2 | out | out |
| P1_9, P1_10, P1_12 | 47, 2, 9 | A3–A5 | out | out |
| P1_2, P1_11 | 6, 4 | (console UART TX, RX) | — | — |
| P2_1 | 46 | /MREQ | out | out |
| P2_2 | 44 | **CLK** | **in** (GBIN, pull-up) | out: the internal 4 MHz |
| P2_3 | 42 | /RD | out | out |
| P2_4 | 37 | /WR | out | out |
| P2_9–P2_12 | 36, 38, 43, 45 | A6–A9 | out | out |
| P3_1–P3_4 | 34, 31, 27, 25 | D0–D3 | bidirectional (pull-ups) | out: bus byte |
| P3_9–P3_12 | 23, 26, 28, 32 | D4–D7 | bidirectional (pull-ups) | out: bus byte |
| P4_4 | 18 | DIP switch 1 = /RESET | in | in |

**All PMOD I/O is 3.3 V and not 5 V tolerant**: use bidirectional level
translators towards a 5 V Z80 bus.

- CLK sits on pin 44, a global-buffer input pin (GBIN), so the external
  clock drives the global clock network directly (`SB_GB_IO`), not through
  fabric routing.
- In the sbc build D shows the byte on the internal bus: the CPU's write
  data while it drives, otherwise what memory / the ACIA returned.
- P1_1 and P1_12 (pins 10, 9) are also the FPGA's own micro-USB D+/D−
  (J8, with a 1.5 kΩ pull-up switched by pin 11): leave that connector
  unplugged when the bus is wired up.
- PMOD4 is the 4-way DIP switch S1 (pins 18–21, 4.7 kΩ pull-ups, ON =
  low); only switch 1 is used. Nothing drives PMOD4: a closed switch is a
  short to ground.
- Not brought out (no pins left): A10–A15, /IORQ, /M1, /RFSH, /HALT,
  /BUSACK. /WAIT, /INT, /NMI, /BUSREQ are tied inactive inside the FPGA;
  /RESET is DIP switch 1 (+ power-on reset).

`make pins` prints the direction every pad really got, read back from the
routed netlists. Chip build:

    P2_1             46  z80_mreq_n   011001    out
    P2_2             44  z80_clk      000001    in -> global buffer pull-up
    P3_1             34  z80_d[0]     101001    bidir tristate, OE from fabric (one net for 8 pads) pull-up
    P1_1             10  z80_a[0]     011001    out

## Chip build: what the bus looks like

A Z80 with a **1 KiB address window** and a memory-only control set:

- Memory: decode /MREQ with /RD or /WR. Refresh cycles show /MREQ low with
  neither /RD nor /WR (no /RFSH pin), so plain SRAM/EPROM ignores them.
- I/O: there is no /IORQ, but an I/O cycle is recognisable as /RD or /WR
  low while /MREQ is high (the port number is on A0–A7). INTA cannot
  happen (/INT is tied off).
- Undriven D reads 0xFF through the pad pull-ups (weak; add 10k pull-ups
  for an open bus that matters).
- Timing (nextpnr, worst case, referenced to the FPGA's clock network,
  which lags the CLK pin by a few ns): outputs change ≤ 13.1 ns after the
  CLK edge that launches them (a Z80A's address delay is up to 110 ns).
  Read data sampled on a falling edge (memory read at T3, I/O read) needs
  ~7 ns of setup.
  **The opcode byte of an M1 cycle needs ~57 ns of setup before the rising
  edge that ends T2** (a Z80A needs 35 ns): the core decodes it in the
  same clock. At 8 MHz that leaves ~180 ns from address to data — fine for
  SRAM or a fast EPROM; slow down CLK for slower memory (there is no
  /WAIT pin).
- `make sim_chip` runs `tb_chip.v`: an external 1 KiB RAM and a 4 MHz CLK
  on the PMOD pins, a program that fills a table, reads it back, calls and
  returns through a stack at the top of the window, does an IN (open bus →
  0xFF) and an OUT (seen as /WR with /MREQ high) — on the RTL and on the
  synthesized netlist with yosys' iCE40 cell models (real `SB_IO` /
  `SB_GB_IO` pad configuration). Both pass, cycle-identical.

## Clocking

The Z80 clock is a real clock, one T-state per period: `rtl/z80_top.v`
keeps all core state on its rising edge and reproduces the chip's
falling-edge pin behaviour (MREQ/RD/WR edges, WAIT / read-data / INT / NMI
sampling) with native negative-edge flops (`docs/clocking.md`); iCE40 has
them (`SB_DFFN*`). No 2× clock, no clock enable.

- **sbc**: the iCE40 PLL's output range is 16–275 MHz, so 4 MHz needs the
  PLL plus a fabric divider: 12 MHz (pin 35, from the iCELink) → `SB_PLL40_2_PAD`
  (DIVR 0, DIVF 63, DIVQ 4: VCO 768 MHz, **48 MHz**) → /12 toggle divider
  (6 PLL periods high, 6 low: an exact **4.000 MHz**, 50 % duty) → `SB_GB`
  → one global net for the core, the ROM/RAM and the SoC. The divider's
  fabric output also drives the CLK pin. `CORE_HZ = 3_500_000` (top.v
  parameter) selects a 42 MHz PLL output (DIVF 55) → 3.500 MHz. The 2_PAD
  PLL's port A passes the 12 MHz reference through on a second global net
  (reset stretch, heartbeat): pin 35 is the PLL's dedicated input pad and
  cannot also be a plain clock input. Port B leaves through the input path
  of pin 37 (P2_4, /WR), so that pin must stay an output.
  Why not 12 MHz / 3 directly: a 50 % duty /3 needs both edges of the
  12 MHz clock (a clock built from logic); both Z80 clock edges matter here.
- **chip**: CLK on P2_2 (pin 44, GBIN) → `SB_GB_IO` → global net → `z80_top`.
  Every rising edge is a T-state; holding CLK stops the CPU, as on a CMOS
  Z80. The 12 MHz clock only runs the reset stretch and heartbeat.

Reset (power-on, DIP switch 1): a 2^16-cycle (~5.5 ms) stretch on the
12 MHz clock swallows switch bounce; assertion is asynchronous, release is
synchronized to the Z80 clock, so the first T1 starts on a clean rising
edge.

## sbc details

Memory map (NASCOM BASIC 4.7 for the RC2014: 8 KiB ROM at 0x0000, RAM
from 0x8000):

| range | what | iCE40 resource |
|---|---|---|
| 0x0000–0x1FFF | ROM (image from `ROM_HEX`, loaded with the bitstream); writable from `ROM_PROTECT_TOP` up — the image rounded up to 1 KiB, so none for NASCOM BASIC | 16 × `SB_RAM40_4K` (EBR) |
| 0x2000–0x7FFF | unmapped: reads 0xFF, writes ignored | — |
| 0x8000–0xFFFF | RAM, 32 KiB | 1 × `SB_SPRAM256KA` (16K × 16, byte lanes via MASKWREN) |

SPRAM cannot be preloaded and powers up with undefined contents; BASIC's
cold start initialises what it uses (the `Memory top?` test finds RAM up
to 0xFFFF: **31948 Bytes free**, as on a 32K RC2014). Both memories read
synchronously — the address is on the bus from the rising edge of T1, the
memory registers it at T2, and the core takes the byte at the rising edge
after T2 (M1) or the falling edge of T3 (read) — so no wait states.

I/O map and interrupts are the same SoC as the IcePi Zero and Tang Primer
25K ports (branches `fpga-icepi-zero-basic`, `fpga-tang-primer-25k-basic`;
`uart.v` and `hex2mem.py` are copies): `IN (0x80)` status (bit 0 RDRF,
bit 1 TDRE), `OUT (0x80)` control (RIE bit 7, TIE = bits 6:5 = 01),
`IN/OUT (0x81)` data, 32-byte RX FIFO; `/INT` = (RIE & RX byte pending) |
(TIE & TX idle), IM 1 (INTA reads 0xFF = RST 38h). TIE matters: the
ROM's ACIA driver has an interrupt-driven TX ring buffer. A TX write that
finds the transmitter still busy holds the CPU with `/WAIT` until the
previous byte is out, so a program that writes without polling TDRE
(Tiny BASIC) loses nothing.

## Resources and timing

nextpnr-ice40 0.11, yosys 0.70 (`synth_ice40 -abc9 -dsp`), default seed:

| | sbc | chip |
|---|---|---|
| logic cells (`ICESTORM_LC`) | 4427 / 5280 (83 %) | 3500 / 5280 (66 %) |
| `SB_LUT4` after synthesis | 3960 | 3349 |
| EBR (`SB_RAM40_4K`) | 16 / 30 | 0 |
| SPRAM | 1 / 4 | 0 |
| PLL | 1 / 1 | 0 |
| I/O | 29 / 39 | 29 / 39 |
| global buffers | 6 / 8 | 4 / 8 |
| Z80 clock Fmax | **7.39 MHz** (constraint 4 MHz) | **8.65 MHz** (constraint 8 MHz) |
| 12 MHz domain Fmax | 75 MHz | 73 MHz |
| 48 MHz divider clock | 135 MHz | — |

Both Z80 clock limits are half-cycle paths into a rising-edge flop: in
the chip build the falling-edge read-data sampler → ALU/carry chain
(58 ns); in the sbc build the falling-edge /RD output → the SoC's
data-in mux → the core (68 ns). `make timing` prints the summary; `make
chip Z80_MHZ_chip=10` re-checks the chip build against another CLK
frequency (the Makefile appends `set_frequency clk_z80 …` to
`icesugar.pcf` per build). nextpnr does not analyse hold times or
external I/O timing.

## Toolchain — venv, no system packages

`make venv` pip-installs the [YoWASP](https://yowasp.org/) WebAssembly
builds of the open iCE40 flow into `.venv/`: `yowasp-yosys` and
`yowasp-nextpnr-ice40`, which also ships `yowasp-icepack` (bitstream) and
`yowasp-icepll` (the PLL settings above). Sources are staged into
`build/` (the core with a wildcard: every `rtl/*.v` / `*.vh`) and the tools
run there, because the YoWASP sandbox only sees the current directory tree.

Synthesis is `synth_ice40` straight on the RTL — **no `proc; async2sync`**
first: that rewrites every async-reset flop as sync reset plus a Q-side
mux, which hides the hold paths from clock-enable inference (measured on
this core: sbc 3935 → 4338 `SB_LUT4`, chip 3395 → 3833). The core's
async-reset flops map onto `SB_DFFR`/`SB_DFFS` and friends, `z80_top`'s
falling-edge flops onto `SB_DFFN*`.

Programming needs no tool at all: the iCELink shows up as a USB drive
(`iCELink`), and a `.bin` copied onto it is written to the SPI flash; the
FPGA boots from it a few seconds later and on every power-up. `make prog`
copies it there when it finds the drive under `/Volumes`, `/media/*` or
`/run/media/*` (`ICELINK=<mount point>` otherwise). MuseLab's command-line
tool `icesprog` (wuxx/icesugar `tools/`, needs libusb and hidapi) does the
same over USB:

    make prog MODE=chip ICELINK=/media/$USER/iCELink
    make prog MODE=chip ICESPROG=icesprog

## Simulation

    make sim            # Verilator, sbc RTL, fast serial (~10 s)
    make sim_real       # same at true 115200 pacing (exercises the TIE path)
    make sim_iverilog   # same script through iverilog (~11 min)
    make sim_gl         # the script on the *synthesized* sbc netlist (~1.5 min)
    make sim_pnr        # the script on the *routed* sbc netlist, as in the bitstream (~1.5 min)
    make sim_chip       # chip build with an external RAM, RTL + netlist
    make sim_tiny       # 1K Tiny BASIC (SIM_BAUD=115200: real pacing)
    make sim_pnr_tiny   # Tiny BASIC on its own bitstream's routed netlist (build_tiny/)
    make ci             # all of these, both bitstreams and the Tiny BASIC bitstream

The PLL is a black box in simulation: the testbench clock stands in for its
48 MHz output, so the real /12 divider makes the 4 MHz Z80 clock and
`CORE_HZ` needs no override (the SPRAM gets a behavioural model with the
same latency and byte lanes). The script boots the real ROM through the
real top level over the UART pins: answer `Memory top?`, `PRINT 123+456` →
`579`, flip DIP switch 1 on and off with a bouncy release, expect the warm-start
`C|W` prompt, answer `W`, expect `Ok`. `make sim_gl` runs the same script
on yosys' iCE40 cell models (EBR ROM image, SPRAM byte lanes, negedge
flops) and finishes on exactly the same clock as the RTL at 115200.
`make sim_pnr` goes one step further, to nextpnr's routed netlist
(`build/top_sbc.pnr.json`): the design as icepack packs it into the
bitstream, every logic cell's LUT and flop as placed, the EBR ROM image,
SPRAM, global buffers and pads, on the same cell models. `sim_pnr.py` ties
the inputs nextpnr left unrouted to the level the hardware reads there
and drives the PLL's dedicated clock paths from the testbench clock, and
both BASICs again finish on exactly the RTL's clock. It is a zero-delay
check of function: timing is nextpnr's job, and icepack's bit encoding
of the `.asc` is not covered.
`make sim_tiny` runs the 1K Tiny BASIC ROM with RAM from 0x0400: it types
a short session one character per echo and compares the whole console
transcript byte for byte with the C model's (`scripts/basicrunner`), so a
TX byte the UART dropped fails it.

    Memory top?
    Z80 BASIC Ver 4.7c
    Copyright (C) 1978 by Microsoft
    31948 Bytes free
    Ok
    PRINT 123+456
     579
    [sim_top] 123+457<DEL>6 = 579 OK -- flipping DIP switch 1 (reset)
    ...
    Cold | Warm start (C|W) ? W
    Ok
    [sim_top] PASS: boot, arithmetic, DIP-switch warm-restart all good

`make ci` is what GitHub Actions runs: on every push to this branch
(`.github/workflows/fpga-icesugar.yml`) and nightly with this branch
merged into main (`targets.yml`), so a core change on main that
breaks the board shows up within a day.

## Limitations

- Not yet run on hardware: everything above is simulation, nextpnr timing
  and the routed-netlist pad check.
- chip: 1 KiB address window (A0–A9), no /IORQ, /M1, /RFSH, /HALT,
  /BUSACK, /WAIT, /INT, /NMI, /BUSREQ on the PMODs; external CLK ≤ 8 MHz;
  3.3 V only.
- No user push button: /RESET is DIP switch 1.
- sbc: Z80 clock fixed at 4 MHz (or 3.5 MHz via `CORE_HZ`); BASIC is
  the RC2014 8K ROM layout, so only 32 KiB of the 64 KiB space is RAM.

## Variants

1K Tiny BASIC keeps its variables and stack at 0x0400–0x07FF, inside the
ROM window, so that part of the EBR has to be writable. The Makefile
does that by itself: `ROM_PROTECT_TOP` defaults to the ROM image rounded
up to 1 KiB (`hex2mem.py --top`, 1024 here; override it on the command
line), and a different `ROM_HEX` rebuilds the image and the bitstreams:

    make sbc ROM_HEX=../../tests/basic/tinybasic_1k.hex

Tiny BASIC's port protocol (status 0x00, data 0x01) has no
transmitter-ready bit, so it writes faster than 115200 baud drains; the
`/WAIT` flow control above holds each such write until the line is free.
Pasting a long program can still overrun the 32-byte RX FIFO (basic1K
needs ~430 Z80 clocks per input character, more than one takes at
115200 baud): give the terminal a per-character delay (1 ms is plenty).
