# Timing model

All timing is expressed in **phases**. Each Z80 T-state = two phases:
`phi=0` (PHI_P, rising-edge region) then `phi=1` (PHI_N, falling-edge region).
"Set at T_n.P" means the action takes effect on the `posedge clk` entering phi=0 of
T_n; "set at T_n.N" means entering phi=1 of T_n. WAIT is sampled at the **.N** of T2
(and of any inserted Tw). Source: Zilog UM0080 timing diagrams (precedence rule 1).

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
  (the fetched opcode) and run `R` increment.
- **T3.P**: `pin_m1_n ← 1`; `pin_rd_n ← 1`; `pin_mreq_n ← 1`; `pin_addr ← {I,R}`;
  `pin_rfsh_n ← 0`. (Refresh address presented.)
- **T3.N**: `pin_mreq_n ← 0` (refresh memory request).
- **T4.P**: (decode continues; this is where decoded control begins driving datapath).
- **T4.N**: `pin_mreq_n ← 1`.
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
- **T3.N**: `pin_wr_n ← 1`; `pin_mreq_n ← 1`; `pin_data_drive ← 0`.

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
- **T2.P**: `pin_iorq_n ← 0`; drive `pin_data_out`, `pin_data_drive ← 1`.
- **T2.N**: `pin_wr_n ← 0`.
- **Tw.N**: sample `pin_wait_n`.
- **T3.N**: `pin_iorq_n ← 1`; `pin_wr_n ← 1`; `pin_data_drive ← 0`.

## Interrupt acknowledge (INTA)

Maskable INT accepted at the end of the current instruction if `IFF1=1` and INT is
sampled low at the **rising edge of the last T-state**. The acknowledge M-cycle is an
M1-like cycle with **two automatic wait states** and a distinctive strobe:
- `pin_m1_n ← 0` and `pin_iorq_n ← 0` together (instead of mreq) to fetch the vector.
- T1 T2 Tw Tw (T3 T4 for the refresh tail).
- IM0: executes the bus-supplied opcode (usually RST). IM1: `RST 38h`. IM2: forms
  address `{I, vector}` and reads the handler pointer.

NMI: similar to M1 but no data fetch used for vector; pushes PC and jumps to `0066h`;
copies `IFF1→IFF2`, clears `IFF1`.

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
    M1.
  - INT / NMI sampled at the rising edge of the last T-state of each
    NOP M-cycle. When sampled active (and IFF1=1 for INT), the next
    M-cycle is the ack (INTA = 7 T, NMI ack = 5 T). PC stays put —
    the ack's saved return address is `halt_addr + 1`, which is what
    RETN must restore.

`make halt2int` runs a focused CPU-only probe that sweeps INT-assert
timing across the HALT NOP M-cycle and verifies the INT-to-INTA delay
stays in the silicon-faithful 3..8 T-state range. FUSE's test `76`
(`PC=halt_addr` after HALT) reflects the pre-Brewer convention and
lives in `tests/fuse/known-fuse-wrong.txt`; our model and redcode/Z80
both side with silicon.

## BUSREQ / BUSACK

`pin_busreq_n` sampled at the rising edge of the last phase of any M-cycle. When
granted, after the current M-cycle the core asserts `pin_busack_n ← 0` and tri-states
(in RTL: stops driving — `pin_data_drive=0`, address/control released to high-Z model
via a `bus_release` mux/enable) until BUSREQ deasserts. No internal tri-states; the
release is modeled with output-enable lines on the pin mux.

## WAIT summary

WAIT is sampled at `.N` of T2 in memory M-cycles, at `.N` of the automatic Tw in I/O
cycles, and at `.N` of T2 (and inserted Tw) in M1. While WAIT is low, the M-cycle
holds its current bus state and re-samples each subsequent `.N` until WAIT releases.
