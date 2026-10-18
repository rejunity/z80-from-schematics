# RZ80 on the Tang Primer 25K — BASIC SBC and Z80 chip replacement

Two bitstreams from one `top.v` for the
[Sipeed Tang Primer 25K](https://wiki.sipeed.com/hardware/en/tang/tang-primer-25k/primer-25k.html)
(Gowin **GW5A-LV25MG121NES**, Arora-V, 50 MHz crystal):

| build | what it is | clock | J3 header |
|---|---|---|---|
| **sbc** (`make sbc`) | BASIC single-board computer: rz80 + 64 KiB RAM with **NASCOM BASIC 4.7** preloaded + 68B50-style ACIA on the dock's BL616 USB-UART (**115200 8N1**) | internal, PLL → **4 MHz** | the Z80 bus as **outputs**, same pinout as the chip build; only /RESET and /NMI are inputs |
| **chip** (`make chip`) | Z80 chip replacement: the live CPU bus on J3, memory and peripherals are whatever you wire up | **external**, CLK in on J3-22 | a Z80's own pin directions |

> Naming: Sipeed's 25K-LUT Tang board is the *Primer* 25K — the Nano
> line tops out at 20K. openFPGALoader knows it as `tangprimer25k`.

    make venv              # once: python venv + pip toolchain (see below)
    make                   # both bitstreams -> build/top_sbc.fs, build/top_chip.fs
    make prog MODE=sbc     # load to FPGA SRAM (volatile); MODE=chip for the other
    make flash MODE=sbc    # write to SPI flash (persistent)

For the sbc build, connect a terminal to the dock's USB serial port:

    screen /dev/cu.usbserial-* 115200      # macOS; Ctrl-A K to quit

Press Enter at the `Memory top?` cold-start prompt and you're at
`Z80 BASIC Ver 4.7c` / `Ok`. The **H10 push-button** (key[0]) or a low
on header /RESET resets the Z80 — RAM survives, so you get the ROM's
warm-start `C|W` prompt. BASIC wants CR line endings (screen sends CR),
Ctrl-C is BREAK, and the backspace key works as in `make basic`: the ACIA hands BASIC a DEL (7Fh, what macOS terminals send for ⌫) as BS (08h). Tiny BASIC has no line editing.

## Clocking

The Z80 clock is a real clock, one T-state per period: `rtl/z80_top.v`
keeps all core state on its rising edge and reproduces the chip's
falling-edge pin behaviour (MREQ/RD/WR edges, WAIT / read-data / INT /
NMI sampling) with native negative-edge flops (`docs/clocking.md`). No 2×
clock, no clock enable, nothing resamples the clock.

- **sbc**: 50 MHz → PLLA → 8 MHz → hard `CLKDIV` /2 → 4 MHz on the global
  clock network (`CORE_HZ = 4_000_000`; `3_500_000` gives 3.516 MHz, the
  Spectrum rate). The PLLA cannot go below 6.25 MHz by itself, and a
  fabric-FF divider never reaches the BSRAM CLK pins skew-free on GW5A
  (−2.9 ns ⇒ hold violations), hence PLL + CLKDIV. The 4 MHz clock is also
  driven out on J3-22.
- **chip**: CLK on J3-22 (ball J1) drives `z80_top` directly. Every
  rising edge is a T-state; WAIT must be valid around the falling edge of
  T2 and read data around the falling edge of T3 — the Z80's own
  setup/hold contract. Holding CLK stops the CPU, as on a CMOS Z80. Keep
  external CLK at or below 10 MHz.

  *Clock routing.* J1 is a Gowin clock pin (`GCLKT_5`) and the right
  ball for CLK, but the open flow (apicula / nextpnr-himbaechel as of
  2026-10) describes the dedicated pin-to-clock-network path on GW5A-25
  only for the crystal ball E2. Left to itself, nextpnr routes CLK to
  each of the ~350 flops through general fabric (`Failed to route net
  'clk_z80' ... using dedicated routing`), with skew that shows up as
  placement-dependent hold violations. So `tang_primer_25k.cst` has
  `CLOCK_LOC "z80_clk" BUFG;`: J1 makes one short fabric hop to a BUFG
  beside it and then rides the global clock network, like the sbc
  build's clock. No hold violations in four placement seeds, and the few
  ns of insertion delay are the same for every flop. Header balls that
  are not GCLKT pins get such a BUFG automatically, so moving CLK gains
  nothing; Gowin's own tools route J1 on its dedicated path. In the sbc
  build z80_clk is an output and the line changes nothing.

RESET (power-on, key[0], header /RESET) asserts asynchronously and its
release is synchronized to the Z80 clock, so the first T1 starts on a
clean rising edge whatever the phase of the release.
`tests/iverilog/tb_reset_top.v` sweeps 200 release phases against a 1 MHz
external clock and `tb_wait_top.v` checks that header /WAIT is honoured
exactly on falling edges (`make bus_controls` at the repo top).

## J3 header pinout (both builds)

The dock's 2×20 J3. J3-11 is fixed +5 V and J3-12 fixed GND; FPGA ball
B2 (J3-14) is faulty on this board, so D4 is rerouted to J3-35, which was
BUSACK's position — **BUSACK is the one Z80 pin not brought out** (37 of
38). All I/O is 3.3 V and **not 5 V tolerant**; use bidirectional level
translators towards a 5 V Z80 system. Full table with FPGA balls in
`tang_primer_25k.cst`.

| direction | chip build | sbc build |
|---|---|---|
| outputs | A0–A15, /M1, /MREQ, /IORQ, /RD, /WR, /RFSH, /HALT | the same, plus CLK (4 MHz), /WAIT (high), /INT (the ACIA's), /BUSREQ (high) |
| bidirectional | D0–D7 (driven only while the CPU drives, released on BUSACK) | D0–D7 as outputs: the CPU's write data, otherwise what memory / the ACIA returned |
| inputs | CLK, /WAIT, /INT, /NMI, /BUSREQ, /RESET | /RESET, /NMI |

In the chip build, address, data and control drivers are released while
the internal BUSACK is asserted (DMA); /HALT stays driven.

## sbc details

Same SoC as the IcePi Zero port (it shares `uart.v` and `hex2mem.py`
with [`../icepi-zero`](../icepi-zero/README.md); see that README for the
memory map, ACIA shim and 68B50 RIE/TIE interrupt semantics). A TX write
that finds the transmitter still busy holds the CPU with `/WAIT` until
the previous byte is out (Tiny BASIC writes without polling TDRE); the
sbc build shows that `/WAIT` on J3. Board deltas:

| | IcePi Zero | Tang Primer 25K |
|---|---|---|
| FPGA | Lattice ECP5 LFE5U-25F | Gowin GW5A-LV25MG121NES |
| Z80 clock | EHXPLLL → 4 MHz | PLLA → 8 MHz → CLKDIV → 4 MHz |
| UART pins | K15/K16 (FT231X) | C3 (TX) / B3 (RX) to the BL616 |
| Buttons | active **low** (pull-ups) | active **high** (pull-downs), H10 = reset |
| LEDs | 5 user LEDs | none on the dock (no led port) |
| Header | 28-pin bus subset (outputs) | full 37-pin Z80 pinout; chip build too |
| P&R | nextpnr-ecp5 + ecppack | nextpnr-himbaechel + apycula `gowin_pack` |
| Bitstream | `top.bit`, `-b icepi-zero` | `top_{sbc,chip}.fs`, `-b tangprimer25k` |

## Toolchain — venv, no system packages

`make venv` pip-installs the whole open Gowin flow into `.venv/`:
`yowasp-yosys` (`synth_gowin -family gw5a`),
`yowasp-nextpnr-himbaechel-gowin` (WASM place & route), and
[`apycula`](https://github.com/YosysHQ/apicula) (Project Apicula's
`gowin_pack` bitstream writer — GW5A/Arora-V support incl. BSRAM, PLLA
and CLKDIV). Flashing is the one native tool: `openFPGALoader`
(`brew install openfpgaloader`).

Like the ECP5 port, synthesis keeps the core's asynchronous reset
(GW5A DFFC/DFFP flops; no `async2sync`, which cost ~3,900 LUTs by
defeating clock-enable inference) and sources are staged into
`build/` for the YoWASP sandbox. The two builds differ only in the
`-chparam EXTERNAL_BUS` passed to yosys.

## Simulation

    make sim        # Verilator, sbc build, fast serial (~seconds)
    make sim_real   # true 115200 pacing — exercises the ROM's
                    # interrupt-driven TX (TIE) path
    make sim_tiny   # 1K Tiny BASIC (SIM_BAUD=115200: real pacing)
    make ci         # all of these, both bitstreams, the Tiny BASIC
                    # bitstream and the top-level `make bus_controls`

The PLL is a black box in simulation; a /12 toggle divider stands in
(4.17 MHz) and the Makefile overrides `CORE_HZ` to match. Script: answer
`Memory top?`, `PRINT 123+456` → `579`, press key[0] with a bouncy
release, expect the warm-start `C|W` prompt, answer `W`, expect `Ok`.

`make sim_tiny` runs the 1K Tiny BASIC ROM with RAM from `0x0400`: it
types a short session one character per echo and compares the whole
console transcript byte for byte with the C model's
(`scripts/basicrunner`), so a TX byte the UART dropped fails it.

The chip build is exercised by the repo-level `make bus_controls`
(`tests/iverilog/tb_wait_top.v`, `tb_reset_top.v`), which instantiate
this `top.v` with an external 1 MHz clock on the header.

`make ci` is what GitHub Actions runs: on every push to this branch
(`.github/workflows/fpga-tang-primer-25k.yml`) and nightly with this branch
merged into main (`targets.yml`), so a core change on main that
breaks the board shows up within a day.

## Known issue: stock BL616 debugger firmware vs openFPGALoader

On this dock the JTAG probe is a BL616 MCU *emulating* an FT2232, and
Sipeed's stock emulation firmware corrupts long JTAG shifts: IDCODE
reads fine, but every SRAM configuration ends `FAIL` with status
`CRC Error + ID Verify Failed` — even loading Sipeed's own prebuilt
bitstreams ([openFPGALoader #539](https://github.com/trabucayre/openFPGALoader/issues/539)
is the same signature). JTAG frequency, openFPGALoader master, and the
GW5A footer-checksum PR make no difference; the corruption is
deterministic.

Fix (verified on this board): update the dock's debugger firmware per
[Sipeed's procedure](https://wiki.sipeed.com/hardware/en/tang/common-doc/update_debugger.html) —
short the `3V3` & `TDO` test points (underside, "Debugger" pad group by
the corner mounting hole) while plugging USB so the BL616 enumerates as
`/dev/cu.usbmodem*`, then flash Sipeed's
`bl616_fpga_partner_25kDock.bin` (release 2025030317). No DevCube GUI
needed — the pip-installable CLI works from macOS:

    .venv/bin/pip install bflb-iot-tool
    .venv/bin/bflb-iot-tool --chipname bl616 --interface uart \
        --port /dev/cu.usbmodem* --baudrate 2000000 \
        --addr 0x0 --firmware bl616_fpga_partner_25kDock.bin --single

Replug normally afterwards; the debugger's USB serial number becomes
the firmware release id, so the tty paths change (e.g.
`/dev/cu.usbserial-20250303171` for the UART console). With the
updated firmware, `make prog` reports `DONE` and BASIC comes up —
verified with the earlier 2× core at a 10 MHz Z80 rate: banner,
arithmetic, RUN loop at ~770 lines/s, Ctrl-C BREAK. The current
single-clock sbc/chip builds have not yet been run on hardware.

Negative-edge flops: yosys's GW5A mapping implements `z80_top`'s
falling-edge samplers and pin-former half as positive-edge DFFs on an
inverted copy of the clock (a LUT1), so both builds show a second clock
`core_nmi_n_DFFSE_D_CLK` in nextpnr's report. Transfers between the two
are half-period paths (125 ns at 4 MHz), so the ~1 ns skew of that net is
harmless. (Alternative:
the open-source [bl616_dirtyjtag](https://github.com/pepijndevos/bl616_dirtyjtag)
firmware, which openFPGALoader supports natively.)

## Variants

    make sbc ROM_HEX=../../tests/basic/tinybasic_1k.hex   # 1K Tiny BASIC

The Makefile write-protects the ROM image rounded up to 1 KiB
(`ROM_PROTECT_TOP`, from `hex2mem.py --top`; override it on the command
line), so Tiny BASIC's variables and stack at `0x0400`–`0x07FF` are RAM,
and a different `ROM_HEX` rebuilds the image and the bitstreams. Pasting
a long program can overrun the 32-byte RX FIFO (basic1K needs ~430 Z80
clocks per input character, more than one takes at 115200 baud): give
the terminal a per-character delay (1 ms is plenty).
