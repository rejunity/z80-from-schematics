# Pin timing: the chip against the NMOS Z80's datasheet

The replica's target is Zilog's NMOS Z80, the Z8400, and above all its
4 MHz grade (Z80A); 6 and 8 MHz (Z80B, Z80H) are a bonus, and the later
CMOS parts are not the reference. This directory checks a laid-out
`z80_chip` against the Z8400's AC characteristics and gives the chip flow
the same contract as constraints.

| | |
|---|---|
| `z8400_nmos.json` | All 53 numbered AC parameters of Zilog PS0178 (PS017801-0602, Z8400 NMOS table, printed pages 38-39): the 4 / 6 / 8 MHz columns, the footnote formulas, which clock edge or pin event each is measured from, provenance. |
| `pin_timing.py` | Measures a LibreLane chip run with OpenSTA and checks every parameter the replica must meet, per process corner; `--write-sdc` writes the same contract as the flow's SDC. |
| `z80_chip_nmos4.sdc` | That SDC for 4 MHz (generated). |

    make -C asic/gf180mcu pin_timing            # the chip's latest run, 4 MHz
    make -C asic/gf180mcu pin_timing GRADE=6    # the 6 MHz grade
    asic/timing/pin_timing.py --write-sdc asic/timing/z80_chip_nmos4.sdc

## What is measured

OpenSTA loads the chip's netlist, its parasitics and the cell, pad and
`z80_top` macro libraries of each corner, with a 100 pF load on every
output (the datasheet's AC test load) and the clock at the CLK pin: 250 ns,
110 ns high and low, 15 ns edges -- what the 4 MHz minimum period leaves
for TwCh = TwCl = 110 ns. From the CLK pin's rising and falling edge it
takes every output pin's delay, each transition, max and min, once through
the pads' data inputs and once through their output enables -- the pad
floating (enable falling) and turning on (rising) apart; and for every
input the setup and hold the replica needs around
the edge that samples it. The CLK pad and the clock tree are part of every
path, as in the datasheet's measurements.

The checks then follow the datasheet's waveforms:

- output delays from the edge each event is assigned to (TdCr(A),
  TdCf(MREQf), ..., the float delays TdCr(Az / Dz / CTz));
- input windows: the setup and hold the replica needs must fit inside the
  window the system guarantees (TsD, TsWAIT, TsINT, ... and their holds;
  read data is held until RD -- IORQ in INTA -- rises, ThD(RDr));
- pulse widths and pin-to-pin separations from the edge times and the min
  / max delays (TdA(MREQf), TwMREQh/l, TwWR, TdD(WRf), TdWRr(D), TdCTr(A),
  TdM1f(IORQf)). The starred minimums take the greater of the printed
  value and the footnote formula at this clock.

STA measures the analog timing of each edge; that the replica's pin
events fall on the edges the datasheet assigns is the cycle-level tests'
job (the perfectz80 pin comparisons). Not checked here: the clock limits
themselves (the system's), TwNMI (z80_top's NMI pulse catcher takes a
pulse of any width), input levels (TTL thresholds against the pads'), and
BUSREQ's grant points (docs/known-differences.md, 16).

## The flow's SDC

`--write-sdc` turns the same table into constraints for `z80_chip`:
outputs at most the datasheet's delay after the CLK pin's edge, inputs
valid the setup time before the sampling edge and held after it. An output
launched on one CLK edge is checked only against the limits from that edge,
an input only against the window around the edge that samples it: every
pin of `z80_top` is a rising-edge flop XOR a falling-edge flop, so its
launch edge is structural, and one SDC covers every bus cycle. Four
exceptions, each in the file: NMI (asynchronous), RESET floating the
address bus (combinational), MREQ IORQ RD WR turning on low (they float
while BUSACK is low and always come back high; STA would hold that
turn-on to the limit of the pin's falling edge), and the read-data hold,
which the datasheet measures from RD's rise rather than the clock --
`pin_timing.py` checks it that way. The resizer repairs the inputs' hold against it: the falling
clock edge reaches the core late through the Schmitt CLK pad, and WAIT
needed ~9 ns of hold before it did.

## Results

GF180 chip, `bi_t` pads at their NMOS-like 4 mA with slow slew, 4 MHz:
PASS at every corner (tt 25 °C 5 V, ss 125 °C 4.5 V, ff -40 °C 5.5 V).
Output delays take 15-45 ns against 65-150 ns limits; the separations
keep 25 ns or more. The tightest margins: TsD(Cr), the opcode's setup, at
the slow corner (+2.8 ns of 35 ns), and the input holds at the fast corner
(+1 to +2 ns) after the flow's repair.

## Sources

`z8400_nmos.json` comes from a separately prepared NMOS timing package
(z80-nmos-timing); its 4 MHz column was checked against the scanned
datasheet tables, whose SHA-256 it records. The package's notes on the
datasheet's inconsistencies hold here too: the printed starred values do
not all agree with their formulas, and parameter 33's 8 MHz entry is
printed positive. Its per-bus-cycle SDCs and waveform tools are not used:
they check each output against every reference edge, which on this
dual-edge pin former pairs launch and capture edges that cannot occur.
