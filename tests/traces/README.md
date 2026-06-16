# Bus-cycle trace programs

Tiny Z80 hex programs used by the **C ↔ RTL** trace-comparison harness
(`make compare`). Each program is loaded at address `0x0000`, run for **400
phases** (= 200 T-states = 100 modern-clock cycles), and the per-phase bus-cycle
trace produced by

  - the C model       (`scripts/tracegen.c`),
  - the iverilog sim  (`tests/iverilog/tb_z80.v`), and
  - the Verilator sim (`tests/verilator/sim_main.cpp`)

is compared by `scripts/compare_traces.py`. All three traces must be identical
phase-by-phase on every program for the gate to pass.

The trace columns (one line per modern-clock phase) are:

    t  phi  m  addr  data_o  data_i  mreq  iorq  rd  wr  m1  rfsh  halt  busack


## What each program exercises

| File                  | Size | What it covers                                                                                  |
|-----------------------|-----:|-------------------------------------------------------------------------------------------------|
| `prog1.hex`           |  30  | core base set — `LD SP,nn`, `LD r,n`, `LD r,r'`, `(HL)` read / write, ALU, `ADD HL,rp`, `RLCA`, `CPL`, `HALT` |
| `prog2.hex`           |  18  | branch / control — `LD SP`, `LD A,n`, `LD B,n`, `INC A`, `DJNZ`, `CALL`, `JR`, `RET`, `HALT`    |
| `prog3_cb.hex`        |  24  | `CB` prefix — rotates / shifts on registers and `(HL)`, `BIT n,r`, `RES n,r`, `SET n,r`         |
| `prog4_ed.hex`        |  56  | `ED` prefix — 16-bit `ADC` / `SBC HL,rp`, `NEG`, `LD I,A` / `LD A,I`, `IM 0/1/2`, `RETI` / `RETN`, `LD (nn),rp` |
| `prog5_ddfd.hex`      |  43  | `DD` / `FD` prefixes — `LD IX/IY,nn`, `LD A,(IX+d)`, `LD (IY+d),n`, `ADD A,IXH`, `INC (IX+d)`   |
| `prog6_block.hex`     |  33  | ED block ops — `LDI` / `LDIR`, `CPI` / `CPIR` with both BC-exhausted and Z-hit terminations    |
| `prog7_ddcb.hex`      |  25  | `DDCB` / `FDCB` — bit ops on `(IX+d)` including the undocumented copy-into-r[z] for `z ≠ 6`, plus `HALT` |
| `prog8_nmi.hex`       |  16  | NMI handling — an NMI is pulsed at phase 30 by both the iverilog tb and the C tracegen; the acknowledge M1 (5 T-states, suppressed decode) plus the implicit `CALL 0x0066` are visible in the trace |

The numbers in the "Size" column are byte counts; all programs fit in a single
ROM-image hex file with no `@addr` directives (they all start at `0x0000`).

Together the 8 programs cover every instruction-class category that contributes
to the bus-cycle protocol: M1 fetch, MRD operand, MWR write, IORQ R / W, refresh,
prefix chaining, displacement reads, RMW, block-op interleave, NMI ACK and HALT.

`prog8_nmi.hex` is special: it's a sea of `00` (NOP) so the CPU just runs NOPs
until the NMI fires at phase 30 — the interesting behaviour is the NMI-ack M1
(5 T-states, suppressed decode, refresh as usual) and the subsequent stack push
+ jump to the NMI vector at `0x0066`, both of which are visible in the
phase-by-phase trace.


## `pin_scenarios/` — pin-event driven scenarios

`make pin_scenarios` runs a second class of trace programs that exercise
the *input* pins (NMI, INT, WAIT, BUSREQ, RESET) on a deterministic phase
schedule. The schedule lives next to each `.hex` as a `.events` sidecar
parsed identically by `scripts/tracegen.c` and `scripts/perfectz80_runner.c`,
so the C model and the perfectz80 gate-level netlist see *exactly* the same
pin transitions at exactly the same phases.

Sidecar format — one event per line, `#` for comments:

    <phase>  <pin>  <0|1>

`<phase>` is the phase index (modern-clock half-tick) at which to drive
the pin, `<pin>` is one of `nmi`, `int`, `wait`, `busreq`, `reset`, and
`<value>` is the level to drive (`0` = asserted/low, `1` = released/high).

| File                          | What it exercises                                                                  |
|-------------------------------|------------------------------------------------------------------------------------|
| `prog9_inta_im1.hex`          | IM 1 INT acceptance — `EI` + NOP loop, INT pulsed at phase 50; expects an INTA M-cycle (`M1` + `IORQ` together, 7 T-states), then RST 38h to the IM1 vector, then `HALT`. |
| `prog10_halt_nmi.hex`         | `HALT` self-refetch loop entered at boot, exited by an NMI pulse at phase 35; expects the NMI-ack 5-T M1, PC push, jump to `0x0066`, then a terminating `HALT`. |
| `prog11_wait_mem.hex`         | WAIT-state insertion on a memory read — `LD A,(0x0100)` triggers an MRD whose T2.N sample lands inside a `wait_n` low window (phases 30..38); expects extra Tw states until WAIT releases. |

`make pin_scenarios` is **informational** today (the make target exits 0 even
on divergence) — every divergence between our model and perfectz80 surfaces
a real silicon-faithfulness audit item rather than a regression. See
[docs/audit-followups.md](../../docs/audit-followups.md) for the running list.


## How they were assembled

Each `.hex` file is hand-assembled — no toolchain dependency. Lines are
whitespace-separated hex bytes, optionally prefixed by `@addr` to set the load
address (none used here). Both the C tracegen and the Verilog testbenches use
the same simple loader.


## See also

  - [../../README.md](../../README.md) — top-level project overview.
  - [../../docs/timing.md](../../docs/timing.md) — the M-cycle / T-state / phase
    timing model these traces verify.
  - [../../docs/verification.md](../../docs/verification.md) — the verification
    layers; this directory implements layer 5.
  - [`tests/iverilog/tb_z80.v`](../iverilog/tb_z80.v) and
    [`tests/verilator/sim_main.cpp`](../verilator/sim_main.cpp) — the RTL
    testbenches that consume these hex files.
  - [`scripts/tracegen.c`](../../scripts/tracegen.c) — the C-side trace producer.
  - [`scripts/compare_traces.py`](../../scripts/compare_traces.py) — the 3-way
    diff driver invoked by `make compare`.
