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

  - **Sub-T-state pin timing fidelity** on the 7 / 12 still-informational
    pin_scenarios (HALT-pin assertion phasing, WAIT / BUSREQ M-cycle
    abort ordering, post-reset M-cycle sequence). Functional behaviour
    is verified via Rak + FUSE + `make halt2int`; the residual is gate-
    level timing fidelity. Tracked under items B2 / B3 in
    [docs/simplifications.md](simplifications.md).
  - **C1 — reset register init**. Our `0xFFFF` vs perfectz80's
    `0x5555` surfaces as informational diffs on `prog_rnd_02`,
    `prog_rnd_03`, `prog_rnd_04`, `prog19_nmi_in_int`. One-constant
    change; was deferred for the silicon-faithfulness sweep
    (`make z80test` was the priority).
  - **A-Z80 as second gate-level oracle** — **dropped.** With LibreLane
    providing an independent sky130-synthesised gate-level reference
    alongside perfectz80's Visual-Z80 port, the third oracle is no
    longer worth the integration cost.
  - **Full LibreLane PnR + STA** — out of scope; needs a tape-out
    target to justify the CI budget.

The branch is ready to merge into `main`.
