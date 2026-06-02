# Z80 silicon-faithful microarchitecture — reference

Inventory of internal-datapath control signals, PLA bit numbering, internal
bus segments, and the per-(M, T) dispatch grid. Source of truth: Baltazar
Studios' Z80 Explorer (die-level reverse-engineering), specifically the
auto-generated `exec_matrix.vh` control matrix:

  https://baltazarstudios.com/webshare/exec_matrix.vh.html

This document collects the signal vocabulary into a single reference that
the c-like-verilog branch's silicon-faithful refactor will use verbatim
(same `ctl_*` names so cross-referencing exec_matrix.vh stays trivial).

## Block partition (real silicon)

| Block | Function | Real Z80 detail |
|---|---|---|
| ALU                | 4-bit unit, applied twice for 8-bit ops. ADD/ADC/SUB/SBC/AND/OR/XOR/CP, INC/DEC, rotates/shifts, BIT/RES/SET, DAA. | Located lower-left on die. Op1/Op2 latches feed it; output goes to result latch, then onto db2. |
| Shifter            | Rotates/shifts; output via `ctl_alu_shift_oe`. | Adjacent to ALU. Some shifts share ALU paths, others use the shifter directly. |
| Flags subsystem    | Tracks SF/ZF/YF/HF/XF/PF/NF/CF plus a shadow set (HF2/CF2 used for the half-carry and carry of the *previous* nibble pass, for DAA and 16-bit ADD/ADC/SBC). | The `ctl_flags_*` group below has separate WE for each flag bit. |
| IDU (incr/decr)    | 16-bit incrementer/decrementer on the address path. INC/DEC of rp (and PC/SP), PC+disp for JR. No flags. | Drives `sb` via `ctl_bus_inc_oe`. Has its own carry input `ctl_inc_cy`. `ctl_inc_dec` selects direction. |
| Register file (GP) | The 13 16-bit GP pairs: BC, DE, HL, AF, BC', DE', HL', AF', IX, IY (+ I, R). | Byte-addressable via `ctl_reg_gp_sel[3:0]` + `ctl_reg_gp_hilo[1:0]`. Reads to db2, writes from db2. |
| Register file (sys)| PC, SP, WZ. | Reads to sb (system bus) via `ctl_reg_sel_pc`/`ctl_reg_sel_wz`/`ctl_reg_use_sp`, writes via `ctl_reg_sys_we_hi/_lo`. |
| Instruction reg    | IR — latches the opcode at M1.T1/T2. | `ctl_ir_we` writes IR from db. |
| Address latch (AL) | Drives the external A pins. | Loaded from sb via `ctl_al_we`; routed to A pins via `ctl_apin_mux`/`_mux2`. |
| External bus pads  | DB / AB pin buffers + WAIT/BUSREQ handling. | `ctl_bus_db_oe` (DB pads to db1), `ctl_bus_db_we` (db1 to DB pads). |
| PLA + Sequencer    | PLA decodes (IR, prefix). Sequencer ("random control logic") generates per-(M, T) control signals. | The "exec matrix" — what this whole doc documents. |
| Interrupt / state  | IFF1, IFF2, IM, NMI / INT acceptance, HALT, IX/IY prefix latches, ED/CB table latches. | `ctl_iff*`, `ctl_im_we`, `ctl_state_*` etc. |

## Internal bus topology

```
   external      ┌──────────┐                   ┌──────────┐
   DB pads ◄────►│ DB pad   ├──db1──┤sw_1├──db2─┤ Register │
                 │ buffer   │                   │   file   │
                 └──────────┘                   │   (GP)   │
                              ctl_bus_db_oe     └──────────┘
                              ctl_bus_db_we                ▲
                                                          db2 ◄── ALU result, shifter, flags
                                                           │
                                                       ┌───┴──┐
                                                  sw_2 │      │ sw_4
                                                       ▼      ▼
                                                ┌──────────┐ sb (16-bit)
                                                │   ALU    │  │
                                                │  + op1   │  ▼
                                                │  + op2   │  ┌──────────┐
                                                └──────────┘  │  IDU     │
                                                              │ (inc/dec)│
                                                              └──────────┘
                                                                    │
                                                              ┌─────┴────┐
                                                              │ Register │
                                                              │  (sys:   │
                                                              │ PC SP WZ)│
                                                              └──────────┘
                                                                    │
                                                              ┌─────▼────┐
                                                              │ Address  │
                                                              │  latch   │
                                                              └──────────┘
                                                                    │
                                                              external AB pads
```

  - `db1` (8 bits) — between DB pads and the internal datapath.
  - `db2` (8 bits) — ALU side and register-file write side.
  - `sb` (16 bits) — system / register bus. Carries 16-bit values between
    the register file (read side), IDU, and the address latch.

Switches (each a one-hot mux on the destination side, NOT a literal
tri-state — synthesizable for FPGA and ASIC):
  - `ctl_sw_1u`, `ctl_sw_1d` — direction-gate db1 ↔ db2 (up/down).
  - `ctl_sw_2u`, `ctl_sw_2d` — direction-gate db2 ↔ ALU side.
  - `ctl_sw_4u`, `ctl_sw_4d` — direction-gate db2 ↔ sb (cross between
    8-bit data path and 16-bit register/address path).
  - `ctl_sw_mask543_en` — special mask used in some address paths.

Bus contention is forbidden by microprogram contract: at any
`(M-cycle, T-state)`, at most one `*_oe` per bus segment is asserted.
We add a synth-time assertion in simulation to catch microcode bugs.

## `ctl_*` signal inventory (verbatim from exec_matrix.vh)

### ALU

| Signal | Function |
|---|---|
| `ctl_alu_op1_oe`        | Drive op1 latch onto ALU input. |
| `ctl_alu_op2_oe`        | Drive op2 latch onto ALU input. |
| `ctl_alu_op1_sel_bus`   | Op1 input: from internal bus (else stays). |
| `ctl_alu_op1_sel_low`   | Op1 input: low nibble (for two-pass 8-bit ops). |
| `ctl_alu_op1_sel_zero`  | Op1 input: zero (for NEG, =0-A). |
| `ctl_alu_op2_sel_bus`   | Op2 input: from internal bus. |
| `ctl_alu_op2_sel_lq`    | Op2 input: latched-Q (Q register, for DAA). |
| `ctl_alu_op2_sel_zero`  | Op2 input: zero. |
| `ctl_alu_sel_op2_high`  | Op2 high nibble (second pass of byte op). |
| `ctl_alu_sel_op2_neg`   | Op2 negated (subtract). |
| `ctl_alu_op_low`        | Operate on low nibble (first pass). |
| `ctl_alu_oe`            | ALU result onto db2. |
| `ctl_alu_res_oe`        | Result latch enable. |
| `ctl_alu_shift_oe`      | Shifter result onto db2 (CB rotates). |
| `ctl_alu_bs_oe`         | Bit-shift specific output enable. |
| `ctl_alu_core_R`        | ALU core signal R (rotate/shift family). |
| `ctl_alu_core_hf`       | ALU core signal hf (half-carry). |
| `ctl_alu_zero_16bit`    | Zero-extend the ALU result for 16-bit consumers. |
| `ctl_daa_oe`            | DAA correction output to db2. |
| `ctl_66_oe`             | Force-drive 0x66 onto bus (NMI vector low byte). |

### Flags (S, Z, Y, H, X, P, N, C — plus shadow HF2/CF2)

| Signal | Function |
|---|---|
| `ctl_flags_alu`         | Load flags from ALU side. |
| `ctl_flags_bus`         | Load flags from internal data bus (RETN, etc.). |
| `ctl_flags_oe`          | Drive F register onto db2. |
| `ctl_flags_sz_we`       | Update SF, ZF. |
| `ctl_flags_xy_we`       | Update XF, YF. |
| `ctl_flags_pf_we`       | Update PF. |
| `ctl_flags_hf_we`       | Update HF. |
| `ctl_flags_cf_we`       | Update CF. |
| `ctl_flags_nf_we`       | Update NF. |
| `ctl_flags_nf_set`      | Force NF=1 (subtract path). |
| `ctl_flags_nf_clr`      | Force NF=0 (add path). |
| `ctl_flags_cf_set`      | Force CF=1 (SCF). |
| `ctl_flags_cf_cpl`      | Complement CF (CCF). |
| `ctl_flags_hf_cpl`      | Complement HF (CCF uses old CF→HF). |
| `ctl_flags_cf2_we`      | Update shadow CF2 (for next-nibble carry). |
| `ctl_flags_hf2_we`      | Update shadow HF2 (for next-nibble half-carry). |
| `ctl_flags_use_cf2`     | Read CF2 instead of CF (second-nibble pass). |
| `ctl_flags_cf2_sel_daa` | Source CF2 from DAA correction. |
| `ctl_flags_cf2_sel_shift` | Source CF2 from shifter output. |
| `ctl_pf_sel`            | PF source select (parity vs overflow). |

### Register file

| Signal | Function |
|---|---|
| `ctl_reg_gp_sel[3:0]`   | Select GP register pair index. |
| `ctl_reg_gp_hilo[1:0]`  | Select byte within pair (hi/lo or both). |
| `ctl_reg_gp_we`         | Write enable to GP register file. |
| `ctl_reg_in_hi`         | Write source: db2 high byte. |
| `ctl_reg_in_lo`         | Write source: db2 low byte. |
| `ctl_reg_out_hi`        | Read to db2 / db high byte. |
| `ctl_reg_out_lo`        | Read to db2 / db low byte. |
| `ctl_reg_sel_pc`        | Use PC as the system register source. |
| `ctl_reg_sel_wz`        | Use WZ as the system register source. |
| `ctl_reg_sel_ir`        | Use IR as source (for I/R register access). |
| `ctl_reg_sys_we`        | System-register write enable. |
| `ctl_reg_sys_we_hi`     | System register: write high byte only. |
| `ctl_reg_sys_we_lo`     | System register: write low byte only. |
| `ctl_reg_sys_hilo[1:0]` | System register byte select. |
| `ctl_reg_use_sp`        | Use SP as the system register source. |
| `ctl_reg_not_pc`        | Inhibit PC update this cycle. |
| `ctl_reg_ex_af`         | Trigger EX AF,AF' (swap AF and AF2). |
| `ctl_reg_ex_de_hl`      | Trigger EX DE,HL (swap pairs). |
| `ctl_reg_exx`           | Trigger EXX (swap all primes). |

### Internal bus / switches / external pads

| Signal | Function |
|---|---|
| `ctl_bus_db_oe`         | DB pads → db1. |
| `ctl_bus_db_we`         | db1 → DB pads. |
| `ctl_bus_inc_oe`        | IDU output → sb. |
| `ctl_bus_zero_oe`       | Zero → bus (for various clear paths). |
| `ctl_bus_ff_oe`         | 0xFF → bus (used for RST vectors, etc.). |
| `ctl_sw_1u/_1d`         | db1 ↔ db2 direction-gate. |
| `ctl_sw_2u/_2d`         | db2 ↔ ALU side. |
| `ctl_sw_4u/_4d`         | db2 ↔ sb cross. |
| `ctl_sw_mask543_en`     | Bit-mask for RST address formation (yyy×8). |
| `ctl_al_we`             | Address latch write enable (sb → AL). |
| `ctl_apin_mux`          | AB pin output mux (AL vs refresh). |
| `ctl_apin_mux2`         | Secondary AB mux. |

### IDU

| Signal | Function |
|---|---|
| `ctl_inc_cy`            | IDU carry-in (1 = INC, 0 with `inc_dec` = DEC). |
| `ctl_inc_dec`           | IDU direction: 0 = add, 1 = subtract. |
| `ctl_inc_limit6`        | Limit increment to low 7 bits (refresh R register). |

### Interrupt / state / prefix latches

| Signal | Function |
|---|---|
| `ctl_iff1_iff2`         | Copy IFF1 → IFF2 (NMI acceptance) or v.v. (RETN). |
| `ctl_iffx_bit`          | IFF{1,2} target bit. |
| `ctl_iffx_we`           | Write IFF{1,2}. |
| `ctl_im_we`             | Write IM register. |
| `ctl_no_ints`           | Inhibit interrupt acceptance this cycle (after EI). |
| `ctl_state_halt_set`    | Enter HALT. |
| `ctl_state_ixiy_we`     | Set IX/IY prefix latch. |
| `ctl_state_ixiy_clr`    | Clear IX/IY prefix latch. |
| `ctl_state_iy_set`      | Set IY (else IX). |
| `ctl_state_tbl_cb_set`  | Set CB-table latch. |
| `ctl_state_tbl_ed_set`  | Set ED-table latch. |
| `ctl_state_tbl_we`      | Generic table-latch write. |
| `ctl_state_alu`         | Activate ALU state. |
| `ctl_repeat_we`         | Set repeat-flag (for LDIR/CPIR/etc.). |

### Misc

| Signal | Function |
|---|---|
| `ctl_ir_we`             | Write IR from db. |
| `ctl_eval_cond`         | Evaluate condition (cc[y]) this cycle. |
| `ctl_cond_short`        | Short-circuit branch (skip remaining M-cycles). |
| `ctl_iorw`              | I/O read/write select. |
| `ctl_shift_en`          | Enable shifter. |

## M / T grid

The microprogram fires at each `(M-cycle, T-state)` cell:

  - M-cycles: M1 (4–6T), M2..M5 (3–5T each, sometimes 6T for INTA)
  - T-states: T1, T2, T3 standard; T4 for M1 (refresh); T5+ for stretched
    M-cycles (`EX (SP),HL`, INTA, displacement preamble, etc.)

Some `(M, T)` cells contain no control-signal assertions — those are
"dead" phases that just advance the timing counters.

## PLA bit numbering

The Z80 PLA produces ~99 numbered outputs that the exec matrix gates on
(plus combinations like `pla[91] & pla[20]` to discriminate sub-cases).
The bit numbering matches Z80 Explorer's die mapping. The full set used
in exec_matrix.vh:

```
pla[0..3, 5..13, 15..17, 20..21, 23..31, 33..35, 37..40, 42..53, 55..59,
    61, 64..66, 68..70, 72..86, 88..89, 91..92, 95..97]
```

Gaps in numbering correspond to PLA rows that aren't used by any opcode
or that handle internal-only cases (prefix latch transitions, etc.).
This refactor will need a Z80-Explorer-compatible PLA bit-mapping
function (z80_pla outputs → pla[N] indices).

## Per-(M, T) microcode style (example)

```verilog
if (pla[17] & ~pla[50]) begin
    if (M1 & T1) begin
        ctl_reg_gp_we    = 1;
        ctl_reg_gp_sel   = op54;          // pair index from opcode
        ctl_reg_gp_hilo  = {~rsel3, rsel3};
        ctl_reg_in_hi    = 1;
        ctl_reg_in_lo    = 1;             // from ALU side
        ctl_sw_2d        = 1;
        ctl_sw_1d        = 1;
        ctl_bus_db_oe    = 1;             // ext DB → db1
    end
    if (M1 & T4) begin
        validPLA = 1;
        nextM    = 1;
        ctl_mRead = 1;
    end
    if (M2 & T1) begin
        fMRead          = 1;
        ctl_reg_sel_pc  = 1;
        ctl_reg_sys_hilo = 2'b11;          // 16-bit PC
        ctl_al_we       = 1;               // sb → AL
    end
    if (M2 & T2) begin
        fMRead          = 1;
        ctl_reg_sys_we  = 1;
        ctl_reg_sel_pc  = 1;
        ctl_reg_sys_hilo = 2'b11;
        pc_inc_hold     = (in_halt | in_intr | in_nmi);
        ctl_inc_cy      = ~pc_inc_hold;    // INC
        ctl_bus_inc_oe  = 1;               // IDU → sb
    end
    if (M2 & T3) begin
        fMRead = 1;
        setM1  = 1;
    end
end
```

In the refactored RTL, the file pattern is:
  - Outer `if (PLA cond) begin ... end` blocks, one per PLA group;
  - Inner `if (M1 & T1) ... if (M1 & T4) ... if (M2 & T1) ...` cells;
  - Inside each cell, set the `ctl_*` signals for that phase.

## Datapath response

Each datapath block listens for its `ctl_*` signals and acts:

  - **Register file**: combinational read mux based on `ctl_reg_gp_sel`
    + `ctl_reg_gp_hilo` (or `ctl_reg_sel_pc/wz/ir` for system regs).
    Clocked write on `ctl_reg_gp_we` / `ctl_reg_sys_we` at the next
    clock edge.
  - **ALU**: op1/op2 latches updated on `ctl_alu_op1_oe`/`_op2_oe`
    (with `_sel_*` selectors picking source). Result drives db2 when
    `ctl_alu_oe` is asserted.
  - **IDU**: combinational over `(in, ctl_inc_cy, ctl_inc_dec)`; drives
    sb when `ctl_bus_inc_oe`.
  - **Address latch**: registered; loaded from sb when `ctl_al_we`.
  - **Internal buses**: one-hot mux from all asserted drivers (with
    assertion checks).
  - **IR**: registered; loaded from db when `ctl_ir_we`.
  - **Flags**: per-bit WE; sources selected by `ctl_flags_alu` /
    `_bus` / individual `_cpl` / `_set` modifiers.

## Plan integration

This document is Phase 1 of the silicon-faithful refactor described in
`.claude/plans/iterative-foraging-yao.md`. Subsequent phases use this
signal vocabulary to build the new microarchitecture:

  - Phase 2: `z80_idu` block (uses `ctl_inc_cy`, `ctl_inc_dec`,
    `ctl_bus_inc_oe`, `ctl_inc_limit6`).
  - Phase 3: `z80_regfile` module (uses the GP and sys signal groups).
  - Phase 4: internal bus segments + switches.
  - Phase 5: extended `z80_alu` (op1/op2 latches + new flag modes).
  - Phase 6: rewrite the sequencer as per-(M, T) matrix.
  - Phase 7: verification + final docs sync.
