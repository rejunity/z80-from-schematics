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

    // ---- "do anything?" output for cross-checking against the legacy
    //      case dispatch in z80_core.v during migration. When the migrated
    //      set covers an opcode, seq_active[<exec>] is 1 and core skips
    //      the legacy branch. ----
    output reg          seq_active,

    // ---- control signals (drive the legacy datapath unchanged for now) ----
    //      Only `fin` is wired for the initial NOP / HALT migration.
    //      The rest of the ctl_* bundle (regfile/ALU/IDU/bus) gets
    //      added as more opcode families migrate. ----
    output reg          fin
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
        seq_active = 1'b0;
        fin        = 1'b0;

        case (eff_exec)
        `EXEC_NOP, `EXEC_ILLEGAL: begin
            seq_active = 1'b1;
            // NOP finishes at the end of M1 (T4) — the M1 fetch already
            // happened, and there's nothing else to do.
            if (M1 & T4) fin = 1'b1;
        end

        `EXEC_HALT: begin
            // HALT is *almost* trivial — z80_core.v also has to back up PC
            // by 1 so external observers see PC at the HALT opcode. That
            // bit stays in the legacy dispatch for now (HALT touches PC,
            // and the PC datapath wiring will land with later phases).
            seq_active = 1'b0;
        end

        default: seq_active = 1'b0;
        endcase
    end

endmodule
