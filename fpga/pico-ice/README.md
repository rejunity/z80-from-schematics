# RZ80 on the pico-ice — BASIC SBC and Z80 chip replacement

Two bitstreams from one `top.v` for the
[pico-ice](https://pico-ice.tinyvision.ai/) by tinyVision.ai (Lattice
**iCE40UP5K-SG48** + Raspberry Pi **RP2040**, board Rev3):

| build | what it is | clock | headers |
|---|---|---|---|
| **sbc** (`make sbc`) | BASIC single-board computer: rz80 + 8 KiB ROM with **NASCOM BASIC 4.7** + 32 KiB RAM + 68B50-style ACIA on the RP2040's USB-UART bridge (**115200 8N1**) | internal: RP2040's 12 MHz → PLL 24 MHz → ÷6 → **4 MHz** | the Z80 bus as **outputs**, same pins as the chip build; SW2 = /RESET |
| **chip** (`make chip`) | Z80 chip replacement: the live CPU bus on the headers, memory and peripherals are whatever you wire up | **external**, CLK in on Pmod 1 pin 10 (ICE44) | CLK, D0–D7, /MREQ, /RD, /WR, A0–A12 (25 pins, see [Pinout](#pinout)) |

    make venv              # once: python venv + pip-installed YoWASP toolchain
    make                   # both bitstreams -> build/top_sbc.bin, build/top_chip.bin
    make prog MODE=sbc     # into the FPGA's CRAM over USB DFU (volatile)
    make flash MODE=sbc    # into the FPGA's config flash over USB DFU (persistent)
    make uf2 MODE=sbc      # build/top_sbc.uf2 for the board's USB drive (no tools)

All three load paths go through the pico-ice's stock RP2040 firmware
(**pico-ice-default**, current release v1.6.1), which the board ships with:

* `make prog` = `dfu-util -d 1209:b1c0 --alt 1 --download build/top_sbc.bin`
  (alt 1 = iCE40 CRAM; lost on power-cycle),
* `make flash` = `dfu-util -d 1209:b1c0 --alt 0 --download build/top_sbc.bin --reset`
  (alt 0 = the iCE40's SPI flash; the RP2040 reboots and the FPGA
  configures from flash from then on),
* `make uf2` converts the bitstream to UF2 (`bin2uf2.py`, byte-for-byte what
  tinyVision's [`bin2uf2`](https://github.com/tinyvision-ai-inc/uf2-utils)
  writes); copy it onto the `pico-ice` USB drive
  (`make uf2 MODE=sbc UF2_DRIVE=/media/$USER/pico-ice` does the copy) and the
  firmware writes the flash and restarts the FPGA.

`dfu-util` is the one native tool (`apt install dfu-util`, `brew install
dfu-util`); Linux may need tinyVision's udev rule for `1209:b1c0`
([doc](https://pico-ice.tinyvision.ai/md_programming__the__fpga.html)).

For the sbc build, open the BASIC console. The stock firmware enumerates
**three** USB serial ports: #0 is its own help shell, **#1 is the UART bridge
to the FPGA**, #2 an SPI bridge. On Linux that is normally the middle one:

    screen /dev/ttyACM1 115200      # Ctrl-A K to quit; macOS: the 2nd /dev/cu.usbmodem*

Set the terminal to **115200**: the bridge programs the RP2040's UART with
whatever baud rate the host opens the port at (and opening any of the
ports at 1200 baud reboots the RP2040 into its USB bootloader — the
Arduino convention). Press Enter at the `Memory top?` cold-start prompt
and you're at `Z80 BASIC Ver 4.7c` / `Ok`. **SW2**, the button wired to
the FPGA (ICE10), resets the Z80 — RAM survives, so you get the ROM's
warm-start `C|W` prompt. SW1 resets the RP2040, which also reconfigures
the FPGA from its flash.

Terminal notes: BASIC wants CR line endings (screen sends CR — good),
Ctrl-C is BREAK, and the backspace key works as in `make basic`: the ACIA hands BASIC a DEL (7Fh, what macOS terminals send for ⌫) as BS (08h). Tiny BASIC has no line editing. The stock bridge has no flow control and drops host bytes
when the RP2040's 32-byte UART FIFO is full, so paste long programs with a
per-character delay.

## The board, as this port uses it

Checked against tinyVision's documentation, the pico-ice-sdk and the Rev3
schematic netlist (sources at the end).

* **FPGA**: iCE40UP5K-SG48 — 5,280 logic cells (LUT4 + FF), 30 EBR
  (`SB_RAM40_4K`, 4 kbit, preloadable from the bitstream), 4 SPRAM
  (`SB_SPRAM256KA`, 16K × 16, *not* preloadable), one PLL (`SB_PLL40`,
  16–275 MHz out), the RGB LED driver.
* **Clock**: there is no oscillator on the FPGA side. The RP2040 (12 MHz
  crystal) exports a clock on **GPIO24** (clock output GPOUT2) through
  27 Ω to **ICE35** (`IOT_46b_G0`, also the PLL's input pad). The stock
  firmware sets it to **12 MHz** = 48 MHz USB PLL ÷ 4 (`ice_fpga_init(12)`;
  firmware v1.1 changed it from 48 MHz, so anything from v1.1 on).
* **UART**: the stock firmware bridges USB-CDC #1 to the RP2040's UART0:
  GPIO0 (TX) → **ICE27**, GPIO1 (RX) ← **ICE25**, at the baud rate the host
  sets (115200 default).
* **Buttons**: **SW2** → **ICE10**, active low (10 kΩ pull-up, 100 nF to
  GND), and the same net is a header hole. **SW1** resets the RP2040 (solder
  jumper J4 can reassign it to RP2040 GPIO28).
* **RGB LED**: ICE39/40/41 (the UP5K's RGB0/1/2 current sinks) =
  green/blue/red through 1 kΩ, active low. The same nets go to RP2040
  GPIO12/15/13, which the stock firmware leaves high-impedance.
* **Programming**: stock firmware: USB drive `pico-ice` (UF2, family
  0x792e7263, 256-byte payloads from flash address 0) and DFU `1209:b1c0`
  with alt 0 = flash, alt 1 = CRAM. Both flash writers hold the FPGA in
  reset (CRESET low) while they write.
* **Header I/O**: tinyVision count "32 FPGA GPIO on 0.1" headers". For a
  Z80 bus, **25** of them are free:

  | where | iCE40 pins | notes |
  |---|---|---|
  | Pmod 1 | ICE2, 3, 4, 44, 45, 46, 47, 48 | iCE40 only; I/O bank 2, whose supply VIO2 is on the Pmod's power pins (3.3 V via solder jumper SJ5) |
  | Pmod 2 | ICE28, 31, 32, 34, 36, 38, 42, 43 | iCE40 only |
  | Pmod 3 | ICE18, 19, 20, 21, 23, 26 (+ 25, 27) | wired to RP2040 GPIO7…0, which the stock firmware leaves as inputs with their default pull-downs; ICE25/27 are the UART |
  | left long header | ICE6, ICE9, ICE11 | iCE40 only |

  The other 7: **ICE35** (the clock), **ICE25/ICE27** (the UART the sbc
  build needs), **ICE10** (SW2), **ICE12/ICE13** (QSPI IO2/IO3 = /WP and
  /HOLD of the FPGA's config flash, shared with the 8 MB PSRAM — the
  RP2040 reads that flash while the FPGA runs, e.g. for the drive's
  `CURRENT.UF2`) and **ICE37** (the PSRAM's chip select, through a
  2N7002: driving it selects the PSRAM, which would then fight the flash
  on the shared SPI data line). Also on the headers, but not usable as bus
  pins: ICE14–17 (that SPI bus, driven by the RP2040), RGB0–2, CDONE,
  CRESET.

## Pinout

The shared rule for all our boards: signals in priority order — CLK,
D0–D7, /MREQ, /RD, /WR (12); A0, A1, … A15 (28); /IORQ, /M1, /RESET, /INT
(32); /WAIT, /NMI, /BUSREQ, /BUSACK, /HALT, /RFSH (38) — and a board takes
the longest prefix that fits its user pins. **25 free pins → the prefix up
to A12.** Same pins in both builds (`pico-ice.pcf`); Pmod pin numbers are
Digilent's (1–4 one row, 7–10 the other, 5/11 GND, 6/12 power).

| signal | iCE40 | header | chip build | sbc build |
|---|---|---|---|---|
| A0, A1, A2, A3 | ICE4, ICE2, ICE47, ICE45 | Pmod 1 pins 1–4 | out | out |
| /MREQ | ICE3 | Pmod 1 pin 7 | out | out |
| /RD | ICE48 | Pmod 1 pin 8 | out | out |
| /WR | ICE46 | Pmod 1 pin 9 | out | out |
| **CLK** | ICE44 (global buffer pad G6) | Pmod 1 pin 10 | **in** (weak pull-up) | out (4 MHz) |
| D0, D1, D2, D3 | ICE43, ICE38, ICE34, ICE31 | Pmod 2 pins 1–4 | bidirectional | out |
| D4, D5, D6, D7 | ICE42, ICE36, ICE32, ICE28 | Pmod 2 pins 7–10 | bidirectional | out |
| A4, A5 | ICE21, ICE19 | Pmod 3 pins 3–4 (RP2040 GPIO2, 3) | out | out |
| A6, A7, A8, A9 | ICE26, ICE23, ICE20, ICE18 | Pmod 3 pins 7–10 (GPIO4…7) | out | out |
| A10, A11, A12 | ICE6, ICE9, ICE11 | left long header | out | out |
| /RESET | ICE10 (SW2) | left long header | in | in |
| UART RX / TX | ICE27 / ICE25 | Pmod 3 pins 1 / 2 (RP2040 GPIO0 / 1) | unused (TX idles high) | console |

* **chip**: D is driven only while the CPU drives it (`SB_IO` with
  `OUTPUT_ENABLE` = z80_top's `data_drive`) and has weak pull-ups, so an
  undriven bus reads 0xFF. CLK goes from the pad straight onto a global
  clock net (`SB_GB_IO`). /RESET is SW2's net: pull ICE10 low (open drain)
  to reset from outside. Not brought out: A13–A15, /IORQ, /M1, /RFSH,
  /HALT, /BUSACK; /WAIT, /INT, /NMI, /BUSREQ are tied inactive inside.
  Without /IORQ an external I/O decoder can still tell the cycles apart:
  /RD or /WR low while /MREQ is high is an I/O cycle (interrupt
  acknowledge never happens, /INT is not brought out).
* **sbc**: every bus pin is an output; D shows the byte on the internal
  bus (the CPU's write data while it drives, otherwise what memory / the
  ACIA returned).
* The strobes, CLK and D are on the two iCE40-only Pmods on purpose:
  Pmod 3's pins are also RP2040 inputs with pull-downs, so while the FPGA
  is unconfigured (power-up, every CRAM load) they are pulled low — fine
  for address bits, not for /MREQ, /RD, /WR. Add pull-ups on the Z80 side
  if your system needs defined strobes while the FPGA (re)configures.
* All I/O is **3.3 V and not 5 V tolerant**: use level translators towards
  a 5 V Z80 system (for D, a '245-type transceiver can take its direction
  from /RD).

## Clocking

The Z80 clock is a real clock, one T-state per period: `rtl/z80_top.v`
keeps all core state on its rising edge and reproduces the chip's
falling-edge pin behaviour (MREQ/RD/WR edges, WAIT / read-data / INT /
NMI sampling) with native negative-edge flops — iCE40 logic tiles have a
per-tile clock inversion (`SB_DFFN*`), see `docs/clocking.md`. No 2×
clock, no clock enable, nothing resamples the clock.

* **sbc**: ICE35's 12 MHz → `SB_PLL40_2_PAD`. Port A passes the 12 MHz
  reference onto a global net (the *board clock*: reset stretch, LED
  heartbeat); port B makes **24 MHz** (DIVR 0, DIVF 63, DIVQ 5: VCO
  768 MHz ÷ 32). A toggle divider ÷6 turns that into **4.000 MHz at 50 %
  duty**, which `SB_GB` puts on a global net; z80_top, both memories, the
  UART and the ACIA all run in that one domain. Why not 12 MHz ÷ 3
  directly: the iCE40 PLL cannot output below 16 MHz, and an odd divider
  has no 50 % duty without using both input edges — and with a core that
  works on both edges, both half-periods matter. `CORE_HZ = 3_500_000`
  sets DIVF 55 (VCO 672 MHz → 21 MHz) for exactly 3.5 MHz. At 4 MHz the
  UART divisor is 35: 114,286 baud, −0.8 %. The CLK header pin outputs
  the same 4 MHz.
* **chip**: CLK on ICE44 (`SB_GB_IO`, global-buffer input G6) drives
  `z80_top` directly. Every rising edge is a T-state; opcodes are taken at
  the rising edge of T3 and read data at its falling edge, the Z80's own
  sampling points. Holding CLK stops the CPU, as on a CMOS Z80; with
  nothing attached the pad's pull-up holds CLK high. The 12 MHz board
  clock still runs the reset stretch.

RESET (power-on, SW2) is debounced and stretched (~5.5 ms) on the 12 MHz
board clock, asserted asynchronously and released synchronously to the
Z80 clock, so the first T1 always starts on a clean rising edge.

## sbc details

Same SoC as the IcePi Zero port (`uart.v` and the ACIA shim are copied from
there, `hex2mem.py` extended with an image size), with the memory split the
UP5K wants:

| range | what | implementation |
|---|---|---|
| 0x0000–0x1FFF | ROM, NASCOM BASIC 4.7 (RC2014 build, 0x0000–0x1FD9); writable from `ROM_PROTECT_TOP` up — the image rounded up to 1 KiB, so none for NASCOM BASIC | 16 EBR, preloaded at configuration from `ROM_HEX` (`hex2mem.py … 8192`) |
| 0x2000–0x7FFF | unmapped | reads 0xFF, writes ignored |
| 0x8000–0xFFFF | RAM, 32 KiB | one `SB_SPRAM256KA` (16K × 16; A0 picks the byte via `MASKWREN`) |

Both memories read synchronously with one T-state of latency — address on
the pins from the rising edge of T1, registered at T2, consumed by the core
at the rising edge of T3 (opcode fetch) or its falling edge (memory read) —
so no wait states. /WR is low across exactly one rising edge (T3), so each
write fires once. The SPRAM powers up with arbitrary contents; BASIC sizes
and clears what it uses (`Memory top?` → 31948 bytes free). A Z80 reset
does not touch RAM, hence the warm start.

I/O: the 68B50-style ACIA at **0x80** (status: bit 0 RDRF, bit 1 TDRE /
control) and **0x81** (data), a 32-byte RX FIFO, `/INT` = (RIE & RDRF) |
(TIE & TDRE) in IM 1 (INTA reads 0xFF = RST 38h). TIE matters: the ROM's
z88dk ACIA driver has an interrupt-driven TX ring buffer. Tiny BASIC's
ports 0x00/0x01 are decoded too (see Variants). A TX write that finds the
transmitter still busy holds the CPU with `/WAIT` until the previous byte
is out, so a program that writes without polling TDRE (Tiny BASIC) loses
nothing.

RGB LED: green = heartbeat (board clock alive), blue = serial activity
(chip build: /MREQ activity), red = Z80 in reset or halted.

## Utilisation and timing

nextpnr-ice40, UP5K-SG48 (`make`, `make timing`):

| | sbc | chip |
|---|---|---|
| logic cells | 4,664 / 5,280 (88 %) | 3,742 / 5,280 (70 %) |
| yosys `SB_LUT4` / flip-flops / `SB_CARRY` | 4,175 / 837 / 244 | 3,562 / 466 / 219 |
| EBR (`SB_RAM40_4K`) | 16 / 30 | 0 |
| SPRAM | 1 / 4 | 0 |
| I/O | 29 / 39 (+ RGB driver) | 29 / 39 (+ RGB driver) |
| global buffers | 7 / 8 | 4 / 8 |
| PLL | 1 / 1 | 0 |
| **Z80 clock Fmax** | **8.67 MHz** (constraint 4 MHz) | **12.10 MHz** (constraint 8 MHz) |
| board clock Fmax | 72.3 MHz (12 MHz); PLL 24 MHz domain 127.9 MHz | 79.7 MHz (12 MHz) |

The core alone is ~3.6k LUT4 (the chip build is essentially that); the SoC
adds ~600 LUT4 and ~370 flip-flops, 256 of those the 32-byte RX FIFO (the
iCE40 has no distributed RAM, and the FIFO head is read asynchronously).

Both Z80-clock limits are **half-cycle paths** — z80_top works on both
edges, so nextpnr's Fmax is 1 / (2 × the worst falling-to-rising path):

* chip (41.3 ns): read data sampled at the falling edge (`data_in_neg`) →
  the address unit's 16-bit adders (nn + 1, PC + e, IX + d) → the address
  latch at the next rising edge. The ALU is no longer on such a path:
  like the die, the core writes an ALU result at T2 of the next M1 (row 19
  of docs/known-differences.md), not at the edge after the read;
* sbc (57.7 ns): the pin former's falling-edge flop → /RD → the SoC's
  data-in mux (I/O vs memory) → the same adders → the address latch.

Header timing of the chip build (nextpnr estimates, `make timing`, no I/O
constraints set): D → rising edge ≈ 41 ns (the opcode fetch at T3 of M1
goes straight into decode), D → falling edge ≈ 8 ns (memory / I/O read
data), clock → pins ≈ 14 ns. At 4 MHz that leaves ~445 ns from address
to opcode and ~600 ns from address to read data; at 8 MHz ~195 / ~290 ns.

## Toolchain — venv, no system packages

`make venv` creates `.venv/` and pip-installs the
[YoWASP](https://yowasp.org/) WebAssembly builds of the open iCE40 flow:
`yowasp-yosys` (`synth_ice40 -abc9 -dsp`) and `yowasp-nextpnr-ice40`, which
also ships the IceStorm tools (`yowasp-icepack`, `yowasp-icepll`). Sources
are staged into `build/` (the core with wildcards: `rtl/*.v`, `rtl/*.vh`)
and the tools run from there because the YoWASP sandbox only sees the
current directory tree. The two builds differ only in the
`-chparam EXTERNAL_BUS` passed to yosys and the Z80 clock constraint the
Makefile appends to `pico-ice.pcf` (4 MHz sbc, 8 MHz chip). The figures
above are from yowasp-yosys 0.70 and yowasp-nextpnr-ice40 0.11.1.

Synthesis runs `synth_ice40` directly on the RTL — no `proc; async2sync`
first, which would turn every async-reset flop into sync-reset plus a mux,
defeat clock-enable inference and cost thousands of LUTs (the core would
not fit the UP5K).

## Simulation

    make sim            # Verilator, sbc build, fast serial (~10 s)
    make sim_real       # same at the hardware's serial pacing (TIE path)
    make sim_iverilog   # the boot script through iverilog (~6 min)
    make sim_chip       # iverilog, chip build: external bus on the pins (~2 s)
    make sim_tiny       # Verilator, 1K Tiny BASIC (SIM_BAUD=57143: hardware pacing)
    make sim_pnr        # the boot script on the *routed* sbc netlist, as in the bitstream (~1.5 min)
    make sim_pnr_tiny   # Tiny BASIC on its own bitstream's routed netlist (build_tiny/)
    make ci             # all of these, both bitstreams (+ UF2) and the Tiny BASIC bitstream

The sbc sims boot the *real* ROM through the *real* top-level — clock
divider, POR, EBR ROM, behavioural SPRAM, UART bits on the pins,
interrupt-driven RX and TX — and script a session: answer `Memory top?`,
wait for `Ok`, type `PRINT 123+456`, expect `579`; the Verilator script
then presses SW2 with a bouncy release and expects the warm-start prompt.
The PLL is a black box in simulation: its output is replaced by the 12 MHz
input (×1), so the same ÷6 divider gives a 2 MHz Z80 clock and the sim
builds set `CORE_HZ` to match. `make sim` runs the UART at 16 Z80 clocks
per bit; `make sim_real` at 35, the hardware's ratio (115200 baud at
4 MHz).

Verilator result on this branch:

    RC2014 - MS Basic Loader
    z88dk - feilipu

    Memory top?
    Z80 BASIC Ver 4.7c
    Copyright (C) 1978 by Microsoft
    31948 Bytes free
    Ok
    PRINT 123+456
     579
    [sim_top] 123+456=579 OK -- pressing SW2 (reset)
    ...
    Cold | Warm start (C|W) ? W
    Ok
    [sim_top] PASS: boot, arithmetic, SW2 warm-restart all good

`make sim_tiny` runs the 1K Tiny BASIC ROM with RAM from 0x0400: it types
a short session one character per echo and compares the whole console
transcript byte for byte with the C model's (`scripts/basicrunner`), so a
TX byte the UART dropped fails it.

`make sim_pnr` runs the boot script on nextpnr's routed netlist
(`build/top_sbc.pnr.json`): the design as icepack packs it into the
bitstream, every logic cell's LUT and flop as placed, the EBR ROM image,
SPRAM, global buffers and pads, on yosys' iCE40 cell models. `sim_pnr.py`
ties the inputs nextpnr left unrouted to the level the hardware reads
there and drives the PLL's dedicated clock paths from the testbench clock
(the same ×1 stand-in, so 2 MHz); the netlist's UART divisor is the
hardware's, 35 Z80 clocks per bit, so this is `make sim_real`'s pacing,
and both BASICs finish on exactly the RTL's clock at that pacing. It is a
zero-delay check of function: timing is nextpnr's job, and icepack's bit
encoding of the `.asc` is not covered.

`make sim_chip` (`tb_chip.v`) is the chip build seen from the outside: an
external 4 MHz CLK, an 8 KiB SRAM on A0–A12 / /MREQ / /RD / /WR and an
output port decoded from /WR with /MREQ high run a small program (LDIR a
string into RAM, CALL a print routine, `OUT (81h),A`) and must print
`Z80 chip OK` without bus contention.

`make ci` is what GitHub Actions runs: on every push to this branch
(`.github/workflows/fpga-pico-ice.yml`) and nightly with this branch
merged into main (`targets.yml`), so a core change on main that
breaks the board shows up within a day.

## Variants

1K Tiny BASIC instead of NASCOM:

    make sbc ROM_HEX=../../tests/basic/tinybasic_1k.hex

It keeps its variables and stack at 0x0400–0x07FF, inside the ROM window.
The Makefile makes that part of the block RAM writable by itself:
`ROM_PROTECT_TOP` defaults to the ROM image rounded up to 1 KiB
(`hex2mem.py --top`, 1024 here; override it on the command line), and a
different `ROM_HEX` rebuilds the image and the bitstreams. Tiny BASIC
writes its output without polling TDRE; the `/WAIT` flow control holds
each such write until the line is free. Pasting a long program can still
overrun the 32-byte RX FIFO (basic1K needs ~430 Z80 clocks per input
character, more than one takes at 115200 baud), and the RP2040's
USB-UART bridge drops bytes beyond its own 32-byte FIFO too: give the
terminal a per-character delay (1 ms is plenty).

## Limitations

* **Not run on hardware yet** — no board was at hand. Verified: both
  bitstreams build and meet timing, pin directions in the packed netlists,
  the simulations above.
* **chip build: 25 signals, 8 KiB address window.** No A13–A15 means no
  RC2014-style memory map (BASIC wants RAM at 0x8000), no /IORQ, /M1,
  /RFSH (no DRAM refresh), /WAIT, /INT, /NMI, /BUSREQ, /BUSACK, /HALT. If
  you accept that the RP2040's reads of the FPGA's flash (the drive's
  `CURRENT.UF2`) can come back corrupted while the design runs, ICE12 and
  ICE13 could carry A13/A14; there is no safe pin for A15.
* Needs the stock firmware's 12 MHz on ICE35 (v1.1 or later); a custom
  RP2040 firmware with another clock changes every frequency here.
* The chip build is constrained and timed for an external CLK up to
  8 MHz; the opcode path wants D valid ~41 ns before the rising edge of T3.
* 3.3 V I/O only (see Pinout).

## Sources

* pico-ice documentation: <https://pico-ice.tinyvision.ai/> — pinout,
  Pmods, "Programming the FPGA" (DFU alt settings, UF2 drive), "Getting
  started" (stock firmware: 12 MHz on GPIO24 → ICE35, UART on RP0/RP1 =
  ICE27/ICE25).
* Hardware repo: <https://github.com/tinyvision-ai-inc/pico-ice> —
  `README.md` (32 FPGA GPIO), `Docs/pinout/data.py` (header holes),
  `Board/Rev3/pico-ice.xml` and `.kicad_sch` (netlist: clock via R12 27 Ω,
  SW2 with R21/C14, J4, SRAM_SS → 2N7002, SJ5/VIO2, I/O banks),
  `Firmware/pico-ice-default/` (`main.c`: `ice_fpga_init(12)`, UART0 at
  115200), [releases](https://github.com/tinyvision-ai-inc/pico-ice/releases)
  (default firmware v1.6.1; v1.1 changed the exported clock to 12 MHz).
* SDK: <https://github.com/tinyvision-ai-inc/pico-ice-sdk> —
  `rtl/pico_ice.pcf` (pin catalogue), `include/boards/pico_ice.h`,
  `src/ice_fpga_data.c` (clock GPIO24, CRESET GPIO27, CDONE GPIO26), and at
  the commit firmware v1.6.1 pins (8d0fd06): `src/ice_fpga.c`
  (`CLK_USB / (48 / freq_mhz)`), `src/ice_usb.c` (DFU alt 0 = flash,
  alt 1 = CRAM; UART baud from the host's line coding),
  `src/tinyuf2_board.c` (flash writes stop the FPGA), `src/ice_led.c`.
* UF2 tools: <https://github.com/tinyvision-ai-inc/uf2-utils> (`bin2uf2`,
  family ID 0x792e7263).
