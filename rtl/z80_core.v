// ===========================================================================
// z80_core.v - the Z80 engine, organised the way the die is.
//
//   decode PLA       z80_pla: (prefix state, opcode) -> instruction class
//                    (exec) and its fields (registers, ALU op, cc, ...)
//   sequencer        T-state / M-cycle counters, bus-cycle type and length,
//                    WAIT, BUSREQ, NMI/INT sampling and the instruction
//                    boundary (accept NMI / INT / HALT or fetch the next M1)
//   execute          the random control logic: (exec, M-cycle) -> control
//                    word ctl_* -- which bus cycle starts next and at which
//                    address, what the incrementer does, which registers
//                    load what. Narrow select signals only; no data here.
//   register file    BC DE HL AF, BC' DE' HL' AF', IX IY SP PC WZ. Reads:
//                    one indexed pair onto the address-side bus, the rp[p]
//                    and HL/IX/IY pairs, byte reads onto the data side.
//                    Writes: a 16-bit pair port, a byte port, the flag
//                    port, and the PC / SP / WZ loads.
//   address side     abus (16 bit) -> IDU (+1 / -1 / -2) -> address latch
//                    (m_addr) and register write-back; the effective-
//                    address adder (PC+e, IX+d) next to it
//   ALU              z80_alu (8 bit, flags) and the 16-bit add / subtract
//                    for ADD HL,rp / ADC HL,rp / SBC HL,rp
//   data latches     tmp8 (read data), m_wdata (write data), tmpl / tmph /
//                    tmp16 (operand bytes and words)
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
    input  wire        reset_n,
    // inputs consumed on the rising edge (live pins)
    input  wire [7:0]  data_in,        // M1 opcode / INTA vector, end of T2.N / Tw2.N
    input  wire        busreq_n,       // BUSREQ, instruction boundary + grant
    input  wire        nmi_n,          // NMI edge detect, clock-low half
    // inputs sampled on the falling edge by z80_top
    input  wire [7:0]  data_in_neg,    // MRD data at T3, IORD data at T4
    input  wire        int_n_neg,      // INT level at T_last.P
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
    // Separate loop indices for the register-file next-state block and the
    // clocked block: one `integer` assigned from two processes is a flop
    // with a conflicting combinational driver to Yosys.
    integer i, ri;

    // =======================================================================
    // registered state
    // =======================================================================
    reg [15:0] rf [0:12];      // register file, physical pairs (see below)
    // Exchange flip-flops. EX DE,HL / EXX / EX AF,AF' move no data: like
    // the die they flip which physical pair answers to which name. rf[0..3]
    // and rf[4..7] are the two banks; with all flip-flops clear (reset)
    // physical = logical (RFP_* indices), so a testbench may preload rf[]
    // straight after reset. Read registers back through rfl[].
    reg        xbank;          // EXX: BC DE HL are rf[4..6] (else rf[0..2])
    reg        xaf;            // EX AF,AF': AF is rf[7] (else rf[3])
    reg  [1:0] xdh;            // EX DE,HL, per physical bank: DE and HL swapped
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
    reg [7:0]  tmp8, tmpl, tmph;
    reg [15:0] tmp16;
    reg        nmi_pending, prev_nmi_n, ei_delay, suppress_decode, bus_granted;
    reg        nmi_sampled, int_sampled;  // latched at T_last.P per UM0080
    reg [1:0]  irq_seq;        // 0 none, 1 NMI, 2 INT, 3 HALT-nop
    reg        in_reset_hold;  // set by async reset, cleared (sync) by the
                               // first rising edge after release; nothing
                               // moves while it is set

    // ---- next state ----
    reg [15:0] rf_n [0:12];
    wire       xbank_n, xaf_n;
    wire [1:0] xdh_n;
    reg [7:0]  reg_i_n, reg_r_n, ir_n;
    reg [3:0]  t_n, m_len_n;
    reg [2:0]  m_cycle_n, bus_op_n;
    reg [15:0] m_addr_n;
    reg [7:0]  m_wdata_n;
    reg [2:0]  prefix_n;
    reg        iff1_n, iff2_n;
    reg [1:0]  im_n;
    reg        halted_n;
    reg [7:0]  tmp8_n, tmpl_n, tmph_n;
    reg [15:0] tmp16_n;
    reg [7:0]  reg_q_n;
    reg        f_modified_n;
    reg        nmi_pending_n, prev_nmi_n_n, ei_delay_n, suppress_decode_n, bus_granted_n;
    reg        nmi_sampled_n, int_sampled_n;
    reg        nmi_pending_mid;   // nmi_pending after the clock-high-half edge detect
    reg [1:0]  irq_seq_n;
    reg        allow_int;

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
    // Bus released to a DMA master (BUSACK low), or core frozen in reset
    // hold: strobes idle, data bus not driven -- matches the C model's
    // bus-grant idle pins. The address image holds its last value (the
    // real chip floats it; a pad ring can use busack_n as output enable).
    // The *_pos image is never forced idle for reset: while reset_n is low
    // z80_top's former is held at the idle pattern asynchronously, and the
    // first rising edge after release must present T1.P of the first M1.
    wire idle_pos = bus_granted_n;                 // T-state being entered
    wire idle_neg = bus_granted | in_reset_hold;   // current T-state
    assign addr_pos       = tp_addr;
    assign data_out_pos   = tp_dout;
    assign data_drive_pos = idle_pos ? 1'b0 : tp_drive;
    assign m1_n_pos       = idle_pos | tp_m1;
    assign mreq_n_pos     = idle_pos | tp_mreq;
    assign iorq_n_pos     = idle_pos | tp_iorq;
    assign rd_n_pos       = idle_pos | tp_rd;
    // wr_n_pos is assigned below: it needs `stall` (WR hold across Tw).
    assign rfsh_n_pos     = idle_pos | tp_rfsh;
    assign addr_neg       = tn_addr;
    assign data_out_neg   = tn_dout;
    assign data_drive_neg = idle_neg ? 1'b0 : tn_drive;
    assign m1_n_neg       = idle_neg | tn_m1;
    assign mreq_n_neg     = idle_neg | tn_mreq;
    assign iorq_n_neg     = idle_neg | tn_iorq;
    assign rd_n_neg       = idle_neg | tn_rd;
    assign wr_n_neg       = idle_neg | tn_wr;
    assign rfsh_n_neg     = idle_neg | tn_rfsh;
    // Silicon-faithful halt pin: assert at T4.N of the HALT instruction's
    // M1 (one half-T-state before instruction-done flips `halted`), so
    // the pin LEADS the flag by one phase -- matches perfectz80's
    // gate-level trace on prog10_halt_nmi / prog19_nmi_in_int. After
    // instruction-done, the `halted` register takes over. Mirrors
    // cmodel/z80_core.c halt_n driving. Being a clock-low-half event it
    // only appears in the *_neg image.
    wire exec_halt_in_m1 = (exec_w == `EXEC_HALT)
                        && (bus_op == `BUSOP_M1)
                        && (t_state == 4'd4);
    assign halt_n_pos   = halted_n ? 1'b0 : 1'b1;
    assign halt_n_neg   = (halted || exec_halt_in_m1) ? 1'b0 : 1'b1;
    // BUSACK follows the grant flag, which only changes on the rising edge.
    assign busack_n_pos = bus_granted_n ? 1'b0 : 1'b1;
    assign busack_n_neg = bus_granted   ? 1'b0 : 1'b1;
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
    assign wr_n_pos = idle_pos | (hold_wr ? 1'b0 : tp_wr);
    // the byte read this T-state: live opcode / vector, the falling-edge
    // MRD / IORD sample, or the data latch
    wire [7:0] rbyte = latch_pos ? data_in : latch_neg ? data_in_neg : tmp8;

    // the sequencer's step points
    wire m_end     = (t_state + 4'd1) > m_len;          // last T-state of the M-cycle
    wire ex_edge   = !in_reset_hold && !bus_granted && !stall && m_end;  // execute runs
    wire pc_m1_inc = !in_reset_hold && latch_pos && !stall &&
                     (bus_op == `BUSOP_M1) && !suppress_decode;          // opcode fetch PC+1

    // =======================================================================
    // register file: read side
    // =======================================================================
    // logical pair -> physical pair through the exchange flip-flops
    function [3:0] phys;
        input [3:0] l; input xb; input xa; input [1:0] xd;
        reg b, s;
        begin
            b = ((l == `RFP_BC2) || (l == `RFP_DE2) || (l == `RFP_HL2)) ? ~xb : xb;
            s = b ? xd[1] : xd[0];
            case (l)
                `RFP_BC, `RFP_BC2: phys = {1'b0, b, 2'd0};
                `RFP_DE, `RFP_DE2: phys = {1'b0, b, s ? 2'd2 : 2'd1};
                `RFP_HL, `RFP_HL2: phys = {1'b0, b, s ? 2'd1 : 2'd2};
                `RFP_AF:           phys = {1'b0, xa, 2'd3};
                `RFP_AF2:          phys = {1'b0, ~xa, 2'd3};
                default:           phys = l;
            endcase
        end
    endfunction
    wire [15:0] p_bc0 = rf[0], p_de0 = rf[1], p_hl0 = rf[2], p_af0 = rf[3];
    wire [15:0] p_bc1 = rf[4], p_de1 = rf[5], p_hl1 = rf[6], p_af1 = rf[7];
    wire        x_sw  = xbank ? xdh[1] : xdh[0];   // DE/HL swapped in the live bank
    wire        x_sw2 = xbank ? xdh[0] : xdh[1];   // ... in the other bank
    wire [15:0] r_bc  = xbank ? p_bc1 : p_bc0;
    wire [15:0] r_de  = xbank ? (x_sw ? p_hl1 : p_de1) : (x_sw ? p_hl0 : p_de0);
    wire [15:0] r_hl  = xbank ? (x_sw ? p_de1 : p_hl1) : (x_sw ? p_de0 : p_hl0);
    wire [15:0] r_af  = xaf ? p_af1 : p_af0;
    wire [15:0] r_bc2 = xbank ? p_bc0 : p_bc1;
    wire [15:0] r_de2 = xbank ? (x_sw2 ? p_hl0 : p_de0) : (x_sw2 ? p_hl1 : p_de1);
    wire [15:0] r_hl2 = xbank ? (x_sw2 ? p_de0 : p_hl0) : (x_sw2 ? p_de1 : p_hl1);
    wire [15:0] r_af2 = xaf ? p_af0 : p_af1;
    wire [15:0] r_ix  = rf[`RFP_IX],  r_iy  = rf[`RFP_IY],  r_sp  = rf[`RFP_SP];
    wire [15:0] r_pc  = rf[`RFP_PC],  r_wz  = rf[`RFP_WZ];
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
    // Block-IO flag inputs: data + (C +/- 1) for INxR, data + L for OTxR.
    wire [8:0] bk_ini = {1'b0, tmpl} + {1'b0, (r_bc[7:0] + (aux_w[0] ? 8'hFF : 8'h1))};
    wire [8:0] bk_out = {1'b0, tmpl} + {1'b0, (r_hl[7:0] + (aux_w[0] ? 8'hFF : 8'h1))};

    // Banks-2018 INIR/INDR/OTIR/OTDR repeat-M-cycle flag fold-in.
    // Mirrors redcode INXR_OTXR_COMMON exactly (see scripts/refs/
    // redcode_z80/Z80.c line 1233). Inputs: data byte (tmpl after the
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
                              alu_bit = {2'b0, iff2}; end
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
                        alu_bit = {2'b0, ((r_bc - 16'd1) != 16'd0)};
                    end
                    2'd1: if (m_cycle == 3'd2) begin
                        alu_md = `FLAG_BLOCK_CP; alu_a = A_cur; alu_b = rbyte;
                        alu_bit = {2'b0, ((r_bc - 16'd1) != 16'd0)};
                    end
                    2'd2: if (m_cycle == 3'd4) begin
                        alu_md = `FLAG_BLOCK_IO;
                        alu_a = tmpl;                      // data
                        alu_b = r_bc[15:8] - 8'd1;         // newB
                        alu_xy = {4'b0, bk_ini[8], bk_ini[2:0]};
                    end
                    default: if (m_cycle == 3'd4) begin   // 2'd3 = OUT
                        alu_md = `FLAG_BLOCK_IO;
                        alu_a = tmpl;                      // data
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
    // address latch input
    localparam [2:0] AL_ABUS = 3'd0,   // the address-side bus
                     AL_IDU  = 3'd1,   // abus +1 / -1 / -2
                     AL_EA   = 3'd2,   // effective-address adder
                     AL_A16  = 3'd3,   // 16-bit ALU sum (ADD HL,rp internal cycle)
                     AL_HLS  = 3'd4;   // HL +/- 1 (CPI / CPD internal cycle)
    // address-side bus source
    localparam [2:0] AB_REG  = 3'd0,   // register pair, see RI_*
                     AB_NN   = 3'd1,   // {rbyte, tmpl}: the operand word nn
                     AB_AN   = 3'd2,   // {A, n}: IN A,(n) / OUT (n),A port
                     AB_IV   = 3'd3,   // {I, vector}: IM 2 table address
                     AB_T16  = 3'd4,   // tmp16
                     AB_NBC  = 3'd5;   // {B-1, C}: OUTI / OUTD port
    // register pair on abus / written by the pair port
    localparam [2:0] RI_PC = 3'd0, RI_SP = 3'd1, RI_WZ = 3'd2, RI_HL = 3'd3,
                     RI_DE = 3'd4, RI_BC = 3'd5, RI_HLP = 3'd6, RI_RP = 3'd7;
    // incrementer
    localparam [1:0] INC_P1 = 2'd0, INC_M1 = 2'd1, INC_M2 = 2'd2;
    // effective-address adder displacement (base: PC or HL/IX/IY)
    localparam [1:0] EA_RB = 2'd0, EA_TL = 2'd1, EA_M1 = 2'd2;
    // write data
    localparam [2:0] DB_ZERO = 3'd0, DB_REG = 3'd1, DB_RI = 3'd2, DB_R8 = 3'd3,
                     DB_RB = 3'd4, DB_ALU = 3'd5, DB_CB = 3'd6, DB_TL = 3'd7;
    localparam [1:0] BI_PC = 2'd0, BI_RP = 2'd1, BI_HLP = 2'd2, BI_A = 2'd3;
    // PC / SP / WZ loads
    localparam [2:0] PC_HOLD = 3'd0, PC_IDU = 3'd1, PC_EA = 3'd2, PC_NN = 3'd3,
                     PC_T16 = 3'd4, PC_ABUS = 3'd5, PC_VEC = 3'd6;
    localparam [1:0] VEC_RST = 2'd0, VEC_NMI = 2'd1, VEC_IM = 2'd2;
    localparam [1:0] SP_HOLD = 2'd0, SP_IDU = 2'd1, SP_ABUS = 2'd2;
    localparam [2:0] WZ_HOLD = 3'd0, WZ_IDU = 3'd1, WZ_ALO = 3'd2, WZ_EA = 3'd3,
                     WZ_NN = 3'd4, WZ_TMP = 3'd5, WZ_VEC = 3'd6;
    // pair write port source
    localparam [2:0] PW_IDU = 3'd0, PW_NN = 3'd1, PW_A16 = 3'd2, PW_TMP = 3'd3, PW_BCD = 3'd4;
    // byte write port destination / source
    localparam [2:0] BW_RI = 3'd0, BW_R8 = 3'd1, BW_A = 3'd2, BW_B = 3'd3,
                     BW_DDCB = 3'd4, BW_HLPL = 3'd5, BW_HLPH = 3'd6;
    localparam [2:0] BS_RI = 3'd0, BS_ALU = 3'd1, BS_RB = 3'd2, BS_CB = 3'd3, BS_BD = 3'd4;
    // flag port source
    localparam [2:0] FS_ALU = 3'd0, FS_A16 = 3'd1, FS_BLD = 3'd2, FS_BCP = 3'd3, FS_BIO = 3'd4;
    // operand temporaries
    localparam [1:0] TL_HOLD = 2'd0, TL_RB = 2'd1, TL_RRD = 2'd2;
    localparam [1:0] T16_HOLD = 2'd0, T16_NN = 2'd1, T16_IV = 2'd2;
    // interrupt enables
    localparam [1:0] IFF_HOLD = 2'd0, IFF_DI = 2'd1, IFF_EI = 2'd2, IFF_RETN = 2'd3;

    reg        ctl_mstart;             // start the next M-cycle
    reg        ctl_m1;                 // fetch the next opcode of a prefixed instruction
    reg        ctl_fin;                // instruction done -> boundary
    reg [2:0]  ctl_bop;                //   bus cycle type of the next M-cycle
    reg [3:0]  ctl_mx;                 //   extra T-states on top of its base length
    reg [2:0]  ctl_al;                 //   address latch input
    reg [2:0]  ctl_ab, ctl_ri;         // address-side bus source / pair
    reg [1:0]  ctl_inc;                // incrementer operation
    reg        ctl_ea_pc;              // EA base: PC (else HL/IX/IY)
    reg [1:0]  ctl_ea_d;               // EA displacement
    reg [2:0]  ctl_db;                 // write data
    reg [1:0]  ctl_bi;                 //   byte read pair for DB_REG
    reg        ctl_bhi;                //   ... high byte
    reg [2:0]  ctl_pc, ctl_wz;         // PC / WZ loads
    reg [1:0]  ctl_sp;                 // SP load
    reg [1:0]  ctl_vec;                // vector for PC_VEC / WZ_VEC
    reg        ctl_pw;                 // pair write port
    reg [2:0]  ctl_pwi, ctl_pws;       //   pair / source
    reg        ctl_bw;                 // byte write port
    reg [2:0]  ctl_bwd, ctl_bws;       //   destination / source
    reg        ctl_fw;                 // flag port
    reg [2:0]  ctl_fs;                 //   source
    reg        ctl_hl_step, ctl_de_step;   // block-move pointer steps
    reg        ctl_ex_dehl, ctl_ex_af, ctl_exx;
    reg [1:0]  ctl_tmpl, ctl_tmp16;
    reg        ctl_tmph;
    reg [1:0]  ctl_iff;
    reg        ctl_im, ctl_halt, ctl_ld_i, ctl_ld_r, ctl_prefix;
    reg        ctl_cbx, ctl_cbr;       // bit-op unit: DDCB opcode fields / register operand
    reg [2:0]  mm;                     // M-cycle of the operation after the (IX+d) preamble
    reg [2:0]  mem_ri;                 // memory operand pointer: HL/IX/IY or WZ
    reg        mem_ea;                 //   ... or IX+d straight from the EA adder

    // next prefix state of a prefix opcode
    reg [2:0]  pfx_next;
    always @* begin
        case (ir)
            8'hDD:   pfx_next = `PFX_DD;
            8'hFD:   pfx_next = `PFX_FD;
            8'hED:   pfx_next = `PFX_ED;
            8'hCB:   pfx_next = (prefix == `PFX_DD) ? `PFX_DDCB :
                                (prefix == `PFX_FD) ? `PFX_FDCB : `PFX_CB;
            default: pfx_next = prefix;
        endcase
    end

    // ---- micro-operation groups ----
    // start the next M-cycle: bus cycle, address latch input, extra T-states
    task mstart; input [2:0] bop; input [2:0] al; input [3:0] extra;
        begin ctl_mstart = 1'b1; ctl_bop = bop; ctl_al = al; ctl_mx = extra; end
    endtask
    // read the next operand byte: MRD (PC), PC <- PC+1
    task op_fetch; input [3:0] extra;
        begin
            ctl_ab = AB_REG; ctl_ri = RI_PC; ctl_inc = INC_P1; ctl_pc = PC_IDU;
            mstart(`BUSOP_MRD, AL_ABUS, extra);
        end
    endtask
    // a bus cycle at a register pair
    task at_pair; input [2:0] bop; input [2:0] rsel; input [3:0] extra;
        begin ctl_ab = AB_REG; ctl_ri = rsel; mstart(bop, AL_ABUS, extra); end
    endtask
    // a bus cycle at the memory operand: (HL), (IX+d) or (WZ)
    task at_mem; input [2:0] bop; input [3:0] extra;
        begin
            if (mem_ea) begin ctl_ea_pc = 1'b0; ctl_ea_d = EA_RB; mstart(bop, AL_EA, extra); end
            else at_pair(bop, mem_ri, extra);
        end
    endtask
    // push one byte: SP <- SP-1, MWR (SP)
    task push_byte; input [1:0] bsel; input hi;
        begin
            ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_M1; ctl_sp = SP_IDU;
            ctl_db = DB_REG; ctl_bi = bsel; ctl_bhi = hi;
            mstart(`BUSOP_MWR, AL_IDU, 4'd0);
        end
    endtask
    // pop one byte: MRD at SP+1, SP <- SP+1 (the first pop byte reads (SP))
    task pop_next;
        begin
            ctl_tmpl = TL_RB;
            ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_P1; ctl_sp = SP_IDU;
            mstart(`BUSOP_MRD, AL_IDU, 4'd0);
        end
    endtask
    // the popped word is complete: SP <- SP+1
    task pop_done;
        begin ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_P1; ctl_sp = SP_IDU; end
    endtask
    // byte / flag writes
    task wr_byte; input [2:0] dst; input [2:0] src;
        begin ctl_bw = 1'b1; ctl_bwd = dst; ctl_bws = src; end
    endtask
    task wr_flags; input [2:0] src;
        begin ctl_fw = 1'b1; ctl_fs = src; end
    endtask
    // A <- ALU result unless the op is CP, F <- ALU flags
    task alu_to_a;
        begin if (alu_op_w != `ALU_CP) wr_byte(BW_A, BS_ALU); wr_flags(FS_ALU); end
    endtask
    // repeat of a block instruction: PC <- PC-2, WZ <- PC-1, 5 internal T
    task blk_repeat; input [2:0] fsrc;
        begin
            ctl_ab = AB_REG; ctl_ri = RI_PC; ctl_inc = INC_M2; ctl_pc = PC_IDU;
            ctl_ea_pc = 1'b1; ctl_ea_d = EA_M1; ctl_wz = WZ_EA;
            wr_flags(fsrc);
            mstart(`BUSOP_INTERNAL, AL_IDU, 4'd5);
        end
    endtask

    always @* begin
        ctl_mstart = 1'b0; ctl_m1 = 1'b0; ctl_fin = 1'b0;
        ctl_bop = `BUSOP_NONE; ctl_mx = 4'd0; ctl_al = AL_ABUS;
        ctl_ab = AB_REG; ctl_ri = RI_PC; ctl_inc = INC_P1;
        ctl_ea_pc = 1'b0; ctl_ea_d = EA_RB;
        ctl_db = DB_ZERO; ctl_bi = BI_PC; ctl_bhi = 1'b0;
        ctl_pc = PC_HOLD; ctl_sp = SP_HOLD; ctl_wz = WZ_HOLD; ctl_vec = VEC_RST;
        ctl_pw = 1'b0; ctl_pwi = RI_RP; ctl_pws = PW_IDU;
        ctl_bw = 1'b0; ctl_bwd = BW_RI; ctl_bws = BS_RI;
        ctl_fw = 1'b0; ctl_fs = FS_ALU;
        ctl_hl_step = 1'b0; ctl_de_step = 1'b0;
        ctl_ex_dehl = 1'b0; ctl_ex_af = 1'b0; ctl_exx = 1'b0;
        ctl_tmpl = TL_HOLD; ctl_tmph = 1'b0; ctl_tmp16 = T16_HOLD;
        ctl_iff = IFF_HOLD; ctl_im = 1'b0; ctl_halt = 1'b0;
        ctl_ld_i = 1'b0; ctl_ld_r = 1'b0; ctl_prefix = 1'b0;
        ctl_cbx = 1'b0; ctl_cbr = 1'b0;
        mm = m_cycle; mem_ri = RI_HLP; mem_ea = 1'b0;

        // (IX+d) / (IY+d) displacement preamble: read d, then 5 internal
        // T-states while WZ = IX+d is formed (not during interrupt/HALT
        // sequences)
        if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w && m_cycle == 3'd1) begin
            op_fetch(4'd0);
        end else if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w && m_cycle == 3'd2
                     && eff_exec != `EXEC_LD_M_N) begin
            ctl_ea_pc = 1'b0; ctl_ea_d = EA_RB; ctl_wz = WZ_EA;
            mstart(`BUSOP_INTERNAL, AL_EA, 4'd5);
        end else begin
            // LD (IX+d),n: WZ = IX+d while n is read (no internal cycle; the
            // n read is 2 T longer), then the operation runs one M-cycle in
            if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w &&
                eff_exec == `EXEC_LD_M_N && m_cycle == 3'd2) begin
                ctl_ea_pc = 1'b0; ctl_ea_d = EA_RB; ctl_wz = WZ_EA;
            end
            if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w) begin
                mem_ri = RI_WZ;
                if (eff_exec == `EXEC_LD_M_N) begin
                    mm = m_cycle - 3'd1;
                    mem_ea = (m_cycle == 3'd2);
                end else
                    mm = m_cycle - 3'd2;
            end

            case (eff_exec)
            `EXEC_NOP, `EXEC_ILLEGAL: ctl_fin = 1'b1;
            `EXEC_PREFIX: begin
                ctl_prefix = 1'b1;
                if (pfx_next == `PFX_DDCB || pfx_next == `PFX_FDCB)
                    op_fetch(4'd0);                       // DD CB d op: read d
                else
                    ctl_m1 = 1'b1;                        // fetch the prefixed opcode
            end
            `EXEC_DDCB: begin
                ctl_cbx = 1'b1;
                if (m_cycle == 3'd2) begin                // d -> tmpl; read op
                    ctl_tmpl = TL_RB; op_fetch(4'd0);
                end else if (m_cycle == 3'd3) begin       // op -> tmph; WZ = IX+d
                    ctl_tmph = 1'b1;
                    ctl_ea_pc = 1'b0; ctl_ea_d = EA_TL; ctl_wz = WZ_EA;
                    mstart(`BUSOP_INTERNAL, AL_EA, 4'd2);
                end else if (m_cycle == 3'd4)
                    at_pair(`BUSOP_MRD, RI_WZ, 4'd1);     // read (IX+d), 4 T
                else if (m_cycle == 3'd5) begin
                    if (tmph[7:6] == `CB_BIT) begin
                        wr_flags(FS_ALU); ctl_fin = 1'b1;
                    end else begin
                        if (tmph[7:6] == `CB_ROT) wr_flags(FS_ALU);
                        wr_byte(BW_DDCB, BS_CB);          // undocumented copy to r[z]
                        ctl_db = DB_CB;
                        at_pair(`BUSOP_MWR, RI_WZ, 4'd0);
                    end
                end else ctl_fin = 1'b1;
            end
            `EXEC_DI:   begin ctl_iff = IFF_DI; ctl_fin = 1'b1; end
            `EXEC_EI:   begin ctl_iff = IFF_EI; ctl_fin = 1'b1; end
            // Silicon-faithful per Brewer 2014 + Woodmass HALT2INT: PC stays
            // past the HALT byte (the M1 fetch already incremented it); HALT
            // NOP M-cycles re-fetch there and NMI/INT exit doesn't bump it.
            `EXEC_HALT: begin ctl_halt = 1'b1; ctl_fin = 1'b1; end

            `EXEC_LD_R_R: begin wr_byte(BW_RI, BS_RI); ctl_fin = 1'b1; end
            `EXEC_ALU_R:  begin alu_to_a; ctl_fin = 1'b1; end
            `EXEC_INC_R, `EXEC_DEC_R: begin
                wr_byte(BW_RI, BS_ALU); wr_flags(FS_ALU); ctl_fin = 1'b1;
            end
            `EXEC_ROT_A, `EXEC_DAA, `EXEC_CPL: begin
                wr_byte(BW_A, BS_ALU); wr_flags(FS_ALU); ctl_fin = 1'b1;
            end
            `EXEC_SCF, `EXEC_CCF: begin wr_flags(FS_ALU); ctl_fin = 1'b1; end

            `EXEC_EX_DE_HL: begin ctl_ex_dehl = 1'b1; ctl_fin = 1'b1; end
            `EXEC_EX_AF:    begin ctl_ex_af = 1'b1; ctl_fin = 1'b1; end
            `EXEC_EXX:      begin ctl_exx = 1'b1; ctl_fin = 1'b1; end
            `EXEC_JP_HL: begin
                ctl_ab = AB_REG; ctl_ri = RI_HLP; ctl_pc = PC_ABUS; ctl_fin = 1'b1;
            end

            `EXEC_INC_RP, `EXEC_DEC_RP: begin
                if (m_cycle == 3'd1) begin
                    ctl_ab = AB_REG; ctl_ri = RI_RP;
                    ctl_inc = (eff_exec == `EXEC_INC_RP) ? INC_P1 : INC_M1;
                    ctl_pw = 1'b1; ctl_pwi = RI_RP; ctl_pws = PW_IDU;
                    mstart(`BUSOP_INTERNAL, AL_IDU, 4'd2);
                end else ctl_fin = 1'b1;
            end
            `EXEC_LD_SP_HL: begin
                if (m_cycle == 3'd1) begin
                    ctl_sp = SP_ABUS; at_pair(`BUSOP_INTERNAL, RI_HLP, 4'd2);
                end else ctl_fin = 1'b1;
            end
            `EXEC_ADD_HL_RP: begin
                if (m_cycle == 3'd1) begin
                    ctl_ab = AB_REG; ctl_ri = RI_HLP; ctl_inc = INC_P1; ctl_wz = WZ_IDU;  // WZ = HL+1
                    ctl_pw = 1'b1; ctl_pwi = RI_HLP; ctl_pws = PW_A16;
                    wr_flags(FS_A16);
                    mstart(`BUSOP_INTERNAL, AL_A16, 4'd7);
                end else ctl_fin = 1'b1;
            end

            `EXEC_LD_R_N: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else begin wr_byte(BW_RI, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_ALU_N: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else begin alu_to_a; ctl_fin = 1'b1; end
            end
            `EXEC_LD_R_M: begin
                if (mm == 3'd1) at_mem(`BUSOP_MRD, 4'd0);
                else begin wr_byte(BW_RI, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_ALU_M: begin
                if (mm == 3'd1) at_mem(`BUSOP_MRD, 4'd0);
                else begin alu_to_a; ctl_fin = 1'b1; end
            end
            `EXEC_ST_M_R: begin
                if (mm == 3'd1) begin ctl_db = DB_RI; at_mem(`BUSOP_MWR, 4'd0); end
                else ctl_fin = 1'b1;
            end
            `EXEC_LD_M_N: begin
                // LD (IX+d),n folds the 2 T of the IX+d add into the n read
                if (mm == 3'd1) op_fetch((idx_w != 2'd0 && use_disp_w) ? 4'd2 : 4'd0);
                else if (mm == 3'd2) begin ctl_db = DB_RB; at_mem(`BUSOP_MWR, 4'd0); end
                else ctl_fin = 1'b1;
            end
            `EXEC_INC_M, `EXEC_DEC_M: begin
                if (mm == 3'd1) at_mem(`BUSOP_MRD, 4'd1);
                else if (mm == 3'd2) begin
                    wr_flags(FS_ALU); ctl_db = DB_ALU; at_mem(`BUSOP_MWR, 4'd0);
                end else ctl_fin = 1'b1;
            end

            `EXEC_LD_RP_NN: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin ctl_tmpl = TL_RB; op_fetch(4'd0); end
                else begin ctl_pw = 1'b1; ctl_pwi = RI_RP; ctl_pws = PW_NN; ctl_fin = 1'b1; end
            end
            `EXEC_JP, `EXEC_JP_CC: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin ctl_tmpl = TL_RB; op_fetch(4'd0); end
                else begin
                    ctl_wz = WZ_NN;                       // MEMPTR = nn, taken or not
                    if (exec_w == `EXEC_JP || cc_taken) ctl_pc = PC_NN;
                    ctl_fin = 1'b1;
                end
            end
            `EXEC_JR, `EXEC_JR_CC: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin
                    if (eff_exec == `EXEC_JR || cc_taken) begin
                        ctl_ea_pc = 1'b1; ctl_ea_d = EA_RB; ctl_pc = PC_EA; ctl_wz = WZ_EA;
                        mstart(`BUSOP_INTERNAL, AL_EA, 4'd5);
                    end else ctl_fin = 1'b1;
                end else ctl_fin = 1'b1;
            end
            `EXEC_DJNZ: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                else if (m_cycle == 3'd2) op_fetch(4'd0);
                else if (m_cycle == 3'd3) begin
                    wr_byte(BW_B, BS_BD);                 // B <- B-1
                    if (b_dec != 8'd0) begin
                        ctl_ea_pc = 1'b1; ctl_ea_d = EA_RB; ctl_pc = PC_EA; ctl_wz = WZ_EA;
                        mstart(`BUSOP_INTERNAL, AL_EA, 4'd5);
                    end else ctl_fin = 1'b1;
                end else ctl_fin = 1'b1;
            end

            `EXEC_CALL, `EXEC_CALL_CC: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin ctl_tmpl = TL_RB; op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    ctl_tmp16 = T16_NN; ctl_wz = WZ_NN;
                    if (exec_w == `EXEC_CALL || cc_taken) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                    else ctl_fin = 1'b1;
                end
                else if (m_cycle == 3'd4) push_byte(BI_PC, 1'b1);
                else if (m_cycle == 3'd5) push_byte(BI_PC, 1'b0);
                else begin ctl_pc = PC_T16; ctl_fin = 1'b1; end
            end
            `EXEC_RET, `EXEC_RETN: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_SP, 4'd0);
                else if (m_cycle == 3'd2) pop_next;
                else begin
                    pop_done; ctl_pc = PC_NN; ctl_wz = WZ_NN;
                    if (eff_exec == `EXEC_RETN) ctl_iff = IFF_RETN;
                    ctl_fin = 1'b1;
                end
            end
            `EXEC_RET_CC: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                else if (m_cycle == 3'd2) begin
                    if (cc_taken) at_pair(`BUSOP_MRD, RI_SP, 4'd0);
                    else ctl_fin = 1'b1;
                end
                else if (m_cycle == 3'd3) pop_next;
                else begin pop_done; ctl_pc = PC_NN; ctl_wz = WZ_NN; ctl_fin = 1'b1; end
            end
            `EXEC_RST: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                else if (m_cycle == 3'd2) push_byte(BI_PC, 1'b1);
                else if (m_cycle == 3'd3) push_byte(BI_PC, 1'b0);
                else begin ctl_vec = VEC_RST; ctl_pc = PC_VEC; ctl_wz = WZ_VEC; ctl_fin = 1'b1; end
            end
            `EXEC_PUSH: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                else if (m_cycle == 3'd2) push_byte(BI_RP, 1'b1);
                else if (m_cycle == 3'd3) push_byte(BI_RP, 1'b0);
                else ctl_fin = 1'b1;
            end
            `EXEC_POP: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_SP, 4'd0);
                else if (m_cycle == 3'd2) pop_next;
                else begin
                    pop_done; ctl_pw = 1'b1; ctl_pwi = RI_RP; ctl_pws = PW_NN; ctl_fin = 1'b1;
                end
            end

            `EXEC_LD_A_RP: begin
                if (m_cycle == 3'd1) begin
                    ctl_inc = INC_P1; ctl_wz = WZ_IDU;          // WZ = rp+1
                    at_pair(`BUSOP_MRD, RI_RP, 4'd0);
                end else begin wr_byte(BW_A, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_LD_RP_A: begin
                if (m_cycle == 3'd1) begin
                    ctl_inc = INC_P1; ctl_wz = WZ_ALO;          // WZ = {A, rp.lo+1}
                    ctl_db = DB_REG; ctl_bi = BI_A; ctl_bhi = 1'b1;
                    at_pair(`BUSOP_MWR, RI_RP, 4'd0);
                end else ctl_fin = 1'b1;
            end
            `EXEC_LD_A_NN: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin ctl_tmpl = TL_RB; op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    ctl_tmp16 = T16_NN;
                    ctl_ab = AB_NN; ctl_inc = INC_P1; ctl_wz = WZ_IDU;   // WZ = nn+1
                    mstart(`BUSOP_MRD, AL_ABUS, 4'd0);
                end else begin wr_byte(BW_A, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_LD_NN_A: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin ctl_tmpl = TL_RB; op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    ctl_ab = AB_NN; ctl_inc = INC_P1; ctl_wz = WZ_ALO;   // WZ = {A, n.lo+1}
                    ctl_db = DB_REG; ctl_bi = BI_A; ctl_bhi = 1'b1;
                    mstart(`BUSOP_MWR, AL_ABUS, 4'd0);
                end else ctl_fin = 1'b1;
            end
            `EXEC_LD_HL_NN, `EXEC_LD_RP_NNA: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin ctl_tmpl = TL_RB; op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    ctl_tmp16 = T16_NN;
                    ctl_ab = AB_NN; ctl_inc = INC_P1; ctl_wz = WZ_IDU;   // WZ = nn+1
                    mstart(`BUSOP_MRD, AL_ABUS, 4'd0);
                end else if (m_cycle == 3'd4) begin
                    if (eff_exec == `EXEC_LD_HL_NN) wr_byte(BW_HLPL, BS_RB);
                    else ctl_tmpl = TL_RB;
                    ctl_ab = AB_T16; ctl_inc = INC_P1;
                    mstart(`BUSOP_MRD, AL_IDU, 4'd0);
                end else begin
                    if (eff_exec == `EXEC_LD_HL_NN) wr_byte(BW_HLPH, BS_RB);
                    else begin ctl_pw = 1'b1; ctl_pwi = RI_RP; ctl_pws = PW_NN; end
                    ctl_fin = 1'b1;
                end
            end
            `EXEC_LD_NN_HL, `EXEC_LD_NNA_RP: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin ctl_tmpl = TL_RB; op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    ctl_tmp16 = T16_NN;
                    ctl_ab = AB_NN; ctl_inc = INC_P1; ctl_wz = WZ_IDU;   // WZ = nn+1
                    ctl_db = DB_REG; ctl_bhi = 1'b0;
                    ctl_bi = (eff_exec == `EXEC_LD_NN_HL) ? BI_HLP : BI_RP;
                    mstart(`BUSOP_MWR, AL_ABUS, 4'd0);
                end else if (m_cycle == 3'd4) begin
                    ctl_ab = AB_T16; ctl_inc = INC_P1;
                    ctl_db = DB_REG; ctl_bhi = 1'b1;
                    ctl_bi = (eff_exec == `EXEC_LD_NN_HL) ? BI_HLP : BI_RP;
                    mstart(`BUSOP_MWR, AL_IDU, 4'd0);
                end else ctl_fin = 1'b1;
            end

            `EXEC_IN_A_N: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin
                    ctl_ab = AB_AN; ctl_inc = INC_P1; ctl_wz = WZ_IDU;   // WZ = {A,n}+1
                    mstart(`BUSOP_IORD, AL_ABUS, 4'd0);
                end else begin wr_byte(BW_A, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_OUT_N_A: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin
                    ctl_ab = AB_AN; ctl_inc = INC_P1; ctl_wz = WZ_ALO;   // WZ = {A, n+1}
                    ctl_db = DB_REG; ctl_bi = BI_A; ctl_bhi = 1'b1;
                    mstart(`BUSOP_IOWR, AL_ABUS, 4'd0);
                end else ctl_fin = 1'b1;
            end
            `EXEC_EX_SP_HL: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_SP, 4'd0);
                else if (m_cycle == 3'd2) begin
                    ctl_tmpl = TL_RB;
                    ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_P1;
                    mstart(`BUSOP_MRD, AL_IDU, 4'd1);
                end else if (m_cycle == 3'd3) begin
                    ctl_tmph = 1'b1;
                    ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_P1;
                    ctl_db = DB_REG; ctl_bi = BI_HLP; ctl_bhi = 1'b1;
                    mstart(`BUSOP_MWR, AL_IDU, 4'd0);
                end else if (m_cycle == 3'd4) begin
                    ctl_db = DB_REG; ctl_bi = BI_HLP; ctl_bhi = 1'b0;
                    at_pair(`BUSOP_MWR, RI_SP, 4'd2);
                end else begin
                    ctl_pw = 1'b1; ctl_pwi = RI_HLP; ctl_pws = PW_TMP; ctl_wz = WZ_TMP;
                    ctl_fin = 1'b1;
                end
            end

            `EXEC_CB_R: begin
                ctl_cbr = 1'b1;
                if (cb_kind_w == `CB_ROT || cb_kind_w == `CB_BIT) wr_flags(FS_ALU);
                if (cb_kind_w != `CB_BIT) wr_byte(BW_R8, BS_CB);
                ctl_fin = 1'b1;
            end
            `EXEC_CB_M: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_HL, 4'd1);
                else if (cb_kind_w == `CB_BIT) begin wr_flags(FS_ALU); ctl_fin = 1'b1; end
                else if (m_cycle == 3'd2) begin
                    if (cb_kind_w == `CB_ROT) wr_flags(FS_ALU);
                    ctl_db = DB_CB; at_pair(`BUSOP_MWR, RI_HL, 4'd0);
                end else ctl_fin = 1'b1;
            end

            // ---------- ED page (non-block) ----------
            `EXEC_ADC16, `EXEC_SBC16: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_HL, 4'd7);
                else begin
                    ctl_ab = AB_REG; ctl_ri = RI_HL; ctl_inc = INC_P1; ctl_wz = WZ_IDU;  // WZ = HL+1
                    ctl_pw = 1'b1; ctl_pwi = RI_HL; ctl_pws = PW_A16;
                    wr_flags(FS_A16); ctl_fin = 1'b1;
                end
            end
            `EXEC_NEG: begin wr_byte(BW_A, BS_ALU); wr_flags(FS_ALU); ctl_fin = 1'b1; end
            `EXEC_IM:  begin ctl_im = 1'b1; ctl_fin = 1'b1; end
            `EXEC_LD_I_A, `EXEC_LD_R_A: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                else begin
                    if (eff_exec == `EXEC_LD_I_A) ctl_ld_i = 1'b1; else ctl_ld_r = 1'b1;
                    ctl_fin = 1'b1;
                end
            end
            `EXEC_LD_A_IR: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                else begin wr_byte(BW_A, BS_ALU); wr_flags(FS_ALU); ctl_fin = 1'b1; end
            end
            `EXEC_IN_C: begin
                if (m_cycle == 3'd1) begin
                    ctl_inc = INC_P1; ctl_wz = WZ_IDU;          // WZ = BC+1
                    at_pair(`BUSOP_IORD, RI_BC, 4'd0);
                end else begin
                    wr_byte(BW_R8, BS_RB);                     // r[y]; y=6 writes nothing
                    wr_flags(FS_ALU); ctl_fin = 1'b1;
                end
            end
            `EXEC_OUT_C: begin
                if (m_cycle == 3'd1) begin
                    ctl_inc = INC_P1; ctl_wz = WZ_IDU;          // WZ = BC+1
                    ctl_db = DB_R8;                            // r[y]; y=6 sends 0 (NMOS)
                    at_pair(`BUSOP_IOWR, RI_BC, 4'd0);
                end else ctl_fin = 1'b1;
            end
            `EXEC_RRD, `EXEC_RLD: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_HL, 4'd0);
                else if (m_cycle == 3'd2) begin
                    // A and flags from z80_alu (FLAG_RRD / FLAG_RLD); the new
                    // (HL) byte is nibble routing, parked in tmpl
                    wr_byte(BW_A, BS_ALU); wr_flags(FS_ALU);
                    ctl_tmpl = TL_RRD;
                    ctl_ab = AB_REG; ctl_ri = RI_HL; ctl_inc = INC_P1; ctl_wz = WZ_IDU;  // WZ = HL+1
                    mstart(`BUSOP_INTERNAL, AL_ABUS, 4'd4);
                end else if (m_cycle == 3'd3) begin
                    ctl_db = DB_TL; at_pair(`BUSOP_MWR, RI_HL, 4'd0);
                end else ctl_fin = 1'b1;
            end

            // ---------- ED block instructions ----------
            // aux: [0]=dec  [2:1]=cat(0 LD,1 CP,2 IN,3 OUT)  [3]=repeat
            `EXEC_BLOCK: begin
                case (aux_w[2:1])
                2'd0: begin                                   // LDI LDD LDIR LDDR
                    if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_HL, 4'd0);
                    else if (m_cycle == 3'd2) begin
                        ctl_db = DB_RB; at_pair(`BUSOP_MWR, RI_DE, 4'd2);   // 5 T write
                    end else if (m_cycle == 3'd3) begin
                        ctl_pw = 1'b1; ctl_pwi = RI_BC; ctl_pws = PW_BCD;   // BC-1
                        ctl_hl_step = 1'b1; ctl_de_step = 1'b1;
                        wr_flags(FS_ALU);
                        // Banks 2018: on repeat YF/XF come from PC.13 / PC.11
                        if (aux_w[3] && bc_dec != 16'd0) blk_repeat(FS_BLD);
                        else ctl_fin = 1'b1;
                    end else ctl_fin = 1'b1;
                end
                2'd1: begin                                   // CPI CPD CPIR CPDR
                    if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_HL, 4'd0);
                    else if (m_cycle == 3'd2) begin
                        ctl_pw = 1'b1; ctl_pwi = RI_BC; ctl_pws = PW_BCD;   // BC-1
                        ctl_ab = AB_REG; ctl_ri = RI_WZ;
                        ctl_inc = aux_w[0] ? INC_M1 : INC_P1; ctl_wz = WZ_IDU;   // WZ +/- 1
                        ctl_hl_step = 1'b1;
                        wr_flags(FS_ALU);
                        mstart(`BUSOP_INTERNAL, AL_HLS, 4'd5);
                    end else if (m_cycle == 3'd3) begin
                        if (aux_w[3] && r_bc != 16'd0 && !F_cur[6]) blk_repeat(FS_BCP);
                        else ctl_fin = 1'b1;
                    end else ctl_fin = 1'b1;
                end
                2'd2: begin                                   // INI IND INIR INDR
                    if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                    else if (m_cycle == 3'd2) begin
                        ctl_inc = aux_w[0] ? INC_M1 : INC_P1; ctl_wz = WZ_IDU;  // WZ = BC +/- 1
                        at_pair(`BUSOP_IORD, RI_BC, 4'd0);
                    end else if (m_cycle == 3'd3) begin
                        ctl_tmpl = TL_RB; ctl_db = DB_RB;
                        at_pair(`BUSOP_MWR, RI_HL, 4'd0);
                    end else if (m_cycle == 3'd4) begin
                        wr_byte(BW_B, BS_BD);                 // B-1
                        ctl_hl_step = 1'b1;
                        wr_flags(FS_ALU);
                        if (aux_w[3] && b_dec != 8'd0) blk_repeat(FS_BIO);
                        else ctl_fin = 1'b1;
                    end else ctl_fin = 1'b1;
                end
                default: begin                                // OUTI OUTD OTIR OTDR
                    if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                    else if (m_cycle == 3'd2) at_pair(`BUSOP_MRD, RI_HL, 4'd0);
                    else if (m_cycle == 3'd3) begin
                        ctl_tmpl = TL_RB;
                        wr_byte(BW_B, BS_BD);                 // B-1, then out to {B-1, C}
                        ctl_ab = AB_NBC; ctl_inc = aux_w[0] ? INC_M1 : INC_P1; ctl_wz = WZ_IDU;
                        ctl_db = DB_RB;
                        mstart(`BUSOP_IOWR, AL_ABUS, 4'd0);
                    end else if (m_cycle == 3'd4) begin
                        ctl_hl_step = 1'b1;
                        wr_flags(FS_ALU);
                        if (aux_w[3] && r_bc[15:8] != 8'd0) blk_repeat(FS_BIO);
                        else ctl_fin = 1'b1;
                    end else ctl_fin = 1'b1;
                end
                endcase
            end

            // ---------- interrupt acceptance sequences ----------
            `EXEC_NMI: begin
                if (m_cycle == 3'd1) push_byte(BI_PC, 1'b1);
                else if (m_cycle == 3'd2) push_byte(BI_PC, 1'b0);
                else begin ctl_vec = VEC_NMI; ctl_pc = PC_VEC; ctl_wz = WZ_VEC; ctl_fin = 1'b1; end
            end
            `EXEC_INT: begin
                if (m_cycle == 3'd1) push_byte(BI_PC, 1'b1);
                else if (m_cycle == 3'd2) push_byte(BI_PC, 1'b0);
                else if (m_cycle == 3'd3) begin
                    if (im == 2'd2) begin
                        ctl_tmp16 = T16_IV; ctl_ab = AB_IV;
                        mstart(`BUSOP_MRD, AL_ABUS, 4'd0);
                    end else begin
                        ctl_vec = VEC_IM; ctl_pc = PC_VEC; ctl_wz = WZ_VEC; ctl_fin = 1'b1;
                    end
                end
                else if (m_cycle == 3'd4) begin
                    ctl_tmpl = TL_RB; ctl_ab = AB_T16; ctl_inc = INC_P1;
                    mstart(`BUSOP_MRD, AL_IDU, 4'd0);
                end
                else begin ctl_pc = PC_NN; ctl_wz = WZ_NN; ctl_fin = 1'b1; end
            end

            default: ctl_fin = 1'b1;
            endcase
        end
    end

    // =======================================================================
    // address side: abus, incrementer, effective-address adder
    // =======================================================================
    reg  [15:0] rf_ab;
    always @* begin
        case (ctl_ri)
            RI_PC:   rf_ab = r_pc;
            RI_SP:   rf_ab = r_sp;
            RI_WZ:   rf_ab = r_wz;
            RI_HL:   rf_ab = r_hl;
            RI_DE:   rf_ab = r_de;
            RI_BC:   rf_ab = r_bc;
            RI_HLP:  rf_ab = rf_hlp;
            default: rf_ab = rf_rp;
        endcase
    end
    wire [7:0]  b_dec = r_bc[15:8] - 8'd1;      // B-1 (DJNZ, INI, OUTI)
    reg  [15:0] abus;
    always @* begin
        case (ctl_ab)
            AB_REG:  abus = rf_ab;
            AB_NN:   abus = {rbyte, tmpl};
            AB_AN:   abus = {A_cur, rbyte};
            AB_IV:   abus = {reg_i, tmp8};
            AB_T16:  abus = tmp16;
            default: abus = {b_dec, r_bc[7:0]};
        endcase
    end
    // IDU: 16-bit incrementer / decrementer on the address side
    wire [15:0] idu = abus + ((ctl_inc == INC_P1) ? 16'h0001 :
                              (ctl_inc == INC_M1) ? 16'hFFFF : 16'hFFFE);
    // effective address: PC + e (JR, DJNZ, block repeat PC-1), IX + d
    wire [15:0] ea_base = ctl_ea_pc ? r_pc : rf_hlp;
    wire [7:0]  ea_d    = (ctl_ea_d == EA_RB) ? rbyte : (ctl_ea_d == EA_TL) ? tmpl : 8'hFF;
    wire [15:0] ea      = ea_base + {{8{ea_d[7]}}, ea_d};
    // opcode fetch PC+1 (the M1 latch edge never runs the execute step)
    wire [15:0] pc_inc  = r_pc + 16'd1;
    // block-move pointer and counter steps
    wire [15:0] bc_dec  = r_bc - 16'd1;
    wire [15:0] hl_step = r_hl + (aux_w[0] ? 16'hFFFF : 16'h0001);
    wire [15:0] de_step = r_de + (aux_w[0] ? 16'hFFFF : 16'h0001);

    // 16-bit ALU: ADD HL,rp / ADC HL,rp / SBC HL,rp (HL is IX/IY under DD/FD)
    wire        a16_sub = (exec_w == `EXEC_SBC16);
    wire        a16_cy  = (exec_w == `EXEC_ADC16) ? F_cur[0] : (a16_sub ? ~F_cur[0] : 1'b0);
    wire [15:0] a16_y   = a16_sub ? ~rf_rp : rf_rp;
    wire [16:0] a16_s   = {1'b0, rf_hlp} + {1'b0, a16_y} + {16'd0, a16_cy};
    wire [15:0] a16     = a16_s[15:0];
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

    // address latch input
    reg  [15:0] al_in;
    always @* begin
        case (ctl_al)
            AL_ABUS: al_in = abus;
            AL_IDU:  al_in = idu;
            AL_EA:   al_in = ea;
            AL_A16:  al_in = a16;
            default: al_in = hl_step;
        endcase
    end
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
            default: wdata = tmpl;
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
            default: pw_idx = rp_sel_w;
        endcase
        case (ctl_pws)
            PW_IDU:  pw_data = idu;
            PW_NN:   pw_data = {rbyte, tmpl};
            PW_A16:  pw_data = a16;
            PW_TMP:  pw_data = {tmph, tmpl};
            default: pw_data = bc_dec;
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
            default: f_data = banks_io_rep_f(tmpl,
                                  (aux_w[2:1] == 2'd2) ? b_dec : r_bc[15:8],
                                  (aux_w[2:1] == 2'd2) ? bk_ini : bk_out,
                                  idu[15:8]);
        endcase
    end

    // physical targets of the logical write ports
    wire [3:0] pw_phys = phys(pw_idx,   xbank, xaf, xdh);
    wire [3:0] bw_phys = phys(bw_pair,  xbank, xaf, xdh);
    wire [3:0] af_phys = phys(`RFP_AF,  xbank, xaf, xdh);
    wire [3:0] hl_phys = phys(`RFP_HL,  xbank, xaf, xdh);
    wire [3:0] de_phys = phys(`RFP_DE,  xbank, xaf, xdh);
    // exchanges flip the renaming, effective from the next T-state
    assign xbank_n = xbank ^ (ex_edge & ctl_exx);
    assign xaf_n   = xaf   ^ (ex_edge & ctl_ex_af);
    assign xdh_n   = xdh   ^ ((ex_edge & ctl_ex_dehl) ? (xbank ? 2'b10 : 2'b01) : 2'b00);

    always @* begin
        for (i = 0; i < 13; i = i + 1) rf_n[i] = rf[i];
        if (pc_m1_inc) rf_n[`RFP_PC] = pc_inc;
        if (ex_edge) begin
            for (i = 0; i < 13; i = i + 1) begin
                if (ctl_pw && pw_phys == i) rf_n[i] = pw_data;
                if (ctl_bw && bw_en && bw_phys == i) begin
                    if (bw_hi) rf_n[i][15:8] = bw_data;
                    else       rf_n[i][7:0]  = bw_data;
                end
                if (ctl_fw      && af_phys == i) rf_n[i][7:0] = f_data;
                if (ctl_hl_step && hl_phys == i) rf_n[i] = hl_step;
                if (ctl_de_step && de_phys == i) rf_n[i] = de_step;
            end
            case (ctl_pc)
                PC_IDU:  rf_n[`RFP_PC] = idu;
                PC_EA:   rf_n[`RFP_PC] = ea;
                PC_NN:   rf_n[`RFP_PC] = {rbyte, tmpl};
                PC_T16:  rf_n[`RFP_PC] = tmp16;
                PC_ABUS: rf_n[`RFP_PC] = abus;
                PC_VEC:  rf_n[`RFP_PC] = vec;
                default: ;
            endcase
            case (ctl_sp)
                SP_IDU:  rf_n[`RFP_SP] = idu;
                SP_ABUS: rf_n[`RFP_SP] = abus;
                default: ;
            endcase
            case (ctl_wz)
                WZ_IDU:  rf_n[`RFP_WZ] = idu;
                WZ_ALO:  rf_n[`RFP_WZ] = {A_cur, idu[7:0]};
                WZ_EA:   rf_n[`RFP_WZ] = ea;
                WZ_NN:   rf_n[`RFP_WZ] = {rbyte, tmpl};
                WZ_TMP:  rf_n[`RFP_WZ] = {tmph, tmpl};
                WZ_VEC:  rf_n[`RFP_WZ] = vec;
                default: ;
            endcase
        end
    end

    // logical F after this edge (EX AF,AF' renames AF on the same edge)
    wire [7:0] f_next = xaf_n ? rf_n[7][7:0] : rf_n[3][7:0];

    // =======================================================================
    // sequencer
    // =======================================================================
    always @* begin
        reg_i_n = reg_i; reg_r_n = reg_r; ir_n = ir;
        t_n = t_state; m_cycle_n = m_cycle;
        bus_op_n = bus_op; m_len_n = m_len; m_addr_n = m_addr; m_wdata_n = m_wdata;
        prefix_n = prefix; iff1_n = iff1; iff2_n = iff2; im_n = im; halted_n = halted;
        tmp8_n = tmp8; tmpl_n = tmpl; tmph_n = tmph; tmp16_n = tmp16;
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

        // Everything below is one T-state of work. While in_reset_hold is
        // set (the first rising edge after reset release) nothing moves:
        // that edge merely ENTERS T1 of the first M1.
        if (!in_reset_hold) begin

        // ===== clock-low-half work: what the chip did on the FALLING edge
        // of this T-state. Inputs are the *_neg copies z80_top captured on
        // that edge. =====
        // NMI is edge-triggered: falling edge seen between the previous
        // rising edge and this T-state's falling edge.
        nmi_pending_mid = nmi_pending | (prev_nmi_n & ~nmi_n_neg);

        // NMI/INT silicon sample point: rising edge of the last T-state of
        // the current M-cycle (= T_last.P) per Zilog UM0080 -- the level
        // present during the clock-high half, as captured by the negedge
        // sampler. Refreshed once per M-cycle; the last M-cycle's sample is
        // the one the instruction boundary uses to accept.
        if (t_state == m_len) begin
            nmi_sampled_n = nmi_pending_mid;
            int_sampled_n = ~int_n_neg;
        end

        // read-data latch at the falling edge of T3 (MRD) / T4 (IORD)
        if (latch_neg) tmp8_n = data_in_neg;

        // ===== clock-high-half work: what the chip does on THIS rising
        // edge. Inputs are the live pins. =====
        // NMI edge detect, second half: falling edge between this T-state's
        // falling edge and now.
        prev_nmi_n_n  = nmi_n;
        nmi_pending_n = nmi_pending_mid | (nmi_n_neg & ~nmi_n);

        // M1 opcode / INTA vector latch at the end of T2.N. Not while WAIT
        // is holding T2 as a Tw -- the opcode is captured on the rising
        // edge that ends the LAST Tw (mirrors cmodel `!stalled`). PC+1 of
        // the fetch is in the register-file block (pc_m1_inc).
        if (latch_pos && !stall) begin
            tmp8_n  = data_in;
            reg_r_n = {reg_r[7], (reg_r[6:0] + 7'd1)};
            if (bus_op == `BUSOP_M1) begin
                ir_n = data_in;
                if (suppress_decode) suppress_decode_n = 1'b0;     // ack / halt: no PC++ / decode
            end else if (im == 2'd0) begin                         // INTA
                // IM0: the device supplies the opcode; PC still points at
                // its operands, so decode it without a fetch / PC++.
                ir_n = data_in; irq_seq_n = 2'd0;
            end
        end

        // bus grant (DMA): idle while BUSREQ held. BUSREQ is sampled on the
        // rising edge (as on silicon), so the post-grant M1 always starts
        // T1 on a rising edge.
        if (bus_granted) begin
            if (busreq_n) begin                  // released: resume with a fresh M1
                bus_granted_n = 1'b0;
                bus_op_n = `BUSOP_M1; m_addr_n = r_pc; m_len_n = 4'd4;
                t_n = 4'd1; m_cycle_n = 3'd1;
            end
            // else: hold all state (idle)
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
            case (ctl_tmpl)
                TL_RB:   tmpl_n = rbyte;
                TL_RRD:  tmpl_n = rrd_mem;
                default: ;
            endcase
            if (ctl_tmph) tmph_n = rbyte;
            case (ctl_tmp16)
                T16_NN:  tmp16_n = {rbyte, tmpl};
                T16_IV:  tmp16_n = {reg_i, tmp8};
                default: ;
            endcase
            case (ctl_iff)
                IFF_DI:   begin iff1_n = 1'b0; iff2_n = 1'b0; end
                IFF_EI:   begin iff1_n = 1'b1; iff2_n = 1'b1; ei_delay_n = 1'b1; end
                IFF_RETN: iff1_n = iff2;
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
                // begin_next: decide bus grant / NMI / INT / HALT / next opcode
                if (!busreq_n) begin
                    bus_granted_n = 1'b1;            // DMA owns the bus next cycle
                end else begin
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
                        bus_op_n = `BUSOP_M1; m_addr_n = rf_n[`RFP_PC]; m_wdata_n = 8'h0;
                        m_len_n = 4'd5; t_n = 4'd1; m_cycle_n = 3'd1;
                        suppress_decode_n = 1'b1;
                    end else if (allow_int && int_sampled_n && iff1_n) begin
                        int_sampled_n = 1'b0;
                        // Same: silicon-faithful HALT exit, no PC bump.
                        halted_n = 1'b0; iff1_n = 1'b0; iff2_n = 1'b0;
                        irq_seq_n = 2'd2;
                        bus_op_n = `BUSOP_INTA; m_addr_n = rf_n[`RFP_PC]; m_wdata_n = 8'h0;
                        // T1 T2 Tw Tw T3 T4 (+1 internal state for IM1/IM2's PC push)
                        m_len_n = (im == 2'd0) ? 4'd6 : 4'd7; t_n = 4'd1; m_cycle_n = 3'd1;
                    end else if (halted_n) begin
                        irq_seq_n = 2'd3;
                        bus_op_n = `BUSOP_M1; m_addr_n = rf_n[`RFP_PC]; m_wdata_n = 8'h0;
                        m_len_n = 4'd4; t_n = 4'd1; m_cycle_n = 3'd1;
                        suppress_decode_n = 1'b1;
                    end else begin
                        irq_seq_n = 2'd0;
                        bus_op_n = `BUSOP_M1; m_addr_n = rf_n[`RFP_PC]; m_wdata_n = 8'h0;
                        m_len_n = 4'd4; t_n = 4'd1; m_cycle_n = 3'd1;
                    end
                end
            end
        end
        end  // !in_reset_hold
    end

    // =======================================================================
    // registers
    // =======================================================================
    // Async-assert reset drives every FF to a constant (required for
    // async-reset DFF cell inference, and for gate-level sim where FFs have
    // no initializers). Release is synchronous: the first rising edge after
    // reset_n goes high clears `in_reset_hold` WITHOUT advancing state (the
    // combinational logic holds everything while it is set), so that edge
    // enters T1 of the first M1 at PC=0 and z80_top's former loads the T1.P
    // pin image on it. The C model additionally models silicon's reset-
    // assert filter (~5 half-cycles of continued execution) and release
    // settle (~4 half-cycles) -- RTL resets immediately on reset_n=0 and
    // starts immediately on release; a pin_scenarios (informational)
    // divergence only, see docs/known-differences.md.
    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            // 0x5555 matches perfectz80's gate-level Visual-Z80 netlist
            // boot pattern; see cmodel/z80_core.c z80_reset() comment.
            for (ri = 0; ri < 13; ri = ri + 1) rf[ri] <= 16'h5555;
            rf[`RFP_PC] <= 16'h0000;
            reg_i <= 8'h00; reg_r <= 8'h00; ir <= 8'h00;
            t_state <= 4'd1; m_cycle <= 3'd1;
            bus_op <= `BUSOP_M1; m_len <= 4'd4; m_addr <= 16'h0000; m_wdata <= 8'h00;
            prefix <= `PFX_NONE; iff1 <= 1'b0; iff2 <= 1'b0; im <= 2'd0; halted <= 1'b0;
            tmp8 <= 8'h00; tmpl <= 8'h00; tmph <= 8'h00; tmp16 <= 16'h0000;
            nmi_pending <= 1'b0; prev_nmi_n <= 1'b1; ei_delay <= 1'b0;
            nmi_sampled <= 1'b0; int_sampled <= 1'b0;
            suppress_decode <= 1'b0; bus_granted <= 1'b0; irq_seq <= 2'd0;
            reg_q <= 8'h00; f_modified <= 1'b0;
            xbank <= 1'b0; xaf <= 1'b0; xdh <= 2'b00;
            in_reset_hold <= 1'b1;
        end else begin
            in_reset_hold <= 1'b0;
            for (ri = 0; ri < 13; ri = ri + 1) rf[ri] <= rf_n[ri];
            xbank <= xbank_n; xaf <= xaf_n; xdh <= xdh_n;
            reg_i <= reg_i_n; reg_r <= reg_r_n; ir <= ir_n;
            t_state <= t_n; m_cycle <= m_cycle_n;
            bus_op <= bus_op_n; m_len <= m_len_n; m_addr <= m_addr_n; m_wdata <= m_wdata_n;
            prefix <= prefix_n; iff1 <= iff1_n; iff2 <= iff2_n; im <= im_n; halted <= halted_n;
            tmp8 <= tmp8_n; tmpl <= tmpl_n; tmph <= tmph_n; tmp16 <= tmp16_n;
            nmi_pending <= nmi_pending_n; prev_nmi_n <= prev_nmi_n_n; ei_delay <= ei_delay_n;
            nmi_sampled <= nmi_sampled_n; int_sampled <= int_sampled_n;
            suppress_decode <= suppress_decode_n; bus_granted <= bus_granted_n; irq_seq <= irq_seq_n;
            reg_q <= reg_q_n; f_modified <= f_modified_n;
        end
    end

endmodule
