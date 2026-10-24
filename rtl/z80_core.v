// ===========================================================================
// SPDX-FileCopyrightText: 2026 ReJ aka Renaldas Zioma
// SPDX-License-Identifier: CERN-OHL-W-2.0
// Source Location: https://github.com/rejunity/z80-from-schematics
// See HARDWARE_NOTICE.md for the section 4.1 Product notice.
//
// z80_core.v - the Z80 engine, organised the way the die is.
//
//   decode PLA       z80_pla: (prefix state, opcode) -> instruction class
//                    (exec) and its fields (registers, ALU op, cc, ...)
//   sequencer        (this module) T-state / M-cycle counters, bus-cycle
//                    type and length, WAIT, BUSREQ, NMI/INT sampling and
//                    the instruction boundary (accept NMI / INT / HALT or
//                    fetch the next M1)
//   execute          z80_execute, the random control logic: (exec, M, T)
//                    -> control word ctl_* -- which bus cycle starts next
//                    and at which address, what the incrementer does,
//                    which registers load what. Narrow selects, no data.
//   register file    z80_regfile: BC DE HL AF, BC' DE' HL' AF', IX IY SP
//                    PC WZ, renamed by the EX / EXX exchange flip-flops;
//                    a pair write port, a byte port, the flag port, the
//                    PC / SP / WZ loads
//   address side     z80_addr: abus (16 bit) -> IDU (+1 / -1 / -2) ->
//                    address latch (m_addr) and register write-back; the
//                    effective-address adder (PC+e, IX+d) next to it
//   ALU              z80_alu (8 bit, flags) and, here, the 16-bit add /
//                    subtract for ADD HL,rp / ADC HL,rp / SBC HL,rp
//   data latches     tmp8 (read data), m_wdata (write data), tmph (the DDCB
//                    opcode). Operand words assemble in WZ (Z then W), as
//                    on the die; MEMPTR is simply what WZ is left holding.
//   pin timing       z80_timing x2: the pin images of the two halves of a
//                    T-state (z80_top forms the dual-edge pins from them)
//
// Every unit exists once and is shared through select signals from the
// execute block, so an instruction class costs control decode, not a copy
// of its datapath. Behaviour is identical to the C model
// (cmodel/z80_core.c), including the undocumented flags, MEMPTR (WZ) and
// Q; `make compare` checks every phase of every trace program against it.
//
// ONE clock, ONE edge: every register in this module updates on
// `posedge clk`, once per Z80 T-state -- the clock IS the Z80 clock, there
// is no 2x oversampling. DFF only, no latches, no negedge logic, no
// `initial` in synthesizable code; async-assert reset. All state is
// computed combinationally as *_n and registered on posedge clk.
//
// The original chip does work on both clock edges, and the C model keeps
// that as two half-steps per T-state (phi=0 "P" = clock-high half, phi=1
// "N" = clock-low half). Here both half-steps are folded into ONE rising-
// edge evaluation: first the clock-low-half work (MRD/IORD read-data latch,
// INT/NMI sample point) consuming the `*_neg` inputs that z80_top sampled on
// the falling edge, then the clock-high-half work (M1 opcode latch, WAIT
// sample, T-state advance, execute) consuming the live inputs.
//
// Pins leave this module as two images per T-state -- `*_pos`, the value
// for the clock-high half of the T-state being ENTERED (a function of the
// next-state signals, registered by z80_top on this same rising edge), and
// `*_neg`, the value for the clock-low half of the CURRENT T-state (a
// function of registered state, registered by z80_top on the falling edge).
// z80_top turns the pair into real dual-edge pin waveforms; this module
// never sees the falling edge. See docs/clocking.md.
// ===========================================================================
`include "z80_defs.vh"

module z80_core (
    input  wire        clk,
    input  wire        por_n,          // power-on reset (async; not a Z80 pin)
    input  wire        res_s,          // RESET, sampled on this T-state's rising edge (1 = active)
    // inputs consumed on the rising edge (live pins)
    input  wire [7:0]  data_in,        // M1 opcode / INTA vector, end of T2.N / Tw2.N
    input  wire        busreq_n,       // BUSREQ at this T-state's rising edge (z80_top)
    input  wire        nmi_n,          // NMI edge detect, clock-low half
    // inputs sampled on the falling edge by z80_top
    input  wire [7:0]  data_in_neg,    // MRD data at T3, IORD data at T4
    input  wire        int_n_rise,     // INT level at this T-state's rising edge
    input  wire        nmi_n_neg,      // NMI edge detect, clock-high half
    input  wire        wait_n_neg,     // WAIT at the falling edge of T2 / Tw
    // pin images -- *_pos: clock-high half of the NEXT T-state (from
    // next-state logic); *_neg: clock-low half of the CURRENT T-state
    output wire [15:0] addr_pos,
    output wire [15:0] addr_neg,
    output wire [7:0]  data_out_pos,
    output wire [7:0]  data_out_neg,
    output wire        data_drive_pos,
    output wire        data_drive_neg,
    output wire        m1_n_pos,
    output wire        m1_n_neg,
    output wire        mreq_n_pos,
    output wire        mreq_n_neg,
    output wire        iorq_n_pos,
    output wire        iorq_n_neg,
    output wire        rd_n_pos,
    output wire        rd_n_neg,
    output wire        wr_n_pos,
    output wire        wr_n_neg,
    output wire        rfsh_n_pos,
    output wire        rfsh_n_neg,
    output wire        halt_n_pos,
    output wire        halt_n_neg,
    output wire        busack_n_pos,
    output wire        busack_n_neg,
    output wire        addr_drive_pos,  // the address pads are driven
    output wire        addr_drive_neg,
    output wire        ctl_drive_pos,   // the MREQ IORQ RD WR pads are driven
    output wire        ctl_drive_neg,
    // debug (for trace)
    output wire [3:0]  dbg_t,
    output wire [2:0]  dbg_m
);

    // =======================================================================
    // registered state
    // =======================================================================
    reg [7:0]  reg_i, reg_r, ir;
    reg [7:0]  reg_q;          // Q: F left by the previous F-modifying instruction, else 0
    // WZ bytes the die writes inside the M-cycle under way (z80_execute's
    // wz_later; cmodel's wzp_v / wzp_kz / wzp_kw): the value and the
    // T-states in whose low half Z and W land (0: none)
    reg [15:0] wzp_v;
    reg [2:0]  wzp_kz, wzp_kw;
    reg [3:0]  t_state;
    reg [2:0]  m_cycle;
    reg [2:0]  bus_op;
    reg [3:0]  m_len;
    reg [15:0] m_addr;         // address latch
    reg [7:0]  m_wdata;        // write-data latch
    reg [2:0]  prefix;
    reg        iff1, iff2;
    reg [1:0]  im;
    reg        halted;
    reg [7:0]  tmp8, tmph;
    reg        nmi_pending, prev_nmi_n, ei_delay, suppress_decode, bus_granted;
    reg        nmi_sampled, int_sampled;  // latched at T_last.P per UM0080
    reg [1:0]  irq_seq;        // 0 none, 1 NMI, 2 INT, 3 HALT-nop
    reg        im0_ins;        // the instruction came from the interrupting device (IM0)
    // RESET (perfectz80): res2 is the die's full reset, taken from res_s on
    // each falling edge except in the low half of an M1's T2, where it
    // holds; while it was set on the previous falling edge no T-state
    // follows the current one (rst_idle) until it clears. sres is the
    // special-reset latch, taken from res_s in that low half; srst_m1
    // marks the opcode fetch it replaces.
    reg        res2, sres, rst_idle, srst_m1;
    reg        in_tw;    // this T-state is a Tw (WAIT held the last): not T2
    reg        rd_hold;        // RD still low from a T1 cut (one idle T-state)
    // Register write-back on the die's edges (docs/timing.md "Register
    // write-back"): the result of an 8-bit load or ALU instruction goes
    // into its register in the next M1's (INTA's) T2.N, F in its T3.N --
    // the die runs the ALU there, from its operand latches. pend: such an
    // instruction ended; its decode (prefix, ir) and operands (the
    // registers, the read byte in tmp8) stay until that T2's end, where the
    // ALU runs again and the writes land (`fire`). pend1: the same for F
    // in that M1's T1.N (the block compares, the I/O repeats; `fire1`);
    // pend_rep: the block instruction repeated (Banks' X / Y). pend_pv:
    // LD A,I / R's P/V, the boundary's IFF2 decision. pf16: ADD / ADC / SBC
    // HL's F, held (pf_val; pf_cut, after a cut on that T2) -- their H
    // waits in ph_val for the second internal M-cycle's end. pex_*: EX
    // AF,AF' (the next M1's T1.N), EXX and EX DE,HL (its T2.N).
    reg        pend, pend1, pend_rep, pend_pv, pf16, pex_af, pex_x, pex_dh;
    reg  [7:0] pf_val, pf_cut, ph_val;
    reg [15:0] addr_hold;      // the address the last T-state's low half drove

    // ---- next state ----
    reg [7:0]  reg_i_n, reg_r_n, ir_n;
    reg [3:0]  t_n, m_len_n;
    reg [2:0]  m_cycle_n, bus_op_n;
    reg [15:0] m_addr_n;
    reg [7:0]  m_wdata_n;
    reg [2:0]  prefix_n;
    reg        iff1_n, iff2_n;
    reg [1:0]  im_n;
    reg        halted_n;
    reg [7:0]  tmp8_n, tmph_n;
    reg [7:0]  reg_q_n;
    reg [15:0] wzp_v_n;
    reg [2:0]  wzp_kz_n, wzp_kw_n;
    reg        nmi_pending_n, prev_nmi_n_n, ei_delay_n, suppress_decode_n, bus_granted_n;
    reg        nmi_sampled_n, int_sampled_n;
    reg        nmi_pending_mid;   // nmi_pending after the clock-high-half edge detect
    reg [1:0]  irq_seq_n;
    reg        im0_ins_n;
    reg        allow_int;
    reg        res2_n, sres_n, rst_idle_n, srst_m1_n, in_tw_n;
    reg        pend_n, pend1_n, pend_rep_n, pend_pv_n, pf16_n, pex_af_n, pex_x_n, pex_dh_n;
    reg  [7:0] pf_val_n, pf_cut_n, ph_val_n;

    // =======================================================================
    // decode PLA
    // =======================================================================
    wire [6:0] exec_w;
    wire [4:0] flag_mode_w;
    wire [2:0] alu_op_w, rf_src_w, rf_dst_w, cc_w, rot_op_w, bit_index_w;
    wire [1:0] cb_kind_w, idx_w;
    wire [3:0] rp_sel_w, aux_w;
    wire [7:0] rst_addr_w;
    wire       use_disp_w;
    // uses_cc / valid are produced by z80_pla but not consumed here. Wire
    // them up so iverilog and Verilator don't warn about missing pins;
    // -Wno-UNUSEDSIGNAL handles the dead-end side.
    wire       uses_cc_w, valid_w;

    z80_pla u_pla (
        .prefix(prefix), .op(ir),
        .exec(exec_w), .flag_mode(flag_mode_w), .alu_op(alu_op_w),
        .rf_src(rf_src_w), .rf_dst(rf_dst_w), .rp_sel(rp_sel_w),
        .cc(cc_w), .rot_op(rot_op_w), .bit_index(bit_index_w), .cb_kind(cb_kind_w),
        .aux(aux_w), .idx(idx_w), .use_disp(use_disp_w),
        .rst_addr(rst_addr_w), .uses_cc(uses_cc_w), .valid(valid_w)
    );

    // active "HL" pair for the current instruction (IX/IY under DD/FD)
    wire [3:0] hlp = (idx_w == 2'd1) ? `RFP_IX : (idx_w == 2'd2) ? `RFP_IY : `RFP_HL;

    // effective exec: an active interrupt/HALT sequence overrides the decode
    wire [6:0] eff_exec = (irq_seq == 2'd1) ? `EXEC_NMI :
                          (irq_seq == 2'd2) ? `EXEC_INT :
                          (irq_seq == 2'd3) ? `EXEC_NOP : exec_w;

    // =======================================================================
    // pin images
    // =======================================================================
    // *_pos is the clock-high half of the T-state being ENTERED at this
    // rising edge: a function of the next-state signals, so z80_top can
    // register it on the very edge the state commits. *_neg is the
    // clock-low half of the CURRENT T-state: a function of registered
    // state (a half-cycle path into z80_top's negedge flops). z80_timing is
    // the same pure function the C model calls; phi selects the half.
    wire [15:0] tp_addr, tn_addr;
    wire [7:0]  tp_dout, tn_dout;
    // LD (IX+d),n writes n from the data latch, where its read left it. A
    // RESET seen on the rising edge that starts the write's T1 makes the die
    // load the latch in that T1.N from the u bus, as for any other write
    // (d_dl): it carries (W ^ 7F) | 18, W the address's high byte, and the
    // pads show that until the cut ends the write before WR (perfectz80).
    wire        wd_ub = res_s && !rst_idle && !bus_granted && (bus_op == `BUSOP_MWR) &&
                        (t_state == 4'd1) && (irq_seq == 2'd0) && (eff_exec == `EXEC_LD_M_N) &&
                        (idx_w != 2'd0) && use_disp_w;
    wire [7:0]  wd_ub_val = (m_addr[15:8] ^ 8'h7F) | 8'h18;
    wire        tp_drive, tp_m1, tp_mreq, tp_iorq, tp_rd, tp_wr, tp_rfsh;
    wire        tn_drive, tn_m1, tn_mreq, tn_iorq, tn_rd, tn_wr, tn_rfsh;
    z80_timing u_timing_pos (
        .bus_op(bus_op_n), .t_state(t_n[2:0]), .phi(1'b0), .m_len(m_len_n),
        .m_addr(m_addr_n), .m_wdata(m_wdata_n), .reg_i(reg_i_n), .reg_r(reg_r_n),
        .addr(tp_addr), .data_out(tp_dout), .data_drive(tp_drive),
        .m1_n(tp_m1), .mreq_n(tp_mreq), .iorq_n(tp_iorq),
        .rd_n(tp_rd), .wr_n(tp_wr), .rfsh_n(tp_rfsh)
    );
    z80_timing u_timing_neg (
        .bus_op(bus_op), .t_state(t_state[2:0]), .phi(1'b1), .m_len(m_len),
        .m_addr(m_addr), .m_wdata(wd_ub ? wd_ub_val : m_wdata), .reg_i(reg_i), .reg_r(reg_r),
        .addr(tn_addr), .data_out(tn_dout), .data_drive(tn_drive),
        .m1_n(tn_m1), .mreq_n(tn_mreq), .iorq_n(tn_iorq),
        .rd_n(tn_rd), .wr_n(tn_wr), .rfsh_n(tn_rfsh)
    );
    // Bus released to a DMA master (BUSACK low), or the reset idle: strobes
    // idle, data bus not driven -- matches the C model's idle pins. The
    // address image holds its last value (the real chip floats it; a pad
    // ring can use busack_n / reset as output enables).
    //
    // RESET cuts (perfectz80): `cut` -- RES2 was set on the previous
    // falling edge, so this T-state is the last before the reset idle.
    //  - T1 cut, any cycle: the die forces the M-cycle to an opcode fetch
    //    on that falling edge, so MREQ and RD fall in T1.N even in a write
    //    or I/O cycle; MREQ rises on the next rising edge, RD on the
    //    falling edge after it (RD stays low in the idle T.P);
    //  - T2 cut: MREQ / RD release in T2.N and WR never falls; an I/O
    //    cycle cut after T2 never asserts IORQ / RD / WR at all (io_cut,
    //    from T2.P);
    //  - I/O Tw cut: IORQ and RD / WR release in Tw.N; INTA Tw cut: IORQ.
    //  - the 1- and 2-T internal cycles extend the M1 or a read on the die
    //    (T5 / T6 of the ring, no T1): a cut there drives nothing; the
    //    longer ones are M-cycles of their own, and a cut in their T1 is
    //    like any other -- and when RES2 rises on their T1.N, MREQ falls
    //    there and holds through T2.P;
    //  - a T1 cut followed by a restart at once keeps RD low into the
    //    restarted M1 (rd_hold): it releases only in an idle T.N.
    // the M1's T2 itself: RES2 doesn't hold in a Tw after it (perfectz80)
    wire m1_t2    = ((bus_op == `BUSOP_M1) || (bus_op == `BUSOP_INTA)) && (t_state == 4'd2) && !rst_idle && !in_tw;
    wire ext      = (bus_op == `BUSOP_INTERNAL) && (m_len <= 4'd2);
    wire int_g    = (bus_op == `BUSOP_INTERNAL) && !ext;
    wire int_g_n  = (bus_op_n == `BUSOP_INTERNAL) && (m_len_n > 4'd2);
    // no cut in a bus grant: the bus is the DMA master's (perfectz80)
    wire cut      = res2 && !rst_idle && !bus_granted;
    wire cut_t1   = cut && !ext && (t_state == 4'd1);
    wire cut_t2   = cut && !ext && (t_state == 4'd2);
    wire int_mreq_neg = int_g && (t_state == 4'd1) && !res2 && res2_n && !rst_idle && !bus_granted;
    wire int_mreq_pos = int_g_n && (t_n == 4'd2) && res2_n && !rst_idle_n;
    wire rd_hold_neg  = rst_idle && rd_hold && !res2;          // restart follows this idle T
    wire rd_hold_pos  = cut_t1 || (rst_idle && rd_hold && !res2);
    wire cut_iotw = cut && ((bus_op == `BUSOP_IORD) || (bus_op == `BUSOP_IOWR)) && (t_state == 4'd3);
    wire cut_inta = cut && (bus_op == `BUSOP_INTA) && ((t_state == 4'd3) || (t_state == 4'd4));
    wire io_cut   = res2_n && !rst_idle_n && (t_n == 4'd2) &&
                    ((bus_op_n == `BUSOP_IORD) || (bus_op_n == `BUSOP_IOWR));
    wire idle_pos = bus_granted_n | rst_idle_n;    // T-state being entered
    wire idle_neg = bus_granted | rst_idle;        // current T-state
    // Internal T-states, the reset idle and a bus grant drive no new
    // address: the bus keeps the last one -- the refresh address after an
    // M1, the operand address after a read (perfectz80; the Spectrum's
    // contention tables, "ir:1", "hl:1"), and so do the T-states an M1 or
    // INTA runs past its refresh (an M1's T5 / T6, IM1 / IM2's seventh).
    // m_addr still carries the IDU / EA work of those T-states.
    // A RESET cut in a long internal cycle's T1, with RES2 already set as
    // the T1 starts, forces it into an opcode-fetch-like read: the bus then
    // drives the address latch (perfectz80: HL in ADD HL,rr's M2).
    wire int_cut_pos = int_g_n && (t_n == 4'd1) && res2_n && !rst_idle_n;
    wire int_cut_neg = int_g && (t_state == 4'd1) && res2 && !rst_idle;
    wire hold_pos = idle_pos | ((bus_op_n == `BUSOP_INTERNAL) && !int_cut_pos) ||
                    ((bus_op_n == `BUSOP_M1) && (t_n >= 4'd5)) || ((bus_op_n == `BUSOP_INTA) && (t_n >= 4'd7));
    wire hold_neg = idle_neg | ((bus_op == `BUSOP_INTERNAL) && !int_cut_neg) ||
                    ((bus_op == `BUSOP_M1) && (t_state >= 4'd5)) || ((bus_op == `BUSOP_INTA) && (t_state >= 4'd7));
    assign addr_pos       = hold_pos ? addr_neg : tp_addr;
    assign data_out_pos   = tp_dout;
    assign data_drive_pos = idle_pos ? 1'b0 : tp_drive;
    assign m1_n_pos       = idle_pos | tp_m1;
    assign mreq_n_pos     = int_mreq_pos ? 1'b0 : (idle_pos | tp_mreq);
    assign iorq_n_pos     = idle_pos | io_cut | tp_iorq;
    assign rd_n_pos       = rd_hold_pos ? 1'b0 : (idle_pos | io_cut | tp_rd);
    // wr_n_pos is assigned below: it needs `stall` (WR hold across Tw).
    assign rfsh_n_pos     = idle_pos | tp_rfsh;
    assign addr_neg       = hold_neg ? addr_hold : tn_addr;
    assign data_out_neg   = tn_dout;
    assign data_drive_neg = (idle_neg | cut_t1) ? 1'b0 : tn_drive;
    assign m1_n_neg       = idle_neg | tn_m1;
    assign mreq_n_neg     = (cut_t1 | int_mreq_neg) ? 1'b0 : (idle_neg | cut_t2 | tn_mreq);
    assign iorq_n_neg     = idle_neg | cut_t1 | cut_t2 | cut_iotw | cut_inta | tn_iorq;
    assign rd_n_neg       = (cut_t1 | rd_hold_neg) ? 1'b0 : (idle_neg | cut_t2 | cut_iotw | tn_rd);
    assign wr_n_neg       = idle_neg | cut_t1 | cut_t2 | cut_iotw | tn_wr;
    assign rfsh_n_neg     = idle_neg | tn_rfsh;
    // Silicon-faithful halt pin (perfectz80): it changes on the falling
    // edge of an instruction's last T-state, to the state the next
    // instruction runs in -- low from T4.N of the HALT instruction's M1
    // (half a T-state before instruction-done sets `halted`), high again
    // from T4.N of the HALT NOP whose end accepts an NMI or INT. The
    // rising edge that started that T-state made the decision
    // (nmi_pending / int_n_rise / IFF1), so an interrupt pending at the
    // end of HALT itself never lowers the pin. After instruction-done,
    // the `halted` register takes over. Mirrors cmodel/z80_core.c halt_n
    // driving. Being a clock-low-half event it only appears in the *_neg
    // image. The instruction is the one that runs (eff_exec): the fetch of
    // an NMI response (or of HALT's NOPs) discards its opcode, so a HALT
    // byte read there doesn't lower the pin (perfectz80). In IM0 a HALT
    // handed over in INTA lowers it in the INTA's last T-state.
    wire exec_halt_in_m1 = (eff_exec == `EXEC_HALT)
                        && (((bus_op == `BUSOP_M1) && (t_state == 4'd4)) ||
                            ((bus_op == `BUSOP_INTA) && (t_state == 4'd6)));
    wire halt_exit = (t_state == m_len) && (nmi_pending | (~int_n_rise & iff1));
    // RESET: HALT rises on the falling edge where RES2 or the special-reset
    // latch sets, and stays high (perfectz80)
    wire halt_rst  = res2_n | sres_n;
    assign halt_n_pos   = (halted_n && !halt_rst) ? 1'b0 : 1'b1;
    assign halt_n_neg   = ((halted || exec_halt_in_m1) && !halt_exit && !halt_rst) ? 1'b0 : 1'b1;
    // BUSACK follows the grant flag, which only changes on the rising edge.
    // A granted T-state whose rising edge saw BUSREQ high is the last one:
    // BUSACK rises on its falling edge, the held M-cycle starts after it.
    assign busack_n_pos = bus_granted_n ? 1'b0 : 1'b1;
    // RES2 set in a granted T-state releases it the same way (perfectz80):
    // BUSACK rises in its low half, the reset idle follows
    assign busack_n_neg = (bus_granted && !busreq_n && !res2) ? 1'b0 : 1'b1;
    // The die's pad drivers (perfectz80's pull-up / pull-down gates,
    // scripts/pz80_pads.c): the address floats in a bus grant and in the
    // reset idle, MREQ IORQ RD WR in a bus grant only -- from the first
    // granted half to the rising edge that starts the held M-cycle, half a
    // T-state after BUSACK rises. The cut M-cycle before the reset idle
    // still drives the address; every other pin is always driven.
    assign addr_drive_pos = !idle_pos;
    assign addr_drive_neg = !idle_neg;
    assign ctl_drive_pos  = !bus_granted_n;
    assign ctl_drive_neg  = !bus_granted;
    assign dbg_t = t_state; assign dbg_m = m_cycle;

    // =======================================================================
    // timing-point predicates
    // =======================================================================
    // Read-data latch points (mirror cmodel is_latch_phase):
    //  - latch_neg: MRD at T3 / IORD at T4 -- the chip captures the data bus
    //    on the FALLING edge of that T-state (just before MREQ/IORQ
    //    deassert). z80_top's negedge sampler took the byte half a cycle
    //    ago; it lands in tmp8 at this rising edge and the execute step
    //    consumes it in the same evaluation (`rbyte`).
    //  - latch_pos: M1 opcode at the end of T2.N / INTA vector at the end
    //    of the second automatic Tw.N -- a RISING edge capture of the live
    //    data bus (the rising edge of T3).
    wire latch_neg = ((bus_op == `BUSOP_MRD)  && (t_state == 4'd3)) ||
                     ((bus_op == `BUSOP_IORD) && (t_state == 4'd4));
    wire latch_pos = ((bus_op == `BUSOP_M1)   && (t_state == 4'd2)) ||
                     ((bus_op == `BUSOP_INTA) && (t_state == 4'd4));
    // last refresh T-state (T4 of an M1, count 6 of an INTA): R steps on
    // the rising edge that ends it
    wire rfsh_end  = ((bus_op == `BUSOP_M1)   && (t_state == 4'd4)) ||
                     ((bus_op == `BUSOP_INTA) && (t_state == 4'd6));
    // WAIT *sample edge* per Zilog UM0080: the FALLING edge of T2 for
    // memory cycles, of the automatic Tw for I/O, of the second automatic
    // Tw for INTA. z80_top's negedge sampler took the pin there; the saved
    // decision holds this T-state as a Tw on this rising edge, whatever
    // WAIT does during the clock-low half (tb_wait_edge.v).
    wire wait_sample_phase =
                  ( (((bus_op == `BUSOP_M1) || (bus_op == `BUSOP_MRD) ||
                      (bus_op == `BUSOP_MWR)) && (t_state == 4'd2)) ||
                    (((bus_op == `BUSOP_IORD) || (bus_op == `BUSOP_IOWR)) && (t_state == 4'd3)) ||
                    ((bus_op == `BUSOP_INTA) && (t_state == 4'd4)) );
    wire wait_sampled = wait_sample_phase && (wait_n_neg == 1'b0);
    wire stall = wait_sampled;  // hold current T-state as a Tw
    // Inserted Tw repeats T2 of a memory write; its clock-high half is NOT
    // the initial T2.P before WR first falls (which is what the stateless
    // decoder describes). Keep WR asserted so the write is one continuous
    // pulse from T2.N to T3.N (tb_wait_write.v; cmodel hold_write_strobe).
    wire hold_wr = stall && (bus_op == `BUSOP_MWR);
    assign wr_n_pos = idle_pos | io_cut | (hold_wr ? 1'b0 : tp_wr);
    // the byte read this T-state: live opcode / vector, the falling-edge
    // MRD / IORD sample, or the data latch
    wire [7:0] rbyte = latch_pos ? data_in : latch_neg ? data_in_neg : tmp8;

    // the sequencer's step points. Registers load on every edge that
    // advances (wr_edge). The execute step proper -- bus cycles,
    // sequencing, most register loads -- runs on the last T-state of an
    // M-cycle (ex_edge); the incrementer jobs the die does while the
    // address latch holds (PC+1 of the opcode fetch, block-move pointer
    // and counter steps) run on earlier T-states.
    wire m_end   = (t_state + 4'd1) > m_len;            // last T-state of the M-cycle
    // PC = 0: while RES2 is set (the cut T-state and the reset idle), and on
    // the opcode latch of the special reset's fetch. From registered state
    // only: the sequencer reads pc_next, so it must not drive this.
    wire pc_clr  = res2 | (srst_m1 && latch_pos && !stall && (bus_op == `BUSOP_M1));
    wire wr_edge = !rst_idle && !bus_granted && !stall;
    wire ex_edge = wr_edge && m_end;
    // RESET cuts a T2 that WAIT held, or a Tw after it: what the die wrote in
    // T1's low half stands (the T2 micro-ops), from a Tw on what it wrote in
    // T2's too -- on this edge, as the cut's last (docs/timing.md "Register
    // write-back")
    wire stall_cut = res2 && !rst_idle && !bus_granted && stall && (t_state == 4'd2);
    wire tw_cut    = res2 && !rst_idle && !bus_granted && in_tw && (t_state == 4'd2);
    // a block instruction's last step after a repeat (its internal M-cycle)
    wire blk_rep_fin = (aux_w[2:1] <= 2'd1) ? (m_cycle == 3'd4) : (m_cycle == 3'd5);
    // the classes whose register and / or F writes the die makes in the
    // next M1's T2.N (F in T3.N): deferred to `fire`
    wire defer_cls = (exec_w == `EXEC_LD_R_R) || (exec_w == `EXEC_LD_R_N) || (exec_w == `EXEC_LD_R_M) ||
                     (exec_w == `EXEC_ALU_R)  || (exec_w == `EXEC_ALU_N)  || (exec_w == `EXEC_ALU_M)  ||
                     (exec_w == `EXEC_INC_R)  || (exec_w == `EXEC_DEC_R)  || (exec_w == `EXEC_ROT_A)  ||
                     (exec_w == `EXEC_DAA)    || (exec_w == `EXEC_CPL)    || (exec_w == `EXEC_SCF)    ||
                     (exec_w == `EXEC_CCF)    || (exec_w == `EXEC_NEG)    || (exec_w == `EXEC_CB_R)   ||
                     (exec_w == `EXEC_IN_C)   || (exec_w == `EXEC_LD_A_IR) ||
                     (exec_w == `EXEC_INC_M)  || (exec_w == `EXEC_DEC_M)  || (exec_w == `EXEC_CB_M)   ||
                     (exec_w == `EXEC_DDCB)   || (exec_w == `EXEC_RRD)    || (exec_w == `EXEC_RLD)    ||
                     ((exec_w == `EXEC_BLOCK) && ((aux_w[2:1] == 2'd0) || (aux_w[2] && !blk_rep_fin)));
    // ... in its T1.N: the block compares' F, and the I/O repeats' (every
    // flag latched before that M1)
    wire defer1_cls = (exec_w == `EXEC_BLOCK) && ((aux_w[2:1] == 2'd1) || (aux_w[2] && blk_rep_fin));
    wire a16_cls = (exec_w == `EXEC_ADD_HL_RP) || (exec_w == `EXEC_ADC16) || (exec_w == `EXEC_SBC16);
    // reads straight into a register: written in the read's T3.N, which a
    // RESET cut ending on that T3 blocks
    wire rd_cls  = (exec_w == `EXEC_LD_A_NN) || (exec_w == `EXEC_LD_A_RP) || (exec_w == `EXEC_IN_A_N) ||
                   (exec_w == `EXEC_LD_RP_NN) || (exec_w == `EXEC_POP) || (exec_w == `EXEC_LD_RP_NNA) ||
                   (exec_w == `EXEC_LD_HL_NN);
    // the edges that end the next M1's (INTA's) T1 and T2 -- not a Tw after
    // T2 (WAIT): the writes land there, a RESET cut ending on them blocks
    // them -- but F after a cut on that T2: from the flag latches (f_cut)
    wire m1_any = (bus_op == `BUSOP_M1) || (bus_op == `BUSOP_INTA);
    wire e1     = !rst_idle && !bus_granted && m1_any && (t_state == 4'd1);
    wire e2     = !rst_idle && !bus_granted && m1_any && (t_state == 4'd2) && !in_tw;
    wire fire   = pend && e2;
    wire fire1  = pend1 && e1;
    wire fire16 = pf16 && e2;

    // =======================================================================
    // register file: read side
    // =======================================================================
    wire [15:0] r_bc, r_de, r_hl, r_af, r_bc2, r_de2, r_hl2, r_af2;
    wire [15:0] r_ix, r_iy, r_sp, r_pc, r_wz;
    wire [7:0]  A_cur = r_af[15:8];
    wire [7:0]  F_cur = r_af[7:0];

    // HL / IX / IY pair of the current instruction
    wire [15:0] rf_hlp = (idx_w == 2'd1) ? r_ix : (idx_w == 2'd2) ? r_iy : r_hl;
    // rp[p] / rp2[p] pair named by the PLA (BC DE HL/IX/IY SP AF)
    reg  [15:0] rf_rp;
    always @* begin
        case (rp_sel_w)
            `RFP_BC: rf_rp = r_bc;
            `RFP_DE: rf_rp = r_de;
            `RFP_HL: rf_rp = r_hl;
            `RFP_AF: rf_rp = r_af;
            `RFP_IX: rf_rp = r_ix;
            `RFP_IY: rf_rp = r_iy;
            `RFP_SP: rf_rp = r_sp;
            default: rf_rp = 16'h0000;     // the PLA names no other pair
        endcase
    end
    // r[z] byte without the IX/IY substitution (CB page, OUT (C),r)
    reg  [7:0] r8_src;
    always @* begin
        case (rf_src_w)
            3'd0: r8_src = r_bc[15:8];
            3'd1: r8_src = r_bc[7:0];
            3'd2: r8_src = r_de[15:8];
            3'd3: r8_src = r_de[7:0];
            3'd4: r8_src = r_hl[15:8];
            3'd5: r8_src = r_hl[7:0];
            3'd6: r8_src = 8'h00;
            default: r8_src = A_cur;
        endcase
    end
    // r[z] / r[y] byte with IXH/IXL/IYH/IYL substituted for H/L under
    // DD/FD (not when the instruction has an (IX+d) memory operand)
    wire [7:0] getri_src_val =
        (idx_w != 2'd0 && !use_disp_w && rf_src_w == 3'd4) ? rf_hlp[15:8] :
        (idx_w != 2'd0 && !use_disp_w && rf_src_w == 3'd5) ? rf_hlp[7:0]  :
        (rf_src_w == 3'd0) ? r_bc[15:8] :
        (rf_src_w == 3'd1) ? r_bc[7:0]  :
        (rf_src_w == 3'd2) ? r_de[15:8] :
        (rf_src_w == 3'd3) ? r_de[7:0]  :
        (rf_src_w == 3'd4) ? r_hl[15:8] :
        (rf_src_w == 3'd5) ? r_hl[7:0]  :
        (rf_src_w == 3'd7) ? A_cur : 8'h00;
    wire [7:0] getri_dst_val =
        (idx_w != 2'd0 && !use_disp_w && rf_dst_w == 3'd4) ? rf_hlp[15:8] :
        (idx_w != 2'd0 && !use_disp_w && rf_dst_w == 3'd5) ? rf_hlp[7:0]  :
        (rf_dst_w == 3'd0) ? r_bc[15:8] :
        (rf_dst_w == 3'd1) ? r_bc[7:0]  :
        (rf_dst_w == 3'd2) ? r_de[15:8] :
        (rf_dst_w == 3'd3) ? r_de[7:0]  :
        (rf_dst_w == 3'd4) ? r_hl[15:8] :
        (rf_dst_w == 3'd5) ? r_hl[7:0]  :
        (rf_dst_w == 3'd7) ? A_cur : 8'h00;

    function [3:0] baselen; input [2:0] bop;
        case (bop)
            `BUSOP_M1:   baselen = 4'd4;
            `BUSOP_MRD:  baselen = 4'd3;
            `BUSOP_MWR:  baselen = 4'd3;
            `BUSOP_IORD: baselen = 4'd4;
            `BUSOP_IOWR: baselen = 4'd4;
            `BUSOP_INTA: baselen = 4'd6;
            default:     baselen = 4'd0;
        endcase
    endfunction

    function cc_true; input [7:0] f; input [2:0] sel;
        case (sel)
            3'd0: cc_true = ~f[6];   // NZ
            3'd1: cc_true =  f[6];   // Z
            3'd2: cc_true = ~f[0];   // NC
            3'd3: cc_true =  f[0];   // C
            3'd4: cc_true = ~f[2];   // PO
            3'd5: cc_true =  f[2];   // PE
            3'd6: cc_true = ~f[7];   // P
            default: cc_true = f[7]; // M
        endcase
    endfunction
    wire cc_taken = cc_true(F_cur, cc_w);

    // =======================================================================
    // ALU: operand latches + z80_alu
    // =======================================================================
    // Block-IO flag inputs: data + (C +/- 1) for INxR, data + new L for OTxR.
    // The data byte is still in the read-data latch tmp8 (the IN / MRD
    // M-cycle latched it; the following MWR / IOWR does not).
    wire [8:0] bk_ini = {1'b0, tmp8} + {1'b0, (r_bc[7:0] + (aux_w[0] ? 8'hFF : 8'h1))};
    wire [8:0] bk_out = {1'b0, tmp8} + {1'b0, r_hl[7:0]};   // L already stepped (M3 T1)

    // Banks-2018 INIR/INDR/OTIR/OTDR repeat-M-cycle flag fold-in.
    // Mirrors redcode INXR_OTXR_COMMON exactly (see scripts/refs/
    // redcode_z80/Z80.c line 1233). Inputs: data byte (tmp8 after the
    // IN/MRD M-cycle), newB (B post-decrement), bk = {hcf, t[7:0]}
    // 9-bit sum (bk_ini for INxR, bk_out for OTxR), pch = high byte of
    // post-rewind PC (= (PC-2)[15:8]) used for YF/XF fold-in.
    function [7:0] banks_io_rep_f;
        input [7:0] data;
        input [7:0] newB;
        input [8:0] bk;
        input [7:0] pch;
        reg [7:0] t;
        reg hcf;
        reg [7:0] pf_arg;
        reg pf_bit;
        reg hf, cf;
        begin
            t = bk[7:0];
            hcf = bk[8];
            // redcode's INXR_OTXR_COMMON: PF_PARITY(p ^ ((B +/- 1) & 7))
            // where p = (t & 7) ^ newB. Earlier this missed the `^ newB`
            // term, which broke RTL z80full 102/103 INIR/INDR -> NOP'
            // (CRC 62B504C4 / 21477131 vs expected 454E3531 / 06BC40C4).
            if (hcf) begin
                cf = 1'b1;
                if (data[7]) begin
                    hf     = (newB[3:0] == 4'h0) ? 1'b1 : 1'b0;
                    pf_arg = ((t & 8'h07) ^ newB)
                           ^ ((newB - 8'd1) & 8'h07);
                end else begin
                    hf     = (newB[3:0] == 4'hF) ? 1'b1 : 1'b0;
                    pf_arg = ((t & 8'h07) ^ newB)
                           ^ ((newB + 8'd1) & 8'h07);
                end
            end else begin
                cf     = 1'b0;
                hf     = 1'b0;
                pf_arg = ((t & 8'h07) ^ newB)
                       ^ (newB & 8'h07);
            end
            pf_bit = ~(^pf_arg);
            banks_io_rep_f =
                  (newB & `Z80_SF)               // SF = newB.7
                | (pch & (`Z80_YF | `Z80_XF))    // YF=PCi.13; XF=PCi.11
                | (data[7] ? `Z80_NF : 8'h00)    // NF = data.7
                | (hf      ? `Z80_HF : 8'h00)
                | (pf_bit  ? `Z80_PF : 8'h00)
                | (cf      ? `Z80_CF : 8'h00);   // ZF = 0
        end
    endfunction

    reg  [7:0] alu_a, alu_b, alu_xy;
    reg  [4:0] alu_md;
    reg  [2:0] alu_rot, alu_bit;
    // NMOS race: an INT accepted at the end of LD A,I / LD A,R clears IFF2
    // while the instruction writes P/V from it, so P/V reads 0 (perfectz80;
    // an NMI there leaves P/V = IFF2). This is the boundary's decision: no
    // NMI pending, INT low at the last T-state's rising edge, IFF1 set.
    wire ldair_int = ~nmi_pending & ~int_n_rise & iff1;
    always @* begin
        alu_a = A_cur;
        alu_b = 8'h00;
        alu_md = flag_mode_w;
        alu_xy = 8'h00;
        alu_rot = rot_op_w;
        alu_bit = bit_index_w;
        case (exec_w)
            `EXEC_ALU_R:           alu_b = getri_src_val;
            `EXEC_ALU_N, `EXEC_ALU_M:alu_b = tmp8;    // at fire (the read's latch)
            `EXEC_INC_R, `EXEC_DEC_R:alu_b = getri_dst_val;
            `EXEC_INC_M, `EXEC_DEC_M:alu_b = tmp8;    // the 4 T read's: latched at T3
            `EXEC_CB_R: begin alu_b = r8_src;          alu_xy = r8_src; end
            `EXEC_CB_M: begin alu_b = tmp8;            alu_xy = r_wz[15:8]; end
            `EXEC_DDCB: begin alu_b = tmp8;            alu_xy = r_wz[15:8];
                            // flags from the (IX+d) read on, BIT and the
                            // rotates only: RES / SET leave F (and Q) alone
                            alu_md = (m_cycle != 3'd5 && !pend) ? `FLAG_NONE :
                                     (tmph[7:6] == `CB_BIT) ? `FLAG_BIT :
                                     (tmph[7:6] == `CB_ROT) ? `FLAG_ROT : `FLAG_NONE;
                            alu_rot = tmph[5:3]; alu_bit = tmph[5:3]; end
            `EXEC_NEG: begin alu_md = `FLAG_NEG; alu_a = A_cur; end
            `EXEC_LD_A_IR: begin alu_md = `FLAG_LD_A_I;
                              alu_b = aux_w[0] ? reg_r : reg_i;
                              alu_bit = {2'b0, pend_pv}; end         // at fire
            `EXEC_IN_C: begin alu_md = `FLAG_IN; alu_b = tmp8; end
            `EXEC_RRD, `EXEC_RLD: begin
                alu_md = (exec_w == `EXEC_RRD) ? `FLAG_RRD : `FLAG_RLD;
                alu_a = A_cur; alu_b = tmp8;
            end
            `EXEC_BLOCK: begin
                // (cat=aux_w[2:1], dec=aux_w[0]). The flag mode is live only
                // in the M-cycle whose end commits the flags. tmp8 holds the
                // byte LDI moved; rbyte is the byte CPI is reading.
                // the flags are formed at fire, in the next M1: BC, B and
                // HL are stepped, the byte is in tmp8, A is untouched
                case (aux_w[2:1])
                    2'd0: begin
                        alu_md = `FLAG_BLOCK_LD; alu_a = A_cur; alu_b = tmp8;
                        alu_bit = {2'b0, (r_bc != 16'd0)};
                    end
                    2'd1: begin
                        alu_md = `FLAG_BLOCK_CP; alu_a = A_cur; alu_b = tmp8;
                        alu_bit = {2'b0, (r_bc != 16'd0)};
                    end
                    2'd2: begin
                        alu_md = `FLAG_BLOCK_IO;
                        alu_a = tmp8;                      // data
                        alu_b = r_bc[15:8];                // newB
                        alu_xy = {4'b0, bk_ini[8], bk_ini[2:0]};
                    end
                    default: begin                         // 2'd3 = OUT
                        alu_md = `FLAG_BLOCK_IO;
                        alu_a = tmp8;                      // data
                        alu_b = r_bc[15:8];                // newB
                        alu_xy = {4'b0, bk_out[8], bk_out[2:0]};
                    end
                endcase
            end
            default:             alu_b = 8'h00;
        endcase
    end
    wire [7:0] alu_res, alu_fout;
    z80_alu u_alu (
        .mode(alu_md), .alu_op(alu_op_w), .rot_op(alu_rot),
        .bit_idx(alu_bit), .xy_src(alu_xy),
        .a(alu_a), .b(alu_b), .oldf(F_cur), .q(reg_q),
        .res(alu_res), .fout(alu_fout)
    );

    // =======================================================================
    // execute: control word
    // =======================================================================
`include "z80_ctl.vh"

    wire        ctl_mstart, ctl_m1, ctl_fin;
    wire [2:0]  ctl_bop;
    wire [3:0]  ctl_mx;
    wire [2:0]  ctl_al, ctl_ab, ctl_ri;
    wire [1:0]  ctl_inc;
    wire        ctl_ea_pc;
    wire [1:0]  ctl_ea_d;
    wire [2:0]  ctl_db;
    wire [1:0]  ctl_bi;
    wire        ctl_bhi;
    wire [2:0]  ctl_pc, ctl_wz;
    wire [1:0]  ctl_wzm;
    wire        ctl_wzc;
    wire [2:0]  ctl_wzp, ctl_wzkz, ctl_wzkw;
    wire [1:0]  ctl_sp, ctl_vec;
    wire        ctl_pw;
    wire [2:0]  ctl_pwi;
    wire [1:0]  ctl_pws;
    wire        ctl_bw;
    wire [3:0]  ctl_bwd;
    wire [2:0]  ctl_bws;
    wire        ctl_fw;
    wire [2:0]  ctl_fs;
    wire        ctl_ex_dehl, ctl_ex_af, ctl_exx;
    wire        ctl_tmph;
    wire [1:0]  ctl_iff;
    wire        ctl_im, ctl_halt, ctl_ld_i, ctl_ld_r, ctl_prefix;
    wire        ctl_cbx, ctl_cbr, ctl_pw_late;
    wire [2:0]  pfx_next;
    wire [7:0]  b_dec = r_bc[15:8] - 8'd1;      // B-1 (DJNZ, INI, OUTI)
    // the last edge of a deferred class's instruction (not of an interrupt
    // response, whose decode is stale): its register / F writes wait (fire)
    wire fin_ok, defer_now, defer1_now, a16_now;

    z80_execute u_exec (
        .m_end(m_end), .latch_pos(latch_pos), .bus_op(bus_op),
        .suppress_decode(suppress_decode), .m_cycle(m_cycle), .t_state(t_state),
        .irq_seq(irq_seq), .eff_exec(eff_exec), .exec_w(exec_w),
        .alu_op_w(alu_op_w), .cb_kind_w(cb_kind_w), .idx_w(idx_w),
        .use_disp_w(use_disp_w), .aux_w(aux_w), .prefix(prefix), .ir(ir),
        .ddcb_kind(tmph[7:6]), .im(im), .im0_ins(im0_ins),
        .cc_taken(cc_taken), .zf(A_cur == tmp8), .bc_nz(r_bc != 16'd0),
        .b_nz(r_bc[15:8] != 8'd0), .b_dec_nz(b_dec != 8'd0), .tw_fix(tw_cut),
        .ctl_mstart(ctl_mstart), .ctl_m1(ctl_m1), .ctl_fin(ctl_fin),
        .ctl_bop(ctl_bop), .ctl_mx(ctl_mx), .ctl_al(ctl_al),
        .ctl_ab(ctl_ab), .ctl_ri(ctl_ri), .ctl_inc(ctl_inc),
        .ctl_ea_pc(ctl_ea_pc), .ctl_ea_d(ctl_ea_d),
        .ctl_db(ctl_db), .ctl_bi(ctl_bi), .ctl_bhi(ctl_bhi),
        .ctl_pc(ctl_pc), .ctl_wz(ctl_wz), .ctl_sp(ctl_sp), .ctl_vec(ctl_vec),
        .ctl_wzm(ctl_wzm), .ctl_wzc(ctl_wzc), .ctl_wzp(ctl_wzp), .ctl_wzkz(ctl_wzkz), .ctl_wzkw(ctl_wzkw),
        .ctl_pw(ctl_pw), .ctl_pwi(ctl_pwi), .ctl_pws(ctl_pws),
        .ctl_bw(ctl_bw), .ctl_bwd(ctl_bwd), .ctl_bws(ctl_bws),
        .ctl_fw(ctl_fw), .ctl_fs(ctl_fs),
        .ctl_ex_dehl(ctl_ex_dehl), .ctl_ex_af(ctl_ex_af), .ctl_exx(ctl_exx),
        .ctl_tmph(ctl_tmph), .ctl_iff(ctl_iff), .ctl_im(ctl_im),
        .ctl_halt(ctl_halt), .ctl_ld_i(ctl_ld_i), .ctl_ld_r(ctl_ld_r),
        .ctl_prefix(ctl_prefix), .ctl_cbx(ctl_cbx), .ctl_cbr(ctl_cbr), .ctl_pw_late(ctl_pw_late),
        .pfx_next(pfx_next)
    );

    assign fin_ok     = ex_edge && ctl_fin && (irq_seq == 2'd0);
    assign defer_now  = fin_ok && defer_cls;
    assign defer1_now = fin_ok && defer1_cls;
    assign a16_now    = fin_ok && a16_cls;

    // =======================================================================
    // address side
    // =======================================================================
    wire [15:0] abus, idu, ea, al_in, a16;
    z80_addr u_addr (
        .pc(r_pc), .sp(r_sp), .wz(r_wz), .wzp(wzp_v), .hl(r_hl), .de(r_de), .bc(r_bc),
        .hlp(rf_hlp), .rp(rf_rp), .rbyte(rbyte), .a(A_cur), .i_reg(reg_i),
        .tmp8(tmp8), .b_dec(b_dec),
        .ctl_ab(ctl_ab), .ctl_ri(ctl_ri), .ctl_inc(ctl_inc),
        .ctl_ea_pc(ctl_ea_pc), .ctl_ea_d(ctl_ea_d), .ctl_al(ctl_al),
        .abus(abus), .idu(idu), .ea(ea), .al_in(al_in)
    );

    // 16-bit ALU: ADD HL,rp / ADC HL,rp / SBC HL,rp (HL is IX/IY under DD/FD)
    wire        a16_sub = (exec_w == `EXEC_SBC16);
    wire        a16_cy  = (exec_w == `EXEC_ADC16) ? F_cur[0] : (a16_sub ? ~F_cur[0] : 1'b0);
    wire [15:0] a16_y   = a16_sub ? ~rf_rp : rf_rp;
    wire [16:0] a16_s   = {1'b0, rf_hlp} + {1'b0, a16_y} + {16'd0, a16_cy};
    assign      a16     = a16_s[15:0];
    wire        a16_c   = a16_s[16] ^ a16_sub;                         // carry / borrow
    wire        a16_h   = a16[12] ^ rf_hlp[12] ^ a16_y[12] ^ a16_sub;  // out of bit 11
    wire        a16_v   = ~(rf_hlp[15] ^ a16_y[15]) & (rf_hlp[15] ^ a16[15]);
    wire [7:0]  a16_f   = (exec_w == `EXEC_ADD_HL_RP)
        ? ((F_cur & (`Z80_SF | `Z80_ZF | `Z80_PF)) | (a16[15:8] & (`Z80_YF | `Z80_XF))
           | (a16_h ? `Z80_HF : 8'h00) | (a16_c ? `Z80_CF : 8'h00))
        : ((a16[15] ? `Z80_SF : 8'h00) | ((a16 == 16'h0000) ? `Z80_ZF : 8'h00)
           | (a16[15:8] & (`Z80_YF | `Z80_XF)) | (a16_h ? `Z80_HF : 8'h00)
           | (a16_v ? `Z80_PF : 8'h00) | (a16_sub ? `Z80_NF : 8'h00)
           | (a16_c ? `Z80_CF : 8'h00));

    // bit-operation unit: CB rotates come from the ALU, RES / SET here
    wire        cbx_e   = ctl_cbx || (pend && exec_w == `EXEC_DDCB);
    wire [1:0]  cb_kind = cbx_e ? tmph[7:6] : cb_kind_w;
    wire [2:0]  cb_bit  = cbx_e ? tmph[5:3] : bit_index_w;
    wire [7:0]  cb_in   = (exec_w == `EXEC_CB_R) ? r8_src : tmp8;   // (HL) / (IX+d): the 4 T read's latch
    wire [7:0]  cb_mask = 8'h01 << cb_bit;
    reg  [7:0]  cbres;
    always @* begin
        case (cb_kind)
            `CB_ROT: cbres = alu_res;
            `CB_RES: cbres = cb_in & ~cb_mask;
            `CB_SET: cbres = cb_in | cb_mask;
            default: cbres = cb_in;
        endcase
    end
    // RRD / RLD: the byte written back to (HL)
    wire [7:0]  rrd_mem = (exec_w == `EXEC_RRD) ? {A_cur[3:0], tmp8[7:4]} : {tmp8[3:0], A_cur[3:0]};

    // write-data latch input
    reg  [15:0] db_pair;
    always @* begin
        case (ctl_bi)
            BI_PC:   db_pair = r_pc;
            BI_RP:   db_pair = rf_rp;
            BI_HLP:  db_pair = rf_hlp;
            default: db_pair = r_af;
        endcase
    end
    reg  [7:0]  wdata;
    always @* begin
        case (ctl_db)
            DB_ZERO: wdata = 8'h00;
            DB_REG:  wdata = ctl_bhi ? db_pair[15:8] : db_pair[7:0];
            DB_RI:   wdata = getri_src_val;
            DB_R8:   wdata = r8_src;
            DB_RB:   wdata = rbyte;
            DB_ALU:  wdata = alu_res;
            DB_CB:   wdata = cbres;
            default: wdata = rrd_mem;
        endcase
    end
    // interrupt / restart vectors
    reg  [15:0] vec;
    always @* begin
        case (ctl_vec)
            VEC_NMI: vec = 16'h0066;
            VEC_IM:  vec = (im == 2'd1) ? 16'h0038 : {8'h00, (tmp8 & 8'h38)};
            default: vec = {8'h00, rst_addr_w};
        endcase
    end

    // =======================================================================
    // register file: write side
    // =======================================================================
    // pair write port
    reg  [3:0]  pw_idx;
    reg  [15:0] pw_data;
    always @* begin
        case (ctl_pwi)
            RI_HLP:  pw_idx = hlp;
            RI_HL:   pw_idx = `RFP_HL;
            RI_BC:   pw_idx = `RFP_BC;
            RI_DE:   pw_idx = `RFP_DE;
            default: pw_idx = rp_sel_w;
        endcase
        case (ctl_pws)
            PW_IDU:  pw_data = idu;
            PW_A16:  pw_data = a16;
            default: pw_data = r_wz;
        endcase
    end
    // the deferred instruction's writes at fire -- its last step's, as
    // z80_execute issues them -- and the selects in effect on this edge
    reg         fire_bw, fire_fw;
    reg  [3:0]  fire_bwd;
    reg  [2:0]  fire_bws;
    always @* begin
        fire_bw = 1'b1; fire_fw = 1'b1; fire_bwd = BW_A; fire_bws = BS_ALU;
        case (exec_w)
            `EXEC_LD_R_R:  begin fire_bwd = BW_RI; fire_bws = BS_RI; fire_fw = 1'b0; end
            `EXEC_LD_R_N, `EXEC_LD_R_M:
                           begin fire_bwd = BW_RI; fire_bws = BS_RB; fire_fw = 1'b0; end
            `EXEC_ALU_R, `EXEC_ALU_N, `EXEC_ALU_M:
                           fire_bw = (alu_op_w != `ALU_CP);
            `EXEC_INC_R, `EXEC_DEC_R: fire_bwd = BW_RI;
            `EXEC_SCF, `EXEC_CCF:     fire_bw = 1'b0;
            `EXEC_CB_R:    begin fire_bw = (cb_kind_w != `CB_BIT); fire_bwd = BW_R8; fire_bws = BS_CB;
                                 fire_fw = (cb_kind_w == `CB_ROT) || (cb_kind_w == `CB_BIT); end
            `EXEC_IN_C:    begin fire_bwd = BW_R8; fire_bws = BS_RB; end
            `EXEC_INC_M, `EXEC_DEC_M, `EXEC_BLOCK: fire_bw = 1'b0;          // F
            `EXEC_CB_M:    begin fire_bw = 1'b0; fire_fw = (cb_kind_w != `CB_RES) && (cb_kind_w != `CB_SET); end
            // DD CB: F (BIT, the rotates), the copy to r[z] (all but BIT)
            `EXEC_DDCB:    begin fire_bw = (tmph[7:6] != `CB_BIT); fire_bwd = BW_DDCB; fire_bws = BS_CB;
                                 fire_fw = (tmph[7:6] == `CB_BIT) || (tmph[7:6] == `CB_ROT); end
            default: ;                                   // ROT_A DAA CPL NEG LD_A_IR RRD RLD: A and F
        endcase
    end
    // in the pend window (the next M1's T1 and T2) nothing else writes a
    // byte or F: the deferred op's selects stand there throughout
    wire [3:0]  bwd_e = pend ? fire_bwd : ctl_bwd;
    wire [2:0]  bws_e = pend ? fire_bws : ctl_bws;
    // a block instruction's F after a repeat: Banks' X / Y from PC (rewound)
    wire [2:0]  fs_fire = (exec_w == `EXEC_BLOCK && pend_rep) ? ((aux_w[2:1] <= 2'd1) ? FS_BLD : FS_BIO) : FS_ALU;
    wire [2:0]  fs_e  = (pend || pend1) ? fs_fire : ctl_fs;
    // byte write port: r[y]-style register code (IXH/IXL substitution for
    // BW_RI), A, B, the DDCB copy target, or a byte of HL/IX/IY
    reg  [2:0]  bw_code;
    reg         bw_ix, bw_en, bw_hi;
    reg  [3:0]  bw_pair;
    reg  [7:0]  bw_data;
    always @* begin
        bw_ix = 1'b0;
        case (bwd_e)
            BW_RI:   begin bw_code = rf_dst_w; bw_ix = 1'b1; end
            BW_A:    bw_code = 3'd7;
            BW_B:    bw_code = 3'd0;
            BW_DDCB: bw_code = tmph[2:0];
            default: bw_code = rf_dst_w;
        endcase
        bw_en = 1'b1; bw_hi = ~bw_code[0];
        case (bw_code)
            3'd0, 3'd1: bw_pair = `RFP_BC;
            3'd2, 3'd3: bw_pair = `RFP_DE;
            3'd4, 3'd5: bw_pair = (bw_ix && idx_w != 2'd0 && !use_disp_w) ? hlp : `RFP_HL;
            3'd6:       begin bw_pair = `RFP_HL; bw_en = 1'b0; end     // (HL): not a register
            default:    begin bw_pair = `RFP_AF; bw_hi = 1'b1; end
        endcase
        if (bwd_e == BW_HLPL) begin bw_pair = hlp; bw_hi = 1'b0; bw_en = 1'b1; end
        if (bwd_e == BW_HLPH) begin bw_pair = hlp; bw_hi = 1'b1; bw_en = 1'b1; end
        if (bwd_e == BW_RPL)  begin bw_pair = rp_sel_w; bw_hi = 1'b0; bw_en = 1'b1; end
        if (bwd_e == BW_RPH)  begin bw_pair = rp_sel_w; bw_hi = 1'b1; bw_en = 1'b1; end
        case (bws_e)
            BS_RI:   bw_data = getri_src_val;
            BS_ALU:  bw_data = alu_res;
            BS_RB:   bw_data = pend ? tmp8 : rbyte;   // the read byte: deferred, its latch
            BS_CB:   bw_data = cbres;
            BS_A16:  bw_data = a16[7:0];              // the 16-bit sum's low byte
            BS_PH:   bw_data = ph_val;                // ... its high byte, held
            default: bw_data = b_dec;
        endcase
    end
    // flag port
    reg  [7:0]  f_data;
    always @* begin
        case (fs_e)
            FS_ALU:  f_data = alu_fout;
            FS_A16:  f_data = a16_f;
            // Banks 2018 LDIR/LDDR repeat: YF / XF from PC.13 / PC.11
            FS_BLD:  f_data = (alu_fout & ~(`Z80_YF | `Z80_XF)) | (r_pc[15:8] & (`Z80_YF | `Z80_XF));
            // ... CPIR/CPDR repeat: same fold onto the F CPI left
            FS_BCP:  f_data = (F_cur & ~(`Z80_YF | `Z80_XF)) | (r_pc[15:8] & (`Z80_YF | `Z80_XF));
            // ... INIR/INDR/OTIR/OTDR repeat: full recompute (idu = PC-2)
            default: f_data = banks_io_rep_f(tmp8,
                                  r_bc[15:8],          // B-1 came at M3 T2
                                  (aux_w[2:1] == 2'd2) ? bk_ini : bk_out,
                                  r_pc[15:8]);         // at fire: PC rewound
        endcase
    end

    // F after a RESET cut that ends on the next M1's (INTA's) T2 (cmodel
    // f_cut; docs/timing.md "Register write-back"): the die has begun F's
    // write-back and makes it in the restart's first M1, from its flag
    // latches as the cut left them -- S and Z wait for that T2 (Z preset),
    // P/V holds the parity of the result's bits 0-2, X / Y the internal bus.
    localparam [7:0] F_S = `Z80_SF, F_Z = `Z80_ZF, F_H = `Z80_HF, F_P = `Z80_PF,
                     F_N = `Z80_NF, F_C = `Z80_CF, F_XY = `Z80_YF | `Z80_XF;
    wire [7:0] fc_p3   = (alu_res[0] ^ alu_res[1] ^ alu_res[2]) ? 8'h00 : F_P;
    wire       fc_mem  = (exec_w == `EXEC_INC_M) || (exec_w == `EXEC_DEC_M) ||
                         (exec_w == `EXEC_CB_M) || (exec_w == `EXEC_DDCB);
    wire       fc_bit  = (alu_md == `FLAG_BIT);
    wire [7:0] fc_bitp = ((alu_b & (8'h01 << alu_bit) & 8'h07) != 8'h00) ? 8'h00 : F_P;
    wire [7:0] fc_daa  = f_data[1] ? (alu_a - alu_res) : (alu_res - alu_a);      // the correction
    // ADD / ADC / SBC HL: S Z C from the low bytes, P/V their overflow XOR the
    // high byte's bits 0-2
    wire [8:0] fc_lo9  = {1'b0, rf_hlp[7:0]} + {1'b0, a16_y[7:0]} + {8'd0, a16_cy};
    wire       fc_clo  = fc_lo9[8] ^ a16_sub;
    wire       fc_vlo  = a16_sub ? (((rf_hlp[7:0] ^ rf_rp[7:0]) & (rf_hlp[7:0] ^ fc_lo9[7:0])) >> 7)
                                 : ((~(rf_hlp[7:0] ^ rf_rp[7:0]) & (rf_hlp[7:0] ^ fc_lo9[7:0])) >> 7);
    wire       fc_phi  = fc_vlo ^ a16[8] ^ a16[9] ^ a16[10];
    // ADD / ADC / SBC HL: F after a cut on the next M1's T2, formed with the
    // sum (M2's end) and held: ADD's C still the low bytes'; ADC / SBC's S Z
    // C the low byte's, P/V its overflow XOR the high byte's bits 0-2
    wire [7:0] pf_cut_d = (exec_w == `EXEC_ADD_HL_RP) ? ((a16_f & ~F_C) | (fc_lo9[8] ? F_C : 8'h00))
        : ((fc_lo9[7:0] & F_S) | ((fc_lo9[7:0] == 8'h00) ? F_Z : 8'h00) | (fc_phi ? F_P : 8'h00)
           | (fc_clo ? F_C : 8'h00) | (a16_f & (F_XY | F_H | F_N)));
    reg  [7:0] f_cut;
    always @* begin
        case (exec_w)
            `EXEC_ALU_R, `EXEC_ALU_N, `EXEC_ALU_M:
                f_cut = (F_cur & (F_S | F_C)) | F_Z | fc_p3 | (f_data & (F_H | F_N)) | (alu_b & F_XY);
            `EXEC_INC_R, `EXEC_DEC_R, `EXEC_CB_R, `EXEC_INC_M, `EXEC_DEC_M, `EXEC_CB_M, `EXEC_DDCB:
                f_cut = fc_bit ? ((F_cur & F_S) | F_Z | fc_bitp | (f_data & ~(F_S | F_Z | F_P)))
                               : ((F_cur & F_S) | F_Z | fc_p3 | (f_data & (F_H | F_N | F_C))
                                  | ((fc_mem ? alu_res : alu_b) & F_XY));
            `EXEC_ROT_A:   f_cut = (f_data & ~F_XY) | (alu_a & F_XY);
            `EXEC_DAA:     f_cut = (F_cur & F_S) | F_Z | fc_p3 | (f_data & (F_H | F_N | F_C)) | (fc_daa & F_XY);
            `EXEC_CPL:     f_cut = (f_data & ~F_XY) | ((alu_a | F_cur) & F_XY);
            `EXEC_SCF, `EXEC_CCF:
                f_cut = (f_data & ~(F_H | F_C)) | (alu_a[3] ? F_H : 8'h00) | (F_cur & F_C);
            `EXEC_IN_C:    f_cut = (F_cur & F_S) | F_Z | fc_p3 | (f_data & (F_XY | F_H | F_N | F_C));
            `EXEC_NEG:     f_cut = (F_cur & (F_S | F_C)) | F_Z | fc_p3 | (f_data & (F_H | F_N)) | ((alu_a | F_cur) & F_XY);
            `EXEC_RRD, `EXEC_RLD:
                f_cut = (F_cur & F_S) | F_Z | fc_p3 | (f_data & (F_H | F_N | F_C)) | (rrd_mem & F_XY);
            `EXEC_LD_A_IR: f_cut = (F_cur & F_S) | F_Z | fc_p3 | (f_data & (F_H | F_N | F_C)) | (aux_w[0] ? `Z80_XF : 8'h00);
            // every flag latched before the next M1 but H: LDI's from A + data,
            // INI's from B-1 once more, OUTI's k's carry where B-1 borrowed
            // from bit 4
            `EXEC_BLOCK:
                f_cut = (f_data & ~F_H)
                        | ((aux_w[2:1] == 2'd0) ? (((alu_a[3:0] + tmp8[3:0]) & 5'h10) != 5'h00 ? F_H : 8'h00) :
                           (aux_w[2:1] == 2'd2) ? ((alu_b[3:0] == 4'h0) ? F_H : 8'h00) :
                           (aux_w[2:1] == 2'd3) ? ((alu_b[3:0] == 4'hF && bk_out[8]) ? F_H : 8'h00) : 8'h00);
            default:       f_cut = F_cur;
        endcase
    end

    // PC / SP / WZ loads. The SP-1 of a push's first byte belongs, on the
    // die, to the falling edge that ends the M-cycle before it (M1 T5, the
    // NMI / INT response, CALL's extra T) -- the edge a RESET cut blocks
    // (perfectz80) -- so it is dropped when this T-state is the last before
    // the reset idle. The second byte's SP-1 happens in T2.N of the first
    // write on the die, before any cut that leaves that write's T3 standing.
    wire        push_cut = res2 && !rst_idle && ctl_mstart && (bus_op != `BUSOP_MWR) &&
                           (ctl_sp == SP_IDU) && (ctl_inc == INC_M1);
    reg         pc_we, sp_we, wz_we;
    reg  [15:0] pc_d, sp_d, wz_d;
    always @* begin
        pc_we = 1'b1; sp_we = !push_cut; wz_we = 1'b1;
        case (ctl_pc)
            PC_IDU:  pc_d = idu;
            PC_EA:   pc_d = ea;
            PC_NN:   pc_d = {rbyte, r_wz[7:0]};
            PC_ABUS: pc_d = abus;
            PC_VEC:  pc_d = vec;
            default: begin pc_we = 1'b0; pc_d = idu; end
        endcase
        case (ctl_sp)
            SP_IDU:  sp_d = idu;
            SP_ABUS: sp_d = abus;
            default: begin sp_we = 1'b0; sp_d = idu; end
        endcase
        case (ctl_wz)
            WZ_IDU:  wz_d = idu;
            WZ_ALO:  wz_d = {A_cur, idu[7:0]};
            WZ_EA:   wz_d = ea;
            WZ_Z:    wz_d = {r_wz[15:8], rbyte};
            WZ_W:    wz_d = {rbyte, r_wz[7:0]};
            WZ_IV:   wz_d = {reg_i, tmp8};
            WZ_VEC:  wz_d = vec;
            default: begin wz_we = 1'b0; wz_d = idu; end
        endcase
    end

    // logical register view (RFP_* order) for testbenches, as the program
    // sees it: with the writes the die makes in the next M1 (pend) applied
    // -- they land before anything reads them
    reg  [15:0] rfl_bc, rfl_de, rfl_hl, rfl_af, rfl_ix, rfl_iy, rfl_bc2, rfl_de2, rfl_hl2, rfl_af2, rfl_t;
    always @* begin
        rfl_bc = r_bc; rfl_de = r_de; rfl_hl = r_hl; rfl_af = r_af; rfl_ix = r_ix; rfl_iy = r_iy; rfl_t = 16'h0000;
        if (pend && fire_bw && bw_en) begin
            case (bw_pair)
                `RFP_BC: if (bw_hi) rfl_bc[15:8] = bw_data; else rfl_bc[7:0] = bw_data;
                `RFP_DE: if (bw_hi) rfl_de[15:8] = bw_data; else rfl_de[7:0] = bw_data;
                `RFP_HL: if (bw_hi) rfl_hl[15:8] = bw_data; else rfl_hl[7:0] = bw_data;
                `RFP_AF: if (bw_hi) rfl_af[15:8] = bw_data; else rfl_af[7:0] = bw_data;
                `RFP_IX: if (bw_hi) rfl_ix[15:8] = bw_data; else rfl_ix[7:0] = bw_data;
                `RFP_IY: if (bw_hi) rfl_iy[15:8] = bw_data; else rfl_iy[7:0] = bw_data;
                default: ;
            endcase
        end
        if ((pend && fire_fw) || pend1) rfl_af[7:0] = f_data;
        if (pf16) rfl_af[7:0] = pf_val;
        rfl_bc2 = r_bc2; rfl_de2 = r_de2; rfl_hl2 = r_hl2; rfl_af2 = r_af2;
        if (pex_dh) begin rfl_t = rfl_de; rfl_de = rfl_hl; rfl_hl = rfl_t; end
        if (pex_x) begin
            rfl_t = rfl_bc; rfl_bc = rfl_bc2; rfl_bc2 = rfl_t;
            rfl_t = rfl_de; rfl_de = rfl_de2; rfl_de2 = rfl_t;
            rfl_t = rfl_hl; rfl_hl = rfl_hl2; rfl_hl2 = rfl_t;
        end
        if (pex_af) begin rfl_t = rfl_af; rfl_af = rfl_af2; rfl_af2 = rfl_t; end
    end
    wire [15:0] rfl [0:12] /* verilator public_flat_rd */;
    assign rfl[`RFP_BC]  = rfl_bc; assign rfl[`RFP_DE]  = rfl_de;
    assign rfl[`RFP_HL]  = rfl_hl; assign rfl[`RFP_AF]  = rfl_af;
    assign rfl[`RFP_BC2] = rfl_bc2; assign rfl[`RFP_DE2] = rfl_de2;
    assign rfl[`RFP_HL2] = rfl_hl2; assign rfl[`RFP_AF2] = rfl_af2;
    assign rfl[`RFP_IX]  = rfl_ix; assign rfl[`RFP_IY]  = rfl_iy;
    assign rfl[`RFP_SP]  = r_sp;   assign rfl[`RFP_PC]  = r_pc;
    assign rfl[`RFP_WZ]  = r_wz;

    wire [15:0] pc_next;        // PC after this edge (next M1 address)
    // every port but the deferred writes moves on an advancing edge (and on a
    // cut that ends inside a WAIT: the T2 micro-ops' writes stand)
    wire we_norm = wr_edge || stall_cut;
    // the WZ bytes waiting for this M-cycle land at its end, or at a RESET
    // cut ending on a later T-state (in a Tw after T2: a T2 byte too); the
    // ones the execute step writes from a read are blocked by a cut ending
    // on that read's T3 (ctl_wzc)
    wire wz_cut_tw = in_tw && (t_state == 4'd2);
    wire wzl_z = (wzp_kz != 3'd0) && ((ex_edge && !cut) ||
                 (cut && ((wzp_kz < t_state) || (wz_cut_tw && wzp_kz == 3'd2))));
    wire wzl_w = (wzp_kw != 3'd0) && ((ex_edge && !cut) ||
                 (cut && ((wzp_kw < t_state) || (wz_cut_tw && wzp_kw == 3'd2))));
    wire wzi    = wz_we && we_norm && !(ctl_wzc && cut);
    wire wzi_z  = wzi && ctl_wzm[0];
    wire wzi_w  = wzi && ctl_wzm[1];
    wire [15:0] wz_wd = {wzi_w ? wz_d[15:8] : wzl_w ? wzp_v[15:8] : r_wz[15:8],
                         wzi_z ? wz_d[7:0]  : wzl_z ? wzp_v[7:0]  : r_wz[7:0]};
    // the value a wz_later loads
    reg [15:0] wzp_src;
    always @* begin
        case (ctl_wzp)
            WZP_IDU:  wzp_src = idu;
            WZP_EA:   wzp_src = ea;
            WZP_A:    wzp_src = {A_cur, 8'h00};
            WZP_QFA:  wzp_src = {(reg_q ^ F_cur) | A_cur, 8'h00};
            WZP_IV:   wzp_src = {reg_i, tmp8};
            default:  wzp_src = wzp_v;            // WZP_KEEP
        endcase
    end
    z80_regfile u_regs (
        .clk(clk), .por_n(por_n), .pc_clr(pc_clr), .we(we_norm || fire || fire1 || fire16), .ex_en(1'b1),
        .ex_dehl(pex_dh && e2 && !cut), .ex_af(pex_af && e1 && !cut), .exx(pex_x && e2 && !cut),
        .pw_we(ctl_pw && we_norm && !(ctl_pw_late && cut && !in_tw)), .pw_sel(pw_idx), .pw_d(pw_data),
        .bw_we(fire ? (fire_bw && bw_en && !cut)
                    : (ctl_bw && bw_en && we_norm && !defer_now &&
                       !(cut && ex_edge && (irq_seq == 2'd0) && (rd_cls || (a16_cls && m_cycle == 3'd3))))),
        .bw_sel(bw_pair), .bw_hi(bw_hi), .bw_d(bw_data),
        .f_we(fire ? fire_fw : fire1 ? !cut : fire16 ? 1'b1 : (ctl_fw && we_norm && !defer_now)),
        .f_d(fire16 ? (cut ? pf_cut : pf_val) : (fire && cut) ? f_cut : f_data),
        .pc_we(pc_we && we_norm), .pc_d(pc_d), .sp_we(sp_we && we_norm), .sp_d(sp_d),
        .wz_we(wzi_z || wzi_w || wzl_z || wzl_w), .wz_d(wz_wd),
        .bc(r_bc), .de(r_de), .hl(r_hl), .af(r_af),
        .bc2(r_bc2), .de2(r_de2), .hl2(r_hl2), .af2(r_af2),
        .ix(r_ix), .iy(r_iy), .sp(r_sp), .pc(r_pc), .wz(r_wz),
        .pc_next(pc_next)
    );

    // =======================================================================
    // sequencer
    // =======================================================================
    always @* begin
        reg_i_n = reg_i; reg_r_n = reg_r; ir_n = ir;
        t_n = t_state; m_cycle_n = m_cycle;
        bus_op_n = bus_op; m_len_n = m_len; m_addr_n = m_addr;
        m_wdata_n = wd_ub ? wd_ub_val : m_wdata;     // the u bus's byte stays latched
        prefix_n = prefix; iff1_n = iff1; iff2_n = iff2; im_n = im; halted_n = halted;
        tmp8_n = tmp8; tmph_n = tmph;
        reg_q_n = reg_q;
        wzp_v_n = wzp_v; wzp_kz_n = wzp_kz; wzp_kw_n = wzp_kw;
        // an M-cycle's end lands its WZ bytes (above) and the execute step
        // may leave the next M-cycle's
        if (ex_edge) begin
            wzp_kz_n = 3'd0; wzp_kw_n = 3'd0;
            if (ctl_wzp != WZP_NONE) begin wzp_v_n = wzp_src; wzp_kz_n = ctl_wzkz; wzp_kw_n = ctl_wzkw; end
        end
        pend_n = pend; pend1_n = pend1; pend_rep_n = pend_rep; pend_pv_n = pend_pv;
        pf16_n = pf16; pf_val_n = pf_val; pf_cut_n = pf_cut; ph_val_n = ph_val;
        pex_af_n = pex_af; pex_x_n = pex_x; pex_dh_n = pex_dh;
        // the deferred writes land (a RESET cut ending here blocks them, but
        // F after a cut on T2): the decode held for them goes, and Q is the
        // F written, or 0 -- after a cut too: flags the die has taken for
        // this M1 stay pending through the RESET, and the M1 that restarts
        // writes them; a cut ending on T1 loses them (perfectz80)
        if (fire)  begin pend_n = 1'b0;  prefix_n = `PFX_NONE; reg_q_n = fire_fw ? (cut ? f_cut : f_data) : 8'h00; end
        if (fire1) begin pend1_n = 1'b0; prefix_n = `PFX_NONE; reg_q_n = cut ? 8'h00 : f_data; end
        if (fire16) begin pf16_n = 1'b0; reg_q_n = cut ? pf_cut : pf_val; end
        if (e1) pex_af_n = 1'b0;
        if (e2) begin pex_x_n = 1'b0; pex_dh_n = 1'b0; end
        // ADD / ADC / SBC HL: the sum at the first internal M-cycle's end --
        // L there, H at the second's end, F in the next M1
        if (ex_edge && a16_cls && (m_cycle == 3'd2) && (irq_seq == 2'd0)) begin
            ph_val_n = a16[15:8]; pf_val_n = a16_f; pf_cut_n = pf_cut_d;
        end
        ei_delay_n = ei_delay; suppress_decode_n = suppress_decode;
        bus_granted_n = bus_granted; irq_seq_n = irq_seq; im0_ins_n = im0_ins;
        nmi_sampled_n = nmi_sampled; int_sampled_n = int_sampled;
        nmi_pending_n = nmi_pending; prev_nmi_n_n = prev_nmi_n;
        allow_int = 1'b0;
        nmi_pending_mid = nmi_pending;
        res2_n = res2; sres_n = sres; rst_idle_n = rst_idle; srst_m1_n = srst_m1;
        in_tw_n = stall && !bus_granted;    // the T-state the next edge starts is a Tw

        // ===== RESET (perfectz80; docs/timing.md "RESET") =====
        // RES2 takes the pin sample (res_s, from this T-state's rising edge)
        // on this T-state's falling edge, except in the low half of an M1's
        // T2 (INTA's too), where it holds; there the special-reset latch
        // takes it instead. RES2 clears that latch.
        res2_n = m1_t2 ? res2 : res_s;
        sres_n = res2_n ? 1'b0 : (m1_t2 ? res_s : sres);

        // NMI is edge-triggered: a falling edge seen between the previous
        // rising edge and this T-state's falling edge (mid), or between that
        // and now. Not while the full reset RES2 is set -- such an edge is
        // lost -- but the pin is still followed, so once RES2 clears an edge
        // in the last idle T-state before the restart is taken (perfectz80;
        // cmodel). res2 is RES2 in this T-state's high half, res2_n from its
        // falling edge on.
        nmi_pending_mid = nmi_pending | (prev_nmi_n & ~nmi_n_neg & ~res2);
        prev_nmi_n_n    = nmi_n;
        nmi_pending_n   = nmi_pending_mid | (nmi_n_neg & ~nmi_n & ~res2_n);

        if (rst_idle) begin
            // the T-state ring has stopped: T1 of an M1 at PC (0000) follows
            // once RES2 was clear on the previous falling edge -- the third
            // rising edge after the pin is released
            if (!res2) begin
                rst_idle_n = 1'b0;
                bus_op_n = `BUSOP_M1; m_addr_n = r_pc; m_wdata_n = 8'h00;
                m_len_n = 4'd4; t_n = 4'd1; m_cycle_n = 3'd1;
            end
        end else begin

        // Everything below is one T-state of work.

        // ===== clock-low-half work: what the chip did on the FALLING edge
        // of this T-state. Inputs are the *_neg copies z80_top captured on
        // that edge. =====
        // NMI/INT silicon sample point: the rising edge that starts the
        // last T-state of the current M-cycle (Zilog PS0178 Figure 8,
        // TsINT/ThINT; perfectz80): the INT level z80_top latched on that
        // edge, and the NMI edges seen up to it (nmi_pending as that edge
        // left it). Refreshed once per M-cycle; the last M-cycle's sample
        // is the one the instruction boundary uses to accept.
        if (t_state == m_len && !bus_granted) begin
            nmi_sampled_n = nmi_pending;
            int_sampled_n = ~int_n_rise;
        end

        // read-data latch at the falling edge of T3 (MRD) / T4 (IORD)
        if (latch_neg) tmp8_n = data_in_neg;

        // ===== clock-high-half work: what the chip does on THIS rising
        // edge. Inputs are the live pins. =====
        // During the NMI response (its M1 and the two pushes) the chip
        // holds its NMI flip-flop clear: an NMI edge there is lost, even
        // when NMI then stays low (perfectz80; event sweep nmir_entry).
        // An edge from the handler's first T-state on is kept.
        if (irq_seq == 2'd1) nmi_pending_n = 1'b0;

        // M1 opcode / INTA vector latch at the end of T2.N. Not while WAIT
        // is holding T2 as a Tw -- the opcode is captured on the rising
        // edge that ends the LAST Tw (mirrors cmodel `!stalled`). PC+1 of
        // the fetch is an IDU job of the same edge (execute, mid-M-cycle).
        if (latch_pos && !stall) begin
            tmp8_n  = data_in;
            if (bus_op == `BUSOP_M1) begin
                ir_n = data_in;
                if (suppress_decode) suppress_decode_n = 1'b0;     // ack / halt: no PC++ / decode
                // special reset (perfectz80): the fetch after the one whose
                // T2 rising edge saw RESET runs as a NOP that leaves PC at
                // 0000; I, R, the IFFs and IM are kept
                if (srst_m1) begin ir_n = 8'h00; srst_m1_n = 1'b0; end
            end else if (im == 2'd0) begin                         // INTA
                // IM0: the device supplies the opcode, decoded without a
                // fetch / PC++. Until the instruction ends, PC stays where
                // it is: the operands are read at PC, and the opcode after
                // a prefix comes in another INTA cycle (perfectz80).
                ir_n = data_in; irq_seq_n = 2'd0; im0_ins_n = 1'b1;
            end
        end

        // refresh counter: R steps (low 7 bits) on the rising edge that
        // ends the refresh -- T4 of an M1, the second refresh T-state of an
        // INTA -- so the refresh address shows I:R from before the step, as
        // on silicon (perfectz80). Ahead of the execute step, so LD R,A's
        // write wins.
        if (rfsh_end && !bus_granted) reg_r_n = {reg_r[7], (reg_r[6:0] + 7'd1)};

        // bus grant (DMA, perfectz80 / PS0178 Figure 10): the M-cycle that
        // follows the grant is set up and held, the bus idles with BUSACK
        // low. BUSREQ seen high on this T-state's rising edge releases it:
        // BUSACK rises on the falling edge, the held M-cycle starts T1 on
        // the next rising edge.
        if (bus_granted) begin
            if (busreq_n) bus_granted_n = 1'b0;
        end
        // WAIT sampled low: hold this T-state as a Tw
        else if (stall) begin
        end
        // advance one T-state
        else if (!m_end) begin
            t_n = t_state + 4'd1;
        end
        // last T-state of the M-cycle: the execute step
        else begin
            // a prefix's M1 takes the last instruction's flags and writes
            // none: Q is 0 for the opcode after it
            if (ctl_prefix) begin prefix_n = pfx_next; reg_q_n = 8'h00; end
            if (ctl_tmph) tmph_n = rbyte;
            case (ctl_iff)
                IFF_DI:   begin iff1_n = 1'b0; iff2_n = 1'b0; end
                IFF_EI:   begin iff1_n = 1'b1; iff2_n = 1'b1; ei_delay_n = 1'b1; end
                // RETN / RETI copy IFF2 to IFF1 after this boundary's INT
                // decision, which still sees the old IFF1: INT held in an
                // NMI handler is taken one instruction after RETN
                // (perfectz80). IFF1 implies IFF2, so that is EI's block
                // whenever RETN raises IFF1.
                IFF_RETN: begin iff1_n = iff2; ei_delay_n = ~iff1; end
                default:  ;
            endcase
            if (ctl_im)    im_n = aux_w[1:0];
            if (ctl_halt)  halted_n = 1'b1;
            if (ctl_ld_i)  reg_i_n = A_cur;
            if (ctl_ld_r)  reg_r_n = A_cur;

            if (ctl_mstart) begin
                bus_op_n  = ctl_bop;
                m_addr_n  = al_in;
                m_wdata_n = wdata;
                m_len_n   = baselen(ctl_bop) + ctl_mx;
                t_n       = 4'd1;
                m_cycle_n = m_cycle + 3'd1;
            end
            if (ctl_m1) begin
                bus_op_n = `BUSOP_M1; m_addr_n = r_pc; m_wdata_n = 8'h0;
                m_len_n = 4'd4; t_n = 4'd1; m_cycle_n = 3'd1;
                // IM0: the device hands over the opcode after a prefix
                if (im0_ins) begin bus_op_n = `BUSOP_INTA; m_len_n = 4'd6; end
            end

            if (ctl_fin) begin
                // a deferred instruction keeps its decode and Q until fire
                prefix_n = (defer_now || defer1_now) ? prefix : `PFX_NONE; im0_ins_n = 1'b0;
                // Q: F if the instruction wrote F, else 0 (Sean Young 4.1; z80full
                // 007 SCF+CCF, 102/103 INIR/INDR->NOP') -- the instructions that
                // write F commit it with their F write (fire, fire1, fire16);
                // the rest, EX AF,AF' among them, write none
                reg_q_n = (defer_now || defer1_now || a16_now) ? reg_q : 8'h00;
                if (defer_now)  begin pend_n = 1'b1; pend_pv_n = iff2 & ~ldair_int; pend_rep_n = blk_rep_fin; end
                if (defer1_now) begin pend1_n = 1'b1; pend_rep_n = blk_rep_fin; end
                if (a16_now) pf16_n = 1'b1;
                if (fin_ok) begin pex_af_n = ctl_ex_af; pex_x_n = ctl_exx; pex_dh_n = ctl_ex_dehl; end
                // begin_next: decide NMI / INT / HALT / next opcode
                begin
                    allow_int = ~ei_delay_n;         // ei_delay_n reflects EI this cycle
                    ei_delay_n = 1'b0;
                    // Accept based on the SAMPLED interrupt state (latched at
                    // T_last.P), not the live signals. The sampled latches
                    // freeze the silicon's accept decision one phase before
                    // the boundary; that matches Zilog UM0080 NMI/INT timing.
                    if (nmi_sampled_n) begin
                        nmi_sampled_n = 1'b0;
                        nmi_pending_n = 1'b0;
                        // Silicon-faithful HALT exit (Brewer 2014 /
                        // Woodmass HALT2INT): PC is ALREADY past the HALT
                        // byte. Just clear halted_n; no PC bump.
                        halted_n = 1'b0;
                        // IFF1 off, IFF2 as EI / DI / RETN left it
                        // (perfectz80): a nested NMI keeps the enable the
                        // RETNs bring back (cmodel)
                        iff1_n = 1'b0;
                        irq_seq_n = 2'd1;
                        bus_op_n = `BUSOP_M1; m_addr_n = pc_next; m_wdata_n = 8'h0;
                        m_len_n = 4'd5; t_n = 4'd1; m_cycle_n = 3'd1;
                        suppress_decode_n = 1'b1;
                    end else if (allow_int && int_sampled_n && iff1_n) begin
                        int_sampled_n = 1'b0;
                        // Same: silicon-faithful HALT exit, no PC bump.
                        halted_n = 1'b0; iff1_n = 1'b0; iff2_n = 1'b0;
                        irq_seq_n = 2'd2;
                        bus_op_n = `BUSOP_INTA; m_addr_n = pc_next; m_wdata_n = 8'h0;
                        // T1 T2 Tw Tw T3 T4 (+1 internal state for IM1/IM2's PC push),
                        // in the mode the instruction just ending set: an IM n
                        // taking effect at this boundary (perfectz80, op_ed46_int)
                        m_len_n = (im_n == 2'd0) ? 4'd6 : 4'd7; t_n = 4'd1; m_cycle_n = 3'd1;
                    end else if (halted_n) begin
                        irq_seq_n = 2'd3;
                        bus_op_n = `BUSOP_M1; m_addr_n = pc_next; m_wdata_n = 8'h0;
                        m_len_n = 4'd4; t_n = 4'd1; m_cycle_n = 3'd1;
                        suppress_decode_n = 1'b1;
                        srst_m1_n = sres_n;
                    end else begin
                        irq_seq_n = 2'd0;
                        bus_op_n = `BUSOP_M1; m_addr_n = pc_next; m_wdata_n = 8'h0;
                        m_len_n = 4'd4; t_n = 4'd1; m_cycle_n = 3'd1;
                        srst_m1_n = sres_n;
                    end
                end
            end
            // BUSREQ seen low on the rising edge that started this, the
            // M-cycle's last T-state: the bus goes to the DMA master after
            // it, at any M-cycle boundary -- the next M-cycle (or the next
            // instruction, or the interrupt response) waits for the release.
            // A 1- or 2-T internal cycle extends this M-cycle on the die (an
            // M1's T5 / T6, CALL's 4-T read): no boundary before it.
            if (!busreq_n && !((bus_op_n == `BUSOP_INTERNAL) && (m_len_n <= 4'd2)))
                bus_granted_n = 1'b1;
        end
        end  // !rst_idle

        // RES2 was set on the previous falling edge: this T-state was the
        // last. What it did stands, but no T-state follows it; the reset
        // clears PC, I, R, IFF1 / IFF2, IM and HALT and keeps every other
        // register, WZ and the exchange flip-flops (perfectz80). The clears
        // repeat on every idle edge while RES2 stays set, so RESET alone
        // brings a chip without power-on reset (por_n tied off) to a known
        // state. The die finishes some results in the next M1's T1-T3 (A
        // after INC A, DE after POP DE, EX DE,HL), which a cut there loses
        // and RZ80 keeps: docs/known-differences.md row 19.
        if (res2) begin
            rst_idle_n = 1'b1;
            reg_i_n = 8'h00; reg_r_n = 8'h00;
            iff1_n = 1'b0; iff2_n = 1'b0; im_n = 2'd0; halted_n = 1'b0;
            prefix_n = `PFX_NONE; irq_seq_n = 2'd0; suppress_decode_n = 1'b0; im0_ins_n = 1'b0;
            ei_delay_n = 1'b0; nmi_sampled_n = 1'b0; int_sampled_n = 1'b0;
            nmi_pending_n = 1'b0; bus_granted_n = 1'b0; srst_m1_n = 1'b0;
            in_tw_n = 1'b0;
            pend_n = 1'b0; pend1_n = 1'b0; pf16_n = 1'b0; pex_af_n = 1'b0; pex_x_n = 1'b0; pex_dh_n = 1'b0;
            wzp_kz_n = 3'd0; wzp_kw_n = 3'd0;
            // Q: the flags stay pending through the RESET only when the cut
            // ends after the T1 of the M1 they wait for, which took them in
            // its T1.N, and before its T3.N writes them -- for an INTA, by
            // its second automatic Tw; or on the T1 of one with nothing
            // waiting to be written, whose T1.N takes no new flags (the M1
            // a restart begins, cut again); else none are pending at the
            // restart (perfectz80)
            if (!rst_idle && !(m1_any && !bus_granted &&
                               (((t_state >= 4'd2) &&
                                 (t_state <= ((bus_op == `BUSOP_INTA) ? 4'd4 : 4'd3))) ||
                                ((t_state == 4'd1) &&
                                 !(pend | pend1 | pf16 | pex_af | pex_x | pex_dh)))))
                reg_q_n = 8'h00;
        end
    end

    // =======================================================================
    // registers
    // =======================================================================
    // The power-on reset drives every FF to a constant (async-reset DFF
    // cells; gate-level sim has no initializers) and leaves the core in the
    // RESET idle (res2, rst_idle), as if the pin had been held: T1 of the
    // first M1 comes on the third rising edge after RESET is seen high.
    // The RESET pin itself acts synchronously, through res_s (above).
    always @(posedge clk or negedge por_n) begin
        if (!por_n) begin
            reg_i <= 8'h00; reg_r <= 8'h00; ir <= 8'h00;
            t_state <= 4'd1; m_cycle <= 3'd1;
            bus_op <= `BUSOP_M1; m_len <= 4'd4; m_addr <= 16'h0000; m_wdata <= 8'h00;
            prefix <= `PFX_NONE; iff1 <= 1'b0; iff2 <= 1'b0; im <= 2'd0; halted <= 1'b0;
            tmp8 <= 8'h00; tmph <= 8'h00;
            nmi_pending <= 1'b0; prev_nmi_n <= 1'b1; ei_delay <= 1'b0;
            nmi_sampled <= 1'b0; int_sampled <= 1'b0;
            suppress_decode <= 1'b0; bus_granted <= 1'b0; irq_seq <= 2'd0; im0_ins <= 1'b0;
            reg_q <= 8'h00;
            wzp_v <= 16'h0000; wzp_kz <= 3'd0; wzp_kw <= 3'd0;
            pend <= 1'b0; pend1 <= 1'b0; pend_rep <= 1'b0; pend_pv <= 1'b0; pf16 <= 1'b0;
            pf_val <= 8'h00; pf_cut <= 8'h00; ph_val <= 8'h00;
            pex_af <= 1'b0; pex_x <= 1'b0; pex_dh <= 1'b0;
            res2 <= 1'b1; sres <= 1'b0; rst_idle <= 1'b1; srst_m1 <= 1'b0; rd_hold <= 1'b0;
            in_tw <= 1'b0;
            addr_hold <= 16'h0000;
        end else begin
            addr_hold <= addr_neg;
            res2 <= res2_n; sres <= sres_n; rst_idle <= rst_idle_n; srst_m1 <= srst_m1_n;
            in_tw <= in_tw_n;
            rd_hold <= cut_t1;
            reg_i <= reg_i_n; reg_r <= reg_r_n; ir <= ir_n;
            t_state <= t_n; m_cycle <= m_cycle_n;
            bus_op <= bus_op_n; m_len <= m_len_n; m_addr <= m_addr_n; m_wdata <= m_wdata_n;
            prefix <= prefix_n; iff1 <= iff1_n; iff2 <= iff2_n; im <= im_n; halted <= halted_n;
            tmp8 <= tmp8_n; tmph <= tmph_n;
            nmi_pending <= nmi_pending_n; prev_nmi_n <= prev_nmi_n_n; ei_delay <= ei_delay_n;
            nmi_sampled <= nmi_sampled_n; int_sampled <= int_sampled_n;
            suppress_decode <= suppress_decode_n; bus_granted <= bus_granted_n; irq_seq <= irq_seq_n;
            im0_ins <= im0_ins_n;
            reg_q <= reg_q_n;
            wzp_v <= wzp_v_n; wzp_kz <= wzp_kz_n; wzp_kw <= wzp_kw_n;
            pend <= pend_n; pend1 <= pend1_n; pend_rep <= pend_rep_n; pend_pv <= pend_pv_n; pf16 <= pf16_n;
            pf_val <= pf_val_n; pf_cut <= pf_cut_n; ph_val <= ph_val_n;
            pex_af <= pex_af_n; pex_x <= pex_x_n; pex_dh <= pex_dh_n;
        end
    end

endmodule
