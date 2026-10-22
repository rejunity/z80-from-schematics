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
    output wire [15:0] pc_next,         // PC (the next M1 address at an instruction boundary)
    output wire [7:0]  f_next,          // F (EX AF,AF' renames AF on the same edge)
    // Register write-back as the die does it (docs/timing.md "Register
    // write-back"): the die writes a result in the low half of a T-state,
    // which a RESET cut ending on that T-state blocks. The core writes
    // most results earlier; each byte written before the die's edge is
    // logged here with its old value and the edge it becomes permanent
    // on -- due = {next, m, t}: T-state t of M-cycle m, or (next) of the
    // next M1 / INTA cycle -- and a cut before that edge puts it back.
    input  wire        wb_adv,          // the sequencer advances on this edge (commits)
    input  wire        wb_held,         // ... or a bus grant holds the next M-cycle (its T1's)
    input  wire        wb_tw,           // ... and the cut ends on a Tw after T2 (T3's too)
    input  wire        wb_fin,          // ... and the instruction ends on it
    input  wire        wb_cut,          // a RESET cut ends on this edge
    input  wire        wb_t2,           // ... on the next M1's (INTA's) T2: F from the flag latches
    input  wire [7:0]  f_cut,           // ... as the die then writes it (with an F write)
    input  wire [2:0]  wb_m,            // the T-state this edge ends
    input  wire [3:0]  wb_t,
    input  wire        wb_m1,           // ... in an M1 or INTA cycle
    input  wire        pw_log_lo, pw_log_hi,
    input  wire [15:0] pw_old,          // the pair written, before this edge (the core has it)
    input  wire [7:0]  pw_due_lo, pw_due_hi,
    input  wire        bw_log,
    input  wire [7:0]  bw_old,          // the byte written, before this edge (the core has it)
    input  wire [7:0]  bw_due,
    input  wire        f_log,           // F waits ...
    input  wire [7:0]  f_due,           // ... for the next M1's T3 (or T2)
    input  wire        sp_log,
    input  wire [7:0]  sp_due
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

    // A RESET cut's restores (the write-back log below) go through the byte
    // write, one byte an edge on the edges after the cut -- the reset idle
    // and the restart's first T-states, where nothing else writes and nothing
    // reads them yet: slot 0's low and high byte, then slot 1's.
    reg        r_lo [0:1], r_hi [0:1];          // byte to put back
    reg  [3:0] s_p  [0:1];                      // the slot's physical pair
    reg [15:0] s_old[0:1];                      // ... and old value
    wire       rs_0l = r_lo[0], rs_0h = r_hi[0] && !rs_0l;
    wire       rs_1l = r_lo[1] && !r_lo[0] && !r_hi[0], rs_1h = r_hi[1] && !r_lo[0] && !r_hi[0] && !r_lo[1];
    wire       rs_we = (rs_0l || rs_0h || rs_1l || rs_1h) && !(we && bw_we);
    wire [3:0] rs_p  = (rs_0l || rs_0h) ? s_p[0] : s_p[1];
    wire       rs_hi = rs_0h || rs_1h;
    wire [7:0] rs_d  = rs_0l ? s_old[0][7:0] : rs_0h ? s_old[0][15:8] : rs_1l ? s_old[1][7:0] : s_old[1][15:8];
    wire       bw_e  = (we && bw_we) || rs_we;
    wire [3:0] bw_pe = rs_we ? rs_p : bw_phys;
    wire       bw_he = rs_we ? rs_hi : bw_hi;
    wire [7:0] bw_de = rs_we ? rs_d : bw_d;

    always @* begin
        for (i = 0; i < 13; i = i + 1) rf_n[i] = rf[i];
        for (i = 0; i < 13; i = i + 1) begin
            if (we && pw_we && pw_phys == i) rf_n[i] = pw_d;
            if (bw_e && bw_pe == i) begin
                if (bw_he) rf_n[i][15:8] = bw_de;
                else       rf_n[i][7:0]  = bw_de;
            end
            if (we && f_we && af_phys == i) rf_n[i][7:0] = f_d;
        end
        if (we) begin
            if (pc_we) rf_n[`RFP_PC] = pc_d;
            if (sp_we) rf_n[`RFP_SP] = sp_d;
            if (wz_we) rf_n[`RFP_WZ] = wz_d;
        end
        if (pc_clr) rf_n[`RFP_PC] = 16'h0000;
    end
    assign pc_next = rf_n[`RFP_PC];
    assign f_next  = xaf_n ? rf_n[7][7:0] : rf_n[3][7:0];

    // ---- write-back log: two pair slots, a due per byte; the exchanges ----
    reg        s_vl [0:1], s_vh [0:1];       // byte waiting (s_p, s_old above)
    reg  [7:0] s_dl [0:1], s_dh [0:1];        // due {next, m, t}
    reg        x_af, x_bank;                  // EX AF / EXX waiting (next M1's T2 / T3)
    reg  [1:0] x_dh;                          // EX DE,HL waiting (T3), per bank
    reg        s_vl_n [0:1], s_vh_n [0:1];
    reg  [3:0] s_p_n  [0:1];
    reg [15:0] s_old_n[0:1];
    reg  [7:0] s_dl_n [0:1], s_dh_n [0:1];
    reg        x_af_n, x_bank_n; reg [1:0] x_dh_n;
    reg        s_f [0:1], s_f_n [0:1];       // the low byte waiting is F
    reg  [7:0] fc, fc_n;                      // ... and for a cut on the next M1's T2 this
    reg        r_lo_n [0:1], r_hi_n [0:1];
    reg        xbank_w, xaf_w; reg [1:0] xdh_w;
    integer    k;

    function due_now;                          // does due d fall on this edge?
        input [7:0] d; input [2:0] m; input [3:0] t; input m1;
        due_now = (d[3:0] == t) && (d[7] ? m1 : (d[6:4] == m));
    endfunction

    // log the low (ml) and / or high (mh) byte of physical pair p -- old
    // value o, dues dl / dh -- into the slots (f: the low byte is F)
    task log_pair; input [3:0] p; input ml; input mh; input [15:0] o; input [7:0] dl; input [7:0] dh; input f;
        reg done, hit;
        integer q;                             // a temporary (at module level yosys made it a latch)
        begin
            done = 1'b0;
            for (q = 0; q < 2; q = q + 1) begin
                // the slot that holds p, else a free one (not one a restore uses)
                hit = !done && (((s_vl_n[q] || s_vh_n[q]) && s_p_n[q] == p) ||
                                (!s_vl_n[q] && !s_vh_n[q] && !r_lo_n[q] && !r_hi_n[q] &&
                                 !((s_vl_n[1 - q] || s_vh_n[1 - q]) && s_p_n[1 - q] == p)));
                if (hit) begin
                    if (!s_vl_n[q] && !s_vh_n[q]) s_p_n[q] = p;
                    if (mh) begin if (!s_vh_n[q]) s_old_n[q][15:8] = o[15:8]; s_vh_n[q] = 1'b1; s_dh_n[q] = dh; end
                    if (ml) begin if (!s_vl_n[q]) s_old_n[q][7:0]  = o[7:0];  s_vl_n[q] = 1'b1; s_dl_n[q] = dl; s_f_n[q] = f; end
                    done = 1'b1;
                end
            end
        end
    endtask

    always @* begin
        for (k = 0; k < 2; k = k + 1) begin
            s_vl_n[k] = s_vl[k]; s_vh_n[k] = s_vh[k]; s_p_n[k] = s_p[k];
            s_old_n[k] = s_old[k]; s_dl_n[k] = s_dl[k]; s_dh_n[k] = s_dh[k]; s_f_n[k] = s_f[k];
        end
        // the restore byte written on this edge is done
        r_lo_n[0] = r_lo[0] && !rs_0l; r_hi_n[0] = r_hi[0] && !rs_0h;
        r_lo_n[1] = r_lo[1] && !rs_1l; r_hi_n[1] = r_hi[1] && !rs_1h;
        if (!rs_we) begin r_lo_n[0] = r_lo[0]; r_hi_n[0] = r_hi[0]; r_lo_n[1] = r_lo[1]; r_hi_n[1] = r_hi[1]; end
        x_af_n = x_af; x_bank_n = x_bank; x_dh_n = x_dh;
        fc_n = fc;
        // 1. what the die makes permanent on this edge; an instruction's end
        //    also ends what it writes within itself
        if (wb_adv || wb_held) begin
            for (k = 0; k < 2; k = k + 1) begin
                if (s_vl_n[k] && (due_now(s_dl_n[k], wb_m, wb_t, wb_m1) || (wb_tw && due_now(s_dl_n[k], wb_m, 4'd3, wb_m1)) ||
                                  (wb_fin && !s_dl_n[k][7])))
                    s_vl_n[k] = 1'b0;
                if (s_vh_n[k] && (due_now(s_dh_n[k], wb_m, wb_t, wb_m1) || (wb_tw && due_now(s_dh_n[k], wb_m, 4'd3, wb_m1)) ||
                                  (wb_fin && !s_dh_n[k][7])))
                    s_vh_n[k] = 1'b0;
            end
            if (wb_m1 && wb_t == 4'd2) x_af_n = 1'b0;
            if (wb_m1 && (wb_t == 4'd3 || wb_tw)) begin x_bank_n = 1'b0; x_dh_n = 2'b00; end
        end
        // 2. this edge's writes that the die makes later
        if (we) begin
            if (pw_we && (pw_log_lo || pw_log_hi))
                log_pair(pw_phys, pw_log_lo, pw_log_hi, pw_old, pw_due_lo, pw_due_hi, 1'b0);
            if (bw_we && bw_log) log_pair(bw_phys, !bw_hi, bw_hi, {bw_old, bw_old}, bw_due, bw_due, 1'b0);
            if (f_we && f_log) begin
                log_pair(af_phys, 1'b1, 1'b0, {8'h00, rf[af_phys][7:0]}, f_due, 8'h00, 1'b1);
                fc_n = f_cut;
            end
            if (sp_we && sp_log) log_pair(`RFP_SP, 1'b1, 1'b1, rf[`RFP_SP], sp_due, sp_due, 1'b0);
        end
        if (ex_en) begin
            if (ex_af)   x_af_n   = ~x_af_n;
            if (exx)     x_bank_n = ~x_bank_n;
            if (ex_dehl) x_dh_n   = x_dh_n ^ (xbank ? 2'b10 : 2'b01);
        end
        // 3. a RESET cut: put back what the die hadn't written -- the bytes
        //    waiting become restores, made on the following edges (above);
        //    F after a cut on the next M1's T2 as the die's flag latches hold it
        xbank_w = xbank_n; xaf_w = xaf_n; xdh_w = xdh_n;
        if (wb_cut) begin
            for (k = 0; k < 2; k = k + 1) begin
                if (wb_t2 && s_vl_n[k] && s_dl_n[k][7] && s_f_n[k]) s_old_n[k][7:0] = fc_n;
                r_lo_n[k] = r_lo_n[k] || s_vl_n[k]; r_hi_n[k] = r_hi_n[k] || s_vh_n[k];
                s_vl_n[k] = 1'b0; s_vh_n[k] = 1'b0;
            end
            xbank_w = xbank_n ^ x_bank_n; xaf_w = xaf_n ^ x_af_n; xdh_w = xdh_n ^ x_dh_n;
            x_af_n = 1'b0; x_bank_n = 1'b0; x_dh_n = 2'b00;
        end
    end

    // 0x5555 matches perfectz80's gate-level Visual-Z80 netlist boot
    // pattern (see cmodel/z80_core.c z80_reset()); PC resets to 0.
    always @(posedge clk or negedge por_n) begin
        if (!por_n) begin
            for (ri = 0; ri < 13; ri = ri + 1) rf[ri] <= 16'h5555;
            rf[`RFP_PC] <= 16'h0000;
            xbank <= 1'b0; xaf <= 1'b0; xdh <= 2'b00;
            for (ri = 0; ri < 2; ri = ri + 1) begin
                s_vl[ri] <= 1'b0; s_vh[ri] <= 1'b0; s_p[ri] <= 4'd0; s_f[ri] <= 1'b0;
                r_lo[ri] <= 1'b0; r_hi[ri] <= 1'b0;
                s_old[ri] <= 16'h0000; s_dl[ri] <= 8'h00; s_dh[ri] <= 8'h00;
            end
            x_af <= 1'b0; x_bank <= 1'b0; x_dh <= 2'b00;
            fc <= 8'h00;
        end else begin
            for (ri = 0; ri < 13; ri = ri + 1) rf[ri] <= rf_n[ri];
            xbank <= xbank_w; xaf <= xaf_w; xdh <= xdh_w;
            for (ri = 0; ri < 2; ri = ri + 1) begin
                s_vl[ri] <= s_vl_n[ri]; s_vh[ri] <= s_vh_n[ri]; s_p[ri] <= s_p_n[ri]; s_f[ri] <= s_f_n[ri];
                r_lo[ri] <= r_lo_n[ri]; r_hi[ri] <= r_hi_n[ri];
                s_old[ri] <= s_old_n[ri]; s_dl[ri] <= s_dl_n[ri]; s_dh[ri] <= s_dh_n[ri];
            end
            x_af <= x_af_n; x_bank <= x_bank_n; x_dh <= x_dh_n;
            fc <= fc_n;
        end
    end
endmodule
