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
- the address agrees wherever both sides transfer data (MREQ or IORQ with RD
  or WR) or refresh (RFSH low: I:R);
- the data agrees wherever both sides drive WR.

Idle addresses are don't-care, as in `compare_signal_timing.py`.

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

## Results (rejunity `main`, 20 October 2026)

| group | cases | match | differ |
|---|---|---|---|
| int | 130 | 130 | 0 |
| nmi | 474 | 474 | 0 |
| nmi_short | 1184 | 1184 | 0 |
| nmi_reentry | 43 | 43 | 0 |
| reset | 112 | 0 | 112 |
| reset_release | 64 | 0 | 64 |
| reset_wait | 32 | 0 | 32 |
| reset_short | 240 | 0 | 240 |
| combo | 96 | 0 | 96 |

1831 of 2375 cases match. When the sweep was written (main `ec3c5d2`) 809
did. Since then `e60d06e` latches INT on the rising edge that starts the
last T-state and catches an NMI pulse of any width, as perfectz80 does:
that fixed the two largest groups, INT and NMI taken half a clock early
(a pulse in the high half of the last T-state) and NMI pulses that span no
falling edge. The HALT pin now rises in the low half of the last T4 when
an INT or NMI ends HALT, and stays high when the interrupt is already
pending at the end of HALT itself (24 cases). A second NMI edge during
the NMI response, from its M1's T1 to the end of its two pushes, is now
lost, as in perfectz80, even when NMI then stays low (20 cases). INT held
in an NMI handler is taken one instruction after RETN: RETN copies IFF2 to
IFF1 after its boundary's INT decision (1 case). An INT accepted at the
end of LD A,I or LD A,R leaves P/V = 0 in the pushed F, the NMOS race
(the LD A,I / LD A,R cases were added with the fix; an NMI there keeps
P/V = IFF2).

`build/event_sweep/report.md` has the per-instruction grids.
`build/event_sweep/diff/` has a side-by-side trace for every differing case.

Where the RTL matches perfectz80:
- INT and NMI are taken at the right instruction boundary from every
  position, for every instruction tried.
- NMI pulses of 1/16 T and 1/4 T are taken from every 1/16 T position.
- The stacked PC and the response cycles are the same, including LDIR
  between iterations, CALL, JR and prefixed instructions.
- NMI held low, two pulses before acceptance, and nested NMIs all match.

The differences left are all RESET.

**RESET.** perfectz80 treats RESET as a sampled input. RZ80 resets
asynchronously:
- **Assertion.** perfectz80 samples RESET at rising edges. It finishes
  the bus cycle in progress and goes idle 0–8 half-cycles after RESET
  falls, and it ignores a pulse that covers no rising edge. RZ80 drops
  every strobe at once and resets on any pulse.
- **Restart.** The first T1 at 0000 comes 3 rising edges after release
  in perfectz80, at every release phase. In RZ80 it comes after 1.
- **Registers.** perfectz80 keeps SP, BC, DE and HL across reset. An SP
  half-decremented by an interrupted PUSH or interrupt response
  (8FFF/8FFE/8FFD) is kept too. RZ80 sets them to 5555 (known
  difference 1).
- **Special reset.** RESET can cover only the rising edge that starts T2
  of an M1 cycle: low somewhere in T1, high again before T3. Then
  perfectz80 restarts at 0000 with I kept (3F), the "special reset"
  (`reset_short`, 36 of 240 cases). A pulse that covers only the rising
  edge into T3 or T4 gives a normal reset, and so does any pulse covering
  two or more rising edges. RZ80 has no special reset.

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
