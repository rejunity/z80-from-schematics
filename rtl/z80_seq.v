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
    output reg          ctl_reg_setri_we,  // setri(rf_dst_w, <src>)
    output reg  [1:0]   ctl_reg_setri_src, // SETRI_SRC_*

    // ---- M-cycle scheduling (multi-M-cycle ops) ----
    output reg          ctl_start_mc,      // start a new M-cycle this T
    output reg  [2:0]   ctl_mc_bus_op,     // BUSOP_*
    output reg  [3:0]   ctl_mc_addr_src,   // ADDR_*
    output reg  [2:0]   ctl_mc_wdata_src,  // WDATA_*
    output reg  [3:0]   ctl_mc_extra_t,    // extra wait T-states
    output reg          ctl_pc_inc,        // PC = PC + 1 (post-fetch IDU)

    // ---- multi-byte assembly (tmpl/tmph/tmp16 latches) ----
    output reg          ctl_tmpl_we,       // tmpl = rbyte
    output reg          ctl_tmph_we,       // tmph = rbyte
    output reg          ctl_tmp16_we,      // tmp16 = {rbyte, tmpl}

    // ---- PC / rp updates from {rbyte, tmpl}; WZ via ctl_wz_op enum ----
    output reg          ctl_pc_set_nn,     // PC = {rbyte, tmpl}  (gated by use_cc)
    output reg          ctl_rp_set_nn,     // rf[rp_sel_w] = {rbyte, tmpl}
    output reg          ctl_use_cc,        // if 1, gate ctl_pc_set_nn on cc_true(F, cc_w)

    // ---- IDU-style rp +/- 1 (16-bit incrementer on the address path) ----
    output reg          ctl_rp_inc,        // rf[rp_sel_w] = rf[rp_sel_w] + 1
    output reg          ctl_rp_dec,        // rf[rp_sel_w] = rf[rp_sel_w] - 1
    output reg          ctl_sp_set_hl,     // SP = rf[hlp]  (LD SP,HL)

    // ---- A register write with source select ----
    output reg          ctl_reg_a_src_rbyte, // when set with ctl_reg_a_we, source = rbyte

    // ---- WZ (MEMPTR) update with enum (supplants ctl_wz_set_nn) ----
    output reg  [2:0]   ctl_wz_op          // WZ_*
);

    // Convenience M/T equality wires keep the case branches readable.
    wire M1 = (m_cycle == 3'd1);
    wire M2 = (m_cycle == 3'd2);
    wire M3 = (m_cycle == 3'd3);
    wire M4 = (m_cycle == 3'd4);
    wire M5 = (m_cycle == 3'd5);
    wire T1 = (t_state == 4'd1);
    wire T2 = (t_state == 4'd2);
    wire T3 = (t_state == 4'd3);
    wire T4 = (t_state == 4'd4);
    /* verilator lint_off UNUSEDSIGNAL */
    wire phi_unused = phi;
    wire t1_unused  = T1;
    wire t2_unused  = T2;
    wire m4_unused  = M4;
    wire m5_unused  = M5;
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
        ctl_reg_setri_we  = 1'b0;
        ctl_reg_setri_src = `SETRI_SRC_ALU_RES;
        ctl_start_mc      = 1'b0;
        ctl_mc_bus_op     = `BUSOP_NONE;
        ctl_mc_addr_src   = `ADDR_PC;
        ctl_mc_wdata_src  = `WDATA_ZERO;
        ctl_mc_extra_t    = 4'd0;
        ctl_pc_inc        = 1'b0;
        ctl_tmpl_we       = 1'b0;
        ctl_tmph_we       = 1'b0;
        ctl_tmp16_we      = 1'b0;
        ctl_pc_set_nn     = 1'b0;
        ctl_rp_set_nn     = 1'b0;
        ctl_use_cc        = 1'b0;
        ctl_rp_inc        = 1'b0;
        ctl_rp_dec        = 1'b0;
        ctl_sp_set_hl     = 1'b0;
        ctl_reg_a_src_rbyte = 1'b0;
        ctl_wz_op         = `WZ_NONE;

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
                ctl_reg_setri_we = 1'b1;       /* source = alu_res    */
                ctl_reg_f_we     = 1'b1;
                fin              = 1'b1;
            end
        end

        // LD r, r' — copy rf_src_w byte to rf_dst_w byte. No ALU usage.
        `EXEC_LD_R_R: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_reg_setri_we  = 1'b1;
                ctl_reg_setri_src = `SETRI_SRC_GETRI_SRC;
                fin               = 1'b1;
            end
        end

        // LD r, n — two M-cycles: M1 fetches the opcode (already done by
        // the time we dispatch here); M2 reads the immediate byte from PC
        // and writes it to rf_dst.
        `EXEC_LD_R_N: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_start_mc    = 1'b1;
                ctl_mc_bus_op   = `BUSOP_MRD;
                ctl_mc_addr_src = `ADDR_PC;
                ctl_pc_inc      = 1'b1;
            end
            if (M2 & T3) begin
                ctl_reg_setri_we  = 1'b1;
                ctl_reg_setri_src = `SETRI_SRC_RBYTE;
                fin               = 1'b1;
            end
        end

        // ALU op with immediate byte — M1 fetch, M2 fetches the byte and
        // the ALU fires with alu_b = rbyte (mux in core). At M2.T3, write
        // A (unless CP) and F.
        `EXEC_ALU_N: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_start_mc    = 1'b1;
                ctl_mc_bus_op   = `BUSOP_MRD;
                ctl_mc_addr_src = `ADDR_PC;
                ctl_pc_inc      = 1'b1;
            end
            if (M2 & T3) begin
                ctl_reg_a_we = (alu_op_w != `ALU_CP);
                ctl_reg_f_we = 1'b1;
                fin          = 1'b1;
            end
        end

        // LD A,(BC) / LD A,(DE) — M1 fetch, M2 reads byte from rp into A;
        //   WZ = rp + 1.
        `EXEC_LD_A_RP: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_start_mc    = 1'b1;
                ctl_mc_bus_op   = `BUSOP_MRD;
                ctl_mc_addr_src = `ADDR_RP;
                ctl_wz_op       = `WZ_RP_INC;
            end
            if (M2 & T3) begin
                ctl_reg_a_we        = 1'b1;
                ctl_reg_a_src_rbyte = 1'b1;
                fin                 = 1'b1;
            end
        end

        // LD (BC),A / LD (DE),A — M1 fetch, M2 writes A to rp; specific WZ
        //   formula = {A_cur, (rp_lo + 1) & 0xFF}.
        `EXEC_LD_RP_A: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_start_mc      = 1'b1;
                ctl_mc_bus_op     = `BUSOP_MWR;
                ctl_mc_addr_src   = `ADDR_RP;
                ctl_mc_wdata_src  = `WDATA_A;
                ctl_wz_op         = `WZ_A_RP_INC;
            end
            if (M2 & T3) fin = 1'b1;
        end

        // INC rp / DEC rp / LD SP,HL — single internal cycle after the M1
        // fetch (2T of "incrementer compute"). The register update happens
        // at M1.T4 dispatch; M2 is the internal cycle, finishing at M2.T2.
        `EXEC_INC_RP: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_rp_inc      = 1'b1;
                ctl_start_mc    = 1'b1;
                ctl_mc_bus_op   = `BUSOP_INTERNAL;
                ctl_mc_addr_src = `ADDR_RP_INC;
                ctl_mc_extra_t  = 4'd2;
            end
            if (M2 & T2) fin = 1'b1;
        end
        `EXEC_DEC_RP: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_rp_dec      = 1'b1;
                ctl_start_mc    = 1'b1;
                ctl_mc_bus_op   = `BUSOP_INTERNAL;
                ctl_mc_addr_src = `ADDR_RP_DEC;
                ctl_mc_extra_t  = 4'd2;
            end
            if (M2 & T2) fin = 1'b1;
        end
        `EXEC_LD_SP_HL: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_sp_set_hl   = 1'b1;
                ctl_start_mc    = 1'b1;
                ctl_mc_bus_op   = `BUSOP_INTERNAL;
                ctl_mc_addr_src = `ADDR_HL;     /* HL or IX/IY per idx_w */
                ctl_mc_extra_t  = 4'd2;
            end
            if (M2 & T2) fin = 1'b1;
        end

        // LD rp,nn — same fetch pattern as JP but writes rp instead of PC.
        // No WZ side effect, no condition.
        `EXEC_LD_RP_NN: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_start_mc    = 1'b1;
                ctl_mc_bus_op   = `BUSOP_MRD;
                ctl_mc_addr_src = `ADDR_PC;
                ctl_pc_inc      = 1'b1;
            end
            if (M2 & T3) begin
                ctl_tmpl_we     = 1'b1;
                ctl_start_mc    = 1'b1;
                ctl_mc_bus_op   = `BUSOP_MRD;
                ctl_mc_addr_src = `ADDR_PC;
                ctl_pc_inc      = 1'b1;
            end
            if (M3 & T3) begin
                ctl_rp_set_nn = 1'b1;
                fin           = 1'b1;
            end
        end

        // JP nn / JP cc,nn — three M-cycles: M1 fetch opcode, M2 read low
        // byte of nn into tmpl, M3 read high byte (rbyte) and assemble
        // PC and WZ. JP_CC gates the PC update on cc_true; WZ is set
        // unconditionally.
        `EXEC_JP, `EXEC_JP_CC: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_start_mc    = 1'b1;
                ctl_mc_bus_op   = `BUSOP_MRD;
                ctl_mc_addr_src = `ADDR_PC;
                ctl_pc_inc      = 1'b1;
            end
            if (M2 & T3) begin
                ctl_tmpl_we     = 1'b1;
                ctl_start_mc    = 1'b1;
                ctl_mc_bus_op   = `BUSOP_MRD;
                ctl_mc_addr_src = `ADDR_PC;
                ctl_pc_inc      = 1'b1;
            end
            if (M3 & T3) begin
                ctl_wz_op     = `WZ_SET_NN;
                ctl_pc_set_nn = 1'b1;
                ctl_use_cc    = (eff_exec == `EXEC_JP_CC);
                fin           = 1'b1;
            end
        end

        // NEG — alu_a = A, alu_b = 0, alu_md = FLAG_NEG (set by PLA), result
        // back to A, flags to F.
        `EXEC_NEG: begin
            seq_active = 1'b1;
            if (M1 & T4) begin
                ctl_reg_a_we = 1'b1;
                ctl_reg_f_we = 1'b1;
                fin          = 1'b1;
            end
        end

        default: seq_active = 1'b0;
        endcase
    end

endmodule
