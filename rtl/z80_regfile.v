// ===========================================================================
// SPDX-FileCopyrightText: 2026 ReJ aka Renaldas Zioma
// SPDX-License-Identifier: CERN-OHL-W-2.0
// Source Location: https://github.com/rejunity/z80-from-schematics
// See HARDWARE_NOTICE.md for the section 4.1 Product notice.
//
// z80_regfile.v - the register file: BC DE HL AF, BC' DE' HL' AF', IX IY
// SP PC WZ as 16-bit pairs.
//
// As on the die, EX DE,HL / EXX / EX AF,AF' move no data. Four exchange
// flip-flops (EXX bank, AF bank, and a DE/HL swap bit per physical bank)
// decide which physical pair answers to BC, DE, HL, AF and their primes;
// reads and writes go through that renaming. rf[0..3] and rf[4..7] are the
// two banks; with the flip-flops clear (reset) physical = logical (RFP_*
// order), so a testbench may preload rf[] (and rf_n[]) straight after
// reset. Read the logical registers back through the outputs (z80_core
// collects them in rfl[]).
//
// Ports: one 16-bit pair write, one byte write (high or low half of a
// pair), the flag port (F), and PC / SP / WZ loads, all taking logical
// pair names. Writes land on `we` edges; the exchanges on `ex_en` edges.
// Only power-on (por_n) resets the file; the RESET pin keeps every pair,
// WZ and the exchange flip-flops and clears just PC (pc_clr), as on the
// NMOS die (perfectz80).
// ===========================================================================
`include "z80_defs.vh"

module z80_regfile (
    input  wire        clk,
    input  wire        por_n,           // power-on reset
    input  wire        pc_clr,          // RESET / special reset: PC = 0 on this edge
    input  wire        we,              // register loads on this edge
    input  wire        ex_en,           // exchanges on this edge
    input  wire        ex_dehl,         // EX DE,HL
    input  wire        ex_af,           // EX AF,AF'
    input  wire        exx,             // EXX
    input  wire        pw_we,           // pair write
    input  wire [3:0]  pw_sel,
    input  wire [15:0] pw_d,
    input  wire        bw_we,           // byte write
    input  wire [3:0]  bw_sel,
    input  wire        bw_hi,
    input  wire [7:0]  bw_d,
    input  wire        f_we,            // flags
    input  wire [7:0]  f_d,
    input  wire        pc_we,
    input  wire [15:0] pc_d,
    input  wire        sp_we,
    input  wire [15:0] sp_d,
    input  wire        wz_we,
    input  wire [15:0] wz_d,
    // logical registers
    output wire [15:0] bc, de, hl, af, bc2, de2, hl2, af2,
    output wire [15:0] ix, iy, sp, pc, wz,
    // after this edge
    output wire [15:0] pc_next          // PC (the next M1 address at an instruction boundary)
);
    integer i, ri;

    reg [15:0] rf   [0:12];             // physical pairs
    reg [15:0] rf_n [0:12];
    reg        xbank;                   // EXX: BC DE HL are rf[4..6] (else rf[0..2])
    reg        xaf;                     // EX AF,AF': AF is rf[7] (else rf[3])
    reg  [1:0] xdh;                     // EX DE,HL, per physical bank: DE and HL swapped

    // logical pair -> physical pair through the exchange flip-flops
    function [3:0] phys;
        input [3:0] l; input xb; input xa; input [1:0] xd;
        reg b, s;
        begin
            b = ((l == `RFP_BC2) || (l == `RFP_DE2) || (l == `RFP_HL2)) ? ~xb : xb;
            s = b ? xd[1] : xd[0];
            case (l)
                `RFP_BC, `RFP_BC2: phys = {1'b0, b, 2'd0};
                `RFP_DE, `RFP_DE2: phys = {1'b0, b, s ? 2'd2 : 2'd1};
                `RFP_HL, `RFP_HL2: phys = {1'b0, b, s ? 2'd1 : 2'd2};
                `RFP_AF:           phys = {1'b0, xa, 2'd3};
                `RFP_AF2:          phys = {1'b0, ~xa, 2'd3};
                default:           phys = l;
            endcase
        end
    endfunction

    // ---- read side ----
    wire [15:0] p_bc0 = rf[0], p_de0 = rf[1], p_hl0 = rf[2], p_af0 = rf[3];
    wire [15:0] p_bc1 = rf[4], p_de1 = rf[5], p_hl1 = rf[6], p_af1 = rf[7];
    wire        x_sw  = xbank ? xdh[1] : xdh[0];   // DE/HL swapped in the live bank
    wire        x_sw2 = xbank ? xdh[0] : xdh[1];   // ... in the other bank
    assign bc  = xbank ? p_bc1 : p_bc0;
    assign de  = xbank ? (x_sw ? p_hl1 : p_de1) : (x_sw ? p_hl0 : p_de0);
    assign hl  = xbank ? (x_sw ? p_de1 : p_hl1) : (x_sw ? p_de0 : p_hl0);
    assign af  = xaf ? p_af1 : p_af0;
    assign bc2 = xbank ? p_bc0 : p_bc1;
    assign de2 = xbank ? (x_sw2 ? p_hl0 : p_de0) : (x_sw2 ? p_hl1 : p_de1);
    assign hl2 = xbank ? (x_sw2 ? p_de0 : p_hl0) : (x_sw2 ? p_de1 : p_hl1);
    assign af2 = xaf ? p_af0 : p_af1;
    assign ix  = rf[`RFP_IX];
    assign iy  = rf[`RFP_IY];
    assign sp  = rf[`RFP_SP];
    assign pc  = rf[`RFP_PC];
    assign wz  = rf[`RFP_WZ];

    // ---- write side ----
    wire [3:0] pw_phys = phys(pw_sel,  xbank, xaf, xdh);
    wire [3:0] bw_phys = phys(bw_sel,  xbank, xaf, xdh);
    wire [3:0] af_phys = phys(`RFP_AF, xbank, xaf, xdh);
    // exchanges flip the renaming, effective from the next T-state
    wire       xbank_n = xbank ^ (ex_en & exx);
    wire       xaf_n   = xaf   ^ (ex_en & ex_af);
    wire [1:0] xdh_n   = xdh   ^ ((ex_en & ex_dehl) ? (xbank ? 2'b10 : 2'b01) : 2'b00);

    always @* begin
        for (i = 0; i < 13; i = i + 1) rf_n[i] = rf[i];
        if (we) begin
            for (i = 0; i < 13; i = i + 1) begin
                if (pw_we && pw_phys == i) rf_n[i] = pw_d;
                if (bw_we && bw_phys == i) begin
                    if (bw_hi) rf_n[i][15:8] = bw_d;
                    else       rf_n[i][7:0]  = bw_d;
                end
                if (f_we && af_phys == i) rf_n[i][7:0] = f_d;
            end
            if (pc_we) rf_n[`RFP_PC] = pc_d;
            if (sp_we) rf_n[`RFP_SP] = sp_d;
            if (wz_we) rf_n[`RFP_WZ] = wz_d;
        end
        if (pc_clr) rf_n[`RFP_PC] = 16'h0000;
    end
    assign pc_next = rf_n[`RFP_PC];

    // 0x5555 matches perfectz80's gate-level Visual-Z80 netlist boot
    // pattern (see cmodel/z80_core.c z80_reset()); PC resets to 0.
    always @(posedge clk or negedge por_n) begin
        if (!por_n) begin
            for (ri = 0; ri < 13; ri = ri + 1) rf[ri] <= 16'h5555;
            rf[`RFP_PC] <= 16'h0000;
            xbank <= 1'b0; xaf <= 1'b0; xdh <= 2'b00;
        end else begin
            for (ri = 0; ri < 13; ri = ri + 1) rf[ri] <= rf_n[ri];
            xbank <= xbank_n; xaf <= xaf_n; xdh <= xdh_n;
        end
    end
endmodule
