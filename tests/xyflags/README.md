# X and Y corpus

Every case `scripts/xy_study.py` found where the reference emulators
(ours, floooh/chips, superzazu, suzukiplan, redcode/Z80) do not all agree
about F, or where one of them differs from the die, plus a random sample
of the cases where they all agree -- with perfectz80's values. FUSE's
format, xz-compressed:

- `tests.in.xz`, `tests.expected.xz`: 4,061 cases. `make xyflags` (C
  model), `make xyflags_rtl` (RTL), `make xyflags_pz80` (the die).
- `record.txt.xz`: every one of the 8,497 cases the study ran on the die,
  with the die's F and each emulator's.

How the cases were made, the tables and what each emulator gets wrong:
[docs/xy-flags.md](../../docs/xy-flags.md).
