# X and Y: the reference emulators against the die

F's bits 5 and 3 (Y and X) are documented nowhere by Zilog, and the
emulators we use as oracles (docs/oracles.md) do not agree about them.
`scripts/xy_study.py` measures how far they agree, puts every case where
they do not -- and a sample of the cases where they do -- on the die
(perfectz80), and keeps the die's answers as a test: `tests/xyflags/`,
run by `make xyflags` (C model), `make xyflags_rtl` (RTL) and `make
xyflags_pz80` (the die), all three in CI.

## How

Every case is a FUSE `tests.in` record: registers, WZ, I, R, IM, the
IFFs, memory and the T-states to run (whole instructions until at least
that many have run).

- `scripts/xy_oracles.cpp` runs it through our C model and the four
  oracles of `scripts/refs` -- floooh/chips, superzazu, suzukiplan,
  redcode/Z80 (as `Z80_MODEL_ZILOG_NMOS`) -- each from that state, and
  prints what each ends with. Every one starts with no flags written by
  the instruction before (Q = 0) where it has the notion (ours, redcode);
  IN reads the port address's high byte, as in FUSE's corpus.
- `scripts/fuse_pz80.c` runs it on the die: a preamble loads the
  registers and falls into the case, ending with `POP AF`, so Q is 0 there
  too; the die's registers are read in T4 of the M1 after the case.

The study (`make xy_study`, seed 1) ran 39,786 cases:

| kind | cases | what |
|------|------:|------|
| single | 25,226 | one instruction: every opcode -- unprefixed, CB, ED, DD, FD, DDCB, FDCB, the undocumented ones -- from 10 random states (registers, WZ, memory, PC), and those with X / Y rules of their own (SCF, CCF, DAA, CPL, BIT n,(HL), BIT n,(IX+d), the block instructions, LD A,I/R, IN r,(C), NEG, RLD, RRD, 16-bit ADC / SBC) from 120 |
| pair | 7,066 | an instruction, then SCF or CCF where it continues: the Q every opcode leaves |
| triple | 6,934 | a flag-writing instruction, an instruction, then SCF or CCF: whether each opcode keeps or clears that Q |
| wpair | 560 | a flag-writing instruction, then SCF or CCF behind prefixes (`DD 37`, `FD DD 3F`, ...) |

The random PC matters: a repeating block instruction takes X and Y from
PC's high byte. A third of the block cases end (BC or B = 1).

7,199 cases, where the five do not all agree about F, ran on the die,
and 1,298 sampled at random from the 32,587 where they all agree.

## The emulators against each other

X / Y equal, in % of the cases both ran to the same end (suzukiplan throws
on undefined ED and DD / FD opcodes; those cases leave it out). Our model
as main had it, before branch `nmos-q`'s Q fix:

|            | chips | superzazu | suzukiplan | redcode |
|------------|------:|----------:|-----------:|--------:|
| ours       | 92.2  | 88.9      | 85.1       | 99.9    |
| chips      |       | 96.1      | 92.4       | 92.1    |
| superzazu  |       |           | 88.8       | 88.7    |
| suzukiplan |       |           |            | 85.1    |

All five give the same X / Y for every unprefixed, CB (BIT n,(HL)
included), DD / FD and DDCB / FDCB opcode, and for LDI, LDD, CPI and CPD
when they do not repeat -- except SCF and CCF, also behind DD / FD, and
BIT n,(IX+d) (24.6 % of its cases agree). They differ in a third of the
ED cases (67.4 % agree), in the I/O block instructions (25.8 % once,
14.5 % repeating), in the repeating LDIR ... CPDR (24.0 %), and in SCF /
CCF on their own (50.4 %) and after another instruction (77.1 % and
92.9 %).

## Against the die

Every case run on the die, X / Y as the die's, then the whole F:

| emulator | cases run | X / Y as the die's | F as the die's |
|----------|----------:|-------------------:|---------------:|
| **redcode/Z80** | 8,497 | **100 %** | **100 %** |
| ours, main (before the Q fix) | 8,497 | 99.38 % (53 differ) | 99.38 % |
| ours, with `nmos-q`'s Q fix | 8,497 | **100 %** | **100 %** |
| floooh/chips | 8,497 | 62.96 % | 62.34 % |
| superzazu | 8,426 | 46.91 % | 43.85 % |
| suzukiplan | 7,517 | 35.16 % | 28.63 % |

Where they disagree (7,199 cases), X / Y as the die's / F as the die's,
by class (ours as main had it):

| class | cases | ours | chips | superzazu | suzukiplan | redcode |
|-------|------:|-----:|------:|----------:|-----------:|--------:|
| DD / FD | 21 | 100 / 100 | 0 / 0 | 0 / 0 | - | 100 / 100 |
| BIT n,(IX+d) | 2,291 | 100 / 100 | 100 / 100 | 100 / 100 | 0 / 0 | 100 / 100 |
| ED | 945 | 100 / 100 | 100 / 100 | 5 / 5 | 80 / 75 | 100 / 100 |
| INI ... OUTD, once | 478 | 100 / 100 | 100 / 100 | 26 / 0 | 100 / 50 | 100 / 100 |
| INIR ... OTDR, repeating | 477 | 100 / 100 | 51 / 40 | 25 / 0 | 51 / 23 | 100 / 100 |
| SCF / CCF | 119 | 100 / 100 | 0 / 0 | 0 / 0 | 0 / 0 | 100 / 100 |
| SCF / CCF after an instruction | 1,876 | 100 / 100 | 2 / 2 | 2 / 1 | 2 / 1 | 100 / 100 |
| SCF / CCF after a flag write and an instruction | 574 | 100 / 100 | 10 / 10 | 10 / 7 | 13 / 2 | 100 / 100 |
| SCF / CCF behind prefixes after a flag write | 53 | 4 / 4 | 4 / 4 | 4 / 4 | 100 / 0 | 100 / 100 |
| LDIR ... CPDR, repeating | 365 | 100 / 100 | 0 / 0 | 0 / 0 | 0 / 0 | 100 / 100 |
| **all** | 7,199 | 99.3 / 99.3 | 56.3 / 55.5 | 37.2 / 33.6 | 24.4 / 16.8 | 100 / 100 |

In all 1,298 sampled cases where the five agree, the die agrees with
them: no case turned up where every emulator is wrong.

What each gets wrong:

- **floooh/chips, superzazu, suzukiplan** take SCF's and CCF's X / Y from
  A alone (as if the instruction before had always written the flags);
  the die takes them from A | F when it did not (docs/flags.md "Q"). None
  of them takes a repeating block instruction's X / Y from PC's high byte
  (Banks, 2018), and their INIR ... OTDR flags are partly wrong.
- **superzazu** also gets the I/O block instructions' flags (it sets Z
  and N only), IN r,(C)'s and LD A,I / LD A,R's wrong.
- **suzukiplan** gets BIT n,(IX+d)'s X / Y wrong (the die takes them from
  the high byte of IX+d), leaves F alone in LD A,I and LD A,R (and its R
  is one short there), and throws on undefined ED and DD / FD opcodes.
- **redcode/Z80** agrees with the die in every case.
- **ours** (main): 53 cases, all Q -- an SCF / CCF behind a DD / FD prefix
  right after a flag write, and SCF / CCF after `FD 08` (EX AF,AF'
  behind a prefix): known differences 29 and 30, which branch `nmos-q`
  fixes. With its fix, which this branch carries, ours agrees with the die
  in all 8,497.

## The test

`tests/xyflags/` (FUSE's format, xz):

- `tests.in.xz`, `tests.expected.xz`: 4,061 cases, every case where any
  emulator's F differs from the die's -- at most three of each opcode, kind
  and set of emulators that differ -- and the 1,298 sampled ones, with the
  die's registers, WZ, PC, R and T-states (IFF1, IFF2 and IM, which
  perfectz80 has no reader for, are our model's). `make xyflags` runs
  them through the C model, `make xyflags_rtl` through the RTL and `make
  xyflags_pz80` on the die again (about five minutes): CI's `c-tests`,
  `rtl-tests` and `parity-tests`. Main's model fails 37 of them, the Q
  cases above.
- `record.txt.xz`: every one of the 8,497 cases that ran on the die, with
  the die's F and each emulator's (`=` where it is the die's).

To run the study again, with another seed or more cases:

```
make xy_study XY_STUDY_ARGS="--seed 2 --k 20"
make xy_study XY_STUDY_ARGS="--recheck"       # our model as built now, on the last run's die cases
```

`build/xy/report.md` has the tables; `--corpus DIR` writes a new corpus,
`--recheck --record FILE` a new record.
