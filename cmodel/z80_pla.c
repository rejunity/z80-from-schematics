/* ============================================================================
 * z80_pla.c - instruction decode PLA (docs/pla.md).
 *
 * Decodes (prefix_state, opcode) into a control word using the standard
 * x/y/z/p/q opcode decomposition rather than a 256-row hand table. Emits named
 * control lines (exec action, ALU op, flag mode, sequence template, operands).
 * Unprefixed space is complete; CB/ED/DD/FD are decoded in later stages.
 * ==========================================================================*/
#include "z80.h"

/* rp[p] / rp2[p] backing pairs */
static const uint8_t rp_tab[4]  = { RFP_BC, RFP_DE, RFP_HL, RFP_SP };
static const uint8_t rp2_tab[4] = { RFP_BC, RFP_DE, RFP_HL, RFP_AF };

static z80_flag_mode_t alu_flag_mode(uint8_t y)
{
    switch (y) {
        case 0: case 1: return FLAG_ADD8; /* ADD ADC */
        case 2: case 3: return FLAG_SUB8; /* SUB SBC */
        case 4: case 5: case 6: return FLAG_LOGIC; /* AND XOR OR */
        default: return FLAG_CP8;          /* CP */
    }
}

static z80_control_t decode_unprefixed(uint8_t op)
{
    z80_control_t c;
    /* zero-init (EXEC_NOP, SEQ_NONE, FLAG_NONE, ...) */
    for (unsigned i = 0; i < sizeof(c); i++) ((uint8_t *)&c)[i] = 0;
    c.valid = true;

    uint8_t x = (uint8_t)(op >> 6);
    uint8_t y = (uint8_t)((op >> 3) & 7);
    uint8_t z = (uint8_t)(op & 7);
    uint8_t p = (uint8_t)(y >> 1);
    uint8_t q = (uint8_t)(y & 1);

    switch (x) {
    case 0:
        switch (z) {
        case 0:
            switch (y) {
            case 0: c.exec = EXEC_NOP;    c.seq = SEQ_NONE; break;
            case 1: c.exec = EXEC_EX_AF;  c.seq = SEQ_NONE; break;
            case 2: c.exec = EXEC_DJNZ;   c.seq = SEQ_JR; c.uses_cc = true; break;
            case 3: c.exec = EXEC_JR;     c.seq = SEQ_JR; break;
            default: /* 4..7: JR cc,e */
                c.exec = EXEC_JR_CC; c.seq = SEQ_JR; c.uses_cc = true;
                c.cc = (uint8_t)(y - 4); break;
            }
            break;
        case 1:
            if (q == 0) { c.exec = EXEC_LD_RP_NN; c.seq = SEQ_IMM16; c.rp_sel = rp_tab[p]; }
            else { c.exec = EXEC_ADD_HL_RP; c.seq = SEQ_NONE; c.rp_sel = rp_tab[p];
                   c.flag_mode = FLAG_ADD16; }
            break;
        case 2:
            if (q == 0) {
                switch (p) {
                case 0: c.exec = EXEC_LD_RP_A; c.rp_sel = RFP_BC; c.seq = SEQ_MWR_HL; c.wz_op = WZ_ADDR_PLUS1; break;
                case 1: c.exec = EXEC_LD_RP_A; c.rp_sel = RFP_DE; c.seq = SEQ_MWR_HL; c.wz_op = WZ_ADDR_PLUS1; break;
                case 2: c.exec = EXEC_LD_NN_HL; c.seq = SEQ_IMM16; c.wz_op = WZ_NN_PLUS1; break;
                case 3: c.exec = EXEC_LD_NN_A;  c.seq = SEQ_IMM16; c.wz_op = WZ_NN_PLUS1; break;
                }
            } else {
                switch (p) {
                case 0: c.exec = EXEC_LD_A_RP; c.rp_sel = RFP_BC; c.seq = SEQ_MRD_HL; c.wz_op = WZ_ADDR_PLUS1; break;
                case 1: c.exec = EXEC_LD_A_RP; c.rp_sel = RFP_DE; c.seq = SEQ_MRD_HL; c.wz_op = WZ_ADDR_PLUS1; break;
                case 2: c.exec = EXEC_LD_HL_NN; c.seq = SEQ_IMM16; c.wz_op = WZ_NN_PLUS1; break;
                case 3: c.exec = EXEC_LD_A_NN;  c.seq = SEQ_IMM16; c.wz_op = WZ_NN_PLUS1; break;
                }
            }
            break;
        case 3:
            if (q == 0) c.exec = EXEC_INC_RP; else c.exec = EXEC_DEC_RP;
            c.rp_sel = rp_tab[p]; c.seq = SEQ_NONE;
            break;
        case 4: /* INC r[y] */
            c.rf_dst = y; c.rf_src = y;
            if (y == REG_iHL) { c.exec = EXEC_INC_M; c.seq = SEQ_RMW_HL; }
            else              { c.exec = EXEC_INC_R; c.seq = SEQ_NONE; }
            c.flag_mode = FLAG_INC8;
            break;
        case 5: /* DEC r[y] */
            c.rf_dst = y; c.rf_src = y;
            if (y == REG_iHL) { c.exec = EXEC_DEC_M; c.seq = SEQ_RMW_HL; }
            else              { c.exec = EXEC_DEC_R; c.seq = SEQ_NONE; }
            c.flag_mode = FLAG_DEC8;
            break;
        case 6: /* LD r[y],n */
            c.rf_dst = y;
            if (y == REG_iHL) { c.exec = EXEC_LD_M_N; c.seq = SEQ_MWR_HL; }
            else              { c.exec = EXEC_LD_R_N; c.seq = SEQ_IMM8; }
            break;
        case 7: /* accumulator/flag ops */
            switch (y) {
            case 0: case 1: case 2: case 3:
                c.exec = EXEC_ROT_A; c.rot_op = y; c.flag_mode = FLAG_ROT_A; break;
            case 4: c.exec = EXEC_DAA; c.flag_mode = FLAG_DAA; break;
            case 5: c.exec = EXEC_CPL; c.flag_mode = FLAG_CPL; break;
            case 6: c.exec = EXEC_SCF; c.flag_mode = FLAG_SCF; break;
            case 7: c.exec = EXEC_CCF; c.flag_mode = FLAG_CCF; break;
            }
            c.seq = SEQ_NONE;
            break;
        }
        break;

    case 1:
        if (y == REG_iHL && z == REG_iHL) { c.exec = EXEC_HALT; c.special |= SPC_HALT; }
        else if (z == REG_iHL) { c.exec = EXEC_LD_R_M; c.rf_dst = y; c.seq = SEQ_MRD_HL; }
        else if (y == REG_iHL) { c.exec = EXEC_ST_M_R; c.rf_src = z; c.seq = SEQ_MWR_HL; }
        else { c.exec = EXEC_LD_R_R; c.rf_dst = y; c.rf_src = z; c.seq = SEQ_NONE; }
        break;

    case 2: /* ALU[y] A, r[z] */
        c.alu_op = (z80_alu_op_t)y;
        c.flag_mode = alu_flag_mode(y);
        if (z == REG_iHL) { c.exec = EXEC_ALU_M; c.seq = SEQ_MRD_HL; }
        else { c.exec = EXEC_ALU_R; c.rf_src = z; c.seq = SEQ_NONE; }
        break;

    case 3:
        switch (z) {
        case 0: c.exec = EXEC_RET_CC; c.seq = SEQ_RET; c.uses_cc = true; c.cc = y; c.wz_op = WZ_DEST; break;
        case 1:
            if (q == 0) { c.exec = EXEC_POP; c.rp_sel = rp2_tab[p]; c.seq = SEQ_POP; }
            else switch (p) {
                case 0: c.exec = EXEC_RET;     c.seq = SEQ_RET; c.wz_op = WZ_DEST; break;
                case 1: c.exec = EXEC_EXX;     c.seq = SEQ_NONE; break;
                case 2: c.exec = EXEC_JP_HL;   c.seq = SEQ_NONE; break;
                case 3: c.exec = EXEC_LD_SP_HL;c.seq = SEQ_NONE; break;
            }
            break;
        case 2: c.exec = EXEC_JP_CC; c.seq = SEQ_IMM16; c.uses_cc = true; c.cc = y; c.wz_op = WZ_NN; break;
        case 3:
            switch (y) {
            case 0: c.exec = EXEC_JP;      c.seq = SEQ_IMM16; c.wz_op = WZ_NN; break;
            case 1: c.exec = EXEC_PREFIX;  c.special |= SPC_PREFIX; break; /* CB */
            case 2: c.exec = EXEC_OUT_N_A; c.seq = SEQ_IO; c.wz_op = WZ_IO; break;
            case 3: c.exec = EXEC_IN_A_N;  c.seq = SEQ_IO; c.wz_op = WZ_IO; break;
            case 4: c.exec = EXEC_EX_SP_HL;c.seq = SEQ_POP; c.wz_op = WZ_DEST; break;
            case 5: c.exec = EXEC_EX_DE_HL;c.seq = SEQ_NONE; break;
            case 6: c.exec = EXEC_DI;      c.special |= SPC_DI; break;
            case 7: c.exec = EXEC_EI;      c.special |= SPC_EI; break;
            }
            break;
        case 4: c.exec = EXEC_CALL_CC; c.seq = SEQ_CALL; c.uses_cc = true; c.cc = y; c.wz_op = WZ_NN; break;
        case 5:
            if (q == 0) { c.exec = EXEC_PUSH; c.rp_sel = rp2_tab[p]; c.seq = SEQ_PUSH; }
            else switch (p) {
                case 0: c.exec = EXEC_CALL;   c.seq = SEQ_CALL; c.wz_op = WZ_NN; break;
                case 1: c.exec = EXEC_PREFIX; c.special |= SPC_PREFIX; break; /* DD */
                case 2: c.exec = EXEC_PREFIX; c.special |= SPC_PREFIX; break; /* ED */
                case 3: c.exec = EXEC_PREFIX; c.special |= SPC_PREFIX; break; /* FD */
            }
            break;
        case 6: /* ALU[y] A,n */
            c.exec = EXEC_ALU_N; c.alu_op = (z80_alu_op_t)y;
            c.flag_mode = alu_flag_mode(y); c.seq = SEQ_IMM8;
            break;
        case 7: c.exec = EXEC_RST; c.rst_addr = (uint8_t)(y * 8); c.seq = SEQ_CALL; break;
        }
        break;
    }
    return c;
}

z80_control_t z80_pla_decode(z80_prefix_t prefix, uint8_t opcode)
{
    if (prefix == PFX_NONE)
        return decode_unprefixed(opcode);

    /* CB/ED/DD/FD/DDCB/FDCB are added in a later stage (task 8). */
    z80_control_t c;
    for (unsigned i = 0; i < sizeof(c); i++) ((uint8_t *)&c)[i] = 0;
    c.valid = false;
    c.exec = EXEC_ILLEGAL;
    return c;
}
