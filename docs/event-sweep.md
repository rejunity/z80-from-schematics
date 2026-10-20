# RESET / INT / NMI timing sweep

`make event_sweep` drives RESET, INT and NMI pulses into the iverilog RTL and
into the perfectz80 transistor-level netlist at every T-state of a set of
instructions. It compares the two half-cycle by half-cycle. CI runs it on
every push; it takes about 45 s on 10 cores and about 2 minutes on a GitHub
runner.

Files:

- `scripts/event_sweep.py`: case list, program, runner, comparison, report
- `tests/iverilog/tb_event_sweep.v`: RTL side (1/16-T event resolution)
- `scripts/event_sweep_pz80.c`: perfectz80 side
- `tests/event_sweep/baseline.txt`: cases where the RTL differs today

## How a case works

Every case runs the same small program:

| address | code |
|---|---|
| 0000 | stores SP, BC, DE, HL, I, AF and R at 7000.., then `JP 0100` |
| 0038 | IM1 handler: counts in 6000, `EI; RETI` |
| 0066 | NMI handler: counts in 6001, optional NOPs, `RETN` |
| 0100 | `DI`, SP=9000, IMn, BC=3 DE=8300 HL=8000 IX=8200 I=3F A=42, optional `EI` |
| 0200 | marker NOP, the instruction under test, 24 NOPs, `HALT` |

Times are counted from the rising edge that starts the marker fetch's T1.
A pulse is placed at T-state `n` of the target (`M4` is the marker's T4, `T1`
is the target's T1), `j`/16 of a clock after that T-state's rising edge.
Both sides then dump `mreq iorq rd wr m1 rfsh halt busack`, the address and
the data bus for every half-cycle from the marker on.

A case matches when:
- every control pin agrees in every half-cycle;
- the address agrees in every half-cycle: transfers, refreshes (I:R), and
  the internal and idle T-states, where the bus keeps its last address;
- the data agrees wherever both sides drive WR.

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

| group | pulses |
|---|---|
| `int` | INT from every T-state of seven instructions until INTA (IM1, EI) |
| `nmi` | 1 T NMI pulse at every T-state of all 15 instructions |
| `nmi_short` | 1/16 T and 1/4 T NMI pulses at every 1/16 T of INC A, LD A,(HL), LD (HL),A, INC (HL) |
| `nmi_reentry` | NMI held low; two pulses before acceptance; a second pulse during the response; nested; RETN with INT pending |
| `reset` | 4 T RESET at every T-state of six instructions |
| `reset_release` | release swept over a clock, 3 and 4 T after assertion |
| `reset_wait` | RESET while a read is held by WAIT |
| `reset_short` | 1/4 to 2 T RESET around M1 (the special-reset window) |
| `combo` | RESET during an INT or NMI response; an NMI edge while RESET is low; NMI or INT held across RESET |

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

## Results (rejunity `nmos-pins`, 20 October 2026)

| group | cases | match | differ |
|---|---|---|---|
| int | 130 | 130 | 0 |
| nmi | 474 | 474 | 0 |
| nmi_short | 1184 | 1184 | 0 |
| nmi_reentry | 43 | 43 | 0 |
| reset | 112 | 112 | 0 |
| reset_release | 64 | 64 | 0 |
| reset_wait | 32 | 32 | 0 |
| reset_short | 240 | 240 | 0 |
| combo | 96 | 96 | 0 |

All 2375 cases match; the baseline is empty. When the sweep was written
(main `ec3c5d2`) 809 did. Since then:
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

Useful options (`scripts/event_sweep.py --help`):
- `--groups nmi,reset` and `--match inc_hl` run a subset.
- `--phases 16` runs the full 1/16-T grid.
- `--jobs N` sets the parallelism.
