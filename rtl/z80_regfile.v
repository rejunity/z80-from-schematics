// ===========================================================================
// z80_regfile.v - 13 × 16-bit register file + I, R, IR.
//
// Mirrors the GP/system register block visible on the Z80 die, using the
// signal vocabulary from Baltazar Studios' Z80 Explorer (exec_matrix.vh —
// see docs/silicon-microarch.md).
//
// Two read paths, one for each internal bus:
//   - GP byte read  → db2 (8-bit): selects one of the 8 byte slots
//     (B C D E H L (HL)=00 A) inside the chosen pair, controlled by
//     ctl_reg_gp_sel + ctl_reg_gp_hilo + ctl_reg_out_hi / _out_lo.
//   - System 16-bit read → sb (16-bit): selects PC, SP, WZ, or IR
//     (for I/R) via ctl_reg_sel_pc / _wz / _ir / _use_sp.
//
// Two write paths:
//   - GP write from db2 (via ctl_reg_in_hi / _in_lo + _gp_we).
//   - System write from sb (via ctl_reg_sys_we / _we_hi / _we_lo).
//
// Plus three pair-swap triggers (EX AF,AF' / EX DE,HL / EXX) and an
// IR latch for the instruction register.
//
// NOTE: This module is added in Phase 3 of the silicon-faithful refactor
// but NOT YET INSTANTIATED. The current z80_core.v still uses its inline
// rf[0:12]. Phase 6 (per-(M, T) sequencer rewrite) will instantiate this
// module and cut the core over to it. The file's purpose at this phase is
// to lock the port surface so the case-dispatch migration in Phase 6 can
// proceed module-by-module instead of in one mass change.
// ===========================================================================
`include "z80_defs.vh"

module z80_regfile (
    input  wire        clk,
    input  wire        reset_n,

    // ---- GP path (db2, 8-bit) ----
    input  wire [3:0]  ctl_reg_gp_sel,    // pair index 0..12 (RFP_*)
    input  wire [1:0]  ctl_reg_gp_hilo,   // 01 = low byte, 10 = high byte, 11 = both
    input  wire        ctl_reg_gp_we,
    input  wire        ctl_reg_in_hi,     // byte source: write the high byte of selected pair
    input  wire        ctl_reg_in_lo,     // byte source: write the low byte
    input  wire        ctl_reg_out_hi,    // read enable for high byte to db_out
    input  wire        ctl_reg_out_lo,    // read enable for low byte to db_out
    input  wire [7:0]  db_in,             // write data from db2
    output reg  [7:0]  db_out,            // read data to db2

    // ---- System 16-bit path (sb, 16-bit) ----
    input  wire        ctl_reg_sel_pc,
    input  wire        ctl_reg_sel_wz,
    input  wire        ctl_reg_sel_ir,    // IR latch (for I,R / interrupt vector)
    input  wire        ctl_reg_use_sp,
    input  wire [1:0]  ctl_reg_sys_hilo,
    input  wire        ctl_reg_sys_we,
    input  wire        ctl_reg_sys_we_hi,
    input  wire        ctl_reg_sys_we_lo,
    input  wire [15:0] sb_in,
    output reg  [15:0] sb_out,

    // ---- Pair-swap triggers ----
    input  wire        ctl_reg_ex_af,     // EX AF,AF'
    input  wire        ctl_reg_ex_de_hl,  // EX DE,HL
    input  wire        ctl_reg_exx,       // EXX (swap BC/DE/HL with primes)

    // ---- IR latch ----
    input  wire        ctl_ir_we,
    input  wire [7:0]  ir_in,
    output wire [7:0]  ir_out,

    // ---- Observability — exposes all pairs (used by traceport / debug) ----
    output wire [15:0] rf_bc, rf_de, rf_hl, rf_af,
    output wire [15:0] rf_bc2, rf_de2, rf_hl2, rf_af2,
    output wire [15:0] rf_ix, rf_iy, rf_sp, rf_pc, rf_wz,
    output wire [7:0]  reg_i, reg_r
);

    reg [15:0] rf [0:12];
    reg [7:0]  reg_i_r, reg_r_r;
    reg [7:0]  ir_r;
    integer    i;

    // observability fanout
    assign rf_bc  = rf[`RFP_BC];  assign rf_de  = rf[`RFP_DE];
    assign rf_hl  = rf[`RFP_HL];  assign rf_af  = rf[`RFP_AF];
    assign rf_bc2 = rf[`RFP_BC2]; assign rf_de2 = rf[`RFP_DE2];
    assign rf_hl2 = rf[`RFP_HL2]; assign rf_af2 = rf[`RFP_AF2];
    assign rf_ix  = rf[`RFP_IX];  assign rf_iy  = rf[`RFP_IY];
    assign rf_sp  = rf[`RFP_SP];  assign rf_pc  = rf[`RFP_PC];
    assign rf_wz  = rf[`RFP_WZ];
    assign reg_i  = reg_i_r;      assign reg_r  = reg_r_r;
    assign ir_out = ir_r;

    // ---- GP byte read combinational ----
    always @* begin
        if (ctl_reg_out_hi)      db_out = rf[ctl_reg_gp_sel][15:8];
        else if (ctl_reg_out_lo) db_out = rf[ctl_reg_gp_sel][7:0];
        else                     db_out = 8'h00;
    end

    // ---- System 16-bit read combinational ----
    always @* begin
        if      (ctl_reg_sel_pc) sb_out = rf[`RFP_PC];
        else if (ctl_reg_sel_wz) sb_out = rf[`RFP_WZ];
        else if (ctl_reg_use_sp) sb_out = rf[`RFP_SP];
        else if (ctl_reg_sel_ir) sb_out = {reg_i_r, reg_r_r};
        else                     sb_out = 16'h0000;
    end

    // ---- Clocked writes + swaps + reset ----
    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            for (i = 0; i < 13; i = i + 1) rf[i] <= 16'hFFFF;
            rf[`RFP_PC] <= 16'h0000;
            reg_i_r <= 8'h00;
            reg_r_r <= 8'h00;
            ir_r    <= 8'h00;
        end else begin
            // GP byte write
            if (ctl_reg_gp_we) begin
                if (ctl_reg_in_hi) rf[ctl_reg_gp_sel][15:8] <= db_in;
                if (ctl_reg_in_lo) rf[ctl_reg_gp_sel][7:0]  <= db_in;
            end

            // System reg write
            if (ctl_reg_sys_we_hi || (ctl_reg_sys_we && ctl_reg_sys_hilo[1])) begin
                if (ctl_reg_sel_pc) rf[`RFP_PC][15:8] <= sb_in[15:8];
                if (ctl_reg_sel_wz) rf[`RFP_WZ][15:8] <= sb_in[15:8];
                if (ctl_reg_use_sp) rf[`RFP_SP][15:8] <= sb_in[15:8];
            end
            if (ctl_reg_sys_we_lo || (ctl_reg_sys_we && ctl_reg_sys_hilo[0])) begin
                if (ctl_reg_sel_pc) rf[`RFP_PC][7:0] <= sb_in[7:0];
                if (ctl_reg_sel_wz) rf[`RFP_WZ][7:0] <= sb_in[7:0];
                if (ctl_reg_use_sp) rf[`RFP_SP][7:0] <= sb_in[7:0];
            end

            // Pair swaps
            if (ctl_reg_ex_af) begin
                rf[`RFP_AF]  <= rf[`RFP_AF2];
                rf[`RFP_AF2] <= rf[`RFP_AF];
            end
            if (ctl_reg_ex_de_hl) begin
                rf[`RFP_DE] <= rf[`RFP_HL];
                rf[`RFP_HL] <= rf[`RFP_DE];
            end
            if (ctl_reg_exx) begin
                rf[`RFP_BC]  <= rf[`RFP_BC2]; rf[`RFP_BC2] <= rf[`RFP_BC];
                rf[`RFP_DE]  <= rf[`RFP_DE2]; rf[`RFP_DE2] <= rf[`RFP_DE];
                rf[`RFP_HL]  <= rf[`RFP_HL2]; rf[`RFP_HL2] <= rf[`RFP_HL];
            end

            // IR latch
            if (ctl_ir_we) ir_r <= ir_in;
        end
    end

endmodule
