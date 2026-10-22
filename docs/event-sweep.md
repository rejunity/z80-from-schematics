# RESET / INT / NMI / BUSREQ / WAIT timing sweep

`make event_sweep` drives RESET, INT, NMI, BUSREQ and WAIT pulses into the
iverilog RTL and into the perfectz80 transistor-level netlist at every
T-state of a set of instructions, in IM1, IM2 and IM0, and of one opcode of
every timing class the die has; it runs every opcode with BUSREQ and WAIT
walked through it. It compares the two half-cycle by half-cycle -- the pins,
the address and which pads the die drives -- and runs every case through
the C model too, which must match the RTL in every half-cycle. CI runs it
on every push, in four shards, from perfectz80's recorded traces.

Files:

- `scripts/event_sweep.py`: case list, program, runner, comparison, report
- `tests/iverilog/tb_event_sweep.v`: RTL side (1/16-T event resolution)
- `scripts/event_sweep_pz80.c`: perfectz80 side
- `scripts/pz80_pads.c`: which of perfectz80's pads the die drives
- `tests/event_sweep/pz80/`: perfectz80's recorded traces, one file per group
- `scripts/event_sweep_c.c`: C model side (`--c-model`)
- `tests/event_sweep/baseline.txt`: cases where the RTL differs today

## How a case works

Every case runs the same small program:

| address | code |
|---|---|
| 0000 | stores SP, BC, DE, HL, I, AF and R at 7000.., then `JP 0100` |
| 0038 | INT handler: counts in 6000, `EI; RETI` |
| 0066 | NMI handler: counts in 6001, optional NOPs, `RETN` |
| 0100 | `DI`, SP=9000, IMn, BC=3 DE=8300 HL=8000 IX=8200 I=3F A=42, optional `EI` |
| 0200 | marker NOP, the instruction under test, 24 NOPs, `HALT` |
| 3800 | `JP 0038` |
| 3F00 | IM2 table: 0038 at every even address, so an odd vector (and FF, which reads 3FFF and 4000) finds 3800 |

The interrupting device puts its byte on the bus in INTA: FF unless the
case gives one (the IM2 vector, or the opcode in IM0). In IM0 it can hand
over more bytes, one per INTA cycle or memory read until it has none left,
as the die takes them: the opcode after a prefix in another INTA cycle, the
operands as memory reads at the interrupted PC. An opcode fetch from memory
ends its instruction. An IN reads what an OUT wrote to its address (FF
before).

Times are counted from the rising edge that starts the marker fetch's T1.
A pulse is placed at T-state `n` of the target (`M4` is the marker's T4, `T1`
is the target's T1), `j`/16 of a clock after that T-state's rising edge.
Both sides then dump `mreq iorq rd wr m1 rfsh halt busack`, the address,
the data bus and the pads driven for every half-cycle from the marker on.
The pads driven are, on perfectz80, every output pad whose pull-up or
pull-down gate is high (the address pads, the data pads, and MREQ IORQ RD
WR M1 RFSH HALT BUSAK), and on RZ80 `z80_top`'s `addr_drive`, `data_drive`
and `ctl_drive` (docs/timing.md "Pad drivers").

Two cases react to the pins, the same way on every side:
- the BUSREQ walk: BUSREQ low from the marker on; when BUSACK is seen low,
  BUSREQ goes high for the release and low again once BUSACK is high, so
  the instruction is granted at every M-cycle end where the die grants;
- the WAIT walk: WAIT low for the three half-cycles from each fall of MREQ
  (not a refresh) or IORQ, so every bus cycle gets the wait states its
  sampling edges see.

A case matches when:
- every control pin agrees in every half-cycle;
- the address agrees in every half-cycle: transfers, refreshes (I:R), and
  the internal and idle T-states, where the bus keeps its last address;
- BUSACK is one of the control pins;
- the data agrees wherever both sides drive WR;
- the same pads are driven in every half-cycle.

Instructions under test:
- INC A (no memory access)
- LD A,(HL) (read)
- LD (HL),A (write)
- INC (HL) (read and write)
- PUSH BC
- LD (nn),HL
- JR
- CALL
- LD (IX+d),A
- NEG
- RLC (HL)
- LDIR
- HALT
- LD A,I and LD A,R (P/V from IFF2)
- ADD HL,BC and ADC HL,BC (internal M-cycles of 4 + 3 T)
- EX (SP),HL, DJNZ (taken), RLD, INC BC
- IN A,(n), OUT (n),A, INI
- EI EI EI; EI DI (INT waits for the instruction after the last EI)
- DD DD + LD IX,nn; FD DD + LD IX,nn; DD before NEG and before NOP;
  RLC (IX+5) (no interrupt between a prefix and its opcode)

| group | pulses |
|---|---|
| `int` | INT from every T-state of eighteen instructions until INTA (IM1, EI) |
| `nmi` | 1 T NMI pulse at every T-state of all 31 instructions |
| `nmi_short` | 1/16 T and 1/4 T NMI pulses at every 1/16 T of INC A, LD A,(HL), LD (HL),A, INC (HL) |
| `nmi_reentry` | NMI held low; two pulses before acceptance; a second pulse during the response; nested; RETN with INT pending |
| `reset` | 4 T RESET at every T-state of ten instructions |
| `reset_release` | release swept over a clock, 3 and 4 T after assertion |
| `reset_wait` | RESET while a read is held by WAIT |
| `reset_short` | 1/4 to 2 T RESET around M1 (the special-reset window) |
| `combo` | RESET during an INT or NMI response; an NMI edge while RESET is low; NMI or INT held across RESET; BUSREQ at the boundary of an INT, an NMI and HALT ending on INT |
| `busreq` | 1 T and 4 T BUSREQ at every T-state of all 31 instructions: grant, BUSACK, resume |
| `wait` | 1 T and 3 T WAIT at every T-state of all 31 instructions |
| `im2` | INT in IM2 from every T-state of INC A, LD A,(HL), LD (HL),A, INC (HL) and HALT, with vectors 40, 41 and FF |
| `im2_combo` | IM2 response after INC A (vector 40); NMI, 1 T and 3 T WAIT, 1 T and 4 T BUSREQ, or 4 T RESET from every T-state of it, its table reads and the handler's first fetch |
| `im0` | twenty instructions handed over in IM0: RST 38 / 00 / 08, CALL, JP, LD A,n, LD A,(nn), JR, INC BC, PUSH BC, HALT, EI, EX (SP),HL, RETI, LD IX,nn, LD (IX+d),n, RLC A, RLC (IX+d), JP (IX), LDIR |
| `im0_combo` | IM0 CALL 1234 after INC A; NMI, WAIT, BUSREQ or RESET (as in `im2_combo`) from every T-state of it |
| `opcodes` | every opcode -- 1,841 programs: unprefixed, CB, ED, DD / FD and DD CB / FD CB, the flag-dependent ones also with F = 00 and FF, DJNZ also with B = 1, INIR / OTIR / INDR / OTDR with B = 2 -- each plain, with the BUSREQ walk, with the WAIT walk, with INT from the marker's T4 (EI) and with a 1 T NMI at its T1 |
| `classes` | one opcode of each of the 68 timing classes the instructions above don't cover (`CLASS_REPS`): 1 T NMI, INT until INTA, 1 T and 4 T BUSREQ, 1 T and 3 T WAIT from every T-state |
| `classes_reset` | 4 T RESET from every T-state of the same 68 |
| `fuzz` | 2,000 random cases (seed 1): 3-10 instructions drawn from every opcode program, a random F and BC, IM0 (an RST byte), IM1 or IM2 (vector 40, 41 or FF), EI or not, and one to three NMI / INT / WAIT / BUSREQ pulses of random widths in the first 40 T-states, 200 T run |
| `fuzz_reset` | the same with RESET among the pins |
| `reset_m1wait` | 4 T RESET at every quarter T-state while 1, 2 or 4 T of WAIT stretch the M1 of INC A and LD A,(HL) (known difference 25) |
| `reset_busreq` | 4 T RESET at every quarter T-state around a 2 T and a 6 T BUSREQ from the T1 of INC A, LD A,(HL) and PUSH BC (known difference 26) |
| `reset_nmi` | a 1 T NMI at every quarter T-state from 2 T before RESET's release to 8 T after it, RESET 3, 4 and 6 T (known difference 27) |
| `opcodes_reset` | 4 T RESET from every T-state of every opcode program of `opcodes`, one position per T-state (run by name: `--groups opcodes_reset`; known difference 19) |

The timing classes: the `opcodes` group's programs grouped by perfectz80's
control pins in their plain run and their BUSREQ and WAIT walks, 86 of
them; the 31 instructions above cover 18. `scripts/event_sweep.py
--classes` prints the table from the recorded traces.

### What a simulation can and can't tell

Neither side models gate delay. Within one clock half, only the order of
events relative to the clock edges matters. So by default each T-state gets
one position per half (`--phases 2`). `--phases 16` runs every 1/16 T, the
grid used on hardware. On the current RTL, `--phases 16` gives exactly 8× the
differing cases with identical grids, so nothing is lost by default.

Pulses shorter than T always sweep all 16 positions, because whether they
span a clock edge depends on that position.

perfectz80 advances in whole half-cycles: a transition is applied at the
start of the half-cycle that contains it. A pulse that starts and ends
inside one half-cycle is applied and removed between two clock steps. The
netlist settles after each write, so a latch that is transparent during
that half sees the pulse. The sweep therefore shows whether the chip's
input logic can catch such a pulse; it does not test the data sheet's
minimum widths, which depend on analog timing.

## Results (rejunity `sweep-all`, 21 October 2026)

| group | cases | match | differ |
|---|---|---|---|
| int | 488 | 488 | 0 |
| nmi | 984 | 984 | 0 |
| nmi_short | 1184 | 1184 | 0 |
| nmi_reentry | 43 | 43 | 0 |
| reset | 206 | 180 | 26 |
| reset_release | 64 | 64 | 0 |
| reset_wait | 32 | 32 | 0 |
| reset_short | 240 | 240 | 0 |
| combo | 132 | 132 | 0 |
| busreq | 1968 | 1968 | 0 |
| wait | 1968 | 1968 | 0 |
| im2 | 258 | 258 | 0 |
| im2_combo | 300 | 300 | 0 |
| im0 | 40 | 40 | 0 |
| im0_combo | 300 | 300 | 0 |
| opcodes | 9205 | 9205 | 0 |
| classes | 13260 | 13260 | 0 |
| classes_reset (run by name) | 2210 | 2040 | 170 |
| fuzz | 2000 | 2000 | 0 |
| reset_m1wait | 264 | 264 | 0 |
| reset_busreq | 312 | 312 | 0 |
| reset_nmi (run by name) | 120 | 48 | 72 |
| fuzz_reset (run by name) | 2000 | 1935 | 65 |

The default run, 33,248 cases, matches perfectz80 in all but 26 -- the
pads driven included -- and the C model matches the RTL in every
half-cycle of all of them. The 26 in the baseline are RESET stopping
ADD HL,BC or INC BC in the T-states where the die still writes their
result (docs/known-differences.md row 19): the register dump after reset
differs, the pins do not. `classes_reset`, RESET at every T-state of the
68 classes, shows how far row 19 reaches: 170 cases in 27 classes (LD
rr,nn, LD rr,(nn), JR, DJNZ, RST, IN r,(C), the DD / FD index forms, LDI /
CPI / INI / OUTI and their repeats) store other registers after the reset
than the die, and in 84 of them the C model and the RTL differ from each
other too -- each writes some results at another T-state. It joins the
default run when row 19 is fixed.

When the sweep was written (main `ec3c5d2`, 2287 cases) 809 differed.
Since then:
- `e60d06e` latches INT on the rising edge that starts the last T-state and
  catches an NMI pulse of any width, as perfectz80 does: INT and NMI were
  taken half a clock early (a pulse in the high half of the last T-state),
  and NMI pulses that span no falling edge were missed.
- The HALT pin rises in the low half of the last T4 when an INT or NMI ends
  HALT, and stays high when the interrupt is already pending at the end of
  HALT itself (24 cases).
- A second NMI edge during the NMI response, from its M1's T1 to the end of
  its two pushes, is lost, as in perfectz80, even when NMI then stays low
  (20 cases).
- INT held in an NMI handler is taken one instruction after RETN: RETN
  copies IFF2 to IFF1 after its boundary's INT decision (1 case).
- An INT accepted at the end of LD A,I or LD A,R leaves P/V = 0 in the
  pushed F, the NMOS race; an NMI there keeps P/V = IFF2 (the LD A,I /
  LD A,R cases came with the fix).
- RESET is the die's sampled RESET (544 cases): sampled on rising edges, a
  full reset taken on falling edges except in an M1's T2, the T-state ring
  stopped after one more T-state with the strobe patterns of a cut cycle,
  registers kept, T1 at 0000 on the third rising edge after release, and
  the special reset. docs/timing.md "RESET" has the details.
- The address is compared in every half-cycle: internal T-states keep the
  last address on the bus.
- BUSREQ is the die's (the `busreq`, `wait` and BUSREQ combo cases came with
  it): sampled like INT, a grant after any M-cycle with the next one held,
  BUSACK falling on the next rising edge and rising on the falling edge of
  the T-state that sees the release, the die's internal M-cycle boundaries
  (ADD / ADC HL,rr: 4 + 3 T). docs/timing.md "BUSREQ / BUSACK".
- IM2 (vectors 40, 41, FF, with NMI, WAIT, BUSREQ and RESET through the
  response and its table reads) and the EI and prefix runs matched when
  they were added.
- IM0 is the die's (`im0`, 26 cases): PC doesn't move until the
  instruction the device hands over ends, its operands are reads at that
  PC, and the opcode after a prefix comes in another INTA cycle
  (docs/timing.md "Interrupt acknowledge").
- With the C model beside the RTL, one case differed: an NMI edge caught by
  the rising edge that ends the NMI response's second push, which the die
  and the RTL lose and the C model kept (`nmir_entry_gap12_p12`); it loses
  it now.

`build/event_sweep/report.md` has the per-instruction grids.
`build/event_sweep/diff/` has a side-by-side trace for every differing case.

## The baseline

`tests/event_sweep/baseline.txt` has one line per case that differs today:

```
<case> <first differing half-cycle> <what differs> <hash of the RTL's compared pins>
```

`make event_sweep` fails in two situations:

- **New difference:** a case differs that isn't listed.
- **Changed difference:** a listed case now differs in another way. Any
  change in the RTL's pins for that case changes the hash.

A listed case that now matches is reported as a warning.

After an intended RTL change:

```
make event_sweep EVENT_SWEEP_ARGS=--update-baseline
git diff tests/event_sweep/baseline.txt
```

Review the baseline diff with `build/event_sweep/diff/<case>.txt` before
committing it.

## The fuzz

The groups above place one or two events where they were written to; the
fuzz puts random code, modes and pins together (`fuzz_cases()`): case i of
a seed is always the same, so a difference reproduces with
`--groups fuzz --fuzz-seed S --fuzz-n N --match fuzz_sS_0000i`. The default
run has seed 1's 2,000 cases, from perfectz80's recorded traces; CI's
nightly run also tries 500 cases of a fresh seed (its run id), perfectz80
live, and fails on any difference. When it was added, seed 1 found two
things nothing else had: a nested NMI's IFF2 (known difference 24) and the
RTL testbench answering every IN with FF where perfectz80's harness and the
C system return what an OUT wrote. `fuzz_reset` (with RESET) then found three
RESET rules the targeted groups had missed -- a Tw after an M1's T2, a bus
grant, an NMI right after the release (known differences 25-27) -- each
swept since by a group of its own; then, with row 19, a cut on the Tw of a
write that WAIT stretched (a push's second SP-1, made in that write's T2)
and the RTL's Q after DD CB RES / SET (an NMI-only case).

## perfectz80's recorded traces

perfectz80 is the slow side: a transistor-level netlist, about 10,000
half-cycles a second. `tests/event_sweep/pz80/<group>.txt.xz` holds its
output for every case of the group, keyed by what the run depends on: the
harness and perfectz80's sources (`event_sweep_pz80.c`, `pz80_pads.c`,
`scripts/refs/perfectz80/`), the program's bytes, the options and the
pulses as perfectz80 resolves them (whole half-cycles). A run takes a
recorded trace where one exists and runs perfectz80 for the rest: CI runs
only the RTL and the C model. Changing the harness or perfectz80 makes
every recorded trace stale, and a new or changed case has none:

```
make event_sweep EVENT_SWEEP_ARGS=--record      # rewrites the files of the groups run
```

`--no-cache` runs perfectz80 for every case.

Useful options (`scripts/event_sweep.py --help`):
- `--groups nmi,reset` and `--match inc_hl` run a subset.
- `--shard K/N` runs every Nth case from the Kth (CI's four jobs).
- `--record` writes perfectz80's traces; `--no-cache` ignores them.
- `--classes` prints the opcodes' timing classes.
- `--phases 16` runs the full 1/16-T grid.
- `--jobs N` sets the parallelism.
