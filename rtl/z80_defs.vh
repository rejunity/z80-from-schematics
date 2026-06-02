// ===========================================================================
// z80_defs.vh - shared constants for the RTL, mirroring the C model enums
// (cmodel/z80.h). Included by all modules so C and Verilog stay aligned.
// Verilog-2001: localparams via `include guard.
// ===========================================================================
`ifndef Z80_DEFS_VH
`define Z80_DEFS_VH

// ---- flag bit masks (F = S Z Y H X P/V N C) ----
`define Z80_SF 8'h80
`define Z80_ZF 8'h40
`define Z80_YF 8'h20
`define Z80_HF 8'h10
`define Z80_XF 8'h08
`define Z80_PF 8'h04
`define Z80_NF 8'h02
`define Z80_CF 8'h01

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
`define FLAG_NONE   5'd0
`define FLAG_ADD8   5'd1
`define FLAG_SUB8   5'd2
`define FLAG_CP8    5'd3
`define FLAG_LOGIC  5'd4
`define FLAG_INC8   5'd5
`define FLAG_DEC8   5'd6
`define FLAG_ROT_A  5'd7
`define FLAG_ROT    5'd8
`define FLAG_BIT    5'd9
`define FLAG_ADD16  5'd10
`define FLAG_ADC16  5'd11
`define FLAG_SBC16  5'd12
`define FLAG_DAA    5'd13
`define FLAG_SCF    5'd14
`define FLAG_CCF    5'd15
`define FLAG_CPL    5'd16
`define FLAG_NEG       5'd17
`define FLAG_BLOCK_LD  5'd18
`define FLAG_BLOCK_CP  5'd19
`define FLAG_BLOCK_IO  5'd20
`define FLAG_LD_A_I    5'd21
`define FLAG_IN        5'd22
`define FLAG_RRD       5'd23
`define FLAG_RLD       5'd24

// ---- exec (datapath action) codes, mirroring z80_exec_t order ----
`define EXEC_NOP        6'd0
`define EXEC_PREFIX     6'd1
`define EXEC_HALT       6'd2
`define EXEC_DI         6'd3
`define EXEC_EI         6'd4
`define EXEC_LD_R_R     6'd5
`define EXEC_LD_R_N     6'd6
`define EXEC_LD_R_M     6'd7
`define EXEC_ST_M_R     6'd8
`define EXEC_LD_M_N     6'd9
`define EXEC_ALU_R      6'd10
`define EXEC_ALU_N      6'd11
`define EXEC_ALU_M      6'd12
`define EXEC_INC_R      6'd13
`define EXEC_DEC_R      6'd14
`define EXEC_INC_M      6'd15
`define EXEC_DEC_M      6'd16
`define EXEC_LD_RP_NN   6'd17
`define EXEC_INC_RP     6'd18
`define EXEC_DEC_RP     6'd19
`define EXEC_ADD_HL_RP  6'd20
`define EXEC_JP         6'd21
`define EXEC_JP_CC      6'd22
`define EXEC_JP_HL      6'd23
`define EXEC_JR         6'd24
`define EXEC_JR_CC      6'd25
`define EXEC_DJNZ       6'd26
`define EXEC_CALL       6'd27
`define EXEC_CALL_CC    6'd28
`define EXEC_RET        6'd29
`define EXEC_RET_CC     6'd30
`define EXEC_RST        6'd31
`define EXEC_PUSH       6'd32
`define EXEC_POP        6'd33
`define EXEC_LD_SP_HL   6'd34
`define EXEC_EX_DE_HL   6'd35
`define EXEC_EX_AF      6'd36
`define EXEC_EXX        6'd37
`define EXEC_EX_SP_HL   6'd38
`define EXEC_LD_A_RP    6'd39
`define EXEC_LD_RP_A    6'd40
`define EXEC_LD_A_NN    6'd41
`define EXEC_LD_NN_A    6'd42
`define EXEC_LD_HL_NN   6'd43
`define EXEC_LD_NN_HL   6'd44
`define EXEC_ROT_A      6'd45
`define EXEC_DAA        6'd46
`define EXEC_CPL        6'd47
`define EXEC_SCF        6'd48
`define EXEC_CCF        6'd49
`define EXEC_IN_A_N     7'd50
`define EXEC_OUT_N_A    7'd51
`define EXEC_CB_R       7'd53
`define EXEC_CB_M       7'd54
`define EXEC_ILLEGAL    7'd52
// ED page
`define EXEC_ADC16      7'd55
`define EXEC_SBC16      7'd56
`define EXEC_NEG        7'd57
`define EXEC_IM         7'd58
`define EXEC_RETN       7'd59
`define EXEC_LD_I_A     7'd60
`define EXEC_LD_R_A     7'd61
`define EXEC_LD_A_IR    7'd62
`define EXEC_LD_NNA_RP  7'd63
`define EXEC_LD_RP_NNA  7'd64
`define EXEC_IN_C       7'd65
`define EXEC_OUT_C      7'd66
`define EXEC_RRD        7'd67
`define EXEC_RLD        7'd68
`define EXEC_BLOCK      7'd69
`define EXEC_DDCB       7'd70
`define EXEC_NMI        7'd71
`define EXEC_INT        7'd72

// CB op kind (= x field of CB opcode)
`define CB_ROT 2'd0
`define CB_BIT 2'd1
`define CB_RES 2'd2
`define CB_SET 2'd3

// ---- IDU operation encodings (rtl/z80_idu.v / cmodel/z80_internal.h) ----
`define IDU_NONE     2'd0
`define IDU_INC      2'd1
`define IDU_DEC      2'd2
`define IDU_ADD_DISP 2'd3

// ---- IFF mutation encoding for ctl_iff_op (z80_seq -> z80_core) ----
`define IFF_NONE  2'd0    // no change
`define IFF_CLEAR 2'd1    // DI: iff1=0, iff2=0
`define IFF_SET   2'd2    // EI: iff1=1, iff2=1
`define IFF_RETN  2'd3    // RETN: iff1=iff2

`endif
