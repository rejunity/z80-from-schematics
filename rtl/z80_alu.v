// ===========================================================================
// z80_alu.v - combinational ALU + flag assembly (mirrors cmodel/z80_alu.c and
// z80_flags.c). Nibble carry chain made explicit for the add/sub half-carry.
// Pure combinational; selected by flag-mode + alu-op.
// ===========================================================================
`include "z80_defs.vh"

module z80_alu (
    input  wire [4:0] mode,      // FM_*
    input  wire [2:0] alu_op,    // ALU_*
    input  wire [2:0] rot_op,    // for ROT_A (0 RLCA 1 RRCA 2 RLA 3 RRA)
    input  wire [7:0] a,         // accumulator / source
    input  wire [7:0] b,         // operand
    input  wire [7:0] oldf,
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
    wire [7:0]  f_add   = (sum[7] ? `FB_S : 8'h0) | (sum == 8'h0 ? `FB_Z : 8'h0)
                        | (sum & (`FB_Y | `FB_X)) | (h_add ? `FB_H : 8'h0)
                        | (ov_add ? `FB_P : 8'h0) | (c_add ? `FB_C : 8'h0);

    // ---- SUB / SBC / CP ----
    wire        cin_sub = (alu_op == `ALU_SBC) ? oldf[0] : 1'b0;
    wire [4:0]  lo_sub  = {1'b0, a[3:0]} - {1'b0, b[3:0]} - cin_sub;
    wire [8:0]  diff9   = {1'b0, a} - {1'b0, b} - cin_sub;
    wire [7:0]  diff    = diff9[7:0];
    wire        h_sub   = lo_sub[4];
    wire        c_sub   = diff9[8];
    wire        ov_sub  = ((a ^ b) & (a ^ diff) & 8'h80) != 8'h00;
    wire [7:0]  xy_sub  = (mode == `FM_CP8) ? b : diff;
    wire [7:0]  f_sub   = `FB_N | (diff[7] ? `FB_S : 8'h0) | (diff == 8'h0 ? `FB_Z : 8'h0)
                        | (h_sub ? `FB_H : 8'h0) | (ov_sub ? `FB_P : 8'h0)
                        | (c_sub ? `FB_C : 8'h0) | (xy_sub & (`FB_Y | `FB_X));

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
    wire [7:0]  f_log  = (lres[7] ? `FB_S : 8'h0) | (lres == 8'h0 ? `FB_Z : 8'h0)
                       | (lres & (`FB_Y | `FB_X))
                       | ((alu_op == `ALU_AND) ? `FB_H : 8'h0)
                       | (par_l ? `FB_P : 8'h0);

    // ---- INC / DEC (operate on b) ----
    wire [7:0]  inc_r = b + 8'd1;
    wire [7:0]  dec_r = b - 8'd1;
    wire [7:0]  f_inc = (oldf & `FB_C) | (inc_r[7] ? `FB_S : 8'h0)
                      | (inc_r == 8'h0 ? `FB_Z : 8'h0) | (inc_r & (`FB_Y | `FB_X))
                      | ((b[3:0] == 4'hF) ? `FB_H : 8'h0) | ((b == 8'h7F) ? `FB_P : 8'h0);
    wire [7:0]  f_dec = (oldf & `FB_C) | `FB_N | (dec_r[7] ? `FB_S : 8'h0)
                      | (dec_r == 8'h0 ? `FB_Z : 8'h0) | (dec_r & (`FB_Y | `FB_X))
                      | ((b[3:0] == 4'h0) ? `FB_H : 8'h0) | ((b == 8'h80) ? `FB_P : 8'h0);

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
    wire [7:0] f_rota = (oldf & (`FB_S | `FB_Z | `FB_P)) | (rr & (`FB_Y | `FB_X))
                      | (rcf ? `FB_C : 8'h0);

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
    wire [7:0] f_daa = (nf ? `FB_N : 8'h0) | (daa_r[7] ? `FB_S : 8'h0)
                     | (daa_r == 8'h0 ? `FB_Z : 8'h0) | (daa_r & (`FB_Y | `FB_X))
                     | (par_d ? `FB_P : 8'h0) | (newc ? `FB_C : 8'h0)
                     | (daa_h ? `FB_H : 8'h0);

    // ---- CPL / SCF / CCF ----
    wire [7:0] cpl_r = ~a;
    wire [7:0] f_cpl = (oldf & (`FB_S | `FB_Z | `FB_P | `FB_C)) | `FB_H | `FB_N
                     | (cpl_r & (`FB_Y | `FB_X));
    wire [7:0] f_scf = (oldf & (`FB_S | `FB_Z | `FB_P)) | `FB_C | (a & (`FB_Y | `FB_X));
    wire [7:0] f_ccf = (oldf & (`FB_S | `FB_Z | `FB_P)) | (oldf[0] ? `FB_H : 8'h0)
                     | (oldf[0] ? 8'h0 : `FB_C) | (a & (`FB_Y | `FB_X));

    // ---- output mux ----
    always @* begin
        case (mode)
            `FM_ADD8: begin res = sum;   fout = f_add; end
            `FM_SUB8: begin res = diff;  fout = f_sub; end
            `FM_CP8:  begin res = diff;  fout = f_sub; end  // core discards res
            `FM_LOGIC:begin res = lres;  fout = f_log; end
            `FM_INC8: begin res = inc_r; fout = f_inc; end
            `FM_DEC8: begin res = dec_r; fout = f_dec; end
            `FM_ROT_A:begin res = rr;    fout = f_rota;end
            `FM_DAA:  begin res = daa_r; fout = f_daa; end
            `FM_CPL:  begin res = cpl_r; fout = f_cpl; end
            `FM_SCF:  begin res = a;     fout = f_scf; end
            `FM_CCF:  begin res = a;     fout = f_ccf; end
            default:  begin res = b;     fout = oldf; end
        endcase
    end
endmodule
