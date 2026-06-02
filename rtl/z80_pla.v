// ===========================================================================
// z80_pla.v - combinational instruction decode (mirrors cmodel/z80_pla.c).
// Maps (prefix, opcode) -> control word using the x/y/z/p/q decomposition.
// Unprefixed space complete; CB/ED/DD/FD decoded in a later stage.
// ===========================================================================
`include "z80_defs.vh"

module z80_pla (
    input  wire [2:0] prefix,
    input  wire [7:0] op,
    output reg  [6:0] exec,
    output reg  [4:0] flag_mode,
    output reg  [2:0] alu_op,
    output reg  [2:0] rf_src,
    output reg  [2:0] rf_dst,
    output reg  [3:0] rp_sel,
    output reg  [2:0] cc,
    output reg  [2:0] rot_op,
    output reg  [2:0] bit_index,
    output reg  [1:0] cb_kind,
    output reg  [3:0] aux,
    output reg  [1:0] idx,
    output reg        use_disp,
    output reg  [7:0] rst_addr,
    output reg        uses_cc,
    output reg        valid
);
    wire [1:0] x = op[7:6];
    wire [2:0] y = op[5:3];
    wire [2:0] z = op[2:0];
    wire [1:0] p = op[5:4];
    wire       q = op[3];

    // rp[p] / rp2[p] backing pairs
    reg [3:0] rp_of_p, rp2_of_p;
    always @* begin
        case (p)
            2'd0: begin rp_of_p = `RFP_BC; rp2_of_p = `RFP_BC; end
            2'd1: begin rp_of_p = `RFP_DE; rp2_of_p = `RFP_DE; end
            2'd2: begin rp_of_p = `RFP_HL; rp2_of_p = `RFP_HL; end
            default: begin rp_of_p = `RFP_SP; rp2_of_p = `RFP_AF; end
        endcase
    end

    // ALU flag-mode from y
    reg [4:0] alu_fm;
    always @* begin
        case (y)
            3'd0,3'd1: alu_fm = `FLAG_ADD8;
            3'd2,3'd3: alu_fm = `FLAG_SUB8;
            3'd4,3'd5,3'd6: alu_fm = `FLAG_LOGIC;
            default:   alu_fm = `FLAG_CP8;
        endcase
    end

    always @* begin
        // defaults
        exec = `EXEC_NOP; flag_mode = `FLAG_NONE; alu_op = 3'd0;
        rf_src = 3'd0; rf_dst = 3'd0; rp_sel = `RFP_BC; cc = 3'd0;
        rot_op = 3'd0; bit_index = 3'd0; cb_kind = 2'd0; aux = 4'd0;
        idx = 2'd0; use_disp = 1'b0;
        rst_addr = 8'h00; uses_cc = 1'b0; valid = 1'b1;

        if (prefix == `PFX_CB) begin
            cb_kind = x; rf_src = z; rf_dst = z; rot_op = y; bit_index = y;
            flag_mode = (x == `CB_ROT) ? `FLAG_ROT : (x == `CB_BIT) ? `FLAG_BIT : `FLAG_NONE;
            exec = (z == 3'd6) ? `EXEC_CB_M : `EXEC_CB_R;
        end else if (prefix == `PFX_ED) begin
            if (x == 2'd1) begin
                case (z)
                3'd0: begin exec = `EXEC_IN_C;  rf_dst = y; end
                3'd1: begin exec = `EXEC_OUT_C; rf_src = y; end
                3'd2: begin rp_sel = rp_of_p;
                            if (q == 1'b0) begin exec = `EXEC_SBC16; flag_mode = `FLAG_SBC16; end
                            else           begin exec = `EXEC_ADC16; flag_mode = `FLAG_ADC16; end end
                3'd3: begin rp_sel = rp_of_p; exec = (q == 1'b0) ? `EXEC_LD_NNA_RP : `EXEC_LD_RP_NNA; end
                3'd4: begin exec = `EXEC_NEG; flag_mode = `FLAG_NEG; end
                3'd5: exec = `EXEC_RETN;
                3'd6: begin exec = `EXEC_IM;
                            aux = ((y == 3'd2) || (y == 3'd6)) ? 4'd1 :
                                  ((y == 3'd3) || (y == 3'd7)) ? 4'd2 : 4'd0; end
                default: begin // z==7
                    case (y)
                    3'd0: exec = `EXEC_LD_I_A;
                    3'd1: exec = `EXEC_LD_R_A;
                    3'd2: begin exec = `EXEC_LD_A_IR; aux = 4'd0; end
                    3'd3: begin exec = `EXEC_LD_A_IR; aux = 4'd1; end
                    3'd4: exec = `EXEC_RRD;
                    3'd5: exec = `EXEC_RLD;
                    default: exec = `EXEC_NOP;
                    endcase
                end
                endcase
            end else if ((x == 2'd2) && (z <= 3'd3) && (y >= 3'd4)) begin
                exec = `EXEC_BLOCK;
                aux = {2'b0, z[1:0]} + ((y == 3'd5 || y == 3'd7) ? 4'd1 : 4'd0)
                    + ((y == 3'd6 || y == 3'd7) ? 4'd8 : 4'd0)
                    + ({2'b0, z[1:0]});  // z*2 + dec + repeat
            end
            // else: EXEC_NOP (undocumented ED slot)
        end else if (prefix == `PFX_DD || prefix == `PFX_FD || prefix == `PFX_NONE) begin
        case (x)
        2'd0: begin
            case (z)
            3'd0: begin
                case (y)
                3'd0: exec = `EXEC_NOP;
                3'd1: exec = `EXEC_EX_AF;
                3'd2: begin exec = `EXEC_DJNZ; uses_cc = 1'b1; end
                3'd3: exec = `EXEC_JR;
                default: begin exec = `EXEC_JR_CC; uses_cc = 1'b1; cc = y - 3'd4; end
                endcase
            end
            3'd1: begin
                if (q == 1'b0) begin exec = `EXEC_LD_RP_NN; rp_sel = rp_of_p; end
                else begin exec = `EXEC_ADD_HL_RP; rp_sel = rp_of_p; flag_mode = `FLAG_ADD16; end
            end
            3'd2: begin
                if (q == 1'b0) begin
                    case (p)
                    2'd0: begin exec = `EXEC_LD_RP_A; rp_sel = `RFP_BC; end
                    2'd1: begin exec = `EXEC_LD_RP_A; rp_sel = `RFP_DE; end
                    2'd2: exec = `EXEC_LD_NN_HL;
                    default: exec = `EXEC_LD_NN_A;
                    endcase
                end else begin
                    case (p)
                    2'd0: begin exec = `EXEC_LD_A_RP; rp_sel = `RFP_BC; end
                    2'd1: begin exec = `EXEC_LD_A_RP; rp_sel = `RFP_DE; end
                    2'd2: exec = `EXEC_LD_HL_NN;
                    default: exec = `EXEC_LD_A_NN;
                    endcase
                end
            end
            3'd3: begin
                if (q == 1'b0) exec = `EXEC_INC_RP; else exec = `EXEC_DEC_RP;
                rp_sel = rp_of_p;
            end
            3'd4: begin
                rf_dst = y; rf_src = y; flag_mode = `FLAG_INC8;
                exec = (y == 3'd6) ? `EXEC_INC_M : `EXEC_INC_R;
            end
            3'd5: begin
                rf_dst = y; rf_src = y; flag_mode = `FLAG_DEC8;
                exec = (y == 3'd6) ? `EXEC_DEC_M : `EXEC_DEC_R;
            end
            3'd6: begin
                rf_dst = y;
                exec = (y == 3'd6) ? `EXEC_LD_M_N : `EXEC_LD_R_N;
            end
            default: begin // z==7
                case (y)
                3'd0,3'd1,3'd2,3'd3: begin exec = `EXEC_ROT_A; rot_op = y; flag_mode = `FLAG_ROT_A; end
                3'd4: begin exec = `EXEC_DAA; flag_mode = `FLAG_DAA; end
                3'd5: begin exec = `EXEC_CPL; flag_mode = `FLAG_CPL; end
                3'd6: begin exec = `EXEC_SCF; flag_mode = `FLAG_SCF; end
                default: begin exec = `EXEC_CCF; flag_mode = `FLAG_CCF; end
                endcase
            end
            endcase
        end
        2'd1: begin
            if (y == 3'd6 && z == 3'd6) exec = `EXEC_HALT;
            else if (z == 3'd6) begin exec = `EXEC_LD_R_M; rf_dst = y; end
            else if (y == 3'd6) begin exec = `EXEC_ST_M_R; rf_src = z; end
            else begin exec = `EXEC_LD_R_R; rf_dst = y; rf_src = z; end
        end
        2'd2: begin
            alu_op = y; flag_mode = alu_fm;
            if (z == 3'd6) exec = `EXEC_ALU_M;
            else begin exec = `EXEC_ALU_R; rf_src = z; end
        end
        default: begin // x==3
            case (z)
            3'd0: begin exec = `EXEC_RET_CC; uses_cc = 1'b1; cc = y; end
            3'd1: begin
                if (q == 1'b0) begin exec = `EXEC_POP; rp_sel = rp2_of_p; end
                else case (p)
                    2'd0: exec = `EXEC_RET;
                    2'd1: exec = `EXEC_EXX;
                    2'd2: exec = `EXEC_JP_HL;
                    default: exec = `EXEC_LD_SP_HL;
                endcase
            end
            3'd2: begin exec = `EXEC_JP_CC; uses_cc = 1'b1; cc = y; end
            3'd3: begin
                case (y)
                3'd0: exec = `EXEC_JP;
                3'd1: exec = `EXEC_PREFIX;       // CB
                3'd2: exec = `EXEC_OUT_N_A;
                3'd3: exec = `EXEC_IN_A_N;
                3'd4: exec = `EXEC_EX_SP_HL;
                3'd5: exec = `EXEC_EX_DE_HL;
                3'd6: exec = `EXEC_DI;
                default: exec = `EXEC_EI;
                endcase
            end
            3'd4: begin exec = `EXEC_CALL_CC; uses_cc = 1'b1; cc = y; end
            3'd5: begin
                if (q == 1'b0) begin exec = `EXEC_PUSH; rp_sel = rp2_of_p; end
                else exec = (p == 2'd0) ? `EXEC_CALL : `EXEC_PREFIX; // DD/ED/FD all prefix
            end
            3'd6: begin exec = `EXEC_ALU_N; alu_op = y; flag_mode = alu_fm; end
            default: begin exec = `EXEC_RST; rst_addr = {y, 3'b000}; end
            endcase
        end
        endcase
        if (prefix != `PFX_NONE) begin            // DD/FD decoration
            idx = (prefix == `PFX_DD) ? 2'd1 : 2'd2;
            case (exec)
                `EXEC_LD_R_M, `EXEC_ST_M_R, `EXEC_ALU_M,
                `EXEC_INC_M, `EXEC_DEC_M, `EXEC_LD_M_N: use_disp = 1'b1;
                `EXEC_EX_DE_HL: idx = 2'd0;          // EX DE,HL unaffected
                default: ;
            endcase
            if (exec == `EXEC_PREFIX) idx = 2'd0;    // DDCB chain: leave plain
            if (rp_sel == `RFP_HL) rp_sel = (prefix == `PFX_DD) ? `RFP_IX : `RFP_IY;
        end
        end else if (prefix == `PFX_DDCB || prefix == `PFX_FDCB) begin
            exec = `EXEC_DDCB;
            idx = (prefix == `PFX_DDCB) ? 2'd1 : 2'd2;
        end else begin
            exec = `EXEC_ILLEGAL; valid = 1'b0;
        end
    end
endmodule
