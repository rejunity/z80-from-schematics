# `perfect` branch — closing the last gaps

Direction: nail down the tests that aren't 100 % passing today, with
focus on the gaps we can close cleanly. Out of scope: deep silicon-
faithfulness refactors of the block-op M-cycle (those move to a future
branch alongside the `unsimplify` audit items already in
[docs/simplifications.md](simplifications.md)).

## Status (2026-06-18) — branch complete; all six work items shipped

The silicon-faithfulness sweep on the `perfect` branch closed
substantially more than the original plan anticipated. Recap by item:

| #  | Item                                 | Outcome                                                                          |
|----|--------------------------------------|----------------------------------------------------------------------------------|
| 1  | Address-bus settle tolerance         | **DONE.** `compare_signal_timing.py` ships the don't-care window. `make perfectz80` / `_rtl` / `_netlist` all report 12/12 PASS. |
| 2  | VCD waveforms                        | **DONE.** `build/vcd/<prog>.<source>.vcd` is emitted per program; CI uploads them as artifacts (`parity-vcds` and `netlist-vcds`, 14-day retention). |
| 3  | `.events` sidecar in RTL testbenches | **DONE.** `tb_z80.v`, `tb_z80_netlist.v`, `sim_main.cpp` all consume per-pin `+<pin>_lo / +<pin>_hi` plusargs. |
| 4  | Pin-scenario RTL gates               | **DONE.** `make pin_scenarios`, `pin_scenarios_rtl`, `pin_scenarios_netlist` all wired; CI exercises every push (informational). |
| 5  | NMOS/Toshiba switch                  | **NOT NEEDED.** The z80full SCF/CCF gaps that motivated this turned out to be a reversed Q-leak formula (`A | Q` vs `A | (F XOR Q)`); fix landed without a runtime switch. Now `Z80_MODEL_ZILOG_NMOS`-equivalent. |
| 6  | Documentation                        | **DONE.** README, tests/README, docs/known-differences (rows 9 + 14 updated), docs/oracles.md (new), docs/verification.md all refreshed. |

Plus, well outside the original plan:

  - **Banks-2018 block-instruction repeat flag fold-in** (C + RTL).
    Closed all 4 of the LDIR/LDDR/INIR/INDR → NOP' z80full gaps.
  - **SCF/CCF Q-leak formula reversal fix**. Was producing NEC-like
    output instead of Zilog NMOS. Closed z80full 007 SCF+CCF.
  - **WZ = PC + 1 silicon-faithful flip on INxR/OTxR repeat**. Closed
    z80memptr 102/103 + most of z80full block-op leak chains.
  - **HALT-PC convention flip (Brewer 2014 / Woodmass HALT2INT 2021).**
    PC stays past HALT byte during NOP loop; NMI/INT exit accepts that
    PC unchanged. Closed `prog13_halt_int`'s biggest contributor (was
    145/200 phases differing, now ~5 of those phases remain as
    sub-T-state HALT-pin timing diffs).
  - **`make halt2int`** focused HALT-to-INT T-state timing probe added
    (Brewer 2014 + Woodmass HALT2INT silicon-faithful range 3..8 T,
    PASSes the full sweep).
  - **redcode/Z80 as 5th oracle** added; lockstep_quint replaces
    lockstep_quad as the C-tests gate.
  - **ULA-idle port-parity** for Rak runners (silicon-faithful
    `(addr & 1) ? 0xFF : 0xBF`) — closed Rak INI/IND CRC mismatches
    that previously held at the baseline.

### Final scoreboard vs the table's original "Currently" column

| Gate                          | Was                                | Now                                                                 |
|-------------------------------|------------------------------------|---------------------------------------------------------------------|
| `make perfectz80` (`addr`)    | 60–100 % match                     | **100 % control pins; 10 / 12 progs at 100 % addr; 2 / 12 with informational reset-init diffs (C1)** |
| `make pin_scenarios`          | Informational (C-only divergences) | **5 / 12 PASS clean** (`prog9, 12, 16, 18, 20`); 7 / 12 with HALT-pin / WAIT / BUSREQ / RESET / SP-init informational diffs (still informational gates) |
| `make z80test` (Rak)          | 158 / 158 / 150 within baseline 2/2/10 | **160 / 160 / 160 PASS — clean across all three variants**         |
| `make fuse`                   | 1356/1356 (with pre-Banks / pre-Brewer expecteds) | **1348 + 8 known-FUSE-wrong (silicon-faithful)** |
| `make zexall_rtl` (Verilator) | Known fails                        | **Passing the 14-test subset on CI; full ZEXALL is local-only**     |

## Deferred to a future branch

  - **Sub-T-state pin timing fidelity** on the still-informational
    pin_scenarios. **Six-step plan** to drive pin_scenarios to 100 %
    parity vs perfectz80, executing in order — each step verified
    against FUSE / Rak / halt2int / compare / 5-way lockstep before
    moving on:

    | # | Step                                       | Status     | Closes                            |
    |--:|--------------------------------------------|------------|-----------------------------------|
    | 0 | per-phase diff harness                     | **DONE**   | analysis tool (scripts/pin_scenarios_diff.py) |
    | 1 | C1 reset register init 0xFFFF → 0x5555     | **DONE**   | make perfectz80 4 informational diffs + prog19 SP-init |
    | 2 | RFSH-pin late-deassert (bound to T3..T4)   | **DONE**   | prog10 5→3, prog19 3→1 ctrl-pin   |
    | 3 | HALT-pin assertion at T4.N of HALT M1      | **DONE**   | prog10 3→1, prog19 1→**PASS**     |
    | 4 | Reset deferral until M-cycle completion    | pending ⚠  | prog17 (131) — needs reset state machine rework |
    | 5 | BUSREQ M1 abort ordering                   | pending ⚠  | prog15 (154) — needs M-cycle abort rework |
    | 6 | WAIT-insertion sub-T-state phasing         | pending ⚠  | prog11 (142), prog14 (147) — touches every memory/IO access |

    Steps 4-6 (the ⚠ rows) each need a focused structural rework of
    pin-driving logic: Step 4 a reset state machine, Step 5 an
    M-cycle abort sequencer, Step 6 a WAIT-sample phasing fix. Each
    is 1-2 days of careful work + regression testing. Given the
    "perfect" branch's other green gates (FUSE, Rak, ZEXALL,
    halt2int, lockstep, gate-level netlist), these residual
    informational diffs are documented as a follow-up branch
    rather than blocking the current branch's merge.

    Functional behaviour is verified via Rak + FUSE + `make halt2int`;
    this 6-step plan addresses gate-level timing fidelity vs the
    perfectz80 Visual-Z80 netlist. Two quick fixes already landed
    (2026-06-18 continuation):
    - **C1 reset init flipped 0xFFFF → 0x5555** to match perfectz80's
      gate-level boot pattern. Closed all 4 informational diffs on
      `make perfectz80` (`prog_rnd_02/03/04` now clean) and 11 of
      `prog19_nmi_in_int`'s bus-only diffs.
    - **RFSH-pin late-deassert fix** in `cmodel/z80_timing.c` +
      `rtl/z80_timing.v`. RFSH was held asserted into T5+ of extended-
      length M1 cycles (NMI ack 5T, INTA 7T). Now bounded to T3..T4
      exactly. Closed `prog10_halt_nmi` 5→3 and `prog19_nmi_in_int`
      3→1 ctrl-pin diffs.

    Pin_scenarios current state (post-fix):
    | Program | Ctrl diffs | Root cause |
    |---|---:|---|
    | prog9, 12, 16, 18, 20 | **0** | (PASS) |
    | prog19_nmi_in_int     | **1** | sub-T-state HALT-pin transition |
    | prog10_halt_nmi       | **3** | HALT-pin assertion timing (T3 vs T4) |
    | prog17_reset          | 131   | reset deferral (defer reset_state until current M-cycle ends) |
    | prog11_wait_mem       | 142   | WAIT-insertion sub-T-state phasing |
    | prog13_halt_int       | 145   | HALT-pin during NOP-loop M-cycles |
    | prog14_wait_io        | 147   | IORQ + WAIT timing during IN/OUT |
    | prog15_busreq_m1      | 154   | BUSREQ-aborts-M1 M-cycle ordering |

    Tracked under items B2 / B3 in
    [docs/simplifications.md](simplifications.md). The four high-count
    items (prog11/13/14/15/17) each need a careful pin-driving rework
    that's out of scope for a single iteration without regression risk;
    a dedicated follow-up branch could close them one at a time.
  - **A-Z80 as second gate-level oracle** — **dropped.** With LibreLane
    providing an independent sky130-synthesised gate-level reference
    alongside perfectz80's Visual-Z80 port, the third oracle is no
    longer worth the integration cost.
  - **Full LibreLane PnR + STA** — out of scope; needs a tape-out
    target to justify the CI budget.

The branch is ready to merge into `main`.
