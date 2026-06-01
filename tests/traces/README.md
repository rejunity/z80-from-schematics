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
