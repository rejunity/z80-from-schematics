// ===========================================================================
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
    // debug (for trace)
    output wire [3:0]  dbg_t,
    output wire [2:0]  dbg_m
);

    // =======================================================================
    // registered state
    // =======================================================================
    reg [7:0]  reg_i, reg_r, ir;
    reg [7:0]  reg_q;          // Q: F left by the previous F-modifying instruction, else 0
    reg        f_modified;     // set this instruction if anything wrote F
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
    // RESET (perfectz80): res2 is the die's full reset, taken from res_s on
    // each falling edge except in the low half of an M1's T2, where it
    // holds; while it was set on the previous falling edge no T-state
    // follows the current one (rst_idle) until it clears. sres is the
    // special-reset latch, taken from res_s in that low half; srst_m1
    // marks the opcode fetch it replaces.
    reg        res2, sres, rst_idle, srst_m1;
    reg        rd_hold;        // RD still low from a T1 cut (one idle T-state)
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
    reg        f_modified_n;
    reg        nmi_pending_n, prev_nmi_n_n, ei_delay_n, suppress_decode_n, bus_granted_n;
    reg        nmi_sampled_n, int_sampled_n;
    reg        nmi_pending_mid;   // nmi_pending after the clock-high-half edge detect
    reg [1:0]  irq_seq_n;
    reg        allow_int;
    reg        res2_n, sres_n, rst_idle_n, srst_m1_n;

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
        .m_addr(m_addr), .m_wdata(m_wdata), .reg_i(reg_i), .reg_r(reg_r),
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
    wire m1_t2    = ((bus_op == `BUSOP_M1) || (bus_op == `BUSOP_INTA)) && (t_state == 4'd2) && !rst_idle;
    wire ext      = (bus_op == `BUSOP_INTERNAL) && (m_len <= 4'd2);
    wire int_g    = (bus_op == `BUSOP_INTERNAL) && !ext;
    wire int_g_n  = (bus_op_n == `BUSOP_INTERNAL) && (m_len_n > 4'd2);
    wire cut      = res2 && !rst_idle;
    wire cut_t1   = cut && !ext && (t_state == 4'd1);
    wire cut_t2   = cut && !ext && (t_state == 4'd2);
    wire int_mreq_neg = int_g && (t_state == 4'd1) && !res2 && res2_n && !rst_idle;
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
    // image.
    wire exec_halt_in_m1 = (exec_w == `EXEC_HALT)
                        && (bus_op == `BUSOP_M1)
                        && (t_state == 4'd4);
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
    assign busack_n_neg = (bus_granted && !busreq_n) ? 1'b0 : 1'b1;
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

    // =======================================================================
    // register file: read side
    // =======================================================================
    wire [15:0] r_bc, r_de, r_hl, r_af, r_bc2, r_de2, r_hl2, r_af2;
    wire [15:0] r_ix, r_iy, r_sp, r_pc, r_wz;
    wire [7:0]  A_cur = r_af[15:8];
    wire [7:0]  F_cur = r_af[7:0];
    // logical register view (RFP_* order) for testbenches
    wire [15:0] rfl [0:12] /* verilator public_flat_rd */;
    assign rfl[`RFP_BC]  = r_bc;  assign rfl[`RFP_DE]  = r_de;
    assign rfl[`RFP_HL]  = r_hl;  assign rfl[`RFP_AF]  = r_af;
    assign rfl[`RFP_BC2] = r_bc2; assign rfl[`RFP_DE2] = r_de2;
    assign rfl[`RFP_HL2] = r_hl2; assign rfl[`RFP_AF2] = r_af2;
    assign rfl[`RFP_IX]  = r_ix;  assign rfl[`RFP_IY]  = r_iy;
    assign rfl[`RFP_SP]  = r_sp;  assign rfl[`RFP_PC]  = r_pc;
    assign rfl[`RFP_WZ]  = r_wz;

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
            `EXEC_ALU_N, `EXEC_ALU_M:alu_b = rbyte;
            `EXEC_INC_R, `EXEC_DEC_R:alu_b = getri_dst_val;
            `EXEC_INC_M, `EXEC_DEC_M:alu_b = rbyte;
            `EXEC_CB_R: begin alu_b = r8_src;          alu_xy = r8_src; end
            `EXEC_CB_M: begin alu_b = rbyte;           alu_xy = r_wz[15:8]; end
            `EXEC_DDCB: begin alu_b = rbyte;           alu_xy = r_wz[15:8];
                            alu_md = (tmph[7:6] == `CB_BIT) ? `FLAG_BIT : `FLAG_ROT;
                            alu_rot = tmph[5:3]; alu_bit = tmph[5:3]; end
            `EXEC_NEG: begin alu_md = `FLAG_NEG; alu_a = A_cur; end
            `EXEC_LD_A_IR: begin alu_md = `FLAG_LD_A_I;
                              alu_b = aux_w[0] ? reg_r : reg_i;
                              alu_bit = {2'b0, iff2 & ~ldair_int}; end
            `EXEC_IN_C: begin alu_md = `FLAG_IN; alu_b = rbyte; end
            `EXEC_RRD, `EXEC_RLD: begin
                alu_md = (exec_w == `EXEC_RRD) ? `FLAG_RRD : `FLAG_RLD;
                alu_a = A_cur; alu_b = rbyte;
            end
            `EXEC_BLOCK: begin
                // (cat=aux_w[2:1], dec=aux_w[0]). The flag mode is live only
                // in the M-cycle whose end commits the flags. tmp8 holds the
                // byte LDI moved; rbyte is the byte CPI is reading.
                case (aux_w[2:1])
                    2'd0: if (m_cycle == 3'd3) begin
                        alu_md = `FLAG_BLOCK_LD; alu_a = A_cur; alu_b = tmp8;
                        alu_bit = {2'b0, (r_bc != 16'd0)};    // BC-1 done at M3 T2
                    end
                    2'd1: if (m_cycle == 3'd2) begin
                        alu_md = `FLAG_BLOCK_CP; alu_a = A_cur; alu_b = rbyte;
                        alu_bit = {2'b0, (r_bc != 16'd0)};    // BC-1 done at M2 T2
                    end
                    2'd2: if (m_cycle == 3'd4) begin
                        alu_md = `FLAG_BLOCK_IO;
                        alu_a = tmp8;                      // data
                        alu_b = r_bc[15:8] - 8'd1;         // newB
                        alu_xy = {4'b0, bk_ini[8], bk_ini[2:0]};
                    end
                    default: if (m_cycle == 3'd4) begin   // 2'd3 = OUT
                        alu_md = `FLAG_BLOCK_IO;
                        alu_a = tmp8;                      // data
                        alu_b = r_bc[15:8];                // already-decremented B
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
    wire        ctl_cbx, ctl_cbr;
    wire [2:0]  pfx_next;
    wire [7:0]  b_dec = r_bc[15:8] - 8'd1;      // B-1 (DJNZ, INI, OUTI)

    z80_execute u_exec (
        .m_end(m_end), .latch_pos(latch_pos), .bus_op(bus_op),
        .suppress_decode(suppress_decode), .m_cycle(m_cycle), .t_state(t_state),
        .irq_seq(irq_seq), .eff_exec(eff_exec), .exec_w(exec_w),
        .alu_op_w(alu_op_w), .cb_kind_w(cb_kind_w), .idx_w(idx_w),
        .use_disp_w(use_disp_w), .aux_w(aux_w), .prefix(prefix), .ir(ir),
        .ddcb_kind(tmph[7:6]), .im(im),
        .cc_taken(cc_taken), .zf(F_cur[6]), .bc_nz(r_bc != 16'd0),
        .b_nz(r_bc[15:8] != 8'd0), .b_dec_nz(b_dec != 8'd0),
        .ctl_mstart(ctl_mstart), .ctl_m1(ctl_m1), .ctl_fin(ctl_fin),
        .ctl_bop(ctl_bop), .ctl_mx(ctl_mx), .ctl_al(ctl_al),
        .ctl_ab(ctl_ab), .ctl_ri(ctl_ri), .ctl_inc(ctl_inc),
        .ctl_ea_pc(ctl_ea_pc), .ctl_ea_d(ctl_ea_d),
        .ctl_db(ctl_db), .ctl_bi(ctl_bi), .ctl_bhi(ctl_bhi),
        .ctl_pc(ctl_pc), .ctl_wz(ctl_wz), .ctl_sp(ctl_sp), .ctl_vec(ctl_vec),
        .ctl_pw(ctl_pw), .ctl_pwi(ctl_pwi), .ctl_pws(ctl_pws),
        .ctl_bw(ctl_bw), .ctl_bwd(ctl_bwd), .ctl_bws(ctl_bws),
        .ctl_fw(ctl_fw), .ctl_fs(ctl_fs),
        .ctl_ex_dehl(ctl_ex_dehl), .ctl_ex_af(ctl_ex_af), .ctl_exx(ctl_exx),
        .ctl_tmph(ctl_tmph), .ctl_iff(ctl_iff), .ctl_im(ctl_im),
        .ctl_halt(ctl_halt), .ctl_ld_i(ctl_ld_i), .ctl_ld_r(ctl_ld_r),
        .ctl_prefix(ctl_prefix), .ctl_cbx(ctl_cbx), .ctl_cbr(ctl_cbr),
        .pfx_next(pfx_next)
    );

    // =======================================================================
    // address side
    // =======================================================================
    wire [15:0] abus, idu, ea, al_in, a16;
    z80_addr u_addr (
        .pc(r_pc), .sp(r_sp), .wz(r_wz), .hl(r_hl), .de(r_de), .bc(r_bc),
        .hlp(rf_hlp), .rp(rf_rp), .rbyte(rbyte), .a(A_cur), .i_reg(reg_i),
        .tmp8(tmp8), .b_dec(b_dec), .a16(a16),
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
    wire [1:0]  cb_kind = ctl_cbx ? tmph[7:6] : cb_kind_w;
    wire [2:0]  cb_bit  = ctl_cbx ? tmph[5:3] : bit_index_w;
    wire [7:0]  cb_in   = ctl_cbr ? r8_src : rbyte;
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
    wire [7:0]  rrd_mem = (exec_w == `EXEC_RRD) ? {A_cur[3:0], rbyte[7:4]} : {rbyte[3:0], A_cur[3:0]};

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
    // byte write port: r[y]-style register code (IXH/IXL substitution for
    // BW_RI), A, B, the DDCB copy target, or a byte of HL/IX/IY
    reg  [2:0]  bw_code;
    reg         bw_ix, bw_en, bw_hi;
    reg  [3:0]  bw_pair;
    reg  [7:0]  bw_data;
    always @* begin
        bw_ix = 1'b0;
        case (ctl_bwd)
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
        if (ctl_bwd == BW_HLPL) begin bw_pair = hlp; bw_hi = 1'b0; bw_en = 1'b1; end
        if (ctl_bwd == BW_HLPH) begin bw_pair = hlp; bw_hi = 1'b1; bw_en = 1'b1; end
        if (ctl_bwd == BW_RPL)  begin bw_pair = rp_sel_w; bw_hi = 1'b0; bw_en = 1'b1; end
        if (ctl_bwd == BW_RPH)  begin bw_pair = rp_sel_w; bw_hi = 1'b1; bw_en = 1'b1; end
        case (ctl_bws)
            BS_RI:   bw_data = getri_src_val;
            BS_ALU:  bw_data = alu_res;
            BS_RB:   bw_data = rbyte;
            BS_CB:   bw_data = cbres;
            default: bw_data = b_dec;
        endcase
    end
    // flag port
    reg  [7:0]  f_data;
    always @* begin
        case (ctl_fs)
            FS_ALU:  f_data = alu_fout;
            FS_A16:  f_data = a16_f;
            // Banks 2018 LDIR/LDDR repeat: YF / XF from PC.13 / PC.11
            FS_BLD:  f_data = (alu_fout & ~(`Z80_YF | `Z80_XF)) | (r_pc[15:8] & (`Z80_YF | `Z80_XF));
            // ... CPIR/CPDR repeat: same fold onto the F CPI left
            FS_BCP:  f_data = (F_cur & ~(`Z80_YF | `Z80_XF)) | (r_pc[15:8] & (`Z80_YF | `Z80_XF));
            // ... INIR/INDR/OTIR/OTDR repeat: full recompute (idu = PC-2)
            default: f_data = banks_io_rep_f(tmp8,
                                  (aux_w[2:1] == 2'd2) ? b_dec : r_bc[15:8],
                                  (aux_w[2:1] == 2'd2) ? bk_ini : bk_out,
                                  idu[15:8]);
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

    wire [15:0] pc_next;        // PC after this edge (next M1 address)
    wire [7:0]  f_next;         // F after this edge (Q commit)
    z80_regfile u_regs (
        .clk(clk), .por_n(por_n), .pc_clr(pc_clr), .we(wr_edge), .ex_en(ex_edge),
        .ex_dehl(ctl_ex_dehl), .ex_af(ctl_ex_af), .exx(ctl_exx),
        .pw_we(ctl_pw), .pw_sel(pw_idx), .pw_d(pw_data),
        .bw_we(ctl_bw && bw_en), .bw_sel(bw_pair), .bw_hi(bw_hi), .bw_d(bw_data),
        .f_we(ctl_fw), .f_d(f_data),
        .pc_we(pc_we), .pc_d(pc_d), .sp_we(sp_we), .sp_d(sp_d),
        .wz_we(wz_we), .wz_d(wz_d),
        .bc(r_bc), .de(r_de), .hl(r_hl), .af(r_af),
        .bc2(r_bc2), .de2(r_de2), .hl2(r_hl2), .af2(r_af2),
        .ix(r_ix), .iy(r_iy), .sp(r_sp), .pc(r_pc), .wz(r_wz),
        .pc_next(pc_next), .f_next(f_next)
    );

    // =======================================================================
    // sequencer
    // =======================================================================
    always @* begin
        reg_i_n = reg_i; reg_r_n = reg_r; ir_n = ir;
        t_n = t_state; m_cycle_n = m_cycle;
        bus_op_n = bus_op; m_len_n = m_len; m_addr_n = m_addr; m_wdata_n = m_wdata;
        prefix_n = prefix; iff1_n = iff1; iff2_n = iff2; im_n = im; halted_n = halted;
        tmp8_n = tmp8; tmph_n = tmph;
        reg_q_n = reg_q;
        // Detect an F write this instruction (mirrors c->f_modified in
        // cmodel/z80_core.c): the ALU is in a flag mode, so one of the
        // execute paths commits alu_fout. Q at instruction-done is F if
        // it was written -- even with an unchanged value -- else 0
        // (Sean Young 4.1; z80full 007 SCF+CCF, 102/103 INIR/INDR->NOP').
        f_modified_n = f_modified | (alu_md != `FLAG_NONE);
        ei_delay_n = ei_delay; suppress_decode_n = suppress_decode;
        bus_granted_n = bus_granted; irq_seq_n = irq_seq;
        nmi_sampled_n = nmi_sampled; int_sampled_n = int_sampled;
        nmi_pending_n = nmi_pending; prev_nmi_n_n = prev_nmi_n;
        allow_int = 1'b0;
        nmi_pending_mid = nmi_pending;
        res2_n = res2; sres_n = sres; rst_idle_n = rst_idle; srst_m1_n = srst_m1;

        // ===== RESET (perfectz80; docs/timing.md "RESET") =====
        // RES2 takes the pin sample (res_s, from this T-state's rising edge)
        // on this T-state's falling edge, except in the low half of an M1's
        // T2 (INTA's too), where it holds; there the special-reset latch
        // takes it instead. RES2 clears that latch.
        res2_n = m1_t2 ? res2 : res_s;
        sres_n = res2_n ? 1'b0 : (m1_t2 ? res_s : sres);

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
        // NMI is edge-triggered: falling edge seen between the previous
        // rising edge and this T-state's falling edge.
        nmi_pending_mid = nmi_pending | (prev_nmi_n & ~nmi_n_neg);

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
        // NMI edge detect, second half: falling edge between this T-state's
        // falling edge and now.
        prev_nmi_n_n  = nmi_n;
        nmi_pending_n = nmi_pending_mid | (nmi_n_neg & ~nmi_n);
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
                // IM0: the device supplies the opcode; PC still points at
                // its operands, so decode it without a fetch / PC++.
                ir_n = data_in; irq_seq_n = 2'd0;
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
            if (ctl_prefix) prefix_n = pfx_next;
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
            if (ctl_ex_af) f_modified_n = 1'b1;   // Q sees the swapped-in F

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
            end

            if (ctl_fin) begin
                prefix_n = `PFX_NONE;
                // Commit Q: F if THIS instruction wrote F, else 0 (Sean Young
                // 4.1 + ZEXALL <daa,cpl,scf,ccf>).
                reg_q_n = f_modified_n ? f_next : 8'h00;
                f_modified_n = 1'b0;
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
                        iff2_n = iff1_n; iff1_n = 1'b0;
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
                        // T1 T2 Tw Tw T3 T4 (+1 internal state for IM1/IM2's PC push)
                        m_len_n = (im == 2'd0) ? 4'd6 : 4'd7; t_n = 4'd1; m_cycle_n = 3'd1;
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
            prefix_n = `PFX_NONE; irq_seq_n = 2'd0; suppress_decode_n = 1'b0;
            ei_delay_n = 1'b0; nmi_sampled_n = 1'b0; int_sampled_n = 1'b0;
            nmi_pending_n = 1'b0; bus_granted_n = 1'b0; srst_m1_n = 1'b0;
            f_modified_n = 1'b0;
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
            suppress_decode <= 1'b0; bus_granted <= 1'b0; irq_seq <= 2'd0;
            reg_q <= 8'h00; f_modified <= 1'b0;
            res2 <= 1'b1; sres <= 1'b0; rst_idle <= 1'b1; srst_m1 <= 1'b0; rd_hold <= 1'b0;
            addr_hold <= 16'h0000;
        end else begin
            addr_hold <= addr_neg;
            res2 <= res2_n; sres <= sres_n; rst_idle <= rst_idle_n; srst_m1 <= srst_m1_n;
            rd_hold <= cut_t1;
            reg_i <= reg_i_n; reg_r <= reg_r_n; ir <= ir_n;
            t_state <= t_n; m_cycle <= m_cycle_n;
            bus_op <= bus_op_n; m_len <= m_len_n; m_addr <= m_addr_n; m_wdata <= m_wdata_n;
            prefix <= prefix_n; iff1 <= iff1_n; iff2 <= iff2_n; im <= im_n; halted <= halted_n;
            tmp8 <= tmp8_n; tmph <= tmph_n;
            nmi_pending <= nmi_pending_n; prev_nmi_n <= prev_nmi_n_n; ei_delay <= ei_delay_n;
            nmi_sampled <= nmi_sampled_n; int_sampled <= int_sampled_n;
            suppress_decode <= suppress_decode_n; bus_granted <= bus_granted_n; irq_seq <= irq_seq_n;
            reg_q <= reg_q_n; f_modified <= f_modified_n;
        end
    end

endmodule
