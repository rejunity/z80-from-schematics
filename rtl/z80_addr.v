// ===========================================================================
// SPDX-FileCopyrightText: 2026 Renaldas Zioma
// SPDX-License-Identifier: CERN-OHL-W-2.0
// Source Location: https://github.com/rejunity/z80-from-schematics
// See HARDWARE_NOTICE.md for the section 4.1 Product notice.
//
// z80_addr.v - the address side: the 16-bit address-side bus (abus), the
// incrementer/decrementer (IDU) on it, the effective-address adder next to
// it, and the input of the address latch (z80_core's m_addr).
//
// abus carries a register pair or one of the address temporaries; the
// IDU forms abus +1 / -1 / -2 for the address latch and for write-back
// (PC+1, SP-1, rp+1, block pointer steps, MEMPTR rules); the EA adder
// forms PC + e (JR, DJNZ, the block-repeat PC-1) and IX/IY + d.
// ===========================================================================
`include "z80_defs.vh"

module z80_addr (
    // register file (logical pairs) and address temporaries
    input  wire [15:0] pc, sp, wz, hl, de, bc,
    input  wire [15:0] wzp,             // WZ as the waiting bytes leave it (AL_WZP)
    input  wire [15:0] hlp,             // HL / IX / IY of the current instruction
    input  wire [15:0] rp,              // rp[p] / rp2[p] named by the PLA
    input  wire [7:0]  rbyte,           // the byte read this T-state
    input  wire [7:0]  a,               // accumulator
    input  wire [7:0]  i_reg,           // I
    input  wire [7:0]  tmp8,            // read-data latch (INTA vector)
    input  wire [7:0]  b_dec,           // B-1 (OUTI port address)
    // control word
    input  wire [2:0]  ctl_ab,
    input  wire [2:0]  ctl_ri,
    input  wire [1:0]  ctl_inc,
    input  wire        ctl_ea_pc,
    input  wire [1:0]  ctl_ea_d,
    input  wire [2:0]  ctl_al,
    output reg  [15:0] abus,
    output wire [15:0] idu,
    output wire [15:0] ea,
    output reg  [15:0] al_in            // address latch input
);
`include "z80_ctl.vh"

    reg  [15:0] rf_ab;
    always @* begin
        case (ctl_ri)
            RI_PC:   rf_ab = pc;
            RI_SP:   rf_ab = sp;
            RI_WZ:   rf_ab = wz;
            RI_HL:   rf_ab = hl;
            RI_DE:   rf_ab = de;
            RI_BC:   rf_ab = bc;
            RI_HLP:  rf_ab = hlp;
            default: rf_ab = rp;
        endcase
    end
    always @* begin
        case (ctl_ab)
            AB_REG:  abus = rf_ab;
            AB_NN:   abus = {rbyte, wz[7:0]};
            AB_AN:   abus = {a, rbyte};
            AB_IV:   abus = {i_reg, tmp8};
            default: abus = {b_dec, bc[7:0]};
        endcase
    end
    // IDU: 16-bit incrementer / decrementer
    assign idu = abus + ((ctl_inc == INC_P1) ? 16'h0001 :
                         (ctl_inc == INC_M1) ? 16'hFFFF : 16'hFFFE);
    // effective address: PC + e (JR, DJNZ, block repeat PC-1), IX + d
    wire [15:0] ea_base = ctl_ea_pc ? pc : hlp;
    wire [7:0]  ea_d    = (ctl_ea_d == EA_RB) ? rbyte : (ctl_ea_d == EA_Z) ? wz[7:0] : 8'hFF;
    assign ea = ea_base + {{8{ea_d[7]}}, ea_d};

    always @* begin
        case (ctl_al)
            AL_ABUS: al_in = abus;
            AL_IDU:  al_in = idu;
            AL_EA:   al_in = ea;
            AL_WZP:  al_in = wzp;
            default: al_in = wz;            // AL_WZ
        endcase
    end
endmodule
