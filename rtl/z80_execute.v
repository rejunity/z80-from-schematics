// ===========================================================================
// SPDX-FileCopyrightText: 2026 ReJ aka Renaldas Zioma
// SPDX-License-Identifier: CERN-OHL-W-2.0
// Source Location: https://github.com/rejunity/z80-from-schematics
// See HARDWARE_NOTICE.md for the section 4.1 Product notice.
//
// z80_execute.v - the execute block: the Z80's random control logic.
//
// Given where the sequencer is (instruction class from the PLA, M-cycle,
// T-state) and a few conditions from the datapath, produce the control
// word for this rising edge. Pure combinational; narrow select signals
// only -- no data passes through here (cf. Z80 Explorer's exec_matrix:
// one row per instruction class, one column per (M, T)).
//
// On the last T-state of an M-cycle (m_end) the word describes the
// execute step: which bus cycle starts next, where its address comes
// from, what the incrementer and the write ports do, whether the
// instruction is done. On earlier T-states only the incrementer's jobs
// that the die does while the address latch holds are issued (PC+1 of the
// opcode fetch, the block-move pointer and counter steps).
// ===========================================================================
`include "z80_defs.vh"

module z80_execute (
    // where the sequencer is
    input  wire        m_end,           // last T-state of the M-cycle
    input  wire        latch_pos,       // opcode / INTA vector latch edge
    input  wire [2:0]  bus_op,
    input  wire        suppress_decode, // HALT / interrupt M1: no PC+1
    input  wire [2:0]  m_cycle,
    input  wire [3:0]  t_state,
    input  wire [1:0]  irq_seq,         // 0 none, 1 NMI, 2 INT, 3 HALT-nop
    // decode
    input  wire [6:0]  eff_exec,        // instruction class (interrupt sequences override)
    input  wire [6:0]  exec_w,          // instruction class from the PLA
    input  wire [2:0]  alu_op_w,
    input  wire [1:0]  cb_kind_w,
    input  wire [1:0]  idx_w,
    input  wire        use_disp_w,
    input  wire [3:0]  aux_w,
    input  wire [2:0]  prefix,
    input  wire [7:0]  ir,
    input  wire [1:0]  ddcb_kind,       // DDCB opcode class (tmph[7:6])
    input  wire [1:0]  im,
    input  wire        im0_ins,         // IM0: the device's instruction; PC doesn't move
    // conditions from the datapath
    input  wire        cc_taken,        // cc[y] holds in F
    input  wire        zf,              // CPI's Z at M3: A == the byte in tmp8
    input  wire        bc_nz,           // BC != 0
    input  wire        b_nz,            // B != 0
    input  wire        b_dec_nz,        // B-1 != 0
    input  wire        tw_fix,          // RESET cuts a Tw after T2: the die's T2 write stands
    // control word
    output reg         ctl_mstart,   // start the next M-cycle
    output reg         ctl_m1,   // fetch the next opcode of a prefixed instruction
    output reg         ctl_fin,   // instruction done -> boundary
    output reg  [2:0]  ctl_bop,   //   bus cycle type of the next M-cycle
    output reg  [3:0]  ctl_mx,   //   extra T-states on top of its base length
    output reg  [2:0]  ctl_al,   //   address latch input
    output reg  [2:0]  ctl_ab,   // address-side bus source / pair
    output reg  [2:0]  ctl_ri,   // address-side bus source / pair
    output reg  [1:0]  ctl_inc,   // incrementer operation
    output reg         ctl_ea_pc,   // EA base: PC (else HL/IX/IY)
    output reg  [1:0]  ctl_ea_d,   // EA displacement
    output reg  [2:0]  ctl_db,   // write data
    output reg  [1:0]  ctl_bi,   //   byte read pair for DB_REG
    output reg         ctl_bhi,   //   ... high byte
    output reg  [2:0]  ctl_pc,   // PC / WZ loads
    output reg  [2:0]  ctl_wz,   // PC / WZ loads
    output reg  [1:0]  ctl_wzm,  //   the bytes it writes: [0] Z, [1] W
    output reg         ctl_wzc,  //   a byte read in its T3.N: a RESET cut ending here blocks it
    output reg  [2:0]  ctl_wzp,  // WZ bytes the die writes inside the next M-cycle (wzp)
    output reg  [2:0]  ctl_wzkz, //   the T-state in whose low half Z lands (0: none)
    output reg  [2:0]  ctl_wzkw, //   ... W
    output reg  [1:0]  ctl_sp,   // SP load
    output reg  [1:0]  ctl_vec,   // vector for PC_VEC / WZ_VEC
    output reg         ctl_pw,   // pair write port
    output reg  [2:0]  ctl_pwi,   //   pair
    output reg  [1:0]  ctl_pws,   //   source
    output reg         ctl_bw,   // byte write port
    output reg  [3:0]  ctl_bwd,   //   destination
    output reg  [2:0]  ctl_bws,   //   source
    output reg         ctl_fw,   // flag port
    output reg  [2:0]  ctl_fs,   //   source
    output reg         ctl_ex_dehl,
    output reg         ctl_ex_af,
    output reg         ctl_exx,
    output reg         ctl_tmph,
    output reg  [1:0]  ctl_iff,
    output reg         ctl_im,
    output reg         ctl_halt,
    output reg         ctl_ld_i,
    output reg         ctl_ld_r,
    output reg         ctl_prefix,
    output reg         ctl_cbx,   // bit-op unit: DDCB opcode fields / register operand
    output reg         ctl_cbr,   // bit-op unit: DDCB opcode fields / register operand
    output reg         ctl_pw_late,  // the pair write is the die's in this T-state's low half:
                                     // a RESET cut ending on it blocks it
    output reg  [2:0]  pfx_next         // next prefix state of a prefix opcode
);
`include "z80_ctl.vh"

    reg [2:0]  mm;   // M-cycle of the operation after the (IX+d) preamble
    reg [2:0]  mem_ri;   // memory operand pointer: HL/IX/IY or WZ
    reg        mem_ea;   //   ... or IX+d straight from the EA adder

    // next prefix state of a prefix opcode
    always @* begin
        case (ir)
            8'hDD:   pfx_next = `PFX_DD;
            8'hFD:   pfx_next = `PFX_FD;
            8'hED:   pfx_next = `PFX_ED;
            8'hCB:   pfx_next = (prefix == `PFX_DD) ? `PFX_DDCB :
                                (prefix == `PFX_FD) ? `PFX_FDCB : `PFX_CB;
            default: pfx_next = prefix;
        endcase
    end

    // ---- micro-operation groups ----
    // start the next M-cycle: bus cycle, address latch input, extra T-states
    task mstart; input [2:0] bop; input [2:0] al; input [3:0] extra;
        begin ctl_mstart = 1'b1; ctl_bop = bop; ctl_al = al; ctl_mx = extra; end
    endtask
    // read the next operand byte: MRD (PC), PC <- PC+1 (in IM0 the device
    // supplies it at PC, which stays: perfectz80)
    task op_fetch; input [3:0] extra;
        begin
            ctl_ab = AB_REG; ctl_ri = RI_PC; ctl_inc = INC_P1; ctl_pc = im0_ins ? PC_HOLD : PC_IDU;
            mstart(`BUSOP_MRD, AL_ABUS, extra);
        end
    endtask
    // a bus cycle at a register pair
    task at_pair; input [2:0] bop; input [2:0] rsel; input [3:0] extra;
        begin ctl_ab = AB_REG; ctl_ri = rsel; mstart(bop, AL_ABUS, extra); end
    endtask
    // a bus cycle at the memory operand: (HL), (IX+d) or (WZ)
    task at_mem; input [2:0] bop; input [3:0] extra;
        begin
            if (mem_ea) begin ctl_ea_pc = 1'b0; ctl_ea_d = EA_RB; mstart(bop, AL_EA, extra); end
            else if (mem_ri == RI_WZ) mstart(bop, AL_WZP, extra);   // IX+d: as it lands
            else at_pair(bop, mem_ri, extra);
        end
    endtask
    // push one byte: SP <- SP-1, MWR (SP)
    task push_byte; input [1:0] bsel; input hi;
        begin
            ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_M1; ctl_sp = SP_IDU;
            ctl_db = DB_REG; ctl_bi = bsel; ctl_bhi = hi;
            mstart(`BUSOP_MWR, AL_IDU, 4'd0);
        end
    endtask
    // second pop byte: MRD at SP+1, SP <- SP+1 (the first byte read (SP)
    // and is stored by the caller: into Z, or into the target pair)
    task pop_next;
        begin
            ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_P1; ctl_sp = SP_IDU;
            mstart(`BUSOP_MRD, AL_IDU, 4'd0);
        end
    endtask
    // the popped word is complete: SP <- SP+1
    task pop_done;
        begin ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_P1; ctl_sp = SP_IDU; end
    endtask
    // byte / flag writes
    task wr_byte; input [3:0] dst; input [2:0] src;
        begin ctl_bw = 1'b1; ctl_bwd = dst; ctl_bws = src; end
    endtask
    task wr_flags; input [2:0] src;
        begin ctl_fw = 1'b1; ctl_fs = src; end
    endtask
    // WZ as the die writes it (cmodel wz_later / wz_land): bytes from the
    // incrementer or the adder land in T2.N (T1.N, T5.N) of the next
    // M-cycle -- held in z80_core's wzp until its end, or a RESET cut that
    // ends after them; bytes read land in that read's T3.N (wz_rd: blocked
    // by a cut ending on that T3)
    task wz_later; input [2:0] src; input [2:0] kz; input [2:0] kw;
        begin ctl_wzp = src; ctl_wzkz = kz; ctl_wzkw = kw; end
    endtask
    task wz_rd; input [2:0] src; input [1:0] bytes;
        begin ctl_wz = src; ctl_wzm = bytes; ctl_wzc = 1'b1; end
    endtask
    // A <- ALU result unless the op is CP, F <- ALU flags
    task alu_to_a;
        begin if (alu_op_w != `ALU_CP) wr_byte(BW_A, BS_ALU); wr_flags(FS_ALU); end
    endtask
    // block-move pointer step on a mid-M-cycle edge: pair <- pair +/- 1
    task blk_step; input [2:0] rsel;
        begin
            ctl_ab = AB_REG; ctl_ri = rsel; ctl_inc = aux_w[0] ? INC_M1 : INC_P1;
            ctl_pw = 1'b1; ctl_pwi = rsel; ctl_pws = PW_IDU;
        end
    endtask
    // repeat of a block instruction: PC <- PC-2, WZ <- PC-1, 5 internal T;
    // the address latch holds PC from before (perfectz80: a RESET cut in
    // the T1 drives it)
    task blk_repeat; input [2:0] fsrc;
        begin
            ctl_ab = AB_REG; ctl_ri = RI_PC; ctl_inc = INC_M2; ctl_pc = PC_IDU;
            ctl_ea_pc = 1'b1; ctl_ea_d = EA_M1; wz_later(WZP_EA, 3'd2, 3'd2);   // WZ = PC+1 in its T2.N
            mstart(`BUSOP_INTERNAL, AL_ABUS, 4'd5);
        end
    endtask
    // an internal M-cycle whose address latch holds WZ as it is (from
    // before the instruction's own WZ write in JR, DJNZ, (IX+d), CPI, RLD /
    // RRD; HL+1 in ADD HL's second) -- a RESET cut in its T1 drives it
    // (perfectz80)
    task int_wz; input [3:0] extra;
        mstart(`BUSOP_INTERNAL, AL_WZ, extra);
    endtask

    always @* begin
        ctl_mstart = 1'b0; ctl_m1 = 1'b0; ctl_fin = 1'b0;
        ctl_bop = `BUSOP_NONE; ctl_mx = 4'd0; ctl_al = AL_ABUS;
        ctl_ab = AB_REG; ctl_ri = RI_PC; ctl_inc = INC_P1;
        ctl_ea_pc = 1'b0; ctl_ea_d = EA_RB;
        ctl_db = DB_ZERO; ctl_bi = BI_PC; ctl_bhi = 1'b0;
        ctl_pc = PC_HOLD; ctl_sp = SP_HOLD; ctl_wz = WZ_HOLD; ctl_vec = VEC_RST;
        ctl_wzm = 2'b11; ctl_wzc = 1'b0; ctl_wzp = WZP_NONE; ctl_wzkz = 3'd0; ctl_wzkw = 3'd0;
        ctl_pw = 1'b0; ctl_pwi = RI_RP; ctl_pws = PW_IDU;
        ctl_bw = 1'b0; ctl_bwd = BW_RI; ctl_bws = BS_RI;
        ctl_fw = 1'b0; ctl_fs = FS_ALU;
        ctl_ex_dehl = 1'b0; ctl_ex_af = 1'b0; ctl_exx = 1'b0;
        ctl_tmph = 1'b0;
        ctl_iff = IFF_HOLD; ctl_im = 1'b0; ctl_halt = 1'b0;
        ctl_ld_i = 1'b0; ctl_ld_r = 1'b0; ctl_prefix = 1'b0;
        ctl_cbx = 1'b0; ctl_cbr = 1'b0; ctl_pw_late = 1'b0;
        mm = m_cycle; mem_ri = RI_HLP; mem_ea = 1'b0;

        // ---- mid-M-cycle micro-ops: the IDU's jobs while the address
        // latch holds (the execute step below owns the last T-state) ----
        if (!m_end) begin
            // a cut ending on a Tw after T2 (WAIT): the SP step the die made in
            // T2's low half, which these cycles otherwise make at their end --
            // a push's second SP-1 in its first write, a pop's SP+1 in each read
            if (tw_fix && (((eff_exec == `EXEC_PUSH || eff_exec == `EXEC_RST) && m_cycle == 3'd3) ||
                           ((eff_exec == `EXEC_CALL || eff_exec == `EXEC_CALL_CC) && m_cycle == 3'd5) ||
                           ((eff_exec == `EXEC_NMI || eff_exec == `EXEC_INT) && m_cycle == 3'd2) ||
                           (eff_exec == `EXEC_EX_SP_HL && m_cycle == 3'd4))) begin
                ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_M1; ctl_sp = SP_IDU;
            end else if (tw_fix && (((eff_exec == `EXEC_POP || eff_exec == `EXEC_RET || eff_exec == `EXEC_RETN) &&
                                     (m_cycle == 3'd2 || m_cycle == 3'd3)) ||
                                    (eff_exec == `EXEC_RET_CC && (m_cycle == 3'd3 || m_cycle == 3'd4)))) begin
                ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_P1; ctl_sp = SP_IDU;
            end
            else if (latch_pos && bus_op == `BUSOP_M1 && !suppress_decode) begin
                ctl_ri = RI_PC; ctl_inc = INC_P1; ctl_pc = PC_IDU;   // opcode fetch: PC+1
            end
            // the block pointer steps in the T2.N of a bus cycle (the die's
            // edges: docs/timing.md "Register write-back") -- LDx HL in the
            // read, DE in the write; CPx HL in the read; INx HL in the write;
            // OUTx HL in the read, with B-1 from its T1 -- and LDx / CPx BC in
            // M3's T4.N
            else if (eff_exec == `EXEC_BLOCK && t_state == 4'd2 &&
                     ((aux_w[2:1] == 2'd0 && (m_cycle == 3'd2 || m_cycle == 3'd3)) ||
                      (aux_w[2:1] == 2'd1 && m_cycle == 3'd2) ||
                      (aux_w[2:1] == 2'd2 && m_cycle == 3'd4) ||
                      (aux_w[2:1] == 2'd3 && m_cycle == 3'd3))) begin
                blk_step((aux_w[2:1] == 2'd0 && m_cycle == 3'd3) ? RI_DE : RI_HL);
                ctl_pw_late = 1'b1;
                if (aux_w[2:1] == 2'd3) wr_byte(BW_B, BS_BD);
            end else if (eff_exec == `EXEC_BLOCK && !aux_w[2] && m_cycle == 3'd3 && t_state == 4'd4) begin
                ctl_ri = RI_BC; ctl_inc = INC_M1;
                ctl_pw = 1'b1; ctl_pwi = RI_BC; ctl_pws = PW_IDU; ctl_pw_late = 1'b1;
            end
            // the register writes the die makes inside an M-cycle (docs/timing.md
            // "Register write-back"): B-1 of INx / OUTx in the I/O or memory
            // read's T2 and of DJNZ in the offset read's T2; EX (SP),HL steps SP
            // up while (SP+1) is written and back at that write's end
            else if (((eff_exec == `EXEC_BLOCK && aux_w[2]) || eff_exec == `EXEC_DJNZ) &&
                     m_cycle == 3'd3 && t_state == 4'd2)
                wr_byte(BW_B, BS_BD);
            else if (eff_exec == `EXEC_EX_SP_HL && m_cycle == 3'd4 && t_state == 4'd1) begin
                ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_P1; ctl_sp = SP_IDU;
            end
        end
        // (IX+d) / (IY+d) displacement preamble: read d, then 5 internal
        // T-states while WZ = IX+d is formed (not during interrupt/HALT
        // sequences)
        else if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w && m_cycle == 3'd1) begin
            op_fetch(4'd0);
        end else if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w && m_cycle == 3'd2
                     && eff_exec != `EXEC_LD_M_N) begin
            ctl_ea_pc = 1'b0; ctl_ea_d = EA_RB; wz_later(WZP_EA, 3'd2, 3'd5);   // Z in T2.N, W in T5.N
            int_wz(4'd5);
        end else begin
            // LD (IX+d),n: WZ = IX+d while n is read (no internal cycle; the
            // n read is 2 T longer), then the operation runs one M-cycle in
            if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w &&
                eff_exec == `EXEC_LD_M_N && m_cycle == 3'd2) begin
                ctl_ea_pc = 1'b0; ctl_ea_d = EA_RB; wz_later(WZP_EA, 3'd2, 3'd5);   // in the n read
            end
            if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w) begin
                mem_ri = RI_WZ;
                if (eff_exec == `EXEC_LD_M_N) begin
                    mm = m_cycle - 3'd1;
                    mem_ea = (m_cycle == 3'd2);
                end else
                    mm = m_cycle - 3'd2;
            end

            case (eff_exec)
            `EXEC_NOP, `EXEC_ILLEGAL: ctl_fin = 1'b1;
            `EXEC_PREFIX: begin
                ctl_prefix = 1'b1;
                if (pfx_next == `PFX_DDCB || pfx_next == `PFX_FDCB)
                    op_fetch(4'd0);                       // DD CB d op: read d
                else
                    ctl_m1 = 1'b1;                        // fetch the prefixed opcode
            end
            `EXEC_DDCB: begin
                ctl_cbx = 1'b1;
                if (m_cycle == 3'd2) begin                // IX+d: Z in the op read's T2.N; read op
                    ctl_ea_pc = 1'b0; ctl_ea_d = EA_RB; wz_later(WZP_EA, 3'd2, 3'd0);
                    op_fetch(4'd0);
                end else if (m_cycle == 3'd3) begin       // op -> tmph; W in the next T2.N
                    ctl_tmph = 1'b1;
                    wz_later(WZP_KEEP, 3'd0, 3'd2);
                    mstart(`BUSOP_INTERNAL, AL_WZP, 4'd2);
                end else if (m_cycle == 3'd4)
                    mstart(`BUSOP_MRD, AL_WZP, 4'd1);     // read (IX+d), 4 T
                else if (m_cycle == 3'd5) begin
                    if (ddcb_kind == `CB_BIT) ctl_fin = 1'b1;     // F in the next M1
                    else begin                            // F and the copy to r[z] too
                        ctl_db = DB_CB;
                        at_pair(`BUSOP_MWR, RI_WZ, 4'd0);
                    end
                end else ctl_fin = 1'b1;
            end
            `EXEC_DI:   begin ctl_iff = IFF_DI; ctl_fin = 1'b1; end
            `EXEC_EI:   begin ctl_iff = IFF_EI; ctl_fin = 1'b1; end
            // Silicon-faithful per Brewer 2014 + Woodmass HALT2INT: PC stays
            // past the HALT byte (the M1 fetch already incremented it); HALT
            // NOP M-cycles re-fetch there and NMI/INT exit doesn't bump it.
            `EXEC_HALT: begin ctl_halt = 1'b1; ctl_fin = 1'b1; end

            `EXEC_LD_R_R: begin wr_byte(BW_RI, BS_RI); ctl_fin = 1'b1; end
            `EXEC_ALU_R:  begin alu_to_a; ctl_fin = 1'b1; end
            `EXEC_INC_R, `EXEC_DEC_R: begin
                wr_byte(BW_RI, BS_ALU); wr_flags(FS_ALU); ctl_fin = 1'b1;
            end
            `EXEC_ROT_A, `EXEC_DAA, `EXEC_CPL: begin
                wr_byte(BW_A, BS_ALU); wr_flags(FS_ALU); ctl_fin = 1'b1;
            end
            `EXEC_SCF, `EXEC_CCF: begin wr_flags(FS_ALU); ctl_fin = 1'b1; end

            `EXEC_EX_DE_HL: begin ctl_ex_dehl = 1'b1; ctl_fin = 1'b1; end
            `EXEC_EX_AF:    begin ctl_ex_af = 1'b1; ctl_fin = 1'b1; end
            `EXEC_EXX:      begin ctl_exx = 1'b1; ctl_fin = 1'b1; end
            `EXEC_JP_HL: begin
                ctl_ab = AB_REG; ctl_ri = RI_HLP; ctl_pc = PC_ABUS; ctl_fin = 1'b1;
            end

            // INC / DEC rr, LD SP,HL: the die writes the pair in M1's T5.N --
            // kept by a cut ending on T6, the extension's end
            `EXEC_INC_RP, `EXEC_DEC_RP: begin
                if (m_cycle == 3'd1) begin
                    ctl_ab = AB_REG; ctl_ri = RI_RP;
                    ctl_inc = (eff_exec == `EXEC_INC_RP) ? INC_P1 : INC_M1;
                    mstart(`BUSOP_INTERNAL, AL_IDU, 4'd2);
                end else begin
                    ctl_ab = AB_REG; ctl_ri = RI_RP;
                    ctl_inc = (eff_exec == `EXEC_INC_RP) ? INC_P1 : INC_M1;
                    ctl_pw = 1'b1; ctl_pwi = RI_RP; ctl_pws = PW_IDU;
                    ctl_fin = 1'b1;
                end
            end
            `EXEC_LD_SP_HL: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_HLP, 4'd2);
                else begin ctl_ab = AB_REG; ctl_ri = RI_HLP; ctl_sp = SP_ABUS; ctl_fin = 1'b1; end
            end
            `EXEC_ADD_HL_RP: begin
                if (m_cycle == 3'd1) begin
                    ctl_ab = AB_REG; ctl_ri = RI_HLP; ctl_inc = INC_P1;
                    wz_later(WZP_IDU, 3'd2, 3'd2);                   // WZ = HL+1 in its T2.N
                    at_pair(`BUSOP_INTERNAL, RI_HLP, 4'd4);         // the latch holds HL
                end
                // the die's M2 and M3: internal M-cycles of 4 and 3 T (BUSREQ
                // can take the bus between them; perfectz80). L in M2's T3.N,
                // H in M3's (the core holds it), F in the next M1 (the core)
                else if (m_cycle == 3'd2) begin wr_byte(BW_HLPL, BS_A16); mstart(`BUSOP_INTERNAL, AL_WZP, 4'd3); end
                else begin wr_byte(BW_HLPH, BS_PH); ctl_fin = 1'b1; end
            end

            `EXEC_LD_R_N: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else begin wr_byte(BW_RI, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_ALU_N: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else begin alu_to_a; ctl_fin = 1'b1; end
            end
            `EXEC_LD_R_M: begin
                if (mm == 3'd1) at_mem(`BUSOP_MRD, 4'd0);
                else begin wr_byte(BW_RI, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_ALU_M: begin
                if (mm == 3'd1) at_mem(`BUSOP_MRD, 4'd0);
                else begin alu_to_a; ctl_fin = 1'b1; end
            end
            `EXEC_ST_M_R: begin
                if (mm == 3'd1) begin ctl_db = DB_RI; at_mem(`BUSOP_MWR, 4'd0); end
                else ctl_fin = 1'b1;
            end
            `EXEC_LD_M_N: begin
                // LD (IX+d),n folds the 2 T of the IX+d add into the n read
                if (mm == 3'd1) op_fetch((idx_w != 2'd0 && use_disp_w) ? 4'd2 : 4'd0);
                else if (mm == 3'd2) begin ctl_db = DB_RB; at_mem(`BUSOP_MWR, 4'd0); end
                else ctl_fin = 1'b1;
            end
            `EXEC_INC_M, `EXEC_DEC_M: begin
                if (mm == 3'd1) at_mem(`BUSOP_MRD, 4'd1);
                else if (mm == 3'd2) begin                // F in the next M1 (the core)
                    ctl_db = DB_ALU; at_mem(`BUSOP_MWR, 4'd0);
                end else ctl_fin = 1'b1;
            end

            `EXEC_LD_RP_NN: begin                     // bytes straight into rp: MEMPTR untouched
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin wr_byte(BW_RPL, BS_RB); op_fetch(4'd0); end
                else begin wr_byte(BW_RPH, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_JP, `EXEC_JP_CC: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin wz_rd(WZ_Z, 2'b01); op_fetch(4'd0); end
                else begin
                    wz_rd(WZ_W, 2'b10);                   // MEMPTR = nn, taken or not
                    if (exec_w == `EXEC_JP || cc_taken) ctl_pc = PC_NN;
                    ctl_fin = 1'b1;
                end
            end
            `EXEC_JR, `EXEC_JR_CC: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin
                    if (eff_exec == `EXEC_JR || cc_taken) begin   // WZ = PC+e: Z in T2.N, W in T5.N
                        ctl_ea_pc = 1'b1; ctl_ea_d = EA_RB; ctl_pc = PC_EA; wz_later(WZP_EA, 3'd2, 3'd5);
                        int_wz(4'd5);
                    end else ctl_fin = 1'b1;
                end else ctl_fin = 1'b1;
            end
            `EXEC_DJNZ: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                else if (m_cycle == 3'd2) op_fetch(4'd0);
                else if (m_cycle == 3'd3) begin       // B-1 came in this read's T2
                    if (b_nz) begin
                        ctl_ea_pc = 1'b1; ctl_ea_d = EA_RB; ctl_pc = PC_EA; wz_later(WZP_EA, 3'd2, 3'd5);
                        int_wz(4'd5);
                    end else ctl_fin = 1'b1;
                end else ctl_fin = 1'b1;
            end

            `EXEC_CALL, `EXEC_CALL_CC: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin wz_rd(WZ_Z, 2'b01); op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    wz_rd(WZ_W, 2'b10);                   // WZ = nn (MEMPTR, taken or not)
                    if (exec_w == `EXEC_CALL || cc_taken) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                    else ctl_fin = 1'b1;
                end
                else if (m_cycle == 3'd4) push_byte(BI_PC, 1'b1);
                else if (m_cycle == 3'd5) push_byte(BI_PC, 1'b0);
                else begin ctl_ri = RI_WZ; ctl_pc = PC_ABUS; ctl_fin = 1'b1; end   // PC <- WZ
            end
            `EXEC_RET, `EXEC_RETN: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_SP, 4'd0);
                else if (m_cycle == 3'd2) begin wz_rd(WZ_Z, 2'b01); pop_next; end
                else begin
                    pop_done; ctl_pc = PC_NN; wz_rd(WZ_W, 2'b10);
                    if (eff_exec == `EXEC_RETN) ctl_iff = IFF_RETN;
                    ctl_fin = 1'b1;
                end
            end
            `EXEC_RET_CC: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                else if (m_cycle == 3'd2) begin
                    if (cc_taken) at_pair(`BUSOP_MRD, RI_SP, 4'd0);
                    else ctl_fin = 1'b1;
                end
                else if (m_cycle == 3'd3) begin wz_rd(WZ_Z, 2'b01); pop_next; end
                else begin pop_done; ctl_pc = PC_NN; wz_rd(WZ_W, 2'b10); ctl_fin = 1'b1; end
            end
            `EXEC_RST: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                else if (m_cycle == 3'd2) push_byte(BI_PC, 1'b1);
                else if (m_cycle == 3'd3) begin           // Z = p in the first push's T3.N
                    push_byte(BI_PC, 1'b0); ctl_vec = VEC_RST; wz_rd(WZ_VEC, 2'b01);
                end
                else begin ctl_vec = VEC_RST; ctl_pc = PC_VEC; wz_rd(WZ_VEC, 2'b10); ctl_fin = 1'b1; end
            end
            `EXEC_PUSH: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                else if (m_cycle == 3'd2) push_byte(BI_RP, 1'b1);
                else if (m_cycle == 3'd3) push_byte(BI_RP, 1'b0);
                else ctl_fin = 1'b1;
            end
            `EXEC_POP: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_SP, 4'd0);
                else if (m_cycle == 3'd2) begin wr_byte(BW_RPL, BS_RB); pop_next; end   // MEMPTR untouched
                else begin
                    pop_done; wr_byte(BW_RPH, BS_RB); ctl_fin = 1'b1;
                end
            end

            `EXEC_LD_A_RP: begin
                if (m_cycle == 3'd1) begin
                    ctl_inc = INC_P1; wz_later(WZP_IDU, 3'd2, 3'd2);   // WZ = rp+1 in the read's T2.N
                    at_pair(`BUSOP_MRD, RI_RP, 4'd0);
                end else begin wr_byte(BW_A, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_LD_RP_A: begin
                if (m_cycle == 3'd1) begin               // WZ = rp+1 in the write's T2.N
                    ctl_inc = INC_P1; wz_later(WZP_IDU, 3'd2, 3'd2);
                    ctl_db = DB_REG; ctl_bi = BI_A; ctl_bhi = 1'b1;
                    at_pair(`BUSOP_MWR, RI_RP, 4'd0);
                end else begin wz_rd(WZ_ALO, 2'b10); ctl_fin = 1'b1; end   // W = A in its T3.N
            end
            // W = (Q ^ F) | A in the operand read's T1.N (the M1's AF access
            // on the bus with A), Z and W the operand bytes, WZ = nn+1 in the
            // transfer's T2.N (and W = A in the write's T3.N)
            `EXEC_LD_A_NN: begin
                if (m_cycle == 3'd1) begin wz_later(WZP_QFA, 3'd0, 3'd1); op_fetch(4'd0); end
                else if (m_cycle == 3'd2) begin wz_rd(WZ_Z, 2'b01); op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    wz_rd(WZ_W, 2'b10);
                    ctl_ab = AB_NN; ctl_inc = INC_P1; wz_later(WZP_IDU, 3'd2, 3'd2);   // WZ = nn+1
                    mstart(`BUSOP_MRD, AL_ABUS, 4'd0);
                end else begin wr_byte(BW_A, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_LD_NN_A: begin
                if (m_cycle == 3'd1) begin wz_later(WZP_QFA, 3'd0, 3'd1); op_fetch(4'd0); end
                else if (m_cycle == 3'd2) begin wz_rd(WZ_Z, 2'b01); op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    wz_rd(WZ_W, 2'b10);
                    ctl_ab = AB_NN; ctl_inc = INC_P1; wz_later(WZP_IDU, 3'd2, 3'd2);   // WZ = nn+1
                    ctl_db = DB_REG; ctl_bi = BI_A; ctl_bhi = 1'b1;
                    mstart(`BUSOP_MWR, AL_ABUS, 4'd0);
                end else begin wz_rd(WZ_ALO, 2'b10); ctl_fin = 1'b1; end   // W = A
            end
            `EXEC_LD_HL_NN, `EXEC_LD_RP_NNA: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin wz_rd(WZ_Z, 2'b01); op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    wz_rd(WZ_W, 2'b10);
                    ctl_ab = AB_NN; ctl_inc = INC_P1; wz_later(WZP_IDU, 3'd2, 3'd2);   // WZ = nn+1
                    mstart(`BUSOP_MRD, AL_ABUS, 4'd0);
                end else if (m_cycle == 3'd4) begin                       // read (nn+1) at WZ
                    wr_byte((eff_exec == `EXEC_LD_HL_NN) ? BW_HLPL : BW_RPL, BS_RB);
                    mstart(`BUSOP_MRD, AL_WZP, 4'd0);
                end else begin
                    wr_byte((eff_exec == `EXEC_LD_HL_NN) ? BW_HLPH : BW_RPH, BS_RB);
                    ctl_fin = 1'b1;
                end
            end
            `EXEC_LD_NN_HL, `EXEC_LD_NNA_RP: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin wz_rd(WZ_Z, 2'b01); op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    wz_rd(WZ_W, 2'b10);
                    ctl_ab = AB_NN; ctl_inc = INC_P1; wz_later(WZP_IDU, 3'd2, 3'd2);   // WZ = nn+1
                    ctl_db = DB_REG; ctl_bhi = 1'b0;
                    ctl_bi = (eff_exec == `EXEC_LD_NN_HL) ? BI_HLP : BI_RP;
                    mstart(`BUSOP_MWR, AL_ABUS, 4'd0);
                end else if (m_cycle == 3'd4) begin                       // write (nn+1) at WZ
                    ctl_db = DB_REG; ctl_bhi = 1'b1;
                    ctl_bi = (eff_exec == `EXEC_LD_NN_HL) ? BI_HLP : BI_RP;
                    mstart(`BUSOP_MWR, AL_WZP, 4'd0);
                end else ctl_fin = 1'b1;
            end

            // W = A in the operand read's T1.N, Z = n in its T3.N, WZ = {A,n}+1
            // in the I/O cycle's T2.N (and OUT's W = A in its last T-state's)
            `EXEC_IN_A_N: begin
                if (m_cycle == 3'd1) begin wz_later(WZP_A, 3'd0, 3'd1); op_fetch(4'd0); end
                else if (m_cycle == 3'd2) begin
                    wz_rd(WZ_Z, 2'b01);
                    ctl_ab = AB_AN; ctl_inc = INC_P1; wz_later(WZP_IDU, 3'd2, 3'd2);   // WZ = {A,n}+1
                    mstart(`BUSOP_IORD, AL_ABUS, 4'd0);
                end else begin wr_byte(BW_A, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_OUT_N_A: begin
                if (m_cycle == 3'd1) begin wz_later(WZP_A, 3'd0, 3'd1); op_fetch(4'd0); end
                else if (m_cycle == 3'd2) begin
                    wz_rd(WZ_Z, 2'b01);
                    ctl_ab = AB_AN; ctl_inc = INC_P1; wz_later(WZP_IDU, 3'd2, 3'd2);   // WZ = {A,n}+1
                    ctl_db = DB_REG; ctl_bi = BI_A; ctl_bhi = 1'b1;
                    mstart(`BUSOP_IOWR, AL_ABUS, 4'd0);
                end else begin wz_rd(WZ_ALO, 2'b10); ctl_fin = 1'b1; end   // W = A
            end
            `EXEC_EX_SP_HL: begin                       // WZ = SP+1 in the first read's T2.N
                if (m_cycle == 3'd1) begin wz_later(WZP_IDU, 3'd2, 3'd2); at_pair(`BUSOP_MRD, RI_SP, 4'd0); end
                else if (m_cycle == 3'd2) begin
                    wz_rd(WZ_Z, 2'b01);                                   // (SP) -> Z
                    ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_P1;
                    mstart(`BUSOP_MRD, AL_IDU, 4'd1);
                end else if (m_cycle == 3'd3) begin
                    ctl_wz = WZ_W; ctl_wzm = 2'b10;                       // (SP+1) -> W, its T3.N (of 4)
                    ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_P1;
                    ctl_db = DB_REG; ctl_bi = BI_HLP; ctl_bhi = 1'b1;
                    mstart(`BUSOP_MWR, AL_IDU, 4'd0);
                end else if (m_cycle == 3'd4) begin
                    // SP back from the +1 of this write's T1; the last write at it
                    ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_M1; ctl_sp = SP_IDU;
                    ctl_db = DB_REG; ctl_bi = BI_HLP; ctl_bhi = 1'b0;
                    mstart(`BUSOP_MWR, AL_IDU, 4'd2);
                end else begin
                    ctl_pw = 1'b1; ctl_pwi = RI_HLP; ctl_pws = PW_WZ;    // HL <- WZ (= MEMPTR)
                    ctl_fin = 1'b1;
                end
            end

            `EXEC_CB_R: begin
                ctl_cbr = 1'b1;
                if (cb_kind_w == `CB_ROT || cb_kind_w == `CB_BIT) wr_flags(FS_ALU);
                if (cb_kind_w != `CB_BIT) wr_byte(BW_R8, BS_CB);
                ctl_fin = 1'b1;
            end
            `EXEC_CB_M: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_HL, 4'd1);
                else if (cb_kind_w == `CB_BIT) ctl_fin = 1'b1;     // F in the next M1
                else if (m_cycle == 3'd2) begin
                    ctl_db = DB_CB; at_pair(`BUSOP_MWR, RI_HL, 4'd0);
                end else ctl_fin = 1'b1;
            end

            // ---------- ED page (non-block) ----------
            `EXEC_ADC16, `EXEC_SBC16: begin                   // internal 4 + 3 T, as ADD; the
                if (m_cycle == 3'd1) begin                                    // sum at the first
                    wz_later(WZP_IDU, 3'd2, 3'd2);                            // one's end, where
                    at_pair(`BUSOP_INTERNAL, RI_HL, 4'd4);                    // WZ = HL+1 in T2.N
                end else if (m_cycle == 3'd2) begin                           // the die has L
                    ctl_inc = INC_P1;
                    wr_byte(BW_HLPL, BS_A16);                                 // HL+1, the
                    ctl_ab = AB_REG; ctl_ri = RI_HL; mstart(`BUSOP_INTERNAL, AL_IDU, 4'd3);  // latch's
                end else begin wr_byte(BW_HLPH, BS_PH); ctl_fin = 1'b1; end   // H; F in the next M1
            end
            `EXEC_NEG: begin wr_byte(BW_A, BS_ALU); wr_flags(FS_ALU); ctl_fin = 1'b1; end
            `EXEC_IM:  begin ctl_im = 1'b1; ctl_fin = 1'b1; end
            `EXEC_LD_I_A, `EXEC_LD_R_A: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                else begin
                    if (eff_exec == `EXEC_LD_I_A) ctl_ld_i = 1'b1; else ctl_ld_r = 1'b1;
                    ctl_fin = 1'b1;
                end
            end
            `EXEC_LD_A_IR: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                else begin wr_byte(BW_A, BS_ALU); wr_flags(FS_ALU); ctl_fin = 1'b1; end
            end
            `EXEC_IN_C: begin
                if (m_cycle == 3'd1) begin
                    ctl_inc = INC_P1; wz_later(WZP_IDU, 3'd2, 3'd2);   // WZ = BC+1 in the I/O T2.N
                    at_pair(`BUSOP_IORD, RI_BC, 4'd0);
                end else begin
                    wr_byte(BW_R8, BS_RB);                     // r[y]; y=6 writes nothing
                    wr_flags(FS_ALU); ctl_fin = 1'b1;
                end
            end
            `EXEC_OUT_C: begin
                if (m_cycle == 3'd1) begin
                    ctl_inc = INC_P1; wz_later(WZP_IDU, 3'd2, 3'd2);   // WZ = BC+1 in the I/O T2.N
                    ctl_db = DB_R8;                            // r[y]; y=6 sends 0 (NMOS)
                    at_pair(`BUSOP_IOWR, RI_BC, 4'd0);
                end else ctl_fin = 1'b1;
            end
            `EXEC_RRD, `EXEC_RLD: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_HL, 4'd0);
                else if (m_cycle == 3'd2) begin
                    int_wz(4'd4);
                end else if (m_cycle == 3'd3) begin
                    // the (HL) byte is still in tmp8 and A is untouched: the
                    // ALU forms A and the flags (FLAG_RRD / FLAG_RLD), the new
                    // (HL) byte is nibble routing
                    ctl_db = DB_RRD; at_pair(`BUSOP_MWR, RI_HL, 4'd0);   // A and F in the next M1
                    wz_later(WZP_IDU, 3'd2, 3'd2);                       // WZ = HL+1 in the write's T2.N
                end else ctl_fin = 1'b1;
            end

            // ---------- ED block instructions ----------
            // aux: [0]=dec  [2:1]=cat(0 LD,1 CP,2 IN,3 OUT)  [3]=repeat
            `EXEC_BLOCK: begin
                case (aux_w[2:1])
                2'd0: begin                                   // LDI LDD LDIR LDDR
                    if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_HL, 4'd0);
                    else if (m_cycle == 3'd2) begin
                        ctl_db = DB_RB; at_pair(`BUSOP_MWR, RI_DE, 4'd2);   // 5 T write
                    end else if (m_cycle == 3'd3) begin
                        // HL, DE and BC stepped at M2 T2 / M3 T2 / M3 T4; F in
                        // the next M1 (Banks 2018: on repeat YF/XF from PC)
                        if (aux_w[3] && bc_nz) blk_repeat(FS_BLD);
                        else ctl_fin = 1'b1;
                    end else ctl_fin = 1'b1;
                end
                2'd1: begin                                   // CPI CPD CPIR CPDR
                    if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_HL, 4'd0);
                    else if (m_cycle == 3'd2) begin
                        // HL stepped at M2 T2, WZ in M3's T2.N, BC at M3 T4
                        ctl_ab = AB_REG; ctl_ri = RI_WZ; ctl_inc = aux_w[0] ? INC_M1 : INC_P1;
                        wz_later(WZP_IDU, 3'd2, 3'd2);
                        int_wz(4'd5);
                    end else if (m_cycle == 3'd3) begin       // F in the next M1
                        if (aux_w[3] && bc_nz && !zf) blk_repeat(FS_BLD);
                        else ctl_fin = 1'b1;
                    end else ctl_fin = 1'b1;
                end
                2'd2: begin                                   // INI IND INIR INDR
                    if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                    else if (m_cycle == 3'd2) begin
                        ctl_inc = aux_w[0] ? INC_M1 : INC_P1;      // WZ = BC +/- 1 in its T2.N
                        wz_later(WZP_IDU, 3'd2, 3'd2);
                        at_pair(`BUSOP_IORD, RI_BC, 4'd0);
                    end else if (m_cycle == 3'd3) begin
                        ctl_db = DB_RB;                       // data stays in tmp8 for the flags
                        at_pair(`BUSOP_MWR, RI_HL, 4'd0);
                    end else if (m_cycle == 3'd4) begin
                        // B-1 came at M3 T2, HL at M4 T2; F in the next M1
                        if (aux_w[3] && b_nz) blk_repeat(FS_BIO);
                        else ctl_fin = 1'b1;
                    end else ctl_fin = 1'b1;
                end
                default: begin                                // OUTI OUTD OTIR OTDR
                    if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                    else if (m_cycle == 3'd2) at_pair(`BUSOP_MRD, RI_HL, 4'd0);
                    else if (m_cycle == 3'd3) begin       // B-1 came at M3 T2: out to BC
                        ctl_ab = AB_REG; ctl_ri = RI_BC; ctl_inc = aux_w[0] ? INC_M1 : INC_P1;
                        wz_later(WZP_IDU, 3'd2, 3'd2);         // WZ = BC +/- 1 in its T2.N
                        ctl_db = DB_RB;
                        mstart(`BUSOP_IOWR, AL_ABUS, 4'd0);
                    end else if (m_cycle == 3'd4) begin
                        // HL stepped at M3 T2; F in the next M1
                        if (aux_w[3] && b_nz) blk_repeat(FS_BIO);
                        else ctl_fin = 1'b1;
                    end else ctl_fin = 1'b1;
                end
                endcase
            end

            // ---------- interrupt acceptance sequences ----------
            `EXEC_NMI: begin
                if (m_cycle == 3'd1) push_byte(BI_PC, 1'b1);
                else if (m_cycle == 3'd2) begin            // Z = 66 in the first push's T3.N
                    push_byte(BI_PC, 1'b0); ctl_vec = VEC_NMI; wz_rd(WZ_VEC, 2'b01);
                end
                else begin ctl_vec = VEC_NMI; ctl_pc = PC_VEC; wz_rd(WZ_VEC, 2'b10); ctl_fin = 1'b1; end
            end
            `EXEC_INT: begin
                // WZ: IM 1 / IM 0 RST p, Z = p in the first push's T3.N, W = 00
                // in the second's; IM 2, {I, vector} in the second push's T2.N
                // (Z) and T3.N (W), I:vector+1 in the table read's T2.N, then
                // the table's bytes in their reads' T3.N
                if (m_cycle == 3'd1) push_byte(BI_PC, 1'b1);
                else if (m_cycle == 3'd2) begin
                    push_byte(BI_PC, 1'b0);
                    if (im == 2'd2) wz_later(WZP_IV, 3'd2, 3'd3);
                    else begin ctl_vec = VEC_IM; wz_rd(WZ_VEC, 2'b01); end
                end
                else if (m_cycle == 3'd3) begin
                    if (im == 2'd2) begin
                        ctl_ab = AB_IV; ctl_inc = INC_P1; wz_later(WZP_IDU, 3'd2, 3'd2);
                        mstart(`BUSOP_MRD, AL_ABUS, 4'd0);
                    end else begin
                        ctl_vec = VEC_IM; ctl_pc = PC_VEC; wz_rd(WZ_VEC, 2'b10); ctl_fin = 1'b1;
                    end
                end
                else if (m_cycle == 3'd4) begin
                    wz_rd(WZ_Z, 2'b01);                                    // low byte -> Z
                    ctl_ab = AB_REG; ctl_ri = RI_WZ; ctl_inc = INC_P1;
                    mstart(`BUSOP_MRD, AL_IDU, 4'd0);
                end
                else begin ctl_pc = PC_NN; wz_rd(WZ_W, 2'b10); ctl_fin = 1'b1; end
            end

            default: ctl_fin = 1'b1;
            endcase
        end
    end

endmodule
