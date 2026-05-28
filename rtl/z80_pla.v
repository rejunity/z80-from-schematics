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
            3'd0,3'd1: alu_fm = `FM_ADD8;
            3'd2,3'd3: alu_fm = `FM_SUB8;
            3'd4,3'd5,3'd6: alu_fm = `FM_LOGIC;
            default:   alu_fm = `FM_CP8;
        endcase
    end

    always @* begin
        // defaults
        exec = `EX_NOP; flag_mode = `FM_NONE; alu_op = 3'd0;
        rf_src = 3'd0; rf_dst = 3'd0; rp_sel = `RFP_BC; cc = 3'd0;
        rot_op = 3'd0; bit_index = 3'd0; cb_kind = 2'd0; aux = 4'd0;
        rst_addr = 8'h00; uses_cc = 1'b0; valid = 1'b1;

        if (prefix == `PFX_CB) begin
            cb_kind = x; rf_src = z; rf_dst = z; rot_op = y; bit_index = y;
            flag_mode = (x == `CB_ROT) ? `FM_ROT : (x == `CB_BIT) ? `FM_BIT : `FM_NONE;
            exec = (z == 3'd6) ? `EX_CB_M : `EX_CB_R;
        end else if (prefix == `PFX_ED) begin
            if (x == 2'd1) begin
                case (z)
                3'd0: begin exec = `EX_IN_C;  rf_dst = y; end
                3'd1: begin exec = `EX_OUT_C; rf_src = y; end
                3'd2: begin rp_sel = rp_of_p;
                            if (q == 1'b0) begin exec = `EX_SBC16; flag_mode = `FM_SBC16; end
                            else           begin exec = `EX_ADC16; flag_mode = `FM_ADC16; end end
                3'd3: begin rp_sel = rp_of_p; exec = (q == 1'b0) ? `EX_LD_NNA_RP : `EX_LD_RP_NNA; end
                3'd4: begin exec = `EX_NEG; flag_mode = `FM_SUB8; end
                3'd5: exec = `EX_RETN;
                3'd6: begin exec = `EX_IM;
                            aux = ((y == 3'd2) || (y == 3'd6)) ? 4'd1 :
                                  ((y == 3'd3) || (y == 3'd7)) ? 4'd2 : 4'd0; end
                default: begin // z==7
                    case (y)
                    3'd0: exec = `EX_LD_I_A;
                    3'd1: exec = `EX_LD_R_A;
                    3'd2: begin exec = `EX_LD_A_IR; aux = 4'd0; end
                    3'd3: begin exec = `EX_LD_A_IR; aux = 4'd1; end
                    3'd4: exec = `EX_RRD;
                    3'd5: exec = `EX_RLD;
                    default: exec = `EX_NOP;
                    endcase
                end
                endcase
            end else if ((x == 2'd2) && (z <= 3'd3) && (y >= 3'd4)) begin
                exec = `EX_BLOCK;
                aux = {2'b0, z[1:0]} + ((y == 3'd5 || y == 3'd7) ? 4'd1 : 4'd0)
                    + ((y == 3'd6 || y == 3'd7) ? 4'd8 : 4'd0)
                    + ({2'b0, z[1:0]});  // z*2 + dec + repeat
            end
            // else: EX_NOP (undocumented ED slot)
        end else if (prefix != `PFX_NONE) begin
            exec = `EX_ILLEGAL; valid = 1'b0;
        end else begin
        case (x)
        2'd0: begin
            case (z)
            3'd0: begin
                case (y)
                3'd0: exec = `EX_NOP;
                3'd1: exec = `EX_EX_AF;
                3'd2: begin exec = `EX_DJNZ; uses_cc = 1'b1; end
                3'd3: exec = `EX_JR;
                default: begin exec = `EX_JR_CC; uses_cc = 1'b1; cc = y - 3'd4; end
                endcase
            end
            3'd1: begin
                if (q == 1'b0) begin exec = `EX_LD_RP_NN; rp_sel = rp_of_p; end
                else begin exec = `EX_ADD_HL_RP; rp_sel = rp_of_p; flag_mode = `FM_ADD16; end
            end
            3'd2: begin
                if (q == 1'b0) begin
                    case (p)
                    2'd0: begin exec = `EX_LD_RP_A; rp_sel = `RFP_BC; end
                    2'd1: begin exec = `EX_LD_RP_A; rp_sel = `RFP_DE; end
                    2'd2: exec = `EX_LD_NN_HL;
                    default: exec = `EX_LD_NN_A;
                    endcase
                end else begin
                    case (p)
                    2'd0: begin exec = `EX_LD_A_RP; rp_sel = `RFP_BC; end
                    2'd1: begin exec = `EX_LD_A_RP; rp_sel = `RFP_DE; end
                    2'd2: exec = `EX_LD_HL_NN;
                    default: exec = `EX_LD_A_NN;
                    endcase
                end
            end
            3'd3: begin
                if (q == 1'b0) exec = `EX_INC_RP; else exec = `EX_DEC_RP;
                rp_sel = rp_of_p;
            end
            3'd4: begin
                rf_dst = y; rf_src = y; flag_mode = `FM_INC8;
                exec = (y == 3'd6) ? `EX_INC_M : `EX_INC_R;
            end
            3'd5: begin
                rf_dst = y; rf_src = y; flag_mode = `FM_DEC8;
                exec = (y == 3'd6) ? `EX_DEC_M : `EX_DEC_R;
            end
            3'd6: begin
                rf_dst = y;
                exec = (y == 3'd6) ? `EX_LD_M_N : `EX_LD_R_N;
            end
            default: begin // z==7
                case (y)
                3'd0,3'd1,3'd2,3'd3: begin exec = `EX_ROT_A; rot_op = y; flag_mode = `FM_ROT_A; end
                3'd4: begin exec = `EX_DAA; flag_mode = `FM_DAA; end
                3'd5: begin exec = `EX_CPL; flag_mode = `FM_CPL; end
                3'd6: begin exec = `EX_SCF; flag_mode = `FM_SCF; end
                default: begin exec = `EX_CCF; flag_mode = `FM_CCF; end
                endcase
            end
            endcase
        end
        2'd1: begin
            if (y == 3'd6 && z == 3'd6) exec = `EX_HALT;
            else if (z == 3'd6) begin exec = `EX_LD_R_M; rf_dst = y; end
            else if (y == 3'd6) begin exec = `EX_ST_M_R; rf_src = z; end
            else begin exec = `EX_LD_R_R; rf_dst = y; rf_src = z; end
        end
        2'd2: begin
            alu_op = y; flag_mode = alu_fm;
            if (z == 3'd6) exec = `EX_ALU_M;
            else begin exec = `EX_ALU_R; rf_src = z; end
        end
        default: begin // x==3
            case (z)
            3'd0: begin exec = `EX_RET_CC; uses_cc = 1'b1; cc = y; end
            3'd1: begin
                if (q == 1'b0) begin exec = `EX_POP; rp_sel = rp2_of_p; end
                else case (p)
                    2'd0: exec = `EX_RET;
                    2'd1: exec = `EX_EXX;
                    2'd2: exec = `EX_JP_HL;
                    default: exec = `EX_LD_SP_HL;
                endcase
            end
            3'd2: begin exec = `EX_JP_CC; uses_cc = 1'b1; cc = y; end
            3'd3: begin
                case (y)
                3'd0: exec = `EX_JP;
                3'd1: exec = `EX_PREFIX;       // CB
                3'd2: exec = `EX_OUT_N_A;
                3'd3: exec = `EX_IN_A_N;
                3'd4: exec = `EX_EX_SP_HL;
                3'd5: exec = `EX_EX_DE_HL;
                3'd6: exec = `EX_DI;
                default: exec = `EX_EI;
                endcase
            end
            3'd4: begin exec = `EX_CALL_CC; uses_cc = 1'b1; cc = y; end
            3'd5: begin
                if (q == 1'b0) begin exec = `EX_PUSH; rp_sel = rp2_of_p; end
                else exec = (p == 2'd0) ? `EX_CALL : `EX_PREFIX; // DD/ED/FD all prefix
            end
            3'd6: begin exec = `EX_ALU_N; alu_op = y; flag_mode = alu_fm; end
            default: begin exec = `EX_RST; rst_addr = {y, 3'b000}; end
            endcase
        end
        endcase
        end
    end
endmodule
