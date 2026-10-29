# Pin timing: the chip against the NMOS Z80's datasheet

The replica's target is Zilog's NMOS Z80, the Z8400, and above all its
4 MHz grade (Z80A); 6 and 8 MHz (Z80B, Z80H) are a bonus, and the later
CMOS parts are not the reference. This directory checks a laid-out
`z80_chip` against the Z8400's AC characteristics and gives the chip flow
the same contract as constraints.

| | |
|---|---|
| `z8400_nmos.json` | All 53 numbered AC parameters of Zilog PS0178 (PS017801-0602, Z8400 NMOS table, printed pages 38-39): the 4 / 6 / 8 MHz columns, the footnote formulas, which clock edge or pin event each is measured from, provenance. |
| `z8400_nmos_1983.json` | The same 53 for the original Z80 CPU, 2.5 MHz, which PS0178 no longer lists: the Z80 column of Zilog's Z8400 product specification of September 1983 (Z80 Family Product Specifications Handbook, February 1984, printed pages 23-24), its footnote formulas and its AC test conditions. |
| `pin_timing.py` | Measures a LibreLane chip run with OpenSTA and checks every parameter the replica must meet, per process corner; `--write-sdc` writes the same contract as the flow's SDC. |
| `z80_chip_nmos4.sdc` | That SDC for 4 MHz (generated). |

    make -C asic/gf180mcu pin_timing            # the chip's latest run, 4 MHz
    make -C asic/gf180mcu pin_timing GRADE=6    # the 6 MHz grade
    make -C asic/gf180mcu pin_timing GRADE=2.5,trs80,spectrum   # several points
    asic/timing/pin_timing.py RUN --all         # every grade and machine clock
    asic/timing/pin_timing.py --write-sdc asic/timing/z80_chip_nmos4.sdc

The chip directories (`asic/gf180mcu/`, `asic/gf180mcu-9t/`,
`asic/ihp-sg13g2/`) are on the `asic-<pdk>` branches, which the Targets
workflow merges into main before it builds them; each chip's job checks
the 4 MHz grade, the 2.5 MHz one and the machines' clocks below after the
layout and fails on any parameter (6 and 8 MHz: by hand, `GRADE=6,8`).

## Grades and old machines' clocks

`--grade` takes one point or several, comma-separated; `--all` takes
every one:

| Point | Clock | Table | |
|---|---|---|---|
| `2.5` | 2.5 MHz, 400 ns, 20 ns edges | Z80 CPU, 1983 | the original Z80 |
| `4` | 4 MHz, 250 ns, 15 ns edges | Z80A, PS0178 | the replica's target |
| `6`, `8` | 6 / 8 MHz | Z80B / Z80H, PS0178 | |
| `trs80` | 1.774 MHz (10.6445 MHz / 6) | Z80 CPU, 1983 | TRS-80 Model I |
| `zx81` | 3.25 MHz (6.5 MHz / 2) | Z80A, PS0178 | ZX80, ZX81 |
| `spectrum` | 3.5 MHz (14 MHz / 4) | Z80A, PS0178 | ZX Spectrum 16K / 48K |
| `spectrum128` | 3.5469 MHz (17.7345 MHz / 5) | Z80A, PS0178 | ZX Spectrum 128K |

A grade runs at its minimum period with the edges that leave the minimum
high and low times; a machine's clock has its grade's edges and the
rest of its period split evenly between high and low. The datasheets'
starred minimums (pulse widths, separations) follow the footnote
expressions at the point's own period, the rest of the table stays as
printed. For the slower points the line under the header checks the clock
against the grade's own limits (TcC, TwCh, TwCl and their maximums) --
the system's, not the replica's: RZ80 is static and takes a clock phase
of any length. The Spectrums' ULA holds CLK high through contended
accesses for up to six more T-states (1.8 us, within PS0178's 2 us for
TwCh). The Spectrum's clock stretching and the ZX80 / ZX81's display
tricks themselves are cycle-level matters: perfectz80 pin scenarios
(`tests/traces/pin_scenarios/`).

What OpenSTA measures depends on the clock's and the inputs' edge only:
the delays from the CLK pin's edges and the setup and hold the inputs
need do not change with the period. So it measures once per corner for
every edge the points share (four: 7.5, 15, 16 and 20 ns, from the 8, 4,
6 and 2.5 MHz grades' periods) and checks each point from that; the
corners and edges run in parallel (`--jobs`, default the CPUs).

The 1983 table differs from PS0178 in how it is measured, not only in
its numbers: its AC test conditions time the outputs where they cross
VOH = 2.0 V rising and VOL = 0.8 V falling (PS0178: 1.5 V both), which
the check takes from the pads' simulated data waveform at the test load
(2.6 / 4.3 ns later at tt, 4.8 / 7.7 at ss, 1.6 / 2.7 at ff on GF180's
`bi_t`), and it gives no derating of the MREQ pulse widths for the load
(PS0178's parameters 10 and 11: 10 ns less per 50 pF over 50). Its
starred values are the footnote expressions at TcC = 400, TwCh = TwCl =
180, TrC = TfC = 20 ns, every one. Two oddities, kept as noted in the
file: parameter 52 is printed TdCf(IORQr) but reads "Clock rising to
IORQ rising" (PS0178's TdCr(IORQr), taken as that), and TwCh has no
printed maximum, a footnote saying that "although static by design,
TwCh of greater than 200 us is not guaranteed" (TwCl: 2000 ns). On main, `make
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
| 6 / 8 MHz (Z80B / Z80H), checked by hand | PASS / PASS | PASS / PASS (its layout of 2026-10-29: 8 MHz FAIL, parameter 33 -1.0 ns at the slow corner) | PASS / PASS |

The 2.5 MHz grade and the old machines' clocks, GF180 7-track (the chip
of `asic-gf180mcu`'s run of 2026-10-26, `pin_timing.py --all`): PASS at
every corner, the tightest margins the inputs' holds at the fast corner
-- ThWAIT +6.4 ns at 2.5 MHz and the TRS-80's 1.774 MHz (the 20 ns edges),
ThBUSREQ +6.4 ns on the 4 MHz table at the ZX80 / ZX81's and the
Spectrums' clocks, as at 4 MHz itself -- and every output 36 ns or more
inside the 1983 table's limits, measured at its 2.0 / 0.8 V. The 8 MHz
grade's tightest is parameter 33, TdD(WRf) for I/O, +1.6 ns: the printed
+55 ns its formula does not give (Sources).

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
