# The Z80's AC characteristics, source by source

One JSON per printed AC table of the NMOS Z80 (and, for reference, three
CMOS Z84C00 tables), transcribed by hand from scans, with the document,
date, pages, URL and SHA-256 of the scan each came from. No scans are
kept here, only the numbers and their citations. `../compare_sources.py`
compares them and writes [DISCREPANCIES.md](DISCREPANCIES.md)
(`--check` tells whether it is current). `pin_timing.py` does not read
these files: its tables are `../z8400_nmos.json` (PS0178) and
`../z8400_nmos_1983.json` (the September 1983 Z80 column).

Each file: `grades` (MHz keys), `parameters` by Zilog's number (1-53),
`values` as printed (`null`: not printed), `starred` (the grades whose
value is period-dependent), the footnote `formula` (`expr` in TcC, TwCh,
TwCl, TrC, TfC plus a per-grade `offset_ns`), `max_values` / `min_values`
for a second bound, the table's `ac_test_conditions` and the edge time
its starred values say they assume (`formula_assumption`). The early
tables (1977-82) have no numbers: `source_symbol` keeps their symbol and
`id` the numbered parameter it became (`null` when none).

Examined but not transcribed (same table as a transcribed edition, by a
visual check of the first page): the 1982 Zilog Data Book's Z80 CPU
(PDF page 34; = 1981, its Z80B column marked preliminary) and the 1983/84
Components Data Book's (PDF page 35; = the February 1984 handbook); the
1992 Zilog book's NMOS Z8400 table (PDF pages 41-42; = 1989, compared by
its OCR text); the Mostek 1981 book's military MKB3880-80 table (PDF page
598; tD(D) 250 ns against 230). Searched without finding a Z80 AC table:
the 1981 SGS MOS book, the 1982 Toshiba Microprocessor Data Book and the
1984 NEC catalog and the 1987 Z80 Technical Manual (a keyword search of
their OCR text only).

## What the comparison finds (2026-10-29)

Ranked by what it changes in what RZ80's pin timing checks.

1. **33 TdD(WRf), I/O write, 8 MHz: +55 is a dropped minus sign; -55 is
   right.** Every Zilog NMOS table prints +55* (preliminary Z80H column of
   the 1984 handbook, 1985, 1989, 1992, PS0178), but its own footnote
   formula, TwCl + TrC - 120, gives -55 at the 8 MHz limit clock
   (TwCl 55, TrC 10; -45 with the 20 ns edges the footnote claims), and
   every other 8 MHz starred value in those tables fits the formula with
   10 ns edges. Printed -55: Sharp LH0080E (NMOS, 8 MHz; 1990 Sharp
   Microcomputers Data Book, pp. 288-289, with the same formula), Zilog
   Z84C0008 (1989 Z80 Family Data Book p. 21; 1992 book p. 35), Toshiba
   TMPZ84C00AP-8 (MPUZ80-49). The 6 MHz column of the same Zilog table
   prints -55 too. RZ80 checked +55 (the greater of the printed value and
   the formula), 110 ns stricter than the part, until e37c7a8: since then
   z8400_nmos.json lists 33 at 8 MHz under "misprint" and pin_timing.py
   takes the footnote formula at the clock checked.

2. **6 MHz: TcC 162, but the starred values computed for 165.** From 1989
   on (and in PS0178) the Z80B's minimum period is 162 ns; 11 TwMREQl, 26
   TdA(IORQf), 29 TdD(WRf), 31 TwWR and 50 TdM1f(IORQf) still print the
   values for 165 (135, 110, 25, 135, 365), 3-10 ns more than their
   formulas give at 162 (132, 107, 22, 132, 359). The 1992 CMOS table
   prints the 162 values. RZ80 checks the printed (stricter) ones; the
   formula's are physically right at 162 ns. Not a failure today; it would
   matter only if a chip missed by less than 10 ns.

3. **The starred values assume a clock no part can have at its minimum
   period.** TcC = TwCh + TwCl + TrC + TfC, yet at 4, 6 and 8 MHz the
   minimum period is shorter than the minimum widths plus the assumed
   edges (250 < 110 + 110 + 20 + 20). The printed starred values combine
   all of them, so at a real minimum-period clock (pin_timing's: 15, 16,
   7.5 ns edges) they are 2.5-10 ns stricter than the formula. RZ80 checks
   the printed values: conservative, no change needed.

4. **TwCh maximum: 2000 ns (1985 on) against 200 us (1977-84, Mostek,
   NEC, Sharp to 1990).** The early tables: "Although static by design,
   testing guarantees tw(PhiH) of 200 us maximum"; from 1985 Zilog prints
   2000 ns for TwCh, the same as TwCl. The NMOS Z80's clock-high phase is
   its static one, so 200 us is the physically meaningful limit and 2000
   ns looks copied from TwCl. RZ80's 4 MHz table checks the Spectrum's
   stretched high phase against 2000 ns (1.84 us, passes); with 200 us it
   passes with more room. No failure either way.

5. **2.5 MHz, 43 TdCr(CTz) (control outputs float): 100 ns in 1977-78
   (Zilog, Mostek, NEC), 110 ns from 1981.** RZ80's 2.5 MHz table checks
   110. The older 100 ns is the stricter guarantee of the parts sold
   then (the TRS-80 era); RZ80's margin there is far larger, so checking
   100 would change nothing today.

6. **33 TdD(WRf) formula, 1977-78 against 1981 on: TwCl + TrC - 210 / -170
   (Z80 / Z80A) became -180 / -140.** The later tables promise the data 30
   ns earlier before WR falls; RZ80 checks the later (stricter) one.

7. **Measurement conditions.** Outputs timed at 2.0 V / 0.8 V up to 1984
   (and by Sharp, Mostek, NEC), at 1.5 V from 1985; load 50 pF with "add
   10 ns per 50 pF up to 200 pF (data) / 100 pF (address, control)" in
   1977-82 (and Sharp, at 100 pF), 100 pF in 1983-84, and from 1989 a
   width derating for 10 and 11 only (minus 10 ns per 50 pF above 50 pF).
   RZ80 already measures each table at its own levels at 100 pF.

8. **Smaller items.** Mostek's MK3880-4 alone prints a *minimum* of 20 ns
   for MREQ's fall after CLK falls (8 TdCf(MREQf)); 37 TwNMI at 8 MHz is
   starred (60*) with no formula in Zilog's tables, plain in Sharp's;
   symbol variants (Sharp TdDm / TdDi for 29 / 33, BUSRQ / BUSAK;
   Toshiba's misprinted TdCr(A) for 45 and TsINTr for 49); the CMOS parts
   hold 10 ns where the NMOS ones hold 0 (18, 39, 47, 49; Toshiba marks
   them and 50 "non-compatible with NMOS Z80"), and the 1992 CMOS table moves 34
   (8 MHz) to 60 and 37 (6 MHz) to 60. The low-power Z8300 (1982) table is
   internally inconsistent (DISCREPANCIES.md (a)), a different part.

## Not verified

- The 1981 Components Data Book's and the 1985 book's test-condition
  pages (load capacitance) were not read; DISCREPANCIES.md says "not
  recorded".
- The NEC table's print is small; its values were read from a 120 dpi
  image and agree with its OCR text and with the 1977 Zilog table.
- The 1978 product specification: only its Z80 page was transcribed.
- No SGS, NEC (other than the 1982 catalog), Hitachi or U880 table was
  found among the scans available; a Zilog Z80 data sheet before 1977, if
  one exists, was not found either.
