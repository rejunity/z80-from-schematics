// ===========================================================================
// z80_defs.vh - shared constants for the RTL, mirroring the C model enums
// (cmodel/z80.h). Included by all modules so C and Verilog stay aligned.
// Verilog-2001: localparams via `include guard.
// ===========================================================================
`ifndef Z80_DEFS_VH
`define Z80_DEFS_VH

// ---- flag bit masks (F = S Z Y H X P/V N C) ----
`define FB_S 8'h80
`define FB_Z 8'h40
`define FB_Y 8'h20
`define FB_H 8'h10
`define FB_X 8'h08
`define FB_P 8'h04
`define FB_N 8'h02
`define FB_C 8'h01

// ---- register-file pair indices (rf[0..12]) ----
`define RFP_BC  4'd0
`define RFP_DE  4'd1
`define RFP_HL  4'd2
`define RFP_AF  4'd3
`define RFP_BC2 4'd4
`define RFP_DE2 4'd5
`define RFP_HL2 4'd6
`define RFP_AF2 4'd7
`define RFP_IX  4'd8
`define RFP_IY  4'd9
`define RFP_SP  4'd10
`define RFP_PC  4'd11
`define RFP_WZ  4'd12

// ---- bus operation codes ----
`define BUSOP_NONE     3'd0
`define BUSOP_M1       3'd1
`define BUSOP_MRD      3'd2
`define BUSOP_MWR      3'd3
`define BUSOP_IORD     3'd4
`define BUSOP_IOWR     3'd5
`define BUSOP_INTA     3'd6
`define BUSOP_INTERNAL 3'd7

// ---- prefix states ----
`define PFX_NONE 3'd0
`define PFX_CB   3'd1
`define PFX_ED   3'd2
`define PFX_DD   3'd3
`define PFX_FD   3'd4
`define PFX_DDCB 3'd5
`define PFX_FDCB 3'd6

// ---- ALU op codes (alu[y]) ----
`define ALU_ADD 3'd0
`define ALU_ADC 3'd1
`define ALU_SUB 3'd2
`define ALU_SBC 3'd3
`define ALU_AND 3'd4
`define ALU_XOR 3'd5
`define ALU_OR  3'd6
`define ALU_CP  3'd7

// ---- flag-update modes (z80_flag_mode_t) ----
`define FM_NONE   5'd0
`define FM_ADD8   5'd1
`define FM_SUB8   5'd2
`define FM_CP8    5'd3
`define FM_LOGIC  5'd4
`define FM_INC8   5'd5
`define FM_DEC8   5'd6
`define FM_ROT_A  5'd7
`define FM_ROT    5'd8
`define FM_BIT    5'd9
`define FM_ADD16  5'd10
`define FM_ADC16  5'd11
`define FM_SBC16  5'd12
`define FM_DAA    5'd13
`define FM_SCF    5'd14
`define FM_CCF    5'd15
`define FM_CPL    5'd16

// ---- exec (datapath action) codes, mirroring z80_exec_t order ----
`define EX_NOP        6'd0
`define EX_PREFIX     6'd1
`define EX_HALT       6'd2
`define EX_DI         6'd3
`define EX_EI         6'd4
`define EX_LD_R_R     6'd5
`define EX_LD_R_N     6'd6
`define EX_LD_R_M     6'd7
`define EX_ST_M_R     6'd8
`define EX_LD_M_N     6'd9
`define EX_ALU_R      6'd10
`define EX_ALU_N      6'd11
`define EX_ALU_M      6'd12
`define EX_INC_R      6'd13
`define EX_DEC_R      6'd14
`define EX_INC_M      6'd15
`define EX_DEC_M      6'd16
`define EX_LD_RP_NN   6'd17
`define EX_INC_RP     6'd18
`define EX_DEC_RP     6'd19
`define EX_ADD_HL_RP  6'd20
`define EX_JP         6'd21
`define EX_JP_CC      6'd22
`define EX_JP_HL      6'd23
`define EX_JR         6'd24
`define EX_JR_CC      6'd25
`define EX_DJNZ       6'd26
`define EX_CALL       6'd27
`define EX_CALL_CC    6'd28
`define EX_RET        6'd29
`define EX_RET_CC     6'd30
`define EX_RST        6'd31
`define EX_PUSH       6'd32
`define EX_POP        6'd33
`define EX_LD_SP_HL   6'd34
`define EX_EX_DE_HL   6'd35
`define EX_EX_AF      6'd36
`define EX_EXX        6'd37
`define EX_EX_SP_HL   6'd38
`define EX_LD_A_RP    6'd39
`define EX_LD_RP_A    6'd40
`define EX_LD_A_NN    6'd41
`define EX_LD_NN_A    6'd42
`define EX_LD_HL_NN   6'd43
`define EX_LD_NN_HL   6'd44
`define EX_ROT_A      6'd45
`define EX_DAA        6'd46
`define EX_CPL        6'd47
`define EX_SCF        6'd48
`define EX_CCF        6'd49
`define EX_IN_A_N     6'd50
`define EX_OUT_N_A    6'd51
`define EX_CB_R       6'd53
`define EX_CB_M       6'd54
`define EX_ILLEGAL    6'd52

// CB op kind (= x field of CB opcode)
`define CB_ROT 2'd0
`define CB_BIT 2'd1
`define CB_RES 2'd2
`define CB_SET 2'd3

`endif
