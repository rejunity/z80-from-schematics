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
    reg [31:0] cycle_n, instr_count_n;
    reg        decoded_n;
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

    // ---- timing pin drive (combinational from registered state) ----
    z80_timing u_timing (
        .bus_op(bus_op), .t_state(t_state[2:0]), .phi(phi),
        .m_addr(m_addr), .m_wdata(m_wdata), .reg_i(reg_i), .reg_r(reg_r),
        .addr(addr), .data_out(data_out), .data_drive(data_drive),
        .m1_n(m1_n), .mreq_n(mreq_n), .iorq_n(iorq_n),
        .rd_n(rd_n), .wr_n(wr_n), .rfsh_n(rfsh_n)
    );
    assign halt_n   = halted ? 1'b0 : 1'b1;
    assign busack_n = 1'b1;            // BUSREQ/BUSACK in a later stage
    assign dbg_t = t_state; assign dbg_phi = phi; assign dbg_m = m_cycle;

    // ---- timing-point predicates ----
    wire islatch =  (phi == 1'b1) &&
                  ( ((bus_op == `BUSOP_M1)   && (t_state == 4'd2)) ||
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
    wire [7:0] A_cur = rf[`RFP_AF][15:8];
    wire [7:0] F_cur = rf[`RFP_AF][7:0];
    always @* begin
        alu_a = A_cur;
        alu_b = 8'h00;
        alu_md = flag_mode_w;
        alu_xy = 8'h00;
        case (exec_w)
            `EX_ALU_R:           alu_b = getri(rf_src_w);
            `EX_ALU_N, `EX_ALU_M:alu_b = rbyte;
            `EX_INC_R, `EX_DEC_R:alu_b = getri(rf_dst_w);
            `EX_INC_M, `EX_DEC_M:alu_b = rbyte;
            `EX_CB_R: begin alu_b = getr8(rf_src_w); alu_xy = getr8(rf_src_w); end
            `EX_CB_M: begin alu_b = rbyte;           alu_xy = rf[`RFP_WZ][15:8]; end
            default:             alu_b = 8'h00;
        endcase
    end
    wire [7:0] alu_res, alu_fout;
    z80_alu u_alu (
        .mode(alu_md), .alu_op(alu_op_w), .rot_op(rot_op_w),
        .bit_idx(bit_index_w), .xy_src(alu_xy),
        .a(alu_a), .b(alu_b), .oldf(F_cur),
        .res(alu_res), .fout(alu_fout)
    );

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

    // ---- combinational next-state ----
    always @* begin
        for (i = 0; i < 13; i = i + 1) rf_n[i] = rf[i];
        reg_i_n = reg_i; reg_r_n = reg_r; ir_n = ir;
        phi_n = phi; t_n = t_state; m_cycle_n = m_cycle;
        bus_op_n = bus_op; m_len_n = m_len; m_addr_n = m_addr; m_wdata_n = m_wdata;
        prefix_n = prefix; iff1_n = iff1; iff2_n = iff2; im_n = im; halted_n = halted;
        tmp8_n = tmp8; tmpl_n = tmpl; tmph_n = tmph; tmp16_n = tmp16;
        cycle_n = cycle + 32'd1; instr_count_n = instr_count; decoded_n = decoded;
        fin = 1'b0;
        add16 = 17'd0; f16 = 8'd0;

        // latch on the current phase
        if (islatch) begin
            if (bus_op == `BUSOP_M1) begin
                ir_n   = data_in;
                tmp8_n = data_in;
                reg_r_n = {reg_r[7], (reg_r[6:0] + 7'd1)};
                rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1;
                decoded_n = 1'b1;
            end else begin
                tmp8_n = data_in;
            end
        end

        // advance phase
        if (phi == 1'b0) begin
            phi_n = 1'b1;
        end else if (stall) begin
            phi_n = 1'b0;                 // hold T-state as Tw
        end else begin
            phi_n = 1'b0;
            if ((t_state + 4'd1) > m_len) begin
                // (IX+d)/(IY+d) displacement preamble: fetch d, then 5T addr calc
                mm = m_cycle; memaddr = rf[hlp];
                if (idx_w != 2'd0 && use_disp_w && m_cycle == 3'd1) begin
                    startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                    rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1;
                end else if (idx_w != 2'd0 && use_disp_w && m_cycle == 3'd2) begin
                    rf_n[`RFP_WZ] = rf[hlp] + {{8{rbyte[7]}}, rbyte};
                    startm(`BUSOP_INTERNAL, rf[hlp] + {{8{rbyte[7]}}, rbyte}, 8'h0, 4'd5);
                end else begin
                if (idx_w != 2'd0 && use_disp_w) begin mm = m_cycle - 3'd2; memaddr = rf[`RFP_WZ]; end
                // ===== micro-sequencer (mirror z80_control.c) =====
                case (exec_w)
                `EX_NOP, `EX_ILLEGAL: fin = 1'b1;
                `EX_PREFIX: begin
                    case (ir)
                        8'hDD: prefix_n = `PFX_DD;
                        8'hFD: prefix_n = `PFX_FD;
                        8'hED: prefix_n = `PFX_ED;
                        8'hCB: prefix_n = (prefix == `PFX_DD) ? `PFX_DDCB :
                                          (prefix == `PFX_FD) ? `PFX_FDCB : `PFX_CB;
                        default: ;
                    endcase
                    bus_op_n = `BUSOP_M1; m_addr_n = rf[`RFP_PC]; m_wdata_n = 8'h0;
                    m_len_n = 4'd4; t_n = 4'd1; phi_n = 1'b0; m_cycle_n = 3'd1; decoded_n = 1'b0;
                end
                `EX_DI: begin iff1_n = 1'b0; iff2_n = 1'b0; fin = 1'b1; end
                `EX_EI: begin iff1_n = 1'b1; iff2_n = 1'b1; fin = 1'b1; end
                `EX_HALT: begin halted_n = 1'b1; fin = 1'b1; end

                `EX_LD_R_R: begin setri(rf_dst_w, getri(rf_src_w)); fin = 1'b1; end
                `EX_ALU_R:  begin if (alu_op_w != `ALU_CP) setr8(3'd7, alu_res);
                                  rf_n[`RFP_AF][7:0] = alu_fout; fin = 1'b1; end
                `EX_INC_R, `EX_DEC_R: begin setri(rf_dst_w, alu_res);
                                  rf_n[`RFP_AF][7:0] = alu_fout; fin = 1'b1; end
                `EX_ROT_A, `EX_DAA, `EX_CPL: begin setr8(3'd7, alu_res);
                                  rf_n[`RFP_AF][7:0] = alu_fout; fin = 1'b1; end
                `EX_SCF, `EX_CCF: begin rf_n[`RFP_AF][7:0] = alu_fout; fin = 1'b1; end

                `EX_EX_DE_HL: begin rf_n[`RFP_DE] = rf[`RFP_HL]; rf_n[`RFP_HL] = rf[`RFP_DE]; fin = 1'b1; end
                `EX_EX_AF:    begin rf_n[`RFP_AF] = rf[`RFP_AF2]; rf_n[`RFP_AF2] = rf[`RFP_AF]; fin = 1'b1; end
                `EX_EXX: begin
                    rf_n[`RFP_BC] = rf[`RFP_BC2]; rf_n[`RFP_BC2] = rf[`RFP_BC];
                    rf_n[`RFP_DE] = rf[`RFP_DE2]; rf_n[`RFP_DE2] = rf[`RFP_DE];
                    rf_n[`RFP_HL] = rf[`RFP_HL2]; rf_n[`RFP_HL2] = rf[`RFP_HL];
                    fin = 1'b1;
                end
                `EX_JP_HL: begin rf_n[`RFP_PC] = rf[hlp]; fin = 1'b1; end

                `EX_INC_RP: begin
                    if (m_cycle == 3'd1) begin rf_n[rp_sel_w] = rf[rp_sel_w] + 16'd1;
                        startm(`BUSOP_INTERNAL, rf[rp_sel_w] + 16'd1, 8'h0, 4'd2); end
                    else fin = 1'b1;
                end
                `EX_DEC_RP: begin
                    if (m_cycle == 3'd1) begin rf_n[rp_sel_w] = rf[rp_sel_w] - 16'd1;
                        startm(`BUSOP_INTERNAL, rf[rp_sel_w] - 16'd1, 8'h0, 4'd2); end
                    else fin = 1'b1;
                end
                `EX_LD_SP_HL: begin
                    if (m_cycle == 3'd1) begin rf_n[`RFP_SP] = rf[hlp];
                        startm(`BUSOP_INTERNAL, rf[hlp], 8'h0, 4'd2); end
                    else fin = 1'b1;
                end
                `EX_ADD_HL_RP: begin
                    if (m_cycle == 3'd1) begin
                        add16 = {1'b0, rf[hlp]} + {1'b0, rf[rp_sel_w]};
                        add12 = {1'b0, rf[hlp][11:0]} + {1'b0, rf[rp_sel_w][11:0]};
                        f16 = (F_cur & (`FB_S | `FB_Z | `FB_P));
                        if (add12[12]) f16 = f16 | `FB_H;
                        if (add16[16]) f16 = f16 | `FB_C;
                        f16 = f16 | (add16[15:8] & (`FB_Y | `FB_X));
                        rf_n[`RFP_WZ] = rf[hlp] + 16'd1;
                        rf_n[hlp] = add16[15:0];
                        rf_n[`RFP_AF][7:0] = f16;
                        startm(`BUSOP_INTERNAL, add16[15:0], 8'h0, 4'd7);
                    end else fin = 1'b1;
                end

                `EX_LD_R_N: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else begin setri(rf_dst_w, rbyte); fin = 1'b1; end
                end
                `EX_ALU_N: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else begin if (alu_op_w != `ALU_CP) setr8(3'd7, alu_res);
                        rf_n[`RFP_AF][7:0] = alu_fout; fin = 1'b1; end
                end
                `EX_LD_R_M: begin
                    if (mm == 3'd1) startm(`BUSOP_MRD, memaddr, 8'h0, 4'd0);
                    else begin setri(rf_dst_w, rbyte); fin = 1'b1; end
                end
                `EX_ALU_M: begin
                    if (mm == 3'd1) startm(`BUSOP_MRD, memaddr, 8'h0, 4'd0);
                    else begin if (alu_op_w != `ALU_CP) setr8(3'd7, alu_res);
                        rf_n[`RFP_AF][7:0] = alu_fout; fin = 1'b1; end
                end
                `EX_ST_M_R: begin
                    if (mm == 3'd1) startm(`BUSOP_MWR, memaddr, getri(rf_src_w), 4'd0);
                    else fin = 1'b1;
                end
                `EX_LD_M_N: begin
                    if (mm == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (mm == 3'd2) startm(`BUSOP_MWR, memaddr, rbyte, 4'd0);
                    else fin = 1'b1;
                end
                `EX_INC_M, `EX_DEC_M: begin
                    if (mm == 3'd1) startm(`BUSOP_MRD, memaddr, 8'h0, 4'd1);
                    else if (mm == 3'd2) begin rf_n[`RFP_AF][7:0] = alu_fout;
                        startm(`BUSOP_MWR, memaddr, alu_res, 4'd0); end
                    else fin = 1'b1;
                end

                `EX_LD_RP_NN: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else begin rf_n[rp_sel_w] = {rbyte, tmpl}; fin = 1'b1; end
                end
                `EX_JP, `EX_JP_CC: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else begin
                        rf_n[`RFP_WZ] = {rbyte, tmpl};
                        if ((exec_w == `EX_JP) || cc_true(F_cur, cc_w)) rf_n[`RFP_PC] = {rbyte, tmpl};
                        fin = 1'b1;
                    end
                end

                `EX_JR: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin
                        rf_n[`RFP_PC] = rf[`RFP_PC] + {{8{rbyte[7]}}, rbyte};
                        rf_n[`RFP_WZ] = rf[`RFP_PC] + {{8{rbyte[7]}}, rbyte};
                        startm(`BUSOP_INTERNAL, rf[`RFP_PC] + {{8{rbyte[7]}}, rbyte}, 8'h0, 4'd5);
                    end else fin = 1'b1;
                end
                `EX_JR_CC: begin
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
                `EX_DJNZ: begin
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

                `EX_CALL, `EX_CALL_CC: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd3) begin
                        tmp16_n = {rbyte, tmpl}; rf_n[`RFP_WZ] = {rbyte, tmpl};
                        if ((exec_w == `EX_CALL) || cc_true(F_cur, cc_w))
                            startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                        else fin = 1'b1;
                    end
                    else if (m_cycle == 3'd4) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[`RFP_PC][15:8], 4'd0); end
                    else if (m_cycle == 3'd5) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[`RFP_PC][7:0], 4'd0); end
                    else begin rf_n[`RFP_PC] = tmp16; fin = 1'b1; end
                end
                `EX_RET: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_MRD, rf[`RFP_SP], 8'h0, 4'd0);
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte; rf_n[`RFP_SP] = rf[`RFP_SP] + 16'd1;
                        startm(`BUSOP_MRD, rf[`RFP_SP] + 16'd1, 8'h0, 4'd0); end
                    else begin rf_n[`RFP_SP] = rf[`RFP_SP] + 16'd1;
                        rf_n[`RFP_PC] = {rbyte, tmpl}; rf_n[`RFP_WZ] = {rbyte, tmpl}; fin = 1'b1; end
                end
                `EX_RET_CC: begin
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
                `EX_RST: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                    else if (m_cycle == 3'd2) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[`RFP_PC][15:8], 4'd0); end
                    else if (m_cycle == 3'd3) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[`RFP_PC][7:0], 4'd0); end
                    else begin rf_n[`RFP_PC] = {8'h00, rst_addr_w};
                        rf_n[`RFP_WZ] = {8'h00, rst_addr_w}; fin = 1'b1; end
                end
                `EX_PUSH: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                    else if (m_cycle == 3'd2) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[rp_sel_w][15:8], 4'd0); end
                    else if (m_cycle == 3'd3) begin rf_n[`RFP_SP] = rf[`RFP_SP] - 16'd1;
                        startm(`BUSOP_MWR, rf[`RFP_SP] - 16'd1, rf[rp_sel_w][7:0], 4'd0); end
                    else fin = 1'b1;
                end
                `EX_POP: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_MRD, rf[`RFP_SP], 8'h0, 4'd0);
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte; rf_n[`RFP_SP] = rf[`RFP_SP] + 16'd1;
                        startm(`BUSOP_MRD, rf[`RFP_SP] + 16'd1, 8'h0, 4'd0); end
                    else begin rf_n[`RFP_SP] = rf[`RFP_SP] + 16'd1;
                        rf_n[rp_sel_w] = {rbyte, tmpl}; fin = 1'b1; end
                end

                `EX_LD_A_RP: begin
                    if (m_cycle == 3'd1) begin rf_n[`RFP_WZ] = rf[rp_sel_w] + 16'd1;
                        startm(`BUSOP_MRD, rf[rp_sel_w], 8'h0, 4'd0); end
                    else begin setr8(3'd7, rbyte); fin = 1'b1; end
                end
                `EX_LD_RP_A: begin
                    if (m_cycle == 3'd1) begin
                        rf_n[`RFP_WZ] = {A_cur, (rf[rp_sel_w][7:0] + 8'd1)};
                        startm(`BUSOP_MWR, rf[rp_sel_w], A_cur, 4'd0); end
                    else fin = 1'b1;
                end
                `EX_LD_A_NN: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd3) begin tmp16_n = {rbyte, tmpl};
                        rf_n[`RFP_WZ] = {rbyte, tmpl} + 16'd1; startm(`BUSOP_MRD, {rbyte, tmpl}, 8'h0, 4'd0); end
                    else begin setr8(3'd7, rbyte); fin = 1'b1; end
                end
                `EX_LD_NN_A: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0); rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd3) begin
                        rf_n[`RFP_WZ] = {A_cur, (tmpl + 8'd1)};
                        startm(`BUSOP_MWR, {rbyte, tmpl}, A_cur, 4'd0); end
                    else fin = 1'b1;
                end
                `EX_LD_HL_NN: begin
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
                `EX_LD_NN_HL: begin
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

                `EX_IN_A_N: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin rf_n[`RFP_WZ] = {A_cur, rbyte} + 16'd1;
                        startm(`BUSOP_IORD, {A_cur, rbyte}, 8'h0, 4'd0); end
                    else begin setr8(3'd7, rbyte); fin = 1'b1; end
                end
                `EX_OUT_N_A: begin
                    if (m_cycle == 3'd1) begin startm(`BUSOP_MRD, rf[`RFP_PC], 8'h0, 4'd0);
                        rf_n[`RFP_PC] = rf[`RFP_PC] + 16'd1; end
                    else if (m_cycle == 3'd2) begin rf_n[`RFP_WZ] = {A_cur, (rbyte + 8'd1)};
                        startm(`BUSOP_IOWR, {A_cur, rbyte}, A_cur, 4'd0); end
                    else fin = 1'b1;
                end
                `EX_EX_SP_HL: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_MRD, rf[`RFP_SP], 8'h0, 4'd0);
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte;
                        startm(`BUSOP_MRD, rf[`RFP_SP] + 16'd1, 8'h0, 4'd1); end
                    else if (m_cycle == 3'd3) begin tmph_n = rbyte;
                        startm(`BUSOP_MWR, rf[`RFP_SP] + 16'd1, rf[hlp][15:8], 4'd0); end
                    else if (m_cycle == 3'd4) startm(`BUSOP_MWR, rf[`RFP_SP], rf[hlp][7:0], 4'd2);
                    else begin rf_n[hlp] = {tmph, tmpl}; rf_n[`RFP_WZ] = {tmph, tmpl}; fin = 1'b1; end
                end

                `EX_CB_R: begin
                    case (cb_kind_w)
                        `CB_ROT: begin setr8(rf_dst_w, alu_res); rf_n[`RFP_AF][7:0] = alu_fout; end
                        `CB_BIT: rf_n[`RFP_AF][7:0] = alu_fout;
                        `CB_RES: setr8(rf_dst_w, getr8(rf_src_w) & ~(8'h01 << bit_index_w));
                        `CB_SET: setr8(rf_dst_w, getr8(rf_src_w) |  (8'h01 << bit_index_w));
                    endcase
                    fin = 1'b1;
                end
                `EX_CB_M: begin
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
                `EX_ADC16, `EX_SBC16: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_HL], 8'h0, 4'd7);
                    else begin
                        rf_n[`RFP_WZ] = rf[`RFP_HL] + 16'd1;
                        if (exec_w == `EX_ADC16) begin
                            r17 = {1'b0, rf[`RFP_HL]} + {1'b0, rf[rp_sel_w]} + {16'b0, F_cur[0]};
                            r13 = {1'b0, rf[`RFP_HL][11:0]} + {1'b0, rf[rp_sel_w][11:0]} + {12'b0, F_cur[0]};
                            edf = 8'h0;
                            if (r13[12]) edf = edf | `FB_H;
                            if ((~(rf[`RFP_HL] ^ rf[rp_sel_w]) & (rf[`RFP_HL] ^ r17[15:0]) & 16'h8000) != 16'h0) edf = edf | `FB_P;
                            if (r17[16]) edf = edf | `FB_C;
                        end else begin
                            r17 = {1'b0, rf[`RFP_HL]} - {1'b0, rf[rp_sel_w]} - {16'b0, F_cur[0]};
                            r13 = {1'b0, rf[`RFP_HL][11:0]} - {1'b0, rf[rp_sel_w][11:0]} - {12'b0, F_cur[0]};
                            edf = `FB_N;
                            if (r13[12]) edf = edf | `FB_H;
                            if (((rf[`RFP_HL] ^ rf[rp_sel_w]) & (rf[`RFP_HL] ^ r17[15:0]) & 16'h8000) != 16'h0) edf = edf | `FB_P;
                            if (r17[16]) edf = edf | `FB_C;
                        end
                        if (r17[15]) edf = edf | `FB_S;
                        if (r17[15:0] == 16'h0) edf = edf | `FB_Z;
                        edf = edf | (r17[15:8] & (`FB_Y | `FB_X));
                        rf_n[`RFP_HL] = r17[15:0];
                        rf_n[`RFP_AF][7:0] = edf;
                        fin = 1'b1;
                    end
                end
                `EX_NEG: begin
                    edv = 8'h00 - A_cur;
                    edf = `FB_N;
                    if (edv[7]) edf = edf | `FB_S;
                    if (edv == 8'h0) edf = edf | `FB_Z;
                    if (A_cur[3:0] != 4'h0) edf = edf | `FB_H;
                    if (A_cur == 8'h80) edf = edf | `FB_P;
                    if (A_cur != 8'h00) edf = edf | `FB_C;
                    edf = edf | (edv & (`FB_Y | `FB_X));
                    rf_n[`RFP_AF][15:8] = edv;
                    rf_n[`RFP_AF][7:0]  = edf;
                    fin = 1'b1;
                end
                `EX_IM: begin im_n = aux_w[1:0]; fin = 1'b1; end
                `EX_RETN: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_MRD, rf[`RFP_SP], 8'h0, 4'd0);
                    else if (m_cycle == 3'd2) begin tmpl_n = rbyte; rf_n[`RFP_SP] = rf[`RFP_SP] + 16'd1;
                        startm(`BUSOP_MRD, rf[`RFP_SP] + 16'd1, 8'h0, 4'd0); end
                    else begin rf_n[`RFP_SP] = rf[`RFP_SP] + 16'd1;
                        rf_n[`RFP_PC] = {rbyte, tmpl}; rf_n[`RFP_WZ] = {rbyte, tmpl};
                        iff1_n = iff2; fin = 1'b1; end
                end
                `EX_LD_I_A: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                    else begin reg_i_n = A_cur; fin = 1'b1; end
                end
                `EX_LD_R_A: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                    else begin reg_r_n = A_cur; fin = 1'b1; end
                end
                `EX_LD_A_IR: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_INTERNAL, rf[`RFP_PC], 8'h0, 4'd1);
                    else begin
                        edv = aux_w[0] ? reg_r : reg_i;
                        rf_n[`RFP_AF][15:8] = edv;
                        edf = F_cur & `FB_C;
                        if (edv[7]) edf = edf | `FB_S;
                        if (edv == 8'h0) edf = edf | `FB_Z;
                        edf = edf | (edv & (`FB_Y | `FB_X));
                        if (iff2) edf = edf | `FB_P;
                        rf_n[`RFP_AF][7:0] = edf; fin = 1'b1;
                    end
                end
                `EX_LD_NNA_RP: begin
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
                `EX_LD_RP_NNA: begin
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
                `EX_IN_C: begin
                    if (m_cycle == 3'd1) begin rf_n[`RFP_WZ] = rf[`RFP_BC] + 16'd1;
                        startm(`BUSOP_IORD, rf[`RFP_BC], 8'h0, 4'd0); end
                    else begin
                        if (rf_dst_w != 3'd6) setr8(rf_dst_w, rbyte);
                        edf = F_cur & `FB_C;
                        if (rbyte[7]) edf = edf | `FB_S;
                        if (rbyte == 8'h0) edf = edf | `FB_Z;
                        edf = edf | (rbyte & (`FB_Y | `FB_X));
                        if (~(^rbyte)) edf = edf | `FB_P;
                        rf_n[`RFP_AF][7:0] = edf; fin = 1'b1;
                    end
                end
                `EX_OUT_C: begin
                    if (m_cycle == 3'd1) begin rf_n[`RFP_WZ] = rf[`RFP_BC] + 16'd1;
                        startm(`BUSOP_IOWR, rf[`RFP_BC], (rf_src_w == 3'd6) ? 8'h0 : getr8(rf_src_w), 4'd0); end
                    else fin = 1'b1;
                end
                `EX_RRD, `EX_RLD: begin
                    if (m_cycle == 3'd1) startm(`BUSOP_MRD, rf[`RFP_HL], 8'h0, 4'd0);
                    else if (m_cycle == 3'd2) begin
                        if (exec_w == `EX_RRD) begin
                            edv   = {A_cur[7:4], rbyte[3:0]};   // new A
                            cbres = {A_cur[3:0], rbyte[7:4]};   // new (HL)
                        end else begin
                            edv   = {A_cur[7:4], rbyte[7:4]};   // new A
                            cbres = {rbyte[3:0], A_cur[3:0]};   // new (HL)
                        end
                        rf_n[`RFP_AF][15:8] = edv;
                        edf = F_cur & `FB_C;
                        if (edv[7]) edf = edf | `FB_S;
                        if (edv == 8'h0) edf = edf | `FB_Z;
                        edf = edf | (edv & (`FB_Y | `FB_X));
                        if (~(^edv)) edf = edf | `FB_P;
                        rf_n[`RFP_AF][7:0] = edf;
                        rf_n[`RFP_WZ] = rf[`RFP_HL] + 16'd1;
                        tmpl_n = cbres;
                        startm(`BUSOP_INTERNAL, rf[`RFP_HL], 8'h0, 4'd4);
                    end
                    else if (m_cycle == 3'd3) startm(`BUSOP_MWR, rf[`RFP_HL], tmpl, 4'd0);
                    else fin = 1'b1;
                end

                default: fin = 1'b1;
                endcase

                if (fin) begin
                    instr_count_n = instr_count + 32'd1;
                    prefix_n = `PFX_NONE;
                    bus_op_n = `BUSOP_M1; m_addr_n = rf_n[`RFP_PC]; m_wdata_n = 8'h0;
                    m_len_n = 4'd4; t_n = 4'd1; phi_n = 1'b0; m_cycle_n = 3'd1; decoded_n = 1'b0;
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
        end else begin
            for (i = 0; i < 13; i = i + 1) rf[i] <= rf_n[i];
            reg_i <= reg_i_n; reg_r <= reg_r_n; ir <= ir_n;
            phi <= phi_n; t_state <= t_n; m_cycle <= m_cycle_n;
            bus_op <= bus_op_n; m_len <= m_len_n; m_addr <= m_addr_n; m_wdata <= m_wdata_n;
            prefix <= prefix_n; iff1 <= iff1_n; iff2 <= iff2_n; im <= im_n; halted <= halted_n;
            tmp8 <= tmp8_n; tmpl <= tmpl_n; tmph <= tmph_n; tmp16 <= tmp16_n;
            cycle <= cycle_n; instr_count <= instr_count_n; decoded <= decoded_n;
        end
    end

endmodule
