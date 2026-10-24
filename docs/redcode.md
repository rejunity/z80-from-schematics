# redcode/Z80 against the die

[redcode/Z80](https://github.com/redcode/Z80) (Manuel Sainz de Baranda y
Goñi, vendored in `scripts/refs/redcode_z80/`) is the most thoroughly
researched of the four reference emulators ([oracles.md](oracles.md)).
This page runs it through every test the replica has -- the instruction
suites, the programs compared with perfectz80 live, and every case of the
pin sweep against perfectz80's recorded traces -- and says where it does
and does not do what the Zilog NMOS die does. `make redcode_tests` runs all
of it (by hand; not in CI: redcode is a reference, not the replica).

## The build

redcode is built as Zilog NMOS: `Z80_MODEL_ZILOG_NMOS` at run time, and
`REDCODE_FLAGS` (Makefile) at compile time -- Q, the special reset, the
full IM0, the unofficial RETI, parity, and
`Z80_WITH_ZILOG_NMOS_LD_A_IR_BUG`. Without that last one,
`Z80_OPTION_LD_A_IR_BUG` -- part of `Z80_MODEL_ZILOG_NMOS` -- does nothing:
the NMOS race in which an INT taken at the end of `LD A,I` / `LD A,R`
clears P/V is compiled out, and 36 of the sweep's 62 cases of INT around
those two differ from the die (pushed F 2D where the die pushes 29).
`REDCODE_FLAGS` lacked it until now; the lockstep, the benchmark and the
X/Y study take no interrupts, so nothing they reported changes.

## The results

| Suite | Target | redcode |
|---|---|---|
| FUSE, 1356 cases (6 against the die's values) | `make redcode_fuse` | 1356 / 1356 |
| X / Y corpus, 4,061 cases with the die's values ([xy-flags.md](xy-flags.md)) | `make redcode_xyflags` | 4061 / 4061 |
| z80test: z80doc, z80flags, z80memptr, z80full, z80ccf | `make redcode_z80test` | 160 / 160 each |
| prelim, ZEXDOC, ZEXALL | `make redcode_zex` | complete, no errors |
| NASCOM BASIC 4.7c (3 scripts, IM1 receive interrupt) and 1K Tiny BASIC | `make redcode_basic` | 4 / 4 |
| KC85 capture, T-states per opcode (`tests/sigrok/`) | `make redcode_sigrok` | 50 / 50 |
| 5-way lockstep on ZEXDOC3 | `make lockstep` | identical, 7,022,691 instructions |
| The 14 trace programs and 15 pin scenarios, perfectz80 live | `make redcode_sweep REDCODE_SWEEP_ARGS=--traces` | 29 / 29 (prog17_reset: but for the instruction RESET cuts) |
| HALT-to-INT, the ten INT placements of `make halt2int` | `make redcode_sweep REDCODE_SWEEP_ARGS=--halt2int` | 6 5 4 7 6 5 4 7 6 5 T: the die's |
| The pin sweep, 78,894 cases (every group and `opcodes_reset`) | `make redcode_sweep` | below |

## The pin sweep at instruction level

redcode runs a whole instruction, or interrupt response, per call
(`z80_run(cpu, 1)`); it has no clock, half-cycles, WAIT or BUSREQ. So
`scripts/event_sweep_redcode.c` is the system an instruction-stepped
emulator lives in: it keeps time in T-states from the marker fetch, as the
sweep's pulses are given, and presents the pins the way the die samples
them:

- **INT**: low or not at the rising edge that starts an instruction's last
  T-state, given with `z80_int` before redcode decides at the boundary.
- **NMI**: the die's flip-flop is set by the pin's falling edge; an edge
  before that same rising edge is taken there (`z80_nmi`). An edge during
  an NMI response the die loses: it goes to redcode right after the
  response, where redcode rejects a second NMI.
- **RESET**: sampled at every rising edge. Seen in the T2 of an M1 cycle
  and gone at the next edge, it is the special reset
  (`z80_special_reset`). Otherwise the die runs the next T-state as a
  cut cycle and idles, and redcode gets `z80_instant_reset`; the fetch at
  0000 comes on the third rising edge after RESET goes, and the die keeps
  no NMI edge from before the second.
- **A pin is one wire**: two pulses that overlap end when the first does,
  as the die's harness drives them; a transition exactly on an edge counts
  after it. The interrupting device drives INT high while M1 and IORQ are
  low, as in the sweep's harnesses.
- **WAIT, BUSREQ**: only time. Where a case has either, the harness takes
  the die's instruction boundaries from its trace, and only the order and
  content of the bus cycles are compared.

Both sides become lists of bus cycles -- opcode fetches (and an NMI
response's discarded fetch, and HALT's NOPs), INTA cycles, the refresh
after each (I and R), memory and I/O reads and writes, each with its
address and data -- and the T-state each M1-type cycle starts in. 22,050
of the 78,894 cases have WAIT or BUSREQ; 33,215 have RESET.

| group | cases | match | cut | latch | differs |
|---|---:|---:|---:|---:|---:|
| int | 488 | 488 | 0 | 0 | 0 |
| nmi | 984 | 984 | 0 | 0 | 0 |
| nmi_short | 1184 | 1184 | 0 | 0 | 0 |
| nmi_reentry | 43 | 43 | 0 | 0 | 0 |
| reset | 206 | 34 | 172 | 0 | 0 |
| reset_release | 64 | 32 | 32 | 0 | 0 |
| reset_wait | 32 | 0 | 32 | 0 | 0 |
| reset_short | 240 | 96 | 144 | 0 | 0 |
| combo | 132 | 40 | 92 | 0 | 0 |
| busreq | 1968 | 1968 | 0 | 0 | 0 |
| wait | 1968 | 1968 | 0 | 0 | 0 |
| im2 | 258 | 258 | 0 | 0 | 0 |
| im2_combo | 300 | 254 | 46 | 0 | 0 |
| im0 | 40 | 40 | 0 | 0 | 0 |
| im0_combo | 300 | 258 | 42 | 0 | 0 |
| opcodes | 9205 | 9205 | 0 | 0 | 0 |
| classes | 13260 | 13260 | 0 | 0 | 0 |
| classes_reset | 2210 | 418 | 1792 | 0 | 0 |
| opcodes_reset | 27768 | 5523 | 22245 | 0 | 0 |
| fuzz | 2000 | 2000 | 0 | 0 | 0 |
| fuzz_reset | 2000 | 1420 | 580 | 0 | 0 |
| reset_m1wait | 264 | 52 | 212 | 0 | 0 |
| reset_busreq | 312 | 72 | 240 | 0 | 0 |
| reset_nmi | 120 | 0 | 120 | 0 | 0 |
| latch | 268 | 60 | 0 | 208 | 0 |
| q_ops | 3682 | 3682 | 0 | 0 | 0 |
| q_seq | 462 | 462 | 0 | 0 | 0 |
| q_events | 2448 | 2246 | 202 | 0 | 0 |
| q_reset | 512 | 168 | 344 | 0 | 0 |
| wz_int | 3712 | 3712 | 0 | 0 | 0 |
| blk_events | 2464 | 2154 | 310 | 0 | 0 |
| **all** | 78894 | **52081** | **26605** | **208** | **0** |

- **match**: the same bus cycles, in order, with the same addresses and
  data, every M1-type cycle in the same T-state (only the order and data
  where WAIT or BUSREQ stretch the die's time).
- **cut**: a RESET stops the die inside an instruction; redcode can only
  finish it or not run it. Everything but that instruction's own bus
  cycles agrees, with the instruction finished (11,000 cases) or
  undone (12,461) -- or only the registers the restart's dump
  stores differ (3,144): the die had not yet written the
  result it writes back in the next M1, or had half of it
  ([known-differences.md](known-differences.md) row 19, f_cut), or a PUSH
  / POP it stopped half-way had moved SP by one.
- **latch**: the `latch` group puts a read's byte on the bus in one
  half-cycle only and a decoy in the others, to find the edge the die
  takes it on ([timing.md](timing.md) "The data latch"); an
  instruction-stepped emulator has no such edge.
- **differs**: none. Nor do F, WZ or Q differ anywhere: where the bus
  cycles match, each instruction's F, WZ and Q as redcode leaves them
  are the die's in T4 of the M1 that follows (perfectz80's state columns,
  [event-sweep.md](event-sweep.md)), up to the first RESET -- with P/V
  aside at an INTA, which the die writes back after the INT has cleared
  IFF2 and redcode clears as it takes the INT (the LD A,I / LD A,R race:
  the same F is pushed).

## What it shows

- Everything redcode models agrees with the die, at instruction level, in
  every case: where an INT or NMI is taken and where not (EI, a prefix
  chain, RETN raising IFF1, a second NMI in the response, DI), HALT and its
  exit, the IM0 / IM1 / IM2 responses (the device's bytes, the vectors,
  the pushes), the NMI response, the NMI / INT interplay of IFF1 and IFF2,
  RETN / RETI, the LD A,I / LD A,R race, the special reset, the registers
  after a reset, I and R through every refresh, Q and WZ through every
  interrupt, and the T-state every instruction and response starts in.
- What the die does inside an instruction -- the cuts of a RESET, the
  results it writes back in the next M1, WAIT and BUSREQ, the data latch
  -- is below what redcode models; the sweep's own models (C, RTL) are
  checked there every half-cycle.
- The host's part: the rules above are the die's, and an emulator built
  around redcode needs them to agree with the die at this level -- above
  all the sampling edge for INT and NMI (an emulator that tests the pins
  between instructions takes an NMI or INT one instruction early whenever
  it comes in the last T-state), the NMI edges a RESET swallows, and
  the release of INT by the interrupting device.
- Nothing found is a redcode bug; worth telling upstream: that
  `Z80_MODEL_ZILOG_NMOS` needs `Z80_WITH_ZILOG_NMOS_LD_A_IR_BUG` at build
  time for its LD A,I / LD A,R race, which is easy to miss (it was missing
  here).
