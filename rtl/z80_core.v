// ===========================================================================
// z80_core.v - top-level core: phase engine + micro-sequencer + register file.
//
// 1:1 port of the C model (cmodel/z80.c + z80_control.c). Single clock, DFF
// only, no latches, no `initial` in synthesizable logic. The clock runs at 2x
// the Z80 rate: each Z80 T-state is two clocks (phi 0 then 1). All state is
// computed combinationally as *_n and registered on posedge clk; async-assert
// reset. External pin timing is produced by z80_timing from registered state.
// ===========================================================================
`include "z80_defs.vh"

module z80_core (
    input  wire        clk,
    input  wire        reset_n,
    // bus
    output wire [15:0] addr,
    input  wire [7:0]  data_in,
    output wire [7:0]  data_out,
    output wire        data_drive,
    output wire        m1_n,
    output wire        mreq_n,
    output wire        iorq_n,
    output wire        rd_n,
    output wire        wr_n,
    output wire        rfsh_n,
    output wire        halt_n,
    output wire        busack_n,
    // control inputs
    input  wire        wait_n,
    input  wire        int_n,
    input  wire        nmi_n,
    input  wire        busreq_n,
    // debug (for trace)
    output wire [3:0]  dbg_t,
    output wire        dbg_phi,
    output wire [2:0]  dbg_m
);
    integer i;

    // ---- registered state ----
    reg [15:0] rf [0:12];
    reg [7:0]  reg_i, reg_r, ir;
    reg [7:0]  reg_q;       // F left by previous F-modifying instruction, else 0
    reg        f_modified;  // set this cycle if any instruction wrote F
    reg        phi;
    reg [3:0]  t_state;
    reg [2:0]  m_cycle;
    reg [2:0]  bus_op;
    reg [3:0]  m_len;
    reg [15:0] m_addr;
    reg [7:0]  m_wdata;
    reg [2:0]  prefix;
    reg        iff1, iff2;
    reg [1:0]  im;
    reg        halted;
    reg [7:0]  tmp8, tmpl, tmph;
    reg [15:0] tmp16;
    reg [31:0] cycle, instr_count;
    reg        decoded;
    reg        nmi_pending, prev_nmi_n, ei_delay, suppress_decode, bus_granted;
    reg [1:0]  irq_seq;        // 0 none, 1 NMI, 2 INT, 3 HALT-nop

    // ---- next-state ----
    reg [15:0] rf_n [0:12];
    reg [7:0]  reg_i_n, reg_r_n, ir_n;
    reg        phi_n;
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
    reg [31:0] cycle_n, instr_count_n;
    reg        decoded_n;
    reg        nmi_pending_n, prev_nmi_n_n, ei_delay_n, suppress_decode_n, bus_granted_n;
    reg [1:0]  irq_seq_n;
    reg        allow_int;
    reg        fin;

    // ---- decode (combinational) ----
    wire [6:0] exec_w;
    wire [4:0] flag_mode_w;
    wire [2:0] alu_op_w, rf_src_w, rf_dst_w, cc_w, rot_op_w, bit_index_w;
    wire [1:0] cb_kind_w, idx_w;
    wire [3:0] rp_sel_w, aux_w;
    wire [7:0] rst_addr_w;
    wire       uses_cc_w, valid_w, use_disp_w;

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

    // ---- timing pin drive (combinational from registered state) ----
    z80_timing u_timing (
        .bus_op(bus_op), .t_state(t_state[2:0]), .phi(phi),
        .m_addr(m_addr), .m_wdata(m_wdata), .reg_i(reg_i), .reg_r(reg_r),
        .addr(addr), .data_out(data_out), .data_drive(data_drive),
        .m1_n(m1_n), .mreq_n(mreq_n), .iorq_n(iorq_n),
        .rd_n(rd_n), .wr_n(wr_n), .rfsh_n(rfsh_n)
    );
    assign halt_n   = halted ? 1'b0 : 1'b1;
    assign busack_n = bus_granted ? 1'b0 : 1'b1;
    assign dbg_t = t_state; assign dbg_phi = phi; assign dbg_m = m_cycle;

    // ---- timing-point predicates ----
    wire islatch =  (phi == 1'b1) &&
                  ( ((bus_op == `BUSOP_M1)   && (t_state == 4'd2)) ||
                    ((bus_op == `BUSOP_INTA) && (t_state == 4'd2)) ||
                    ((bus_op == `BUSOP_MRD)  && (t_state == 4'd3)) ||
                    ((bus_op == `BUSOP_IORD) && (t_state == 4'd4)) );
    wire iswait  =  (phi == 1'b1) &&
                  ( (((bus_op == `BUSOP_M1) || (bus_op == `BUSOP_MRD) ||
                      (bus_op == `BUSOP_MWR)) && (t_state == 4'd2)) ||
                    (((bus_op == `BUSOP_IORD) || (bus_op == `BUSOP_IOWR)) && (t_state == 4'd3)) );
    wire stall   = iswait && (wait_n == 1'b0);
    wire [7:0] rbyte = islatch ? data_in : tmp8;

    // ---- 8-bit register read ----
    function [7:0] getr8; input [2:0] sel;
        case (sel)
            3'd0: getr8 = rf[`RFP_BC][15:8];
            3'd1: getr8 = rf[`RFP_BC][7:0];
            3'd2: getr8 = rf[`RFP_DE][15:8];
            3'd3: getr8 = rf[`RFP_DE][7:0];
            3'd4: getr8 = rf[`RFP_HL][15:8];
            3'd5: getr8 = rf[`RFP_HL][7:0];
            3'd6: getr8 = 8'h00;
            default: getr8 = rf[`RFP_AF][15:8];
        endcase
    endfunction

    // index-aware 8-bit read: H/L -> IXH/IXL only under DD/FD with no memory operand
    function [7:0] getri; input [2:0] sel;
        if (idx_w != 2'd0 && !use_disp_w && sel == 3'd4)      getri = rf[hlp][15:8];
        else if (idx_w != 2'd0 && !use_disp_w && sel == 3'd5) getri = rf[hlp][7:0];
        else getri = getr8(sel);
    endfunction

    function [3:0] baselen; input [2:0] bop;
        case (bop)
            `BUSOP_M1:   baselen = 4'd4;
            `BUSOP_MRD:  baselen = 4'd3;
            `BUSOP_MWR:  baselen = 4'd3;
            `BUSOP_IORD: baselen = 4'd4;
            `BUSOP_IOWR: baselen = 4'd4;
            `BUSOP_INTA: baselen = 4'd5;
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

    // ---- ALU operand selection + instance ----
    reg  [7:0] alu_a, alu_b, alu_xy;
    reg  [4:0] alu_md;
    reg  [2:0] alu_rot, alu_bit;
    wire [7:0] A_cur = rf[`RFP_AF][15:8];
    wire [7:0] F_cur = rf[`RFP_AF][7:0];
    always @* begin
        alu_a = A_cur;
        alu_b = 8'h00;
        alu_md = flag_mode_w;
        alu_xy = 8'h00;
        alu_rot = rot_op_w;
        alu_bit = bit_index_w;
        case (exec_w)
            `EXEC_ALU_R:           alu_b = getri(rf_src_w);
            `EXEC_ALU_N, `EXEC_ALU_M:alu_b = rbyte;
            `EXEC_INC_R, `EXEC_DEC_R:alu_b = getri(rf_dst_w);
            `EXEC_INC_M, `EXEC_DEC_M:alu_b = rbyte;
            `EXEC_CB_R: begin alu_b = getr8(rf_src_w); alu_xy = getr8(rf_src_w); end
            `EXEC_CB_M: begin alu_b = rbyte;           alu_xy = rf[`RFP_WZ][15:8]; end
            `EXEC_DDCB: begin alu_b = rbyte;           alu_xy = rf[`RFP_WZ][15:8];
                            alu_md = (tmph[7:6] == `CB_BIT) ? `FLAG_BIT : `FLAG_ROT;
                            alu_rot = tmph[5:3]; alu_bit = tmph[5:3]; end
            default:             alu_b = 8'h00;
        endcase
    end
    wire [7:0]  alu_res, alu_fout;
    wire [15:0] alu_res16;
    wire [7:0]  alu_mem_byte;
    z80_alu u_alu (
        .mode(alu_md), .alu_op(alu_op_w), .rot_op(alu_rot),
        .bit_idx(alu_bit), .xy_src(alu_xy),
        .a(alu_a), .b(alu_b), .oldf(F_cur), .q(reg_q),
        .a16(16'h0000), .b16(16'h0000), .iff2_in(1'b0),
        .res(alu_res), .fout(alu_fout),
        .res16(alu_res16), .mem_byte(alu_mem_byte)
    );

    // ---- per-(M,T) micro-sequencer (incremental migration target).
    // When seq_active is high for the current eff_exec, the legacy case
    // dispatch's branch for that opcode is gone and seq drives the
    // corresponding ctl_* signals. ----
    wire       seq_active, seq_fin;
    wire [1:0] seq_iff_op;
    wire       seq_ei_delay_set;
    wire       seq_im_we, seq_halt_set, seq_pc_dec1;
    wire       seq_ex_de_hl, seq_ex_af, seq_exx, seq_pc_set_hl;
    wire       seq_reg_a_we, seq_reg_f_we, seq_reg_setri_we;
    wire [1:0] seq_reg_setri_src;
    wire       seq_start_mc, seq_pc_inc;
    wire [2:0] seq_mc_bus_op;
    wire [3:0] seq_mc_addr_src;
    wire [2:0] seq_mc_wdata_src;
    wire [3:0] seq_mc_extra_t;
    z80_seq u_seq (
        .eff_exec(eff_exec),
        .m_cycle(m_cycle),
        .t_state(t_state),
        .phi(phi),
        .alu_op_w(alu_op_w),
        .seq_active(seq_active),
        .fin(seq_fin),
        .ctl_iff_op(seq_iff_op),
        .ctl_ei_delay_set(seq_ei_delay_set),
        .ctl_im_we(seq_im_we),
        .ctl_halt_set(seq_halt_set),
        .ctl_pc_dec1(seq_pc_dec1),
        .ctl_reg_ex_de_hl(seq_ex_de_hl),
        .ctl_reg_ex_af(seq_ex_af),
        .ctl_reg_exx(seq_exx),
        .ctl_pc_set_hl(seq_pc_set_hl),
        .ctl_reg_a_we(seq_reg_a_we),
        .ctl_reg_f_we(seq_reg_f_we),
        .ctl_reg_setri_we(seq_reg_setri_we),
        .ctl_reg_setri_src(seq_reg_setri_src),
        .ctl_start_mc(seq_start_mc),
        .ctl_mc_bus_op(seq_mc_bus_op),
        .ctl_mc_addr_src(seq_mc_addr_src),
        .ctl_mc_wdata_src(seq_mc_wdata_src),
        .ctl_mc_extra_t(seq_mc_extra_t),
        .ctl_pc_inc(seq_pc_inc)
    );

    // ---- mux helpers for seq's address / wdata source selectors ----
    reg  [15:0] seq_addr_val;
    reg  [7:0]  seq_wdata_val;
    always @* begin
        case (seq_mc_addr_src)
            `ADDR_PC:      seq_addr_val = rf[`RFP_PC];
            `ADDR_HL:      seq_addr_val = rf[hlp];
            `ADDR_BC:      seq_addr_val = rf[`RFP_BC];
            `ADDR_DE:      seq_addr_val = rf[`RFP_DE];
            `ADDR_SP:      seq_addr_val = rf[`RFP_SP];
            `ADDR_WZ:      seq_addr_val = rf[`RFP_WZ];
            `ADDR_MEMADDR: seq_addr_val = memaddr;
            `ADDR_TMP16:   seq_addr_val = tmp16;
            default:       seq_addr_val = rf[`RFP_PC];
        endcase
        case (seq_mc_wdata_src)
            `WDATA_A:         seq_wdata_val = A_cur;
            `WDATA_GETRI_SRC: seq_wdata_val = getri(rf_src_w);
            `WDATA_RBYTE:     seq_wdata_val = rbyte;
            default:          seq_wdata_val = 8'h00;
        endcase
    end

    // ---- 8-bit register write into rf_n ----
    task setr8; input [2:0] sel; input [7:0] val;
        case (sel)
            3'd0: rf_n[`RFP_BC][15:8] = val;
            3'd1: rf_n[`RFP_BC][7:0]  = val;
            3'd2: rf_n[`RFP_DE][15:8] = val;
            3'd3: rf_n[`RFP_DE][7:0]  = val;
            3'd4: rf_n[`RFP_HL][15:8] = val;
            3'd5: rf_n[`RFP_HL][7:0]  = val;
            3'd6: ;                       // (HL): memory, not a register
            default: rf_n[`RFP_AF][15:8] = val;
        endcase
    endtask

    // index-aware 8-bit write (H/L -> IXH/IXL under DD/FD, no memory operand)
    task setri; input [2:0] sel; input [7:0] val;
        if (idx_w != 2'd0 && !use_disp_w && sel == 3'd4)
            rf_n[hlp][15:8] = val;
        else if (idx_w != 2'd0 && !use_disp_w && sel == 3'd5)
            rf_n[hlp][7:0] = val;
        else setr8(sel, val);
    endtask

    task startm; input [2:0] bop; input [15:0] a; input [7:0] wd; input [3:0] extra;
        begin
            bus_op_n  = bop;
            m_addr_n  = a;
            m_wdata_n = wd;
            m_len_n   = baselen(bop) + extra;
            t_n       = 4'd1;
            phi_n     = 1'b0;
            m_cycle_n = m_cycle + 3'd1;
        end
    endtask

    // 16-bit ADD HL,rp (inline; IDU/ALU detail handled in C model too)
    reg [16:0] add16;
    reg [12:0] add12;
    reg [7:0]  f16;
    reg [7:0]  cbres;
    reg [16:0] r17;
    reg [12:0] r13;
    reg [7:0]  edf, edv;
    reg [2:0]  mm;
    reg [15:0] memaddr;
    reg [15:0] bbc;            // block: BC after decrement
    reg [8:0]  bk;             // block-IO: data + (C±1) or +L
    reg [7:0]  bdata, bn2, bnewB;
    reg [4:0]  bhc;            // block-CP: low-nibble borrow

    // ---- combinational next-state ----
    always @* begin
        for (i = 0; i < 13; i = i + 1) rf_n[i] = rf[i];
        reg_i_n = reg_i; reg_r_n = reg_r; ir_n = ir;
        phi_n = phi; t_n = t_state; m_cycle_n = m_cycle;
        bus_op_n = bus_op; m_len_n = m_len; m_addr_n = m_addr; m_wdata_n = m_wdata;
        prefix_n = prefix; iff1_n = iff1; iff2_n = iff2; im_n = im; halted_n = halted;
        tmp8_n = tmp8; tmpl_n = tmpl; tmph_n = tmph; tmp16_n = tmp16;
        reg_q_n = reg_q; f_modified_n = f_modified;
        cycle_n = cycle + 32'd1; instr_count_n = instr_count; decoded_n = decoded;
        fin = 1'b0;
        add16 = 17'd0; f16 = 8'd0;
        ei_delay_n = ei_delay; suppress_decode_n = suppress_decode;
        bus_granted_n = bus_granted; irq_seq_n = irq_seq;
        allow_int = 1'b0;

        // NMI: latch a falling edge on nmi_n
        prev_nmi_n_n = nmi_n;
        nmi_pending_n = nmi_pending | (prev_nmi_n & ~nmi_n);

        // latch on the current phase
        if (islatch) begin
            if (bus_op == `BUSOP_M1) begin
                ir_n   = data_in;
                tmp8_n = data_in;
                reg_r_n = {reg_r[7], (reg_r[6:0] + 7'd1)};
                if (suppress_decode) suppress_decode_n = 1'b0;     // ack/halt: no PC++/decode
                else begin rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; decoded_n = 1'b1; end
            end else if (bus_op == `BUSOP_INTA) begin
                tmp8_n = data_in;                                  // interrupt vector byte
                reg_r_n = {reg_r[7], (reg_r[6:0] + 7'd1)};
            end else begin
                tmp8_n = data_in;
            end
        end

        // bus grant (DMA): idle while BUSREQ held
        if (bus_granted) begin
            if (busreq_n) begin                  // released: resume with a fresh M1
                bus_granted_n = 1'b0;
                bus_op_n = `BUSOP_M1; m_addr_n = rf[`RFP_PC]; m_len_n = 4'd4;
                t_n = 4'd1; phi_n = 1'b0; m_cycle_n = 3'd1; decoded_n = 1'b0;
            end
            // else: hold all state (idle)
        end
        // advance phase
        else if (phi == 1'b0) begin
            phi_n = 1'b1;
        end else if (stall) begin
            phi_n = 1'b0;                 // hold T-state as Tw
        end else begin
            phi_n = 1'b0;
            if ((t_state + 4'd1) > m_len) begin
                // (IX+d)/(IY+d) displacement preamble: fetch d, then 5T addr calc
                // (skipped during interrupt/HALT sequences)
                mm = m_cycle; memaddr = rf[hlp];
                if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w && m_cycle == 3'd1) begin
                    startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                    rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1;
                end else if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w && m_cycle == 3'd2 && eff_exec != `EXEC_LD_M_N) begin
                    rf_n[`RFP_WZ] = rf[hlp] + {{8{rbyte[7]}}, rbyte};
                    startm(`BUSOP_INTERNAL, rf[hlp] + {{8{rbyte[7]}}, rbyte}, 8'h0, 4'd5);
                end else begin
                // For LD (IX+d),n on m_cycle==2: compute WZ here (no internal cycle),
                // then fall through to EXEC_LD_M_N with mm==1 (N read with +2T padding).
                if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w && eff_exec == `EXEC_LD_M_N && m_cycle == 3'd2) begin
                    rf_n[`RFP_WZ] = rf[hlp] + {{8{rbyte[7]}}, rbyte};
                end
                if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w) begin
                    if (eff_exec == `EXEC_LD_M_N) begin
                        mm = m_cycle - 3'd1;   /* no internal cycle */
                        memaddr = (m_cycle == 3'd2) ? (rf[hlp] + {{8{rbyte[7]}}, rbyte}) : rf[`RFP_WZ];
                    end else begin
                        mm = m_cycle - 3'd2;
                        memaddr = rf[`RFP_WZ];
                    end
                end
                // ===== micro-sequencer (mirror z80_control.c) =====
                case (eff_exec)
                `EXEC_NOP, `EXEC_ILLEGAL: ;   /* migrated to z80_seq: fin asserted via seq_fin */
                `EXEC_PREFIX: begin
                    case (ir)
                        8'hDD: prefix_n = `PFX_DD;
                        8'hFD: prefix_n = `PFX_FD;
                        8'hED: prefix_n = `PFX_ED;
                        8'hCB: prefix_n = (prefix == `PFX_DD) ? `PFX_DDCB :
                                          (prefix == `PFX_FD) ? `PFX_FDCB : `PFX_CB;
                        default: ;
                    endcase
                    if (prefix_n == `PFX_DDCB || prefix_n == `PFX_FDCB) begin
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); /* read displacement d */
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1;
                    end else begin
                        bus_op_n = `BUSOP_M1; m_addr_n = rf[`RFP_PC]; m_wdata_n = 8'h0;
                        m_len_n = 4'd4; t_n = 4'd1; phi_n = 1'b0; m_cycle_n = 3'd1; decoded_n = 1'b0;
                    end
                end
                `EXEC_DDCB: begin
                    if (m_cycle == 3'd2) begin tmpl_n = rbyte;           /* d */
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd3) begin tmph_n = rbyte;      /* op */
                        rf_n[`RFP_WZ] = rf[hlp] + {{8{tmpl[7]}}, tmpl};
                        startm(`BUSOP_INTERNAL, rf[hlp] + {{8{tmpl[7]}}, tmpl}, 8'h0, 4'd2); end
                    else if (m_cycle == 3'd4) startm(`BUSOP_MRD, rf[`RFP_WZ], 8'h0, 4'd1); /* read (IX+d) 4T */
                    else if (m_cycle == 3'd5) begin
                        if (tmph[7:6] == `CB_BIT) begin rf_n[`RFP_AF][7:0] = alu_fout; fin = 1'b1; end
                        else begin
                            case (tmph[7:6])
                                `CB_ROT: begin cbres = alu_res; rf_n[`RFP_AF][7:0] = alu_fout; end
                                `CB_RES: cbres = rbyte & ~(8'h01 << tmph[5:3]);
                                default: cbres = rbyte |  (8'h01 << tmph[5:3]);
                            endcase
                            if (tmph[2:0] != 3'd6) setr8(tmph[2:0], cbres);  /* undoc copy */
                            startm(`BUSOP_MWR, rf[`RFP_WZ], cbres, 4'd0);
                        end
                    end
                    else fin = 1'b1;
                end
                `EXEC_DI, `EXEC_EI: ;  /* migrated to z80_seq (ctl_iff_op / ctl_ei_delay_set) */
                `EXEC_HALT: ;          /* migrated to z80_seq (ctl_halt_set + ctl_pc_dec1)   */

                `EXEC_LD_R_R: ;  /* migrated to z80_seq (ctl_reg_setri_we + ctl_reg_setri_src=1) */
                `EXEC_ALU_R,
                `EXEC_ROT_A,
                `EXEC_DAA,
                `EXEC_CPL,
                `EXEC_SCF,
                `EXEC_CCF: ;     /* migrated to z80_seq (ctl_reg_a_we / ctl_reg_f_we) */
                `EXEC_INC_R, `EXEC_DEC_R: ;  /* migrated to z80_seq (ctl_reg_setri_we + ctl_reg_f_we) */

                `EXEC_EX_DE_HL,
                `EXEC_EX_AF,
                `EXEC_EXX,
                `EXEC_JP_HL: ;  /* migrated to z80_seq (ctl_reg_ex_* / ctl_pc_set_hl) */

                `EXEC_INC_RP: begin
                    if (m_cycle == 3'd1) begin rf_n[rp_sel_w] = rf[rp_sel_w] + 16'd1;
                        startm(`BUSOP_INTERNAL, rf[rp_sel_w] + 16'd1, 8'h0, 4'd2); end
                    else fin = 1'b1;
                end
                `EXEC_DEC_RP: begin
                    if (m_cycle == 3'd1) begin rf_n[rp_sel_w] = rf[rp_sel_w] - 16'd1;
                        startm(`BUSOP_INTERNAL, rf[rp_sel_w] - 16'd1, 8'h0, 4'd2); end
                    else fin = 1'b1;
                end
                `EXEC_LD_SP_HL: begin
                    if (m_cycle == 3'd1) begin rf_n[`RFP_SP] = rf[hlp];
                        startm(`BUSOP_INTERNAL, rf[hlp], 8'h0, 4'd2); end
                    else fin = 1'b1;
                end
                `EXEC_ADD_HL_RP: begin
                    if (m_cycle == 3'd1) begin
                        add16 = {1'b0, rf[hlp]} + {1'b0, rf[rp_sel_w]};
                        add12 = {1'b0, rf[hlp][11:0]} + {1'b0, rf[rp_sel_w][11:0]};
                        f16 = (F_cur & (`Z80_SF | `Z80_ZF | `Z80_PF));
                        if (add12[12]) f16 = f16 | `Z80_HF;
                        if (add16[16]) f16 = f16 | `Z80_CF;
                        f16 = f16 | (add16[15:8] & (`Z80_YF | `Z80_XF));
                        rf_n[`RFP_WZ] = rf[hlp] + 16'd1;
                        rf_n[hlp] = add16[15:0];
                        rf_n[`RFP_AF][7:0] = f16;
                        startm(`BUSOP_INTERNAL, add16[15:0], 8'h0, 4'd7);
                    end else fin = 1'b1;
                end

                `EXEC_LD_R_N: ;  /* migrated to z80_seq (ctl_start_mc / ctl_pc_inc / setri RBYTE) */
                `EXEC_ALU_N: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else begin if (alu_op_w != `ALU_CP) setr8(3'd7, alu_res);
                        rf_n[`RFP_AF][7:0] = alu_fout; fin = 1'b1; end
                end
                `EXEC_LD_R_M: begin
                    if (mm == 3'd1) startm(`BUSOP_MRD, memaddr, 8'h0, 4'd0);
                    else begin setri(rf_dst_w, rbyte); fin = 1'b1; end
                end
                `EXEC_ALU_M: begin
                    if (mm == 3'd1) startm(`BUSOP_MRD, memaddr, 8'h0, 4'd0);
                    else begin if (alu_op_w != `ALU_CP) setr8(3'd7, alu_res);
                        rf_n[`RFP_AF][7:0] = alu_fout; fin = 1'b1; end
                end
                `EXEC_ST_M_R: begin
                    if (mm == 3'd1) startm(`BUSOP_MWR, memaddr, getri(rf_src_w), 4'd0);
                    else fin = 1'b1;
                end
                `EXEC_LD_M_N: begin
                    if (mm == 3'd1) begin
                        /* LD (IX+d),n folds 2T IX+d compute into N read (5T total) */
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0,
                               (idx_w != 2'd0 && use_disp_w) ? 4'd2 : 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1;
                    end
                    else if (mm == 3'd2) startm(`BUSOP_MWR, memaddr, rbyte, 4'd0);
                    else fin = 1'b1;
                end
                `EXEC_INC_M, `EXEC_DEC_M: begin
                    if (mm == 3'd1) startm(`BUSOP_MRD, memaddr, 8'h0, 4'd1);
                    else if (mm == 3'd2) begin rf_n[`RFP_AF][7:0] = alu_fout;
                        startm(`BUSOP_MWR, memaddr, alu_res, 4'd0); end
                    else fin = 1'b1;
                end

                `EXEC_LD_RP_NN: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else begin rf_n[rp_sel_w] = {rbyte, tmpl}; fin = 1'b1; end
                end
                `EXEC_JP, `EXEC_JP_CC: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else begin
                        rf_n[`RFP_WZ] = {rbyte, tmpl};
                        if ((exec_w == `EXEC_JP) || cc_true(F_cur, cc_w)) rf_n[`RFP_PC] = {rbyte, tmpl};
                        fin = 1'b1;
                    end
                end

                `EXEC_JR: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin
                        rf_n[`RFP_PC] = rf[`RFP_PC] + {{8{rbyte[7]}}, rbyte};
                        rf_n[`RFP_WZ] = rf[`RFP_PC] + {{8{rbyte[7]}}, rbyte};
                        startm(`BUSOP_INTERNAL, rf[`RFP_PC] + {{8{rbyte[7]}}, rbyte}, 8'h0, 4'd5);
                    end else fin = 1'b1;
                end
                `EXEC_JR_CC: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin
                        if (cc_true(F_cur, cc_w)) begin
                            rf_n[`RFP_PC] = rf[`RFP_PC] + {{8{rbyte[7]}}, rbyte};
                            rf_n[`RFP_WZ] = rf[`RFP_PC] + {{8{rbyte[7]}}, rbyte};
                            startm(`BUSOP_INTERNAL, rf[`RFP_PC] + {{8{rbyte[7]}}, rbyte}, 8'h0, 4'd5);
                        end else fin = 1'b1;
                    end else fin = 1'b1;
                end
                `EXEC_DJNZ: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                    else if (m_cycle == 3'd2) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd3) begin
                        rf_n[`RFP_BC][15:8] = rf[`RFP_BC][15:8] - 8'd1;
                        if ((rf[`RFP_BC][15:8] - 8'd1) != 8'd0) begin
                            rf_n[`RFP_PC] = rf[`RFP_PC] + {{8{rbyte[7]}}, rbyte};
                            rf_n[`RFP_WZ] = rf[`RFP_PC] + {{8{rbyte[7]}}, rbyte};
                            startm(`BUSOP_INTERNAL, rf[`RFP_PC] + {{8{rbyte[7]}}, rbyte}, 8'h0, 4'd5);
                        end else fin = 1'b1;
                    end else fin = 1'b1;
                end

                `EXEC_CALL, `EXEC_CALL_CC: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd3) begin
                        tmp16_n = {rbyte, tmpl}; rf_n[`RFP_WZ] = {rbyte, tmpl};
                        if ((exec_w == `EXEC_CALL) || cc_true(F_cur, cc_w))
                            startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                        else fin = 1'b1;
                    end
                    else if (m_cycle == 3'd4) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[`RFP_PC][15:8], 4'd0); end
                    else if (m_cycle == 3'd5) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[`RFP_PC][7:0], 4'd0); end
                    else begin rf_n[`RFP_PC] = tmp16; fin = 1'b1; end
                end
                `EXEC_RET: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_MRD, rf[`RFP_SP], 8'h0, 4'd0);
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte; rf_n[`RFP_SP] = rf[`RFP_SP] + 16'd1;
                        startm(`BUSOP_MRD, rf[`RFP_SP] + 16'd1, 8'h0, 4'd0); end
                    else begin rf_n[`RFP_SP] = rf[`RFP_SP] + 16'd1;
                        rf_n[`RFP_PC] = {rbyte, tmpl}; rf_n[`RFP_WZ] = {rbyte, tmpl}; fin = 1'b1; end
                end
                `EXEC_RET_CC: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                    else if (m_cycle == 3'd2) begin
                        if (cc_true(F_cur, cc_w)) startm(`BUSOP_MRD, rf[`RFP_SP], 8'h0, 4'd0);
                        else fin = 1'b1;
                    end
                    else if (m_cycle == 3'd3) begin tmpl_n = rbyte; rf_n[`RFP_SP] = rf[`RFP_SP] + 16'd1;
                        startm(`BUSOP_MRD, rf[`RFP_SP] + 16'd1, 8'h0, 4'd0); end
                    else begin rf_n[`RFP_SP] = rf[`RFP_SP] + 16'd1;
                        rf_n[`RFP_PC] = {rbyte, tmpl}; rf_n[`RFP_WZ] = {rbyte, tmpl}; fin = 1'b1; end
                end
                `EXEC_RST: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                    else if (m_cycle == 3'd2) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[`RFP_PC][15:8], 4'd0); end
                    else if (m_cycle == 3'd3) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[`RFP_PC][7:0], 4'd0); end
                    else begin rf_n[`RFP_PC] = {8'h00, rst_addr_w};
                        rf_n[`RFP_WZ] = {8'h00, rst_addr_w}; fin = 1'b1; end
                end
                `EXEC_PUSH: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                    else if (m_cycle == 3'd2) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[rp_sel_w][15:8], 4'd0); end
                    else if (m_cycle == 3'd3) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[rp_sel_w][7:0], 4'd0); end
                    else fin = 1'b1;
                end
                `EXEC_POP: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_MRD, rf[`RFP_SP], 8'h0, 4'd0);
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte; rf_n[`RFP_SP] = rf[`RFP_SP] + 16'd1;
                        startm(`BUSOP_MRD, rf[`RFP_SP] + 16'd1, 8'h0, 4'd0); end
                    else begin rf_n[`RFP_SP] = rf[`RFP_SP] + 16'd1;
                        rf_n[rp_sel_w] = {rbyte, tmpl}; fin = 1'b1; end
                end

                `EXEC_LD_A_RP: begin
                    if (m_cycle == 3'd1) begin rf_n[`RFP_WZ] = rf[rp_sel_w] + 16'd1;
                        startm(`BUSOP_MRD, rf[rp_sel_w], 8'h0, 4'd0); end
                    else begin setr8(3'd7, rbyte); fin = 1'b1; end
                end
                `EXEC_LD_RP_A: begin
                    if (m_cycle == 3'd1) begin
                        rf_n[`RFP_WZ] = {A_cur, (rf[rp_sel_w][7:0] + 8'd1)};
                        startm(`BUSOP_MWR, rf[rp_sel_w], A_cur, 4'd0); end
                    else fin = 1'b1;
                end
                `EXEC_LD_A_NN: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd3) begin tmp16_n = {rbyte, tmpl};
                        rf_n[`RFP_WZ] = {rbyte, tmpl} + 16'd1; startm(`BUSOP_MRD, {rbyte, tmpl}, 8'h0, 4'd0); end
                    else begin setr8(3'd7, rbyte); fin = 1'b1; end
                end
                `EXEC_LD_NN_A: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd3) begin
                        rf_n[`RFP_WZ] = {A_cur, (tmpl + 8'd1)};
                        startm(`BUSOP_MWR, {rbyte, tmpl}, A_cur, 4'd0); end
                    else fin = 1'b1;
                end
                `EXEC_LD_HL_NN: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd3) begin tmp16_n = {rbyte, tmpl};
                        rf_n[`RFP_WZ] = {rbyte, tmpl} + 16'd1; startm(`BUSOP_MRD, {rbyte, tmpl}, 8'h0, 4'd0); end
                    else if (m_cycle == 3'd4) begin rf_n[hlp][7:0] = rbyte;
                        startm(`BUSOP_MRD, tmp16 + 16'd1, 8'h0, 4'd0); end
                    else begin rf_n[hlp][15:8] = rbyte; fin = 1'b1; end
                end
                `EXEC_LD_NN_HL: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd3) begin tmp16_n = {rbyte, tmpl};
                        rf_n[`RFP_WZ] = {rbyte, tmpl} + 16'd1;
                        startm(`BUSOP_MWR, {rbyte, tmpl}, rf[hlp][7:0], 4'd0); end
                    else if (m_cycle == 3'd4) startm(`BUSOP_MWR, tmp16 + 16'd1, rf[hlp][15:8], 4'd0);
                    else fin = 1'b1;
                end

                `EXEC_IN_A_N: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin rf_n[`RFP_WZ] = {A_cur, rbyte} + 16'd1;
                        startm(`BUSOP_IORD, {A_cur, rbyte}, 8'h0, 4'd0); end
                    else begin setr8(3'd7, rbyte); fin = 1'b1; end
                end
                `EXEC_OUT_N_A: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin rf_n[`RFP_WZ] = {A_cur, (rbyte + 8'd1)};
                        startm(`BUSOP_IOWR, {A_cur, rbyte}, A_cur, 4'd0); end
                    else fin = 1'b1;
                end
                `EXEC_EX_SP_HL: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_MRD, rf[`RFP_SP], 8'h0, 4'd0);
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_SP] + 16'd1, 8'h0, 4'd1); end
                    else if (m_cycle == 3'd3) begin tmph_n = rbyte;
                        startm(`BUSOP_MWR, rf[`RFP_SP] + 16'd1, rf[hlp][15:8], 4'd0); end
                    else if (m_cycle == 3'd4) startm(`BUSOP_MWR, rf[`RFP_SP], rf[hlp][7:0], 4'd2);
                    else begin rf_n[hlp] = {tmph, tmpl}; rf_n[`RFP_WZ] = {tmph, tmpl}; fin = 1'b1; end
                end

                `EXEC_CB_R: begin
                    case (cb_kind_w)
                        `CB_ROT: begin setr8(rf_dst_w, alu_res); rf_n[`RFP_AF][7:0] = alu_fout; end
                        `CB_BIT: rf_n[`RFP_AF][7:0] = alu_fout;
                        `CB_RES: setr8(rf_dst_w, getr8(rf_src_w) & ~(8'h01 << bit_index_w));
                        `CB_SET: setr8(rf_dst_w, getr8(rf_src_w) |  (8'h01 << bit_index_w));
                    endcase
                    fin = 1'b1;
                end
                `EXEC_CB_M: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_MRD, rf[`RFP_HL], 8'h0, 4'd1);
                    else if (cb_kind_w == `CB_BIT) begin rf_n[`RFP_AF][7:0] = alu_fout; fin = 1'b1; end
                    else if (m_cycle == 3'd2) begin
                        case (cb_kind_w)
                            `CB_ROT: begin cbres = alu_res; rf_n[`RFP_AF][7:0] = alu_fout; end
                            `CB_RES: cbres = rbyte & ~(8'h01 << bit_index_w);
                            `CB_SET: cbres = rbyte |  (8'h01 << bit_index_w);
                            default: cbres = rbyte;
                        endcase
                        startm(`BUSOP_MWR, rf[`RFP_HL], cbres, 4'd0);
                    end else fin = 1'b1;
                end

                /* ---------- ED page (non-block) ---------- */
                `EXEC_ADC16, `EXEC_SBC16: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_HL], 8'h0, 4'd7);
                    else begin
                        rf_n[`RFP_WZ] = rf[`RFP_HL] + 16'd1;
                        if (exec_w == `EXEC_ADC16) begin
                            r17 = {1'b0, rf[`RFP_HL]} + {1'b0, rf[rp_sel_w]} + {16'b0, F_cur[0]};
                            r13 = {1'b0, rf[`RFP_HL][11:0]} + {1'b0, rf[rp_sel_w][11:0]} + {12'b0, F_cur[0]};
                            edf = 8'h0;
                            if (r13[12]) edf = edf | `Z80_HF;
                            if ((~(rf[`RFP_HL] ^ rf[rp_sel_w]) & (rf[`RFP_HL] ^ r17[15:0]) & 16'h8000) != 16'h0) edf = edf | `Z80_PF;
                            if (r17[16]) edf = edf | `Z80_CF;
                        end else begin
                            r17 = {1'b0, rf[`RFP_HL]} - {1'b0, rf[rp_sel_w]} - {16'b0, F_cur[0]};
                            r13 = {1'b0, rf[`RFP_HL][11:0]} - {1'b0, rf[rp_sel_w][11:0]} - {12'b0, F_cur[0]};
                            edf = `Z80_NF;
                            if (r13[12]) edf = edf | `Z80_HF;
                            if (((rf[`RFP_HL] ^ rf[rp_sel_w]) & (rf[`RFP_HL] ^ r17[15:0]) & 16'h8000) != 16'h0) edf = edf | `Z80_PF;
                            if (r17[16]) edf = edf | `Z80_CF;
                        end
                        if (r17[15]) edf = edf | `Z80_SF;
                        if (r17[15:0] == 16'h0) edf = edf | `Z80_ZF;
                        edf = edf | (r17[15:8] & (`Z80_YF | `Z80_XF));
                        rf_n[`RFP_HL] = r17[15:0];
                        rf_n[`RFP_AF][7:0] = edf;
                        fin = 1'b1;
                    end
                end
                `EXEC_NEG: ;  /* migrated to z80_seq: alu mode FLAG_NEG (set by PLA), writes A+F */
                `EXEC_IM: ;  /* migrated to z80_seq (ctl_im_we; val from aux_w) */
                `EXEC_RETN: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_MRD, rf[`RFP_SP], 8'h0, 4'd0);
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte; rf_n[`RFP_SP] = rf[`RFP_SP] + 16'd1;
                        startm(`BUSOP_MRD, rf[`RFP_SP] + 16'd1, 8'h0, 4'd0); end
                    else begin rf_n[`RFP_SP] = rf[`RFP_SP] + 16'd1;
                        rf_n[`RFP_PC] = {rbyte, tmpl}; rf_n[`RFP_WZ] = {rbyte, tmpl};
                        iff1_n = iff2; fin = 1'b1; end
                end
                `EXEC_LD_I_A: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                    else begin reg_i_n = A_cur; fin = 1'b1; end
                end
                `EXEC_LD_R_A: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                    else begin reg_r_n = A_cur; fin = 1'b1; end
                end
                `EXEC_LD_A_IR: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                    else begin
                        edv = aux_w[0] ? reg_r : reg_i;
                        rf_n[`RFP_AF][15:8] = edv;
                        edf = F_cur & `Z80_CF;
                        if (edv[7]) edf = edf | `Z80_SF;
                        if (edv == 8'h0) edf = edf | `Z80_ZF;
                        edf = edf | (edv & (`Z80_YF | `Z80_XF));
                        if (iff2) edf = edf | `Z80_PF;
                        rf_n[`RFP_AF][7:0] = edf; fin = 1'b1;
                    end
                end
                `EXEC_LD_NNA_RP: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd3) begin tmp16_n = {rbyte, tmpl};
                        rf_n[`RFP_WZ] = {rbyte, tmpl} + 16'd1;
                        startm(`BUSOP_MWR, {rbyte, tmpl}, rf[rp_sel_w][7:0], 4'd0); end
                    else if (m_cycle == 3'd4) startm(`BUSOP_MWR, tmp16 + 16'd1, rf[rp_sel_w][15:8], 4'd0);
                    else fin = 1'b1;
                end
                `EXEC_LD_RP_NNA: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd3) begin tmp16_n = {rbyte, tmpl};
                        rf_n[`RFP_WZ] = {rbyte, tmpl} + 16'd1;
                        startm(`BUSOP_MRD, {rbyte, tmpl}, 8'h0, 4'd0); end
                    else if (m_cycle == 3'd4) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, tmp16 + 16'd1, 8'h0, 4'd0); end
                    else begin rf_n[rp_sel_w] = {rbyte, tmpl}; fin = 1'b1; end
                end
                `EXEC_IN_C: begin
                    if (m_cycle == 3'd1) begin rf_n[`RFP_WZ] = rf[`RFP_BC] + 16'd1;
                        startm(`BUSOP_IORD, rf[`RFP_BC], 8'h0, 4'd0); end
                    else begin
                        if (rf_dst_w != 3'd6) setr8(rf_dst_w, rbyte);
                        edf = F_cur & `Z80_CF;
                        if (rbyte[7]) edf = edf | `Z80_SF;
                        if (rbyte == 8'h0) edf = edf | `Z80_ZF;
                        edf = edf | (rbyte & (`Z80_YF | `Z80_XF));
                        if (~(^rbyte)) edf = edf | `Z80_PF;
                        rf_n[`RFP_AF][7:0] = edf; fin = 1'b1;
                    end
                end
                `EXEC_OUT_C: begin
                    if (m_cycle == 3'd1) begin rf_n[`RFP_WZ] = rf[`RFP_BC] + 16'd1;
                        startm(`BUSOP_IOWR, rf[`RFP_BC], (rf_src_w == 3'd6) ? 8'h0 : getr8(rf_src_w), 4'd0); end
                    else fin = 1'b1;
                end
                `EXEC_RRD, `EXEC_RLD: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_MRD, rf[`RFP_HL], 8'h0, 4'd0);
                    else if (m_cycle == 3'd2) begin
                        if (exec_w == `EXEC_RRD) begin
                            edv   = {A_cur[7:4], rbyte[3:0]};   // new A
                            cbres = {A_cur[3:0], rbyte[7:4]};   // new (HL)
                        end else begin
                            edv   = {A_cur[7:4], rbyte[7:4]};   // new A
                            cbres = {rbyte[3:0], A_cur[3:0]};   // new (HL)
                        end
                        rf_n[`RFP_AF][15:8] = edv;
                        edf = F_cur & `Z80_CF;
                        if (edv[7]) edf = edf | `Z80_SF;
                        if (edv == 8'h0) edf = edf | `Z80_ZF;
                        edf = edf | (edv & (`Z80_YF | `Z80_XF));
                        if (~(^edv)) edf = edf | `Z80_PF;
                        rf_n[`RFP_AF][7:0] = edf;
                        rf_n[`RFP_WZ] = rf[`RFP_HL] + 16'd1;
                        tmpl_n = cbres;
                        startm(`BUSOP_INTERNAL, rf[`RFP_HL], 8'h0, 4'd4);
                    end
                    else if (m_cycle == 3'd3) startm(`BUSOP_MWR, rf[`RFP_HL], tmpl, 4'd0);
                    else fin = 1'b1;
                end

                /* ---------- ED block instructions ----------
                   aux: [0]=dec  [2:1]=cat(0 LD,1 CP,2 IN,3 OUT)  [3]=repeat */
                `EXEC_BLOCK: begin
                    if (aux_w[2:1] == 2'd0) begin                 // LDI/LDD/LDIR/LDDR
                        if (m_cycle == 3'd1) startm(`BUSOP_MRD, rf[`RFP_HL], 8'h0, 4'd0);
                        else if (m_cycle == 3'd2)
                            startm(`BUSOP_MWR, rf[`RFP_DE], rbyte, 4'd2); /* 5T write */
                        else if (m_cycle == 3'd3) begin
                            bbc = rf[`RFP_BC] - 16'd1; rf_n[`RFP_BC] = bbc;
                            rf_n[`RFP_HL] = rf[`RFP_HL] + (aux_w[0] ? 16'hFFFF : 16'h1);
                            rf_n[`RFP_DE] = rf[`RFP_DE] + (aux_w[0] ? 16'hFFFF : 16'h1);
                            bn2 = A_cur + tmp8;                   /* A + transferred byte */
                            edf = F_cur & (`Z80_SF | `Z80_ZF | `Z80_CF);
                            if (bn2[1]) edf = edf | `Z80_YF;
                            if (bn2[3]) edf = edf | `Z80_XF;
                            if (bbc != 16'd0) edf = edf | `Z80_PF;
                            rf_n[`RFP_AF][7:0] = edf;
                            if (aux_w[3] && bbc != 16'd0) begin
                                rf_n[`RFP_PC] = rf[`RFP_PC] - 16'd2;
                                rf_n[`RFP_WZ] = rf[`RFP_PC] - 16'd1;
                                startm(`BUSOP_INTERNAL, rf[`RFP_PC] - 16'd2, 8'h0, 4'd5);
                            end else fin = 1'b1;
                        end else fin = 1'b1;
                    end else if (aux_w[2:1] == 2'd1) begin        // CPI/CPD/CPIR/CPDR
                        if (m_cycle == 3'd1) startm(`BUSOP_MRD, rf[`RFP_HL], 8'h0, 4'd0);
                        else if (m_cycle == 3'd2) begin
                            bbc = rf[`RFP_BC] - 16'd1; rf_n[`RFP_BC] = bbc;
                            rf_n[`RFP_WZ] = rf[`RFP_WZ] + (aux_w[0] ? 16'hFFFF : 16'h1);
                            rf_n[`RFP_HL] = rf[`RFP_HL] + (aux_w[0] ? 16'hFFFF : 16'h1);
                            bn2 = A_cur - rbyte;                  /* res */
                            bhc = {1'b0, A_cur[3:0]} - {1'b0, rbyte[3:0]};
                            edf = (F_cur & `Z80_CF) | `Z80_NF;
                            if (bn2[7]) edf = edf | `Z80_SF;
                            if (bn2 == 8'h0) edf = edf | `Z80_ZF;
                            if (bhc[4]) edf = edf | `Z80_HF;
                            if (((bn2 - {7'b0, bhc[4]}) & 8'h02) != 8'h0) edf = edf | `Z80_YF;
                            if (((bn2 - {7'b0, bhc[4]}) & 8'h08) != 8'h0) edf = edf | `Z80_XF;
                            if (bbc != 16'd0) edf = edf | `Z80_PF;
                            rf_n[`RFP_AF][7:0] = edf;
                            startm(`BUSOP_INTERNAL, rf[`RFP_HL] + (aux_w[0] ? 16'hFFFF : 16'h1), 8'h0, 4'd5);
                        end else if (m_cycle == 3'd3) begin
                            if (aux_w[3] && rf[`RFP_BC] != 16'd0 && !(F_cur & `Z80_ZF)) begin
                                rf_n[`RFP_PC] = rf[`RFP_PC] - 16'd2;
                                rf_n[`RFP_WZ] = rf[`RFP_PC] - 16'd1;
                                startm(`BUSOP_INTERNAL, rf[`RFP_PC] - 16'd2, 8'h0, 4'd5);
                            end else fin = 1'b1;
                        end else fin = 1'b1;
                    end else if (aux_w[2:1] == 2'd2) begin        // INI/IND/INIR/INDR
                        if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                        else if (m_cycle == 3'd2) begin
                            rf_n[`RFP_WZ] = rf[`RFP_BC] + (aux_w[0] ? 16'hFFFF : 16'h1);
                            startm(`BUSOP_IORD, rf[`RFP_BC], 8'h0, 4'd0);
                        end else if (m_cycle == 3'd3) begin
                            tmpl_n = rbyte; startm(`BUSOP_MWR, rf[`RFP_HL], rbyte, 4'd0);
                        end else if (m_cycle == 3'd4) begin
                            bdata = tmpl;
                            bnewB = getr8(3'd0) - 8'd1; rf_n[`RFP_BC][15:8] = bnewB;
                            rf_n[`RFP_HL] = rf[`RFP_HL] + (aux_w[0] ? 16'hFFFF : 16'h1);
                            bk = {1'b0, bdata} + {1'b0, (getr8(3'd1) + (aux_w[0] ? 8'hFF : 8'h1))};
                            edf = 8'h0;
                            if (bdata[7]) edf = edf | `Z80_NF;
                            if (bk[8]) edf = edf | (`Z80_HF | `Z80_CF);
                            if (~(^(bnewB ^ {5'b0, bk[2:0]}))) edf = edf | `Z80_PF;
                            if (bnewB[7]) edf = edf | `Z80_SF;
                            if (bnewB == 8'h0) edf = edf | `Z80_ZF;
                            edf = edf | (bnewB & (`Z80_YF | `Z80_XF));
                            rf_n[`RFP_AF][7:0] = edf;
                            if (aux_w[3] && bnewB != 8'h0) begin
                                rf_n[`RFP_PC] = rf[`RFP_PC] - 16'd2;
                                startm(`BUSOP_INTERNAL, rf[`RFP_PC] - 16'd2, 8'h0, 4'd5);
                            end else fin = 1'b1;
                        end else fin = 1'b1;
                    end else begin                                // OUTI/OUTD/OTIR/OTDR
                        if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                        else if (m_cycle == 3'd2) startm(`BUSOP_MRD, rf[`RFP_HL], 8'h0, 4'd0);
                        else if (m_cycle == 3'd3) begin
                            tmpl_n = rbyte;
                            rf_n[`RFP_BC][15:8] = getr8(3'd0) - 8'd1;
                            rf_n[`RFP_WZ] = rf_n[`RFP_BC] + (aux_w[0] ? 16'hFFFF : 16'h1);
                            startm(`BUSOP_IOWR, rf_n[`RFP_BC], rbyte, 4'd0);
                        end else if (m_cycle == 3'd4) begin
                            bdata = tmpl;
                            rf_n[`RFP_HL] = rf[`RFP_HL] + (aux_w[0] ? 16'hFFFF : 16'h1);
                            bk = {1'b0, bdata} + {1'b0, rf_n[`RFP_HL][7:0]};
                            bnewB = getr8(3'd0);
                            edf = 8'h0;
                            if (bdata[7]) edf = edf | `Z80_NF;
                            if (bk[8]) edf = edf | (`Z80_HF | `Z80_CF);
                            if (~(^(bnewB ^ {5'b0, bk[2:0]}))) edf = edf | `Z80_PF;
                            if (bnewB[7]) edf = edf | `Z80_SF;
                            if (bnewB == 8'h0) edf = edf | `Z80_ZF;
                            edf = edf | (bnewB & (`Z80_YF | `Z80_XF));
                            rf_n[`RFP_AF][7:0] = edf;
                            if (aux_w[3] && bnewB != 8'h0) begin
                                rf_n[`RFP_PC] = rf[`RFP_PC] - 16'd2;
                                startm(`BUSOP_INTERNAL, rf[`RFP_PC] - 16'd2, 8'h0, 4'd5);
                            end else fin = 1'b1;
                        end else fin = 1'b1;
                    end
                end

                /* ---------- interrupt acceptance sequences ---------- */
                `EXEC_NMI: begin
                    if (m_cycle == 3'd1) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[`RFP_PC][15:8], 4'd0); end
                    else if (m_cycle == 3'd2) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[`RFP_PC][7:0], 4'd0); end
                    else begin rf_n[`RFP_PC] = 16'h0066; rf_n[`RFP_WZ] = 16'h0066; fin = 1'b1; end
                end
                `EXEC_INT: begin
                    if (m_cycle == 3'd1) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[`RFP_PC][15:8], 4'd0); end
                    else if (m_cycle == 3'd2) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[`RFP_PC][7:0], 4'd0); end
                    else if (m_cycle == 3'd3) begin
                        if (im == 2'd2) begin
                            tmp16_n = {reg_i, tmp8};
                            startm(`BUSOP_MRD, {reg_i, tmp8}, 8'h0, 4'd0);
                        end else begin
                            rf_n[`RFP_PC] = (im == 2'd1) ? 16'h0038 : {8'h00, (tmp8 & 8'h38)};
                            rf_n[`RFP_WZ] = (im == 2'd1) ? 16'h0038 : {8'h00, (tmp8 & 8'h38)};
                            fin = 1'b1;
                        end
                    end
                    else if (m_cycle == 3'd4) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, tmp16 + 16'd1, 8'h0, 4'd0); end
                    else begin rf_n[`RFP_PC] = {rbyte, tmpl}; rf_n[`RFP_WZ] = {rbyte, tmpl}; fin = 1'b1; end
                end

                default: fin = 1'b1;
                endcase

                // Per-(M, T) sequencer override: when seq covers the opcode,
                // its outputs replace the legacy branch's effects.
                if (seq_active) begin
                    fin = seq_fin;
                    case (seq_iff_op)
                        `IFF_CLEAR: begin iff1_n = 1'b0;     iff2_n = 1'b0;     end
                        `IFF_SET:   begin iff1_n = 1'b1;     iff2_n = 1'b1;     end
                        `IFF_RETN:  begin iff1_n = iff2_n;                      end
                        default:    ;
                    endcase
                    if (seq_ei_delay_set) ei_delay_n = 1'b1;
                    if (seq_im_we)        im_n     = aux_w[1:0];
                    if (seq_halt_set)     halted_n = 1'b1;
                    if (seq_pc_dec1)      rf_n[`RFP_PC] = rf_n[`RFP_PC] - 16'd1;
                    if (seq_ex_de_hl) begin
                        rf_n[`RFP_DE] = rf[`RFP_HL];
                        rf_n[`RFP_HL] = rf[`RFP_DE];
                    end
                    if (seq_ex_af) begin
                        rf_n[`RFP_AF]  = rf[`RFP_AF2];
                        rf_n[`RFP_AF2] = rf[`RFP_AF];
                    end
                    if (seq_exx) begin
                        rf_n[`RFP_BC]  = rf[`RFP_BC2]; rf_n[`RFP_BC2] = rf[`RFP_BC];
                        rf_n[`RFP_DE]  = rf[`RFP_DE2]; rf_n[`RFP_DE2] = rf[`RFP_DE];
                        rf_n[`RFP_HL]  = rf[`RFP_HL2]; rf_n[`RFP_HL2] = rf[`RFP_HL];
                    end
                    if (seq_pc_set_hl) rf_n[`RFP_PC] = rf[hlp];
                    if (seq_reg_a_we)     rf_n[`RFP_AF][15:8] = alu_res;
                    if (seq_reg_f_we)     rf_n[`RFP_AF][7:0]  = alu_fout;
                    if (seq_reg_setri_we) begin
                        case (seq_reg_setri_src)
                            `SETRI_SRC_GETRI_SRC: setri(rf_dst_w, getri(rf_src_w));
                            `SETRI_SRC_RBYTE:     setri(rf_dst_w, rbyte);
                            default:              setri(rf_dst_w, alu_res);
                        endcase
                    end
                    if (seq_start_mc) begin
                        startm(seq_mc_bus_op, seq_addr_val, seq_wdata_val, seq_mc_extra_t);
                    end
                    if (seq_pc_inc) rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1;
                end

                if (fin) begin
                    instr_count_n = instr_count + 32'd1;
                    prefix_n = `PFX_NONE;
                    // Commit Q: F if THIS instruction wrote F, else 0. Detect
                    // F-modification by comparing rf_n[AF][7:0] vs rf[AF][7:0]
                    // at instruction end.
                    if (rf_n[`RFP_AF][7:0] != rf[`RFP_AF][7:0])
                        reg_q_n = rf_n[`RFP_AF][7:0];
                    else
                        reg_q_n = 8'h0;
                    f_modified_n = 1'b0;
                    // begin_next: decide bus grant / NMI / INT / HALT / next opcode
                    if (!busreq_n) begin
                        bus_granted_n = 1'b1;            // DMA owns the bus next cycle
                    end else begin
                        allow_int = ~ei_delay_n;         // ei_delay_n reflects EI this cycle
                        ei_delay_n = 1'b0;
                        if (nmi_pending_n) begin
                            nmi_pending_n = 1'b0;
                            // exiting HALT: re-advance PC past the HALT byte
                            if (halted_n) rf_n[`RFP_PC] = rf_n[`RFP_PC] + 16'd1;
                            halted_n = 1'b0;
                            iff2_n = iff1_n; iff1_n = 1'b0;
                            irq_seq_n = 2'd1;
                            bus_op_n = `BUSOP_M1; m_addr_n = rf_n[`RFP_PC]; m_wdata_n = 8'h0;
                            m_len_n = 4'd5; t_n = 4'd1; phi_n = 1'b0; m_cycle_n = 3'd1;
                            decoded_n = 1'b1; suppress_decode_n = 1'b1;
                        end else if (allow_int && !int_n && iff1_n) begin
                            if (halted_n) rf_n[`RFP_PC] = rf_n[`RFP_PC] + 16'd1;
                            halted_n = 1'b0; iff1_n = 1'b0; iff2_n = 1'b0;
                            irq_seq_n = 2'd2;
                            bus_op_n = `BUSOP_INTA; m_addr_n = rf_n[`RFP_PC]; m_wdata_n = 8'h0;
                            m_len_n = 4'd7; t_n = 4'd1; phi_n = 1'b0; m_cycle_n = 3'd1;
                            decoded_n = 1'b1;
                        end else if (halted_n) begin
                            irq_seq_n = 2'd3;
                            bus_op_n = `BUSOP_M1; m_addr_n = rf_n[`RFP_PC]; m_wdata_n = 8'h0;
                            m_len_n = 4'd4; t_n = 4'd1; phi_n = 1'b0; m_cycle_n = 3'd1;
                            decoded_n = 1'b1; suppress_decode_n = 1'b1;
                        end else begin
                            irq_seq_n = 2'd0;
                            bus_op_n = `BUSOP_M1; m_addr_n = rf_n[`RFP_PC]; m_wdata_n = 8'h0;
                            m_len_n = 4'd4; t_n = 4'd1; phi_n = 1'b0; m_cycle_n = 3'd1; decoded_n = 1'b0;
                        end
                    end
                end
                end  // displacement-preamble else
            end else begin
                t_n = t_state + 4'd1;
            end
        end
    end

    // ---- registers ----
    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            for (i = 0; i < 13; i = i + 1) rf[i] <= 16'hFFFF;
            rf[`RFP_PC] <= 16'h0000;
            reg_i <= 8'h00; reg_r <= 8'h00; ir <= 8'h00;
            phi <= 1'b0; t_state <= 4'd1; m_cycle <= 3'd1;
            bus_op <= `BUSOP_M1; m_len <= 4'd4; m_addr <= 16'h0000; m_wdata <= 8'h00;
            prefix <= `PFX_NONE; iff1 <= 1'b0; iff2 <= 1'b0; im <= 2'd0; halted <= 1'b0;
            tmp8 <= 8'h00; tmpl <= 8'h00; tmph <= 8'h00; tmp16 <= 16'h0000;
            cycle <= 32'd0; instr_count <= 32'd0; decoded <= 1'b0;
            nmi_pending <= 1'b0; prev_nmi_n <= 1'b1; ei_delay <= 1'b0;
            suppress_decode <= 1'b0; bus_granted <= 1'b0; irq_seq <= 2'd0;
            reg_q <= 8'h00; f_modified <= 1'b0;
        end else begin
            for (i = 0; i < 13; i = i + 1) rf[i] <= rf_n[i];
            reg_i <= reg_i_n; reg_r <= reg_r_n; ir <= ir_n;
            phi <= phi_n; t_state <= t_n; m_cycle <= m_cycle_n;
            bus_op <= bus_op_n; m_len <= m_len_n; m_addr <= m_addr_n; m_wdata <= m_wdata_n;
            prefix <= prefix_n; iff1 <= iff1_n; iff2 <= iff2_n; im <= im_n; halted <= halted_n;
            tmp8 <= tmp8_n; tmpl <= tmpl_n; tmph <= tmph_n; tmp16 <= tmp16_n;
            cycle <= cycle_n; instr_count <= instr_count_n; decoded <= decoded_n;
            nmi_pending <= nmi_pending_n; prev_nmi_n <= prev_nmi_n_n; ei_delay <= ei_delay_n;
            suppress_decode <= suppress_decode_n; bus_granted <= bus_granted_n; irq_seq <= irq_seq_n;
            reg_q <= reg_q_n; f_modified <= f_modified_n;
        end
    end

endmodule
