// ===========================================================================
// z80_seq.v - per-(M, T) micro-sequencer (the "random control logic" block).
//
// Mirrors the structure of Baltazar Studios' Z80 Explorer exec_matrix.vh,
// except we key the dispatch on our own `exec_w` enum (from z80_pla) rather
// than the die's `pla[N]` bits — the structural pattern is identical:
//
//     case (eff_exec)
//         `EXEC_NOP: begin
//             if (M1 & T4) begin fin = 1'b1; end
//         end
//         `EXEC_LD_R_R: begin
//             if (M1 & T4) begin
//                 ctl_reg_gp_we = 1; ctl_reg_gp_sel = rf_dst_w[2:0];
//                 ctl_reg_in_hi = ...; ctl_reg_in_lo = ...;
//                 fin = 1;
//             end
//         end
//         ...
//     endcase
//
// Pure combinational — runs at every phase. Outputs the ctl_* signal bundle
// that the datapath blocks (z80_regfile, z80_alu, z80_idu, bus muxes,
// address latch) consume. Signal vocabulary verbatim from
// docs/silicon-microarch.md.
//
// MIGRATION STATUS (Phase 6 of c-like-verilog refactor):
// This module currently implements only the trivial EXEC_NOP / EXEC_HALT
// families — the rest of the case-dispatch still lives in z80_core.v's
// always block and runs in parallel. As more EXEC_* families are
// migrated, the corresponding branches will be removed from z80_core.v
// and z80_seq.v will become the sole producer of per-(M, T) control
// signals.
//
// The output `seq_active` is high when the new sequencer has emitted
// signals for the current cycle — the old dispatch is gated off (no-op)
// for that branch.
// ===========================================================================
`include "z80_defs.vh"

module z80_seq (
    // ---- timing inputs ----
    input  wire [6:0]   eff_exec,    // dispatch selector (= exec_w + interrupt override)
    input  wire [2:0]   m_cycle,     // 1-based M-cycle counter
    input  wire [3:0]   t_state,     // 1-based T-state counter
    input  wire         phi,         // 0 = PHI_P, 1 = PHI_N

    // ---- PLA pass-through used by a few cases (alu_op selects CP) ----
    input  wire [2:0]   alu_op_w,

    // ---- "do anything?" output for cross-checking against the legacy
    //      case dispatch in z80_core.v during migration. When the migrated
    //      set covers an opcode, seq_active[<exec>] is 1 and core skips
    //      the legacy branch. ----
    output reg          seq_active,

    // ---- control signals (drive the legacy datapath unchanged for now) ----
    //      The ctl_* bundle grows as more opcode families migrate.
    output reg          fin,
    output reg  [1:0]   ctl_iff_op,        // IFF_NONE / _CLEAR / _SET / _RETN
    output reg          ctl_ei_delay_set,
    output reg          ctl_im_we,         // load IM register from aux_w
    output reg          ctl_halt_set,      // set halted flip-flop
    output reg          ctl_pc_dec1,       // PC = PC - 1 (HALT back-up)
    output reg          ctl_reg_ex_de_hl,  // swap DE <-> HL
    output reg          ctl_reg_ex_af,     // swap AF <-> AF'
    output reg          ctl_reg_exx,       // swap BC/DE/HL <-> primes
    output reg          ctl_pc_set_hl,     // PC = rf[hlp]  (JP HL / JP IX / JP IY)
    output reg          ctl_reg_a_we,      // write A from alu_res
    output reg          ctl_reg_f_we,      // write F from alu_fout
    output reg          ctl_reg_setri_we   // setri(rf_dst_w, alu_res)
);

    // Convenience M/T equality wires keep the case branches readable.
    wire M1 = (m_cycle == 3'd1);
    wire M2 = (m_cycle == 3'd2);  // (unused until non-trivial opcodes migrate)
    wire T1 = (t_state == 4'd1);
    wire T2 = (t_state == 4'd2);  // (unused until non-trivial opcodes migrate)
    wire T3 = (t_state == 4'd3);  // (unused until non-trivial opcodes migrate)
    wire T4 = (t_state == 4'd4);
    /* verilator lint_off UNUSEDSIGNAL */
    wire phi_unused = phi;
    wire m2_unused  = M2;
    wire t2_unused  = T2;
    wire t3_unused  = T3;
    /* verilator lint_on UNUSEDSIGNAL */

    always @* begin
        seq_active       = 1'b0;
        fin              = 1'b0;
        ctl_iff_op       = `IFF_NONE;
        ctl_ei_delay_set = 1'b0;
        ctl_im_we        = 1'b0;
        ctl_halt_set     = 1'b0;
        ctl_pc_dec1      = 1'b0;
        ctl_reg_ex_de_hl = 1'b0;
        ctl_reg_ex_af    = 1'b0;
        ctl_reg_exx      = 1'b0;
        ctl_pc_set_hl    = 1'b0;
        ctl_reg_a_we     = 1'b0;
        ctl_reg_f_we     = 1'b0;
        ctl_reg_setri_we = 1'b0;

        case (eff_exec)
        `EXEC_NOP, `EXEC_ILLEGAL: begin
            seq_active = 1'b1;
            // NOP finishes at the end of M1 (T4) — the M1 fetch already
            // happened, and there's nothing else to do.
            if (M1 & T4) fin = 1'b1;
        end

        `EXEC_DI: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_iff_op = `IFF_CLEAR;
                fin = 1'b1;
            end
        end

        `EXEC_EI: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_iff_op       = `IFF_SET;
                ctl_ei_delay_set = 1'b1;
                fin              = 1'b1;
            end
        end

        `EXEC_IM: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_im_we = 1'b1;             // IM val taken from aux_w in core
                fin = 1'b1;
            end
        end

        `EXEC_HALT: begin
            seq_active = 1'b1;
            // PC was already incremented by the M1 fetch. Back it up so
            // external observers see PC pointing at the HALT opcode; the
            // next M1 will re-fetch (suppressed-decode) until an interrupt
            // wakes us.
            if (M1 & T4) begin
                ctl_halt_set = 1'b1;
                ctl_pc_dec1  = 1'b1;
                fin          = 1'b1;
            end
        end

        `EXEC_EX_DE_HL: begin
            seq_active = 1'b1;
            if (M1 & T4) begin ctl_reg_ex_de_hl = 1'b1; fin = 1'b1; end
        end

        `EXEC_EX_AF: begin
            seq_active = 1'b1;
            if (M1 & T4) begin ctl_reg_ex_af = 1'b1; fin = 1'b1; end
        end

        `EXEC_EXX: begin
            seq_active = 1'b1;
            if (M1 & T4) begin ctl_reg_exx = 1'b1; fin = 1'b1; end
        end

        `EXEC_JP_HL: begin
            seq_active = 1'b1;
            // JP (HL) / JP (IX) / JP (IY): PC = rf[hlp]. The hlp index is
            // resolved in core (depends on idx_w from the PLA).
            if (M1 & T4) begin ctl_pc_set_hl = 1'b1; fin = 1'b1; end
        end

        // ---- ALU instructions that write A and F (or just F) ----
        // The ALU operand mux + z80_alu instance live in core; here we
        // just decide when to commit alu_res to A and alu_fout to F.
        `EXEC_ROT_A, `EXEC_DAA, `EXEC_CPL: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_reg_a_we = 1'b1;
                ctl_reg_f_we = 1'b1;
                fin          = 1'b1;
            end
        end
        `EXEC_SCF, `EXEC_CCF: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_reg_f_we = 1'b1;   /* A unchanged */
                fin          = 1'b1;
            end
        end
        `EXEC_ALU_R: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                // CP doesn't write A back; everything else does.
                ctl_reg_a_we = (alu_op_w != `ALU_CP);
                ctl_reg_f_we = 1'b1;
                fin          = 1'b1;
            end
        end

        // INC r / DEC r — alu_b = getri(rf_dst_w), alu_mode = INC8/DEC8;
        // result back to rf_dst via setri, flags to F.
        `EXEC_INC_R, `EXEC_DEC_R: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_reg_setri_we = 1'b1;
                ctl_reg_f_we     = 1'b1;
                fin              = 1'b1;
            end
        end

        default: seq_active = 1'b0;
        endcase
    end

endmodule
