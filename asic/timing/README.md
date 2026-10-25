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

The chip directories (`asic/gf180mcu/`, `asic/gf180mcu-9t/`,
`asic/ihp-sg13g2/`) are on the `asic-<pdk>` branches, which the Targets
workflow merges into main before it builds them; each chip's job runs the
4 MHz check after the layout and fails on any parameter. On main, `make
timing_sdc_check` keeps the committed SDC in step with the JSON.

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

The pads come from transistor-level simulation (`../spice/`) where
`../spice/pads/<pdk>/` has them: their Liberty files in place of the
PDK's, which stop at 20-33 pF and 1-5 ns input edges and leave the rest
to STA's extrapolation, and the datasheet's own measuring levels (PS0178's
AC test conditions) in place of STA's 50 %: outputs where they cross
1.5 V (VOH = VOL), inputs where they cross 2.0 V rising and 0.8 V falling
(VIH, VIL); the CLK pin stays referenced at its 50 %. `--pads none` checks
on the PDK's libraries at 50 %.

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
pulse of any width), input levels (`../spice/`: every input pad switches
inside the TTL window at every corner), and BUSREQ's grant points
(docs/known-differences.md, 16).

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

The three chips at 4 MHz, PASS at every corner (GF180: tt 25 °C 5 V,
ss 125 °C 4.5 V, ff -40 °C 5.5 V; IHP: typ, slow 125 °C, fast -40 °C),
with the pads as simulated and measured at the datasheet's levels -- the
Targets run on main `a5a33c7` (2026-10-24); in brackets an earlier run
with the PDK's pad libraries at 50 %:

| | GF180, 7-track | GF180, 9-track | IHP SG13G2 |
|---|---|---|---|
| Output pads | `bi_t` 4 mA, slow slew, 5 V | the same | 16 mA, 3.3 V |
| Output delays (limits 65-150 ns) | 7-47 ns (13-43) | 7-46 ns (12-43) | 9-25 ns (10-25) |
| Opcode setup TsD(Cr) at the slow corner (limit 35 ns) | 22.8 ns, +12.2 (+8.9) | 23.6 ns, +11.4 (+7.1) | 5.0 ns, +30.0 (+25.2) |
| Tightest input hold, fast corner | +6.4 ns (+0.6) | +6.4 ns (+0.6) | +1.8 ns (+0.5) |
| 6 / 8 MHz (Z80B / Z80H), checked by hand | PASS / PASS | PASS / PASS | PASS / PASS |

The separations keep 20 ns or more (24 at 50 %): measured at 1.5 V, a
5 V output's low pulse is shorter, its falling edge crossing late and its
rising edge early. The input holds that looked tightest were the pad
libraries' extrapolation to 15 ns edges (`../spice/README.md`). The
GF180 chips' opcode setup is
what `z80_top`'s input paths and clock tree leave: until their cores kept
the library's delay cells out (`EXTRA_EXCLUDED_CELLS`), every port of
`z80_top` had one in front of it, 3-5 ns at the slow corner, and the
6 and 8 MHz grades failed on it; LibreLane's timing repair after global
routing, off by default, added 3-4 ns more. IHP's 4 mA pads, characterised to 12 pF,
took ~70 ns into 100 pF at the slow corner and missed TdCr(WRf) by
6.9 ns; that chip uses the 16 mA pads.

## Sources

`z8400_nmos.json` comes from a separately prepared NMOS timing package
(z80-nmos-timing); its 4 MHz column was checked against the scanned
datasheet tables, whose SHA-256 it records. The package's notes on the
datasheet's inconsistencies hold here too: the printed starred values do
not all agree with their formulas, and parameter 33's 8 MHz entry is
printed positive. Its per-bus-cycle SDCs and waveform tools are not used:
they check each output against every reference edge, which on this
dual-edge pin former pairs launch and capture edges that cannot occur.
