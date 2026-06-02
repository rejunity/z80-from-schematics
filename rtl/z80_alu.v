// ===========================================================================
// z80_alu.v - combinational ALU + flag assembly (mirrors cmodel/z80_alu.c and
// z80_flags.c). Nibble carry chain made explicit for the add/sub half-carry.
// Pure combinational; selected by flag-mode + alu-op.
// ===========================================================================
`include "z80_defs.vh"

module z80_alu (
    input  wire [4:0] mode,      // FLAG_*
    input  wire [2:0] alu_op,    // ALU_*
    input  wire [2:0] rot_op,    // ROT_A (0 RLCA 1 RRCA 2 RLA 3 RRA) / CB rot[y]
    input  wire [2:0] bit_idx,   // CB BIT index
    input  wire [7:0] xy_src,    // CB BIT: source of X/Y flags
    input  wire [7:0] a,         // accumulator / source
    input  wire [7:0] b,         // operand
    input  wire [7:0] oldf,
    input  wire [7:0] q,         // Q register: F left by prev F-modifying instr (else 0)
    output reg  [7:0] res,
    output reg  [7:0] fout
);
    // ---- ADD / ADC ----
    wire        cin_add = (alu_op == `ALU_ADC) ? oldf[0] : 1'b0;
    wire [4:0]  lo_add  = {1'b0, a[3:0]} + {1'b0, b[3:0]} + cin_add;
    wire [8:0]  sum9    = {1'b0, a} + {1'b0, b} + cin_add;
    wire [7:0]  sum     = sum9[7:0];
    wire        h_add   = lo_add[4];
    wire        c_add   = sum9[8];
    wire        ov_add  = (~(a ^ b) & (a ^ sum) & 8'h80) != 8'h00;
    wire [7:0]  f_add   = (sum[7] ? `Z80_SF : 8'h0) | (sum == 8'h0 ? `Z80_ZF : 8'h0)
                        | (sum & (`Z80_YF | `Z80_XF)) | (h_add ? `Z80_HF : 8'h0)
                        | (ov_add ? `Z80_PF : 8'h0) | (c_add ? `Z80_CF : 8'h0);

    // ---- SUB / SBC / CP ----
    wire        cin_sub = (alu_op == `ALU_SBC) ? oldf[0] : 1'b0;
    wire [4:0]  lo_sub  = {1'b0, a[3:0]} - {1'b0, b[3:0]} - cin_sub;
    wire [8:0]  diff9   = {1'b0, a} - {1'b0, b} - cin_sub;
    wire [7:0]  diff    = diff9[7:0];
    wire        h_sub   = lo_sub[4];
    wire        c_sub   = diff9[8];
    wire        ov_sub  = ((a ^ b) & (a ^ diff) & 8'h80) != 8'h00;
    wire [7:0]  xy_sub  = (mode == `FLAG_CP8) ? b : diff;
    wire [7:0]  f_sub   = `Z80_NF | (diff[7] ? `Z80_SF : 8'h0) | (diff == 8'h0 ? `Z80_ZF : 8'h0)
                        | (h_sub ? `Z80_HF : 8'h0) | (ov_sub ? `Z80_PF : 8'h0)
                        | (c_sub ? `Z80_CF : 8'h0) | (xy_sub & (`Z80_YF | `Z80_XF));

    // ---- LOGIC ----
    reg  [7:0] lres;
    always @* begin
        case (alu_op)
            `ALU_AND: lres = a & b;
            `ALU_OR:  lres = a | b;
            `ALU_XOR: lres = a ^ b;
            default:  lres = a;
        endcase
    end
    wire        par_l  = ~(^lres);                  // 1 = even parity
    wire [7:0]  f_log  = (lres[7] ? `Z80_SF : 8'h0) | (lres == 8'h0 ? `Z80_ZF : 8'h0)
                       | (lres & (`Z80_YF | `Z80_XF))
                       | ((alu_op == `ALU_AND) ? `Z80_HF : 8'h0)
                       | (par_l ? `Z80_PF : 8'h0);

    // ---- INC / DEC (operate on b) ----
    wire [7:0]  inc_r = b + 8'd1;
    wire [7:0]  dec_r = b - 8'd1;
    wire [7:0]  f_inc = (oldf & `Z80_CF) | (inc_r[7] ? `Z80_SF : 8'h0)
                      | (inc_r == 8'h0 ? `Z80_ZF : 8'h0) | (inc_r & (`Z80_YF | `Z80_XF))
                      | ((b[3:0] == 4'hF) ? `Z80_HF : 8'h0) | ((b == 8'h7F) ? `Z80_PF : 8'h0);
    wire [7:0]  f_dec = (oldf & `Z80_CF) | `Z80_NF | (dec_r[7] ? `Z80_SF : 8'h0)
                      | (dec_r == 8'h0 ? `Z80_ZF : 8'h0) | (dec_r & (`Z80_YF | `Z80_XF))
                      | ((b[3:0] == 4'h0) ? `Z80_HF : 8'h0) | ((b == 8'h80) ? `Z80_PF : 8'h0);

    // ---- accumulator rotates ----
    reg  [7:0] rr;
    reg        rcf;
    always @* begin
        case (rot_op[1:0])
            2'd0: begin rcf = a[7]; rr = {a[6:0], a[7]};    end // RLCA
            2'd1: begin rcf = a[0]; rr = {a[0], a[7:1]};    end // RRCA
            2'd2: begin rcf = a[7]; rr = {a[6:0], oldf[0]}; end // RLA
            default: begin rcf = a[0]; rr = {oldf[0], a[7:1]}; end // RRA
        endcase
    end
    wire [7:0] f_rota = (oldf & (`Z80_SF | `Z80_ZF | `Z80_PF)) | (rr & (`Z80_YF | `Z80_XF))
                      | (rcf ? `Z80_CF : 8'h0);

    // ---- DAA ----
    wire nf = oldf[1], hf = oldf[4], cf = oldf[0];
    reg  [7:0] corr; reg newc;
    always @* begin
        corr = 8'h00; newc = cf;
        if (hf || (a[3:0] > 4'h9)) corr = corr | 8'h06;
        if (cf || (a > 8'h99))     begin corr = corr | 8'h60; newc = 1'b1; end
    end
    wire [7:0] daa_r = nf ? (a - corr) : (a + corr);
    wire       daa_h = nf ? (hf && (a[3:0] < 4'h6)) : (a[3:0] > 4'h9);
    wire       par_d = ~(^daa_r);
    wire [7:0] f_daa = (nf ? `Z80_NF : 8'h0) | (daa_r[7] ? `Z80_SF : 8'h0)
                     | (daa_r == 8'h0 ? `Z80_ZF : 8'h0) | (daa_r & (`Z80_YF | `Z80_XF))
                     | (par_d ? `Z80_PF : 8'h0) | (newc ? `Z80_CF : 8'h0)
                     | (daa_h ? `Z80_HF : 8'h0);

    // ---- CPL / SCF / CCF ----
    wire [7:0] cpl_r = ~a;
    wire [7:0] f_cpl = (oldf & (`Z80_SF | `Z80_ZF | `Z80_PF | `Z80_CF)) | `Z80_HF | `Z80_NF
                     | (cpl_r & (`Z80_YF | `Z80_XF));
    wire [7:0] f_scf = (oldf & (`Z80_SF | `Z80_ZF | `Z80_PF)) | `Z80_CF | ((a | q) & (`Z80_YF | `Z80_XF));
    wire [7:0] f_ccf = (oldf & (`Z80_SF | `Z80_ZF | `Z80_PF)) | (oldf[0] ? `Z80_HF : 8'h0)
                     | (oldf[0] ? 8'h0 : `Z80_CF) | ((a | q) & (`Z80_YF | `Z80_XF));

    // ---- CB rotates/shifts (operate on b) ----
    reg  [7:0] cbr; reg cbcf;
    always @* begin
        case (rot_op)
            3'd0: begin cbcf = b[7]; cbr = {b[6:0], b[7]};    end // RLC
            3'd1: begin cbcf = b[0]; cbr = {b[0], b[7:1]};    end // RRC
            3'd2: begin cbcf = b[7]; cbr = {b[6:0], oldf[0]}; end // RL
            3'd3: begin cbcf = b[0]; cbr = {oldf[0], b[7:1]}; end // RR
            3'd4: begin cbcf = b[7]; cbr = {b[6:0], 1'b0};    end // SLA
            3'd5: begin cbcf = b[0]; cbr = {b[7], b[7:1]};    end // SRA
            3'd6: begin cbcf = b[7]; cbr = {b[6:0], 1'b1};    end // SLL
            default: begin cbcf = b[0]; cbr = {1'b0, b[7:1]}; end // SRL
        endcase
    end
    wire par_cb = ~(^cbr);
    wire [7:0] f_rot = (cbr[7] ? `Z80_SF : 8'h0) | (cbr == 8'h0 ? `Z80_ZF : 8'h0)
                     | (cbr & (`Z80_YF | `Z80_XF)) | (par_cb ? `Z80_PF : 8'h0)
                     | (cbcf ? `Z80_CF : 8'h0);

    // ---- CB BIT (operate on b, X/Y from xy_src) ----
    wire [7:0] bitmask = (8'h01 << bit_idx);
    wire [7:0] bittest = b & bitmask;
    wire [7:0] f_bit = (oldf & `Z80_CF) | `Z80_HF
                     | ((bittest == 8'h0) ? (`Z80_ZF | `Z80_PF) : 8'h0)
                     | ((bittest & 8'h80) ? `Z80_SF : 8'h0)
                     | (xy_src & (`Z80_YF | `Z80_XF));

    // ---- output mux ----
    always @* begin
        case (mode)
            `FLAG_ROT:  begin res = cbr; fout = f_rot; end
            `FLAG_BIT:  begin res = b;   fout = f_bit; end
            `FLAG_ADD8: begin res = sum;   fout = f_add; end
            `FLAG_SUB8: begin res = diff;  fout = f_sub; end
            `FLAG_CP8:  begin res = diff;  fout = f_sub; end  // core discards res
            `FLAG_LOGIC:begin res = lres;  fout = f_log; end
            `FLAG_INC8: begin res = inc_r; fout = f_inc; end
            `FLAG_DEC8: begin res = dec_r; fout = f_dec; end
            `FLAG_ROT_A:begin res = rr;    fout = f_rota;end
            `FLAG_DAA:  begin res = daa_r; fout = f_daa; end
            `FLAG_CPL:  begin res = cpl_r; fout = f_cpl; end
            `FLAG_SCF:  begin res = a;     fout = f_scf; end
            `FLAG_CCF:  begin res = a;     fout = f_ccf; end
            default:  begin res = b;     fout = oldf; end
        endcase
    end
endmodule
