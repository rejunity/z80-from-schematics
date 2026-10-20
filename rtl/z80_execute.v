// ===========================================================================
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
    // conditions from the datapath
    input  wire        cc_taken,        // cc[y] holds in F
    input  wire        zf,              // F.Z
    input  wire        bc_nz,           // BC != 0
    input  wire        b_nz,            // B != 0
    input  wire        b_dec_nz,        // B-1 != 0
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
    // read the next operand byte: MRD (PC), PC <- PC+1
    task op_fetch; input [3:0] extra;
        begin
            ctl_ab = AB_REG; ctl_ri = RI_PC; ctl_inc = INC_P1; ctl_pc = PC_IDU;
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
    // repeat of a block instruction: PC <- PC-2, WZ <- PC-1, 5 internal T
    task blk_repeat; input [2:0] fsrc;
        begin
            ctl_ab = AB_REG; ctl_ri = RI_PC; ctl_inc = INC_M2; ctl_pc = PC_IDU;
            ctl_ea_pc = 1'b1; ctl_ea_d = EA_M1; ctl_wz = WZ_EA;
            wr_flags(fsrc);
            mstart(`BUSOP_INTERNAL, AL_IDU, 4'd5);
        end
    endtask

    always @* begin
        ctl_mstart = 1'b0; ctl_m1 = 1'b0; ctl_fin = 1'b0;
        ctl_bop = `BUSOP_NONE; ctl_mx = 4'd0; ctl_al = AL_ABUS;
        ctl_ab = AB_REG; ctl_ri = RI_PC; ctl_inc = INC_P1;
        ctl_ea_pc = 1'b0; ctl_ea_d = EA_RB;
        ctl_db = DB_ZERO; ctl_bi = BI_PC; ctl_bhi = 1'b0;
        ctl_pc = PC_HOLD; ctl_sp = SP_HOLD; ctl_wz = WZ_HOLD; ctl_vec = VEC_RST;
        ctl_pw = 1'b0; ctl_pwi = RI_RP; ctl_pws = PW_IDU;
        ctl_bw = 1'b0; ctl_bwd = BW_RI; ctl_bws = BS_RI;
        ctl_fw = 1'b0; ctl_fs = FS_ALU;
        ctl_ex_dehl = 1'b0; ctl_ex_af = 1'b0; ctl_exx = 1'b0;
        ctl_tmph = 1'b0;
        ctl_iff = IFF_HOLD; ctl_im = 1'b0; ctl_halt = 1'b0;
        ctl_ld_i = 1'b0; ctl_ld_r = 1'b0; ctl_prefix = 1'b0;
        ctl_cbx = 1'b0; ctl_cbr = 1'b0;
        mm = m_cycle; mem_ri = RI_HLP; mem_ea = 1'b0;

        // ---- mid-M-cycle micro-ops: the IDU's jobs while the address
        // latch holds (the execute step below owns the last T-state) ----
        if (!m_end) begin
            if (latch_pos && bus_op == `BUSOP_M1 && !suppress_decode) begin
                ctl_ri = RI_PC; ctl_inc = INC_P1; ctl_pc = PC_IDU;   // opcode fetch: PC+1
            end else if (eff_exec == `EXEC_BLOCK && t_state == 4'd1) begin
                case (aux_w[2:1])
                    2'd0:    if (m_cycle == 3'd2) blk_step(RI_HL);        // LDx: HL, M2 T1
                             else if (m_cycle == 3'd3) blk_step(RI_DE);   //      DE, M3 T1
                    2'd1:    if (m_cycle == 3'd2) blk_step(RI_HL);        // CPx: HL, M2 T1
                             else if (m_cycle == 3'd3) begin             //      WZ, M3 T1
                                 ctl_ri = RI_WZ; ctl_inc = aux_w[0] ? INC_M1 : INC_P1;
                                 ctl_wz = WZ_IDU;
                             end
                    2'd2:    if (m_cycle == 3'd4) blk_step(RI_HL);        // INx: HL, M4 T1
                    default: if (m_cycle == 3'd3) blk_step(RI_HL);        // OUTx: HL, M3 T1
                endcase
            end else if (eff_exec == `EXEC_BLOCK && t_state == 4'd2 &&
                         ((aux_w[2:1] == 2'd0 && m_cycle == 3'd3) ||       // LDx: BC-1, M3 T2
                          (aux_w[2:1] == 2'd1 && m_cycle == 3'd2))) begin  // CPx: BC-1, M2 T2
                ctl_ri = RI_BC; ctl_inc = INC_M1;
                ctl_pw = 1'b1; ctl_pwi = RI_BC; ctl_pws = PW_IDU;
            end
        end
        // (IX+d) / (IY+d) displacement preamble: read d, then 5 internal
        // T-states while WZ = IX+d is formed (not during interrupt/HALT
        // sequences)
        else if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w && m_cycle == 3'd1) begin
            op_fetch(4'd0);
        end else if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w && m_cycle == 3'd2
                     && eff_exec != `EXEC_LD_M_N) begin
            ctl_ea_pc = 1'b0; ctl_ea_d = EA_RB; ctl_wz = WZ_EA;
            mstart(`BUSOP_INTERNAL, AL_EA, 4'd5);
        end else begin
            // LD (IX+d),n: WZ = IX+d while n is read (no internal cycle; the
            // n read is 2 T longer), then the operation runs one M-cycle in
            if (irq_seq == 2'd0 && idx_w != 2'd0 && use_disp_w &&
                eff_exec == `EXEC_LD_M_N && m_cycle == 3'd2) begin
                ctl_ea_pc = 1'b0; ctl_ea_d = EA_RB; ctl_wz = WZ_EA;
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
                if (m_cycle == 3'd2) begin                // d -> Z; read op
                    ctl_wz = WZ_Z; op_fetch(4'd0);
                end else if (m_cycle == 3'd3) begin       // op -> tmph; WZ = IX+d
                    ctl_tmph = 1'b1;
                    ctl_ea_pc = 1'b0; ctl_ea_d = EA_Z; ctl_wz = WZ_EA;
                    mstart(`BUSOP_INTERNAL, AL_EA, 4'd2);
                end else if (m_cycle == 3'd4)
                    at_pair(`BUSOP_MRD, RI_WZ, 4'd1);     // read (IX+d), 4 T
                else if (m_cycle == 3'd5) begin
                    if (ddcb_kind == `CB_BIT) begin
                        wr_flags(FS_ALU); ctl_fin = 1'b1;
                    end else begin
                        if (ddcb_kind == `CB_ROT) wr_flags(FS_ALU);
                        wr_byte(BW_DDCB, BS_CB);          // undocumented copy to r[z]
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

            `EXEC_INC_RP, `EXEC_DEC_RP: begin
                if (m_cycle == 3'd1) begin
                    ctl_ab = AB_REG; ctl_ri = RI_RP;
                    ctl_inc = (eff_exec == `EXEC_INC_RP) ? INC_P1 : INC_M1;
                    ctl_pw = 1'b1; ctl_pwi = RI_RP; ctl_pws = PW_IDU;
                    mstart(`BUSOP_INTERNAL, AL_IDU, 4'd2);
                end else ctl_fin = 1'b1;
            end
            `EXEC_LD_SP_HL: begin
                if (m_cycle == 3'd1) begin
                    ctl_sp = SP_ABUS; at_pair(`BUSOP_INTERNAL, RI_HLP, 4'd2);
                end else ctl_fin = 1'b1;
            end
            `EXEC_ADD_HL_RP: begin
                if (m_cycle == 3'd1) begin
                    ctl_ab = AB_REG; ctl_ri = RI_HLP; ctl_inc = INC_P1; ctl_wz = WZ_IDU;  // WZ = HL+1
                    ctl_pw = 1'b1; ctl_pwi = RI_HLP; ctl_pws = PW_A16;
                    wr_flags(FS_A16);
                    at_pair(`BUSOP_INTERNAL, RI_HLP, 4'd4);         // the latch holds HL
                end
                // the die's M2 and M3: internal M-cycles of 4 and 3 T (BUSREQ
                // can take the bus between them; perfectz80)
                else if (m_cycle == 3'd2) at_pair(`BUSOP_INTERNAL, RI_HLP, 4'd3);
                else ctl_fin = 1'b1;
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
                else if (mm == 3'd2) begin
                    wr_flags(FS_ALU); ctl_db = DB_ALU; at_mem(`BUSOP_MWR, 4'd0);
                end else ctl_fin = 1'b1;
            end

            `EXEC_LD_RP_NN: begin                     // bytes straight into rp: MEMPTR untouched
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin wr_byte(BW_RPL, BS_RB); op_fetch(4'd0); end
                else begin wr_byte(BW_RPH, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_JP, `EXEC_JP_CC: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin ctl_wz = WZ_Z; op_fetch(4'd0); end
                else begin
                    ctl_wz = WZ_W;                        // MEMPTR = nn, taken or not
                    if (exec_w == `EXEC_JP || cc_taken) ctl_pc = PC_NN;
                    ctl_fin = 1'b1;
                end
            end
            `EXEC_JR, `EXEC_JR_CC: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin
                    if (eff_exec == `EXEC_JR || cc_taken) begin
                        ctl_ea_pc = 1'b1; ctl_ea_d = EA_RB; ctl_pc = PC_EA; ctl_wz = WZ_EA;
                        mstart(`BUSOP_INTERNAL, AL_EA, 4'd5);
                    end else ctl_fin = 1'b1;
                end else ctl_fin = 1'b1;
            end
            `EXEC_DJNZ: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                else if (m_cycle == 3'd2) op_fetch(4'd0);
                else if (m_cycle == 3'd3) begin
                    wr_byte(BW_B, BS_BD);                 // B <- B-1
                    if (b_dec_nz) begin
                        ctl_ea_pc = 1'b1; ctl_ea_d = EA_RB; ctl_pc = PC_EA; ctl_wz = WZ_EA;
                        mstart(`BUSOP_INTERNAL, AL_EA, 4'd5);
                    end else ctl_fin = 1'b1;
                end else ctl_fin = 1'b1;
            end

            `EXEC_CALL, `EXEC_CALL_CC: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin ctl_wz = WZ_Z; op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    ctl_wz = WZ_W;                        // WZ = nn (MEMPTR, taken or not)
                    if (exec_w == `EXEC_CALL || cc_taken) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                    else ctl_fin = 1'b1;
                end
                else if (m_cycle == 3'd4) push_byte(BI_PC, 1'b1);
                else if (m_cycle == 3'd5) push_byte(BI_PC, 1'b0);
                else begin ctl_ri = RI_WZ; ctl_pc = PC_ABUS; ctl_fin = 1'b1; end   // PC <- WZ
            end
            `EXEC_RET, `EXEC_RETN: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_SP, 4'd0);
                else if (m_cycle == 3'd2) begin ctl_wz = WZ_Z; pop_next; end
                else begin
                    pop_done; ctl_pc = PC_NN; ctl_wz = WZ_W;
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
                else if (m_cycle == 3'd3) begin ctl_wz = WZ_Z; pop_next; end
                else begin pop_done; ctl_pc = PC_NN; ctl_wz = WZ_W; ctl_fin = 1'b1; end
            end
            `EXEC_RST: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                else if (m_cycle == 3'd2) push_byte(BI_PC, 1'b1);
                else if (m_cycle == 3'd3) push_byte(BI_PC, 1'b0);
                else begin ctl_vec = VEC_RST; ctl_pc = PC_VEC; ctl_wz = WZ_VEC; ctl_fin = 1'b1; end
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
                    ctl_inc = INC_P1; ctl_wz = WZ_IDU;          // WZ = rp+1
                    at_pair(`BUSOP_MRD, RI_RP, 4'd0);
                end else begin wr_byte(BW_A, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_LD_RP_A: begin
                if (m_cycle == 3'd1) begin
                    ctl_inc = INC_P1; ctl_wz = WZ_ALO;          // WZ = {A, rp.lo+1}
                    ctl_db = DB_REG; ctl_bi = BI_A; ctl_bhi = 1'b1;
                    at_pair(`BUSOP_MWR, RI_RP, 4'd0);
                end else ctl_fin = 1'b1;
            end
            `EXEC_LD_A_NN: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin ctl_wz = WZ_Z; op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    ctl_ab = AB_NN; ctl_inc = INC_P1; ctl_wz = WZ_IDU;   // WZ = nn+1
                    mstart(`BUSOP_MRD, AL_ABUS, 4'd0);
                end else begin wr_byte(BW_A, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_LD_NN_A: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin ctl_wz = WZ_Z; op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    ctl_ab = AB_NN; ctl_inc = INC_P1; ctl_wz = WZ_ALO;   // WZ = {A, n.lo+1}
                    ctl_db = DB_REG; ctl_bi = BI_A; ctl_bhi = 1'b1;
                    mstart(`BUSOP_MWR, AL_ABUS, 4'd0);
                end else ctl_fin = 1'b1;
            end
            `EXEC_LD_HL_NN, `EXEC_LD_RP_NNA: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin ctl_wz = WZ_Z; op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    ctl_ab = AB_NN; ctl_inc = INC_P1; ctl_wz = WZ_IDU;   // WZ = nn+1
                    mstart(`BUSOP_MRD, AL_ABUS, 4'd0);
                end else if (m_cycle == 3'd4) begin                       // read (nn+1) at WZ
                    wr_byte((eff_exec == `EXEC_LD_HL_NN) ? BW_HLPL : BW_RPL, BS_RB);
                    at_pair(`BUSOP_MRD, RI_WZ, 4'd0);
                end else begin
                    wr_byte((eff_exec == `EXEC_LD_HL_NN) ? BW_HLPH : BW_RPH, BS_RB);
                    ctl_fin = 1'b1;
                end
            end
            `EXEC_LD_NN_HL, `EXEC_LD_NNA_RP: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin ctl_wz = WZ_Z; op_fetch(4'd0); end
                else if (m_cycle == 3'd3) begin
                    ctl_ab = AB_NN; ctl_inc = INC_P1; ctl_wz = WZ_IDU;   // WZ = nn+1
                    ctl_db = DB_REG; ctl_bhi = 1'b0;
                    ctl_bi = (eff_exec == `EXEC_LD_NN_HL) ? BI_HLP : BI_RP;
                    mstart(`BUSOP_MWR, AL_ABUS, 4'd0);
                end else if (m_cycle == 3'd4) begin                       // write (nn+1) at WZ
                    ctl_db = DB_REG; ctl_bhi = 1'b1;
                    ctl_bi = (eff_exec == `EXEC_LD_NN_HL) ? BI_HLP : BI_RP;
                    at_pair(`BUSOP_MWR, RI_WZ, 4'd0);
                end else ctl_fin = 1'b1;
            end

            `EXEC_IN_A_N: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin
                    ctl_ab = AB_AN; ctl_inc = INC_P1; ctl_wz = WZ_IDU;   // WZ = {A,n}+1
                    mstart(`BUSOP_IORD, AL_ABUS, 4'd0);
                end else begin wr_byte(BW_A, BS_RB); ctl_fin = 1'b1; end
            end
            `EXEC_OUT_N_A: begin
                if (m_cycle == 3'd1) op_fetch(4'd0);
                else if (m_cycle == 3'd2) begin
                    ctl_ab = AB_AN; ctl_inc = INC_P1; ctl_wz = WZ_ALO;   // WZ = {A, n+1}
                    ctl_db = DB_REG; ctl_bi = BI_A; ctl_bhi = 1'b1;
                    mstart(`BUSOP_IOWR, AL_ABUS, 4'd0);
                end else ctl_fin = 1'b1;
            end
            `EXEC_EX_SP_HL: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_SP, 4'd0);
                else if (m_cycle == 3'd2) begin
                    ctl_wz = WZ_Z;                                        // (SP) -> Z
                    ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_P1;
                    mstart(`BUSOP_MRD, AL_IDU, 4'd1);
                end else if (m_cycle == 3'd3) begin
                    ctl_wz = WZ_W;                                        // (SP+1) -> W
                    ctl_ab = AB_REG; ctl_ri = RI_SP; ctl_inc = INC_P1;
                    ctl_db = DB_REG; ctl_bi = BI_HLP; ctl_bhi = 1'b1;
                    mstart(`BUSOP_MWR, AL_IDU, 4'd0);
                end else if (m_cycle == 3'd4) begin
                    ctl_db = DB_REG; ctl_bi = BI_HLP; ctl_bhi = 1'b0;
                    at_pair(`BUSOP_MWR, RI_SP, 4'd2);
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
                else if (cb_kind_w == `CB_BIT) begin wr_flags(FS_ALU); ctl_fin = 1'b1; end
                else if (m_cycle == 3'd2) begin
                    if (cb_kind_w == `CB_ROT) wr_flags(FS_ALU);
                    ctl_db = DB_CB; at_pair(`BUSOP_MWR, RI_HL, 4'd0);
                end else ctl_fin = 1'b1;
            end

            // ---------- ED page (non-block) ----------
            `EXEC_ADC16, `EXEC_SBC16: begin                   // internal 4 + 3 T, as ADD
                if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_HL, 4'd4);
                else if (m_cycle == 3'd2) at_pair(`BUSOP_INTERNAL, RI_HL, 4'd3);
                else begin
                    ctl_ab = AB_REG; ctl_ri = RI_HL; ctl_inc = INC_P1; ctl_wz = WZ_IDU;  // WZ = HL+1
                    ctl_pw = 1'b1; ctl_pwi = RI_HL; ctl_pws = PW_A16;
                    wr_flags(FS_A16); ctl_fin = 1'b1;
                end
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
                    ctl_inc = INC_P1; ctl_wz = WZ_IDU;          // WZ = BC+1
                    at_pair(`BUSOP_IORD, RI_BC, 4'd0);
                end else begin
                    wr_byte(BW_R8, BS_RB);                     // r[y]; y=6 writes nothing
                    wr_flags(FS_ALU); ctl_fin = 1'b1;
                end
            end
            `EXEC_OUT_C: begin
                if (m_cycle == 3'd1) begin
                    ctl_inc = INC_P1; ctl_wz = WZ_IDU;          // WZ = BC+1
                    ctl_db = DB_R8;                            // r[y]; y=6 sends 0 (NMOS)
                    at_pair(`BUSOP_IOWR, RI_BC, 4'd0);
                end else ctl_fin = 1'b1;
            end
            `EXEC_RRD, `EXEC_RLD: begin
                if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_HL, 4'd0);
                else if (m_cycle == 3'd2) begin
                    ctl_ab = AB_REG; ctl_ri = RI_HL; ctl_inc = INC_P1; ctl_wz = WZ_IDU;  // WZ = HL+1
                    mstart(`BUSOP_INTERNAL, AL_ABUS, 4'd4);
                end else if (m_cycle == 3'd3) begin
                    // the (HL) byte is still in tmp8 and A is untouched: the
                    // ALU forms A and the flags (FLAG_RRD / FLAG_RLD), the new
                    // (HL) byte is nibble routing
                    wr_byte(BW_A, BS_ALU); wr_flags(FS_ALU);
                    ctl_db = DB_RRD; at_pair(`BUSOP_MWR, RI_HL, 4'd0);
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
                        // HL, DE and BC stepped at M2 T1 / M3 T1 / M3 T2
                        wr_flags(FS_ALU);
                        // Banks 2018: on repeat YF/XF come from PC.13 / PC.11
                        if (aux_w[3] && bc_nz) blk_repeat(FS_BLD);
                        else ctl_fin = 1'b1;
                    end else ctl_fin = 1'b1;
                end
                2'd1: begin                                   // CPI CPD CPIR CPDR
                    if (m_cycle == 3'd1) at_pair(`BUSOP_MRD, RI_HL, 4'd0);
                    else if (m_cycle == 3'd2) begin
                        // HL and BC stepped at M2 T1 / T2; WZ steps at M3 T1
                        wr_flags(FS_ALU);
                        at_pair(`BUSOP_INTERNAL, RI_HL, 4'd5);
                    end else if (m_cycle == 3'd3) begin
                        if (aux_w[3] && bc_nz && !zf) blk_repeat(FS_BCP);
                        else ctl_fin = 1'b1;
                    end else ctl_fin = 1'b1;
                end
                2'd2: begin                                   // INI IND INIR INDR
                    if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                    else if (m_cycle == 3'd2) begin
                        ctl_inc = aux_w[0] ? INC_M1 : INC_P1; ctl_wz = WZ_IDU;  // WZ = BC +/- 1
                        at_pair(`BUSOP_IORD, RI_BC, 4'd0);
                    end else if (m_cycle == 3'd3) begin
                        ctl_db = DB_RB;                       // data stays in tmp8 for the flags
                        at_pair(`BUSOP_MWR, RI_HL, 4'd0);
                    end else if (m_cycle == 3'd4) begin
                        wr_byte(BW_B, BS_BD);                 // B-1 (HL stepped at M4 T1)
                        wr_flags(FS_ALU);
                        if (aux_w[3] && b_dec_nz) blk_repeat(FS_BIO);
                        else ctl_fin = 1'b1;
                    end else ctl_fin = 1'b1;
                end
                default: begin                                // OUTI OUTD OTIR OTDR
                    if (m_cycle == 3'd1) at_pair(`BUSOP_INTERNAL, RI_PC, 4'd1);
                    else if (m_cycle == 3'd2) at_pair(`BUSOP_MRD, RI_HL, 4'd0);
                    else if (m_cycle == 3'd3) begin
                        wr_byte(BW_B, BS_BD);                 // B-1, then out to {B-1, C}
                        ctl_ab = AB_NBC; ctl_inc = aux_w[0] ? INC_M1 : INC_P1; ctl_wz = WZ_IDU;
                        ctl_db = DB_RB;
                        mstart(`BUSOP_IOWR, AL_ABUS, 4'd0);
                    end else if (m_cycle == 3'd4) begin
                        wr_flags(FS_ALU);                     // HL stepped at M3 T1
                        if (aux_w[3] && b_nz) blk_repeat(FS_BIO);
                        else ctl_fin = 1'b1;
                    end else ctl_fin = 1'b1;
                end
                endcase
            end

            // ---------- interrupt acceptance sequences ----------
            `EXEC_NMI: begin
                if (m_cycle == 3'd1) push_byte(BI_PC, 1'b1);
                else if (m_cycle == 3'd2) push_byte(BI_PC, 1'b0);
                else begin ctl_vec = VEC_NMI; ctl_pc = PC_VEC; ctl_wz = WZ_VEC; ctl_fin = 1'b1; end
            end
            `EXEC_INT: begin
                if (m_cycle == 3'd1) push_byte(BI_PC, 1'b1);
                else if (m_cycle == 3'd2) push_byte(BI_PC, 1'b0);
                else if (m_cycle == 3'd3) begin
                    if (im == 2'd2) begin
                        ctl_wz = WZ_IV; ctl_ab = AB_IV;           // WZ = {I, vector}
                        mstart(`BUSOP_MRD, AL_ABUS, 4'd0);
                    end else begin
                        ctl_vec = VEC_IM; ctl_pc = PC_VEC; ctl_wz = WZ_VEC; ctl_fin = 1'b1;
                    end
                end
                else if (m_cycle == 3'd4) begin
                    ctl_wz = WZ_Z;                                         // low byte -> Z
                    ctl_ab = AB_REG; ctl_ri = RI_WZ; ctl_inc = INC_P1;
                    mstart(`BUSOP_MRD, AL_IDU, 4'd0);
                end
                else begin ctl_pc = PC_NN; ctl_wz = WZ_W; ctl_fin = 1'b1; end
            end

            default: ctl_fin = 1'b1;
            endcase
        end
    end

endmodule
