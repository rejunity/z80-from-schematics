# Timing model

All timing is expressed in **phases**. Each Z80 T-state = two phases:
`phi=0` (PHI_P, the clock-high half, entered on the Z80 clock's rising edge)
then `phi=1` (PHI_N, the clock-low half, entered on its falling edge).
"Set at T_n.P" means the pin takes that value from the rising edge of T_n;
"set at T_n.N" means from the falling edge of T_n. "Sampled at T_n.N" means the
value present during the clock-low half is what the chip acts on (the sample
edge is the rising edge that ends it); "sampled/latched at T_n.P" means the
value present during the clock-high half, captured on the falling edge. WAIT
is sampled at the **.N** of T2 (and of any inserted Tw). Source: Zilog UM0080
timing diagrams (precedence rule 1). In the RTL the whole T-state is evaluated
on one rising edge and the wrapper re-times pins/inputs to the two edges —
see `docs/clocking.md`.

Legend: `↓` = assert (drive low for active-low pins), `↑` = deassert.

## M1 — opcode fetch (4 T-states: T1 T2 T3 T4)

```
        T1.P   T1.N   T2.P   T2.N   T3.P   T3.N   T4.P   T4.N
addr    PC                          I:R(refresh)
m1_n    ↓                                  ↑(at T2? see note)
mreq_n         ↓             (rd data)     ↑      ↓(refresh) ↑
rd_n           ↓                    ↑
rfsh_n                              ↓                        ↑
data    (sampled at T2.N -> IR)
```

Phase-exact actions:
- **T1.P**: `pin_addr ← PC`; `pin_m1_n ← 0`; `pin_rfsh_n ← 1`.
- **T1.N**: `pin_mreq_n ← 0`; `pin_rd_n ← 0`.
- **T2.N**: sample `pin_wait_n`. If asserted (low), insert `Tw` (repeat a T2-like
  wait state) and re-sample at next .N. If not waited, latch `pin_data_in → IR`
  (the fetched opcode).
- **T3.P**: `pin_m1_n ← 1`; `pin_rd_n ← 1`; `pin_mreq_n ← 1`; `pin_addr ← {I,R}`;
  `pin_rfsh_n ← 0`. (Refresh address presented.)
- **T3.N**: `pin_mreq_n ← 0` (refresh memory request).
- **T4.P**: (decode continues; this is where decoded control begins driving datapath).
- **T4.N**: `pin_mreq_n ← 1`. `R` steps (low 7 bits) on the rising edge that
  ends T4, so the refresh address is I:R from before the step, as on silicon
  (an INTA steps it at the end of its second refresh T-state).
- After T4.N the next M-cycle of the instruction begins (or the next M1).

Note: M1 deasserts at T3.P in our model. Verified against the perfectz80 gate-level
netlist (`make perfectz80`): perfectz80's per-half-cycle `cpu_step()` reads `m1_n=0`
at T2.N and `m1_n=1` at T3.P, exactly matching us. Some references describe M1
"deasserting late in T2" — that's the continuous-time analog edge; at half-cycle
sample resolution (the granularity at which both we and the gate-level reference
operate) M1 is high starting T3.P. So the model is silicon-faithful at the
resolution at which it models.

The opcode prefix bytes (CB/ED/DD/FD) are themselves fetched with an M1 cycle; the DD/FD
chains keep issuing M1 fetches until a non-prefix opcode arrives. DDCB/FDCB fetch the
displacement and the CB opcode as **operand reads** (not M1) after the two prefix M1s.

## Memory read (3 T-states: T1 T2 T3)

- **T1.P**: `pin_addr ← <effective address>`.
- **T1.N**: `pin_mreq_n ← 0`; `pin_rd_n ← 0`.
- **T2.N**: sample `pin_wait_n`; insert `Tw` if asserted.
- **T3.P**: latch `pin_data_in` (read window: data must be valid by here).
- **T3.N**: `pin_rd_n ← 1`; `pin_mreq_n ← 1`. (Deasserted on the falling-edge
  transition — matches the gate-level / perfectz80 trace and decouples the
  latch phase from the deassert phase.)

If the sequencer asks for extra T-states beyond T3 (e.g. CB `(HL)` reads use
`m_len=4` for the internal RMW compute), the bus is silent for those — MREQ/RD
stay high. The extra T-states are compute padding after the read completes,
not part of the read window.

## Memory write (3 T-states: T1 T2 T3)

- **T1.P**: `pin_addr ← <effective address>`.
- **T1.N**: `pin_mreq_n ← 0`; drive `pin_data_out`, `pin_data_drive ← 1`.
- **T2.N**: sample `pin_wait_n`; insert `Tw` if asserted. `pin_wr_n ← 0`.
- **T3.N**: `pin_wr_n ← 1`; `pin_mreq_n ← 1`. The data stays driven.
- **next T1.P**: `pin_data_drive ← 0` (the data bus floats).

The data outlives `/WR` by the clock-low half of T3 (Zilog PS0178
Figure 6: TdWRr(D) data stable from WR rise, TdCr(Dz) float from the next
clock rise); perfectz80's data bus floats at the next T1.P too.

Inserted Tw states hold `/WR` continuously low, together with `/MREQ` and
stable address/data. Internally Tw reuses `t_state=2`; its rising half must
not be decoded as the initial T2.P before `/WR` first asserts. The RTL keeps
the registered write strobe across the stalled `.N → .P` transition; the C
phase engine applies the same rule. `/WR` is released only at final T3.N.

In this implementation, the first `/WR` assertion and the WAIT sample both
occur at T2.N. A peripheral that starts asserting WAIT only after seeing
`/WR` low is too late for that write, regardless of how long it then holds
WAIT. WAIT must already meet the falling-edge sample deadline; preasserting
WAIT before the write is one way to do that. A late WAIT can instead stall
the following transaction. The write-strobe hold does not change this rule.

## I/O read (4 T-states: T1 T2 Tw T3 — one automatic wait state)

- **T1.P**: `pin_addr ← port` (`A` high byte / `B` / `C` depending on instruction).
- **T2.P**: `pin_iorq_n ← 0`; `pin_rd_n ← 0`.
- **Tw.N**: sample `pin_wait_n` (the automatic Tw is always present; further Tw if
  WAIT held).
- **T3.P**: latch `pin_data_in`.
- **T3.N**: `pin_iorq_n ← 1`; `pin_rd_n ← 1`.

(In the C model and RTL the IORD cycle is `m_len=4`, counting Tw as T3 and the
real T3 as T4. Substitute T4 for T3 above when reading the code; the silicon
contract is the same.)

## I/O write (4 T-states: T1 T2 Tw T3)

- **T1.P**: `pin_addr ← port`.
- **T1.N**: drive `pin_data_out`, `pin_data_drive ← 1`.
- **T2.P**: `pin_iorq_n ← 0`; `pin_wr_n ← 0` -- together, unlike a memory
  write's T2.N (PS0178 Figure 7, TdCr(WRf); perfectz80).
- **Tw.N**: sample `pin_wait_n`.
- **T3.N**: `pin_iorq_n ← 1`; `pin_wr_n ← 1`. The data stays driven.
- **next T1.P**: `pin_data_drive ← 0`, as in a memory write.

## Interrupt acknowledge (INTA)

Maskable INT accepted at the end of the current instruction if `IFF1=1` and INT is
sampled low at the **rising edge of the last T-state**. The acknowledge M-cycle is an
M1-like cycle with **two automatic wait states** ([Zilog UM0080, Figure 9](https://www.zilog.com/docs/z80/um0080.pdf)):

- T1.P: assert `/M1`, with PC on the address bus.
- First automatic Tw.N: assert `/IORQ`, keeping `/M1` low. `/RD` and `/WR`
  stay high; the device recognizes the `/M1` + `/IORQ` overlap.
- Second automatic Tw.N: sample `/WAIT`. Repeat this Tw while the saved sample
  is low, retaining the acknowledge address and strobes.
- On leaving the final Tw for T3.P: consume the vector/opcode and release both
  `/M1` and `/IORQ`. Refresh occupies T3/T4, with `/MREQ` low T3.N through T4.P.
- The implementation counts T1, T2, Tw, Tw, T3, T4 as states 1–6. Thus WAIT
  capture is state 4.N, data is consumed before advancing to state 5.P, and
  refresh uses states 5/6. IM1/IM2 append one internal state (7) before pushing PC.
- IM0 (perfectz80): the device's byte is the opcode, decoded without a
  fetch, and PC doesn't move until that instruction ends. The 6T acknowledge
  replaces the opcode fetch; the instruction's own extra M1 T-states follow
  (RST and PUSH one, INC rr two). Its operands are memory reads at the
  interrupted PC -- a CALL's two operand reads show the same address, so the
  device, not memory, has to answer them -- and the opcode after a prefix
  (CB, DD, ED, FD) comes in another 6T acknowledge cycle; a DD CB
  instruction reads d and the opcode as operands. So RST and CALL push the
  interrupted PC, JR jumps relative to it, LD A,n carries on at it, and a
  HALT handed over halts there, the HALT pin falling in the acknowledge's
  last T-state.
- IM1 ignores the byte and enters `0038h`. IM2 reads the handler pointer at
  `{I, vector}` and `{I, vector}+1` with all eight bits of the vector: an odd
  vector reads it at an odd address (perfectz80).

`make bus_controls` exercises IM0 RST/CALL/JP/NOP, IM1, and IM2 with a nonzero
vector, with and without WAIT and on HALT exit; the device drives the
acknowledge byte and, for IM0 CALL / JP, the operand reads. `test_int_ack.c`
checks the same cases against the C model. The event sweep's `im2`,
`im2_combo`, `im0` and `im0_combo` groups compare IM2 with vectors 40, 41 and
FF, and twenty instructions handed over in IM0, with perfectz80, pins and
address in every half-cycle (docs/event-sweep.md).

NMI: similar to M1 but no data fetch used for vector; pushes PC and jumps to `0066h`;
copies `IFF1→IFF2`, clears `IFF1`. The NMI edge latch is held clear for the whole
response, from its M1's T1 to the last T-state of the second push: an NMI falling
edge there is lost, even when NMI then stays low (perfectz80). An edge from the
handler's first T-state on starts a nested NMI. An NMI edge during an INT response
is kept and taken right after it.

## HALT

`HALT` is the only Z80 instruction whose M1 cycle is allowed to be
"aborted" by an external event. Silicon-faithful convention per Brewer
2014 ("Z80 Special Reset") and verified by Mark Woodmass's HALT2INT v3
(2021):

  - HALT's M1 commits with PC ALREADY past the HALT byte (`PC =
    halt_addr + 1`). No decrement.
  - The HALT-state loop runs internal NOP M-cycles of 4 T-states each,
    each re-fetching at PC (= post-HALT-byte). PC is NOT incremented
    further during the loop.
  - `pin_halt_n` asserts low while halted; refresh continues every NOP
    M1. It falls in the low half of T4 of the HALT M1 and rises in the
    low half of T4 of the NOP M1 whose end accepts an INT or NMI; when
    the interrupt is already pending at the end of HALT, it never falls
    (perfectz80). Only a HALT that runs lowers it: the fetch of an NMI
    response discards its opcode, so an NMI taken just before a HALT
    byte (say at the top of an interrupt handler) leaves the pin high
    (`prog19_nmi_in_int`).
  - INT / NMI sampled at the rising edge of the last T-state of each
    NOP M-cycle. When sampled active (and IFF1=1 for INT), the next
    M-cycle is the ack (IM1/IM2 INTA = 7 T, NMI ack = 5 T). PC stays put —
    the ack's saved return address is `halt_addr + 1`, which is what
    RETN must restore.

`make halt2int` runs a focused CPU-only probe that sweeps INT-assert
timing across the HALT NOP M-cycle and verifies the INT-to-INTA delay
stays in the silicon-faithful 3..8 T-state range. FUSE's test `76`
(`PC=halt_addr` after HALT) reflects the pre-Brewer convention and
lives in `tests/fuse/known-fuse-wrong.txt`; our model and redcode/Z80
both side with silicon.

## BUSREQ / BUSACK

As the NMOS die does it (perfectz80; PS0178 Figure 10):

- BUSREQ is sampled on the rising edge that starts each T-state (`z80_top`
  `busreq_rise`), like INT.
- **Grant**: when it was low at the start of an M-cycle's last T-state, the bus
  goes to the DMA master after that M-cycle -- at any M-cycle boundary, inside
  an instruction too. The next M-cycle (the next one of the instruction, the
  next instruction, or an interrupt response) is set up and held. BUSACK falls
  on the rising edge that ends the M-cycle; the strobes idle, the data bus is
  not driven, and the address keeps its last value (the chip's pads float it on
  BUSACK).
- **Release**: in each granted T-state, BUSREQ high at its rising edge makes it
  the last: BUSACK rises on its falling edge and the held M-cycle starts T1 on
  the next rising edge. The shortest grant is one T-state.
- **M-cycle boundaries** are the die's, internal cycles included: ADD HL,rr and
  ADC / SBC HL,rr run internal M-cycles of 4 and 3 T-states; the 1- and 2-T
  internal states after an M1 or a read (an M1's T5 / T6, CALL's 4-T operand
  read) belong to that M-cycle and are no boundary.
- INT and NMI are not sampled while the bus is granted; at an instruction
  boundary the interrupt decision is taken first and the response waits for
  the release.

No internal tri-states; the release is modeled with output-enable lines on the
pin mux. The event sweep's `busreq` group pins all of this.

## RESET

The NMOS die treats RESET as a sampled input, not as an asynchronous reset.
This is how perfectz80 behaves, traced through its netlist (perfectz80 node
numbers in parentheses); the C model and the RTL follow it, and the event
sweep's 544 RESET cases match.

- **Sample.** A master-slave flip-flop (132 / 136) takes the pin on every
  rising edge. A pulse that covers no rising edge is ignored. (`res_s`,
  `z80_top.res_rise`.)
- **Full reset (RES2; 95 / 197).** It takes the sample on the falling edge in
  the middle of each T-state, except in the low half of an M1's T2 (INTA's
  too), where it holds.
- **Stop.** While RES2 was set on the previous falling edge, no T-state
  follows the current one: the CPU finishes the T-state in hand and the one
  after it, then idles with every strobe high. A cycle cut short releases its
  strobes as follows:
  - cut after T1: MREQ and RD fall in T1.N as in an opcode fetch, even in a
    write, I/O or long internal cycle; MREQ rises on the next rising edge, RD
    on the falling edge after it, or RD stays low into a restart that follows
    at once;
  - cut after T2: MREQ and RD release in T2.N, WR never falls, and an I/O
    cycle asserts nothing at all;
  - cut after an I/O cycle's Tw: IORQ and RD / WR release in Tw.N;
  - the 1- and 2-T internal cycles are extensions of an M1 or a read on the
    die (ring states T5 / T6) and drive nothing; when RES2 rises on the T1.N
    of a longer internal cycle, MREQ falls there and holds through T2.P.
- **Clears.** PC, I, R, IFF1 / IFF2, IM and the HALT state; the HALT pin
  rises on the falling edge where RES2 or the special-reset latch sets. Every
  other register (AF, BC, DE, HL, IX, IY, SP and the alternates), WZ and the
  exchange flip-flops are kept. The SP-1 of a push's first byte, which the die
  does on the falling edge that ends the cycle before the write, is lost when
  the reset stops the ring there.
- **Restart.** T1 of an M1 at 0000 follows once RES2 was clear on the
  previous falling edge: the third rising edge after RESET is released.
- **Special reset (157 / 172).** In the low half of an M1's T2 a second latch
  takes the sample instead of RES2. A pulse that covers that rising edge only
  (low in T1, high again before T3) lets the instruction finish; the next
  opcode fetch then runs as a NOP that leaves PC at 0000, and I, R, the IFFs,
  IM and every register are kept (Brewer 2014). RES2 clears the latch, so a
  pulse covering two or more rising edges is a normal reset.
- **Power-on** is `z80_top.por_n`, not a Z80 pin: every flop to its power-up
  value, the register file to 5555 as perfectz80 comes up, and the core in
  the reset idle until RESET is seen high.

Not modelled: the die finishes some results in the next M1's T1-T3 (A and F
after INC A, A after LD A,(HL), DE after POP DE, the EX DE,HL swap), which a
RESET that stops that M1 early loses and RZ80 keeps; RZ80 runs some long
internal M-cycles as one where the die has two (ADD HL,rr's 4 + 3 T), so a cut
inside them is approximate; RESET during WAIT or a bus grant is untested
(docs/known-differences.md).

## Address bus in internal T-states

Internal T-states drive no new address: the bus keeps the last one -- the
refresh address after an M1 (the Spectrum's contention tables' "ir:1"), the
operand address after a read ("hl:1", "pc+1:1"). The same holds through an
M1's T5 / T6, IM1 / IM2's seventh INTA T-state, the RESET idle and a bus
grant (perfectz80, every half-cycle). The address latch (`m_addr`) still
carries the IDU / EA work of those T-states; the pins take the held value.

## WAIT summary

WAIT is sampled at `.N` of T2 in memory M-cycles, at `.N` of the automatic Tw in I/O
cycles, and at `.N` of T2 (and inserted Tw) in M1. While WAIT is low, the M-cycle
holds its current bus state and re-samples each subsequent `.N` until WAIT releases.

`make bus_controls` tests the sampling edge and continuous memory/IO write
strobes through 20 inserted wait states on the single Z80-rate clock.
`tests/common/test_wait_write.c` covers the same write-strobe behavior in C.
