# RZ80 on the iCEBreaker — BASIC SBC and Z80 chip replacement

Two bitstreams from one `top.v` for the
[iCEBreaker](https://github.com/icebreaker-fpga/icebreaker) (Lattice
**iCE40UP5K-SG48**, 12 MHz oscillator, FT2232H USB: channel A programs the
SPI flash, channel B is a serial port to the FPGA):

| build | what it is | Z80 clock | PMODs |
|---|---|---|---|
| **sbc** (`make sbc`) | BASIC single-board computer: rz80 + 8 KiB ROM with **NASCOM BASIC 4.7** + 32 KiB RAM + 68B50-style ACIA on the board's USB serial port (**115200 8N1**) | internal, PLL → **4 MHz** | the Z80 bus as **outputs** (same pinout as the chip build) |
| **chip** (`make chip`) | Z80 chip replacement: the live CPU bus on the PMODs, memory is whatever you wire up | **external**, CLK in on P1A10 | a Z80's own pin directions |

    make venv              # once: python venv + pip-installed YoWASP toolchain
    make                   # both bitstreams -> build/top_sbc.bin, build/top_chip.bin
    make prog MODE=sbc     # write to the SPI flash (iceprog); MODE=chip for the other

For the sbc build, open the board's *second* USB serial port (FT2232H
interface B; interface A is the programmer):

    screen /dev/ttyUSB1 115200             # Linux
    screen /dev/cu.usbserial-*1 115200     # macOS; Ctrl-A K to quit

Press Enter at the `Memory top?` cold-start prompt and you're at
`Z80 BASIC Ver 4.7c` / `Ok`. The **user button** (next to the USB
connector) resets the Z80; RAM survives, so you get the ROM's warm-start
`C|W` prompt. BASIC wants CR line endings (screen sends CR), Ctrl-C is
BREAK.

LEDs: **green** blinks (~1.4 Hz) while the bitstream runs; **red** shows
serial activity (sbc) or a Z80 that is held in reset / halted (chip — it
stays lit until CLK edges arrive to release the reset).

## PMOD pinout (both builds)

The three PMODs have 24 user I/O, so they carry the first 24 signals of the
pinout order shared by all our boards (strobes first, then as much address
as fits): **CLK, D0–D7, /MREQ, /RD, /WR, A0–A11** — one byte per PMOD.

| PMOD pin | FPGA pin | signal | chip build | sbc build |
|---|---|---|---|---|
| P1A1 | 4 | A8 | out | out |
| P1A2 | 2 | A9 | out | out |
| P1A3 | 47 | A10 | out | out |
| P1A4 | 45 | A11 | out | out |
| P1A7 | 3 | /MREQ | out | out |
| P1A8 | 48 | /RD | out | out |
| P1A9 | 46 | /WR | out | out |
| P1A10 | 44 | **CLK** | **in** (GBIN, pull-up) | out: the internal 4 MHz |
| P1B1–P1B4 | 43, 38, 34, 31 | D0–D3 | bidirectional (pull-ups) | out: bus byte |
| P1B7–P1B10 | 42, 36, 32, 28 | D4–D7 | bidirectional (pull-ups) | out: bus byte |
| P2_1–P2_4 | 27, 25, 21, 19 | A0–A3 | out | out |
| P2_7–P2_10 | 26, 23, 20, 18 | A4–A7 | out | out |

PMOD pins 5/11 are GND and 6/12 are 3.3 V. **All PMOD I/O is 3.3 V and not
5 V tolerant**: use bidirectional level translators towards a 5 V Z80 bus.

- CLK sits on pin 44, one of the two global-buffer input pins (GBIN) on the
  PMODs, so the external clock drives the global clock network directly
  (`SB_GB_IO`), not through fabric routing.
- In the sbc build D shows the byte on the internal bus: the CPU's write
  data while it drives, otherwise what memory / the ACIA returned.
- **P2 is the snap-off button/LED board's PMOD** (LED1–5 on P2_7/P2_1/
  P2_2/P2_8/P2_3, BTN1–3 on P2_9/P2_4/P2_10). Snap it off (or unplug it)
  for the chip build: P2 carries A0–A7 to your bus. Left attached in the
  sbc build, its LEDs glow with address-bus activity; don't press its
  buttons, they would fight the FPGA's address drivers.
- Not brought out (no pins left): A12–A15, /IORQ, /M1, /RFSH, /HALT,
  /BUSACK. /WAIT, /INT, /NMI, /BUSREQ are tied inactive inside the FPGA;
  /RESET is the user button (+ power-on reset).

`make pins` prints the direction every pad really got, read back from the
routed netlists. Chip build:

    P1A7          3  z80_mreq_n   011001    out
    P1A10        44  z80_clk      000001    in -> global buffer pull-up
    P1B1         43  z80_d[0]     101001    bidir tristate, OE from fabric (one net for 8 pads) pull-up
    P2_1         27  z80_a[0]     011001    out

## Chip build: what the bus looks like

A Z80 with a **4 KiB address window** and a memory-only control set:

- Memory: decode /MREQ with /RD or /WR. Refresh cycles show /MREQ low with
  neither /RD nor /WR (no /RFSH pin), so plain SRAM/EPROM ignores them.
- I/O: there is no /IORQ, but an I/O cycle is recognisable as /RD or /WR
  low while /MREQ is high (the port number is on A0–A7). INTA cannot
  happen (/INT is tied off).
- Undriven D reads 0xFF through the pad pull-ups (weak; add 10k pull-ups
  for an open bus that matters).
- Timing (nextpnr, worst case, referenced to the FPGA's clock network,
  which lags the CLK pin by a few ns): outputs change ≤ 13.5 ns after the
  CLK edge that launches them (a Z80A's address delay is up to 110 ns).
  Read data sampled on a falling edge (memory read at T3, I/O read) needs
  ~7.5 ns of setup.
  **The opcode byte of an M1 cycle needs ~58 ns of setup before the rising
  edge that ends T2** (a Z80A needs 35 ns): the core decodes it in the
  same clock. At 8 MHz that leaves ~180 ns from address to data — fine for
  SRAM or a fast EPROM; slow down CLK for slower memory (there is no
  /WAIT pin).
- `make sim_chip` runs `tb_chip.v`: an external 4 KiB RAM and a 4 MHz CLK
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
  PLL plus a fabric divider: 12 MHz (pin 35) → `SB_PLL40_2_PAD`
  (DIVR 0, DIVF 63, DIVQ 4: VCO 768 MHz, **48 MHz**) → /12 toggle divider
  (6 PLL periods high, 6 low: an exact **4.000 MHz**, 50 % duty) → `SB_GB`
  → one global net for the core, the ROM/RAM and the SoC. The divider's
  fabric output also drives the CLK pin. `CORE_HZ = 3_500_000` (top.v
  parameter) selects a 42 MHz PLL output (DIVF 55) → 3.500 MHz. The 2_PAD
  PLL's port A passes the 12 MHz reference through on a second global net
  (reset stretch, heartbeat): pin 35 is the PLL's dedicated input pad and
  cannot also be a plain clock input. Port B leaves through the input path
  of pin 37 (the green LED), so that pin must stay an output.
  Why not 12 MHz / 3 directly: a 50 % duty /3 needs both edges of the
  12 MHz clock (a clock built from logic); both Z80 clock edges matter here.
- **chip**: CLK on P1A10 (pin 44, GBIN) → `SB_GB_IO` → global net → `z80_top`.
  Every rising edge is a T-state; holding CLK stops the CPU, as on a CMOS
  Z80. The 12 MHz oscillator only runs the reset stretch and heartbeat.

Reset (power-on, user button): a 2^16-cycle (~5.5 ms) stretch on the
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
| logic cells (`ICESTORM_LC`) | 4397 / 5280 (83 %) | 3547 / 5280 (67 %) |
| `SB_LUT4` after synthesis | 3935 | 3395 |
| EBR (`SB_RAM40_4K`) | 16 / 30 | 0 |
| SPRAM | 1 / 4 | 0 |
| PLL | 1 / 1 | 0 |
| I/O | 30 / 39 | 30 / 39 |
| global buffers | 6 / 8 | 4 / 8 |
| Z80 clock Fmax | **7.11 MHz** (constraint 4 MHz) | **8.61 MHz** (constraint 8 MHz) |
| 12 MHz domain Fmax | 76 MHz | 74 MHz |
| 48 MHz divider clock | 125 MHz | — |

Both Z80 clock limits are half-cycle paths into a rising-edge flop: in
the chip build the falling-edge read-data sampler → ALU/carry chain
(58 ns); in the sbc build the falling-edge /RD output → the SoC's
data-in mux → the core's opcode decode (70 ns). Other seeds give 8.4–8.6
MHz for the chip build; `-device u` and nextpnr's timing knobs did not
improve it. `make timing` prints the summary; `make chip Z80_MHZ_chip=10`
re-checks the chip build against another CLK frequency (the Makefile
appends `set_frequency clk_z80 …` to `icebreaker.pcf` per build).
nextpnr does not analyse hold times or external I/O timing.

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

Programming is the one native tool, since it needs USB access:
[`iceprog`](https://github.com/YosysHQ/icestorm) (Debian/Ubuntu package
`fpga-icestorm`, or the YosysHQ OSS CAD Suite) writes the SPI flash over
channel A, and the FPGA boots from it at once and on every power-up.
openFPGALoader can be used instead:

    make prog MODE=chip ICEPROG="openFPGALoader -b ice40_generic -f"

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
`579`, press the user button with a bouncy release, expect the warm-start
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
    [sim_top] 123+456=579 OK -- pressing the user button (reset)
    ...
    Cold | Warm start (C|W) ? W
    Ok
    [sim_top] PASS: boot, arithmetic, button warm-restart all good

`make ci` is what GitHub Actions runs: on every push to this branch
(`.github/workflows/fpga-icebreaker.yml`) and nightly with this branch
merged into main (`targets.yml`), so a core change on main that
breaks the board shows up within a day.

## Limitations

- Not yet run on hardware: everything above is simulation, nextpnr timing
  and the routed-netlist pad check.
- chip: 4 KiB address window (A0–A11), no /IORQ, /M1, /RFSH, /HALT,
  /BUSACK, /WAIT, /INT, /NMI, /BUSREQ on the PMODs; external CLK ≤ 8 MHz;
  3.3 V only; the snap-off board must come off P2.
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
