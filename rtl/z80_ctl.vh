// ===========================================================================
// SPDX-FileCopyrightText: 2026 ReJ aka Renaldas Zioma
// SPDX-License-Identifier: CERN-OHL-W-2.0
// Source Location: https://github.com/rejunity/z80-from-schematics
// See HARDWARE_NOTICE.md for the section 4.1 Product notice.
//
// z80_ctl.vh - encodings of the control word (ctl_*) that z80_execute drives
// and the datapath blocks decode. Included inside module bodies.
// ===========================================================================
// Each block uses only some of them.
/* verilator lint_off UNUSEDPARAM */
// address latch input
localparam [2:0] AL_ABUS = 3'd0,   // the address-side bus
                 AL_IDU  = 3'd1,   // abus +1 / -1 / -2
                 AL_EA   = 3'd2,   // effective-address adder
                 AL_A16  = 3'd3;   // 16-bit ALU sum (ADD HL,rp internal cycle)
// address-side bus source
localparam [2:0] AB_REG  = 3'd0,   // register pair, see RI_*
                 AB_NN   = 3'd1,   // {rbyte, Z}: the operand word nn as W arrives
                 AB_AN   = 3'd2,   // {A, n}: IN A,(n) / OUT (n),A port
                 AB_IV   = 3'd3,   // {I, vector}: IM 2 table address
                 AB_NBC  = 3'd4;   // {B-1, C}: OUTI / OUTD port
// register pair on abus / written by the pair port
localparam [2:0] RI_PC = 3'd0, RI_SP = 3'd1, RI_WZ = 3'd2, RI_HL = 3'd3,
                 RI_DE = 3'd4, RI_BC = 3'd5, RI_HLP = 3'd6, RI_RP = 3'd7;
// incrementer
localparam [1:0] INC_P1 = 2'd0, INC_M1 = 2'd1, INC_M2 = 2'd2;
// effective-address adder displacement (base: PC or HL/IX/IY)
localparam [1:0] EA_RB = 2'd0, EA_Z = 2'd1, EA_M1 = 2'd2;
// write data
localparam [2:0] DB_ZERO = 3'd0, DB_REG = 3'd1, DB_RI = 3'd2, DB_R8 = 3'd3,
                 DB_RB = 3'd4, DB_ALU = 3'd5, DB_CB = 3'd6, DB_RRD = 3'd7;
localparam [1:0] BI_PC = 2'd0, BI_RP = 2'd1, BI_HLP = 2'd2, BI_A = 2'd3;
// PC / SP / WZ loads
localparam [2:0] PC_HOLD = 3'd0, PC_IDU = 3'd1, PC_EA = 3'd2, PC_NN = 3'd3,
                 PC_ABUS = 3'd4, PC_VEC = 3'd5;
localparam [1:0] VEC_RST = 2'd0, VEC_NMI = 2'd1, VEC_IM = 2'd2;
localparam [1:0] SP_HOLD = 2'd0, SP_IDU = 2'd1, SP_ABUS = 2'd2;
localparam [2:0] WZ_HOLD = 3'd0, WZ_IDU = 3'd1, WZ_ALO = 3'd2, WZ_EA = 3'd3,
                 WZ_Z = 3'd4, WZ_W = 3'd5, WZ_IV = 3'd6, WZ_VEC = 3'd7;
// pair write port source
localparam [1:0] PW_IDU = 2'd0, PW_A16 = 2'd1, PW_WZ = 2'd2;
// byte write port destination / source
localparam [3:0] BW_RI = 4'd0, BW_R8 = 4'd1, BW_A = 4'd2, BW_B = 4'd3,
                 BW_DDCB = 4'd4, BW_HLPL = 4'd5, BW_HLPH = 4'd6,
                 BW_RPL = 4'd7, BW_RPH = 4'd8;
localparam [2:0] BS_RI = 3'd0, BS_ALU = 3'd1, BS_RB = 3'd2, BS_CB = 3'd3, BS_BD = 3'd4;
// flag port source
localparam [2:0] FS_ALU = 3'd0, FS_A16 = 3'd1, FS_BLD = 3'd2, FS_BCP = 3'd3, FS_BIO = 3'd4;
// interrupt enables
localparam [1:0] IFF_HOLD = 2'd0, IFF_DI = 2'd1, IFF_EI = 2'd2, IFF_RETN = 2'd3;
/* verilator lint_on UNUSEDPARAM */
