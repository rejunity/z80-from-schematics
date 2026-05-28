/* ============================================================================
 * z80_control.c - instruction micro-sequencer.
 *
 * Called once per completed M-cycle. Using the decoded control word and the
 * current m_cycle, it performs the datapath action and either sets up the next
 * M-cycle (z80_start_mcycle / z80_start_m1) or marks the instruction complete
 * (instr_done). This is a structured state machine keyed on the control word's
 * exec line + step index, not a per-opcode behavioral function.
 * ==========================================================================*/
#include "z80_internal.h"

/* ---- small datapath helpers ---- */

static uint8_t getr(z80_t *c, uint8_t r) { return z80_get_r8(c, (z80_reg8_t)r); }
static void    setr(z80_t *c, uint8_t r, uint8_t v) { z80_set_r8(c, (z80_reg8_t)r, v); }

static void finish(z80_t *c) { c->instr_done = true; }

static void do_alu(z80_t *c, uint8_t b)
{
    uint8_t a = z80_A(c), res = 0, f = 0;
    uint8_t cin = (z80_F(c) & Z80_CF) ? 1u : 0u;
    switch (c->ctl.alu_op) {
        case ALU_ADD: f = z80_flags_add8(a, b, 0,   0, &res); z80_setA(c, res); break;
        case ALU_ADC: f = z80_flags_add8(a, b, cin, 0, &res); z80_setA(c, res); break;
        case ALU_SUB: f = z80_flags_sub8(a, b, 0,   false, 0, &res); z80_setA(c, res); break;
        case ALU_SBC: f = z80_flags_sub8(a, b, cin, false, 0, &res); z80_setA(c, res); break;
        case ALU_AND: f = z80_flags_logic(ALU_AND, a, b, &res); z80_setA(c, res); break;
        case ALU_XOR: f = z80_flags_logic(ALU_XOR, a, b, &res); z80_setA(c, res); break;
        case ALU_OR:  f = z80_flags_logic(ALU_OR,  a, b, &res); z80_setA(c, res); break;
        case ALU_CP:  f = z80_flags_sub8(a, b, 0, true, 0, &res); break; /* no writeback */
    }
    z80_setF(c, f);
}

static void do_add16(z80_t *c)
{
    uint16_t hl = c->rf[RFP_HL];
    uint16_t rp = c->rf[c->ctl.rp_sel];
    uint32_t r  = (uint32_t)hl + rp;
    uint8_t f = z80_F(c) & (Z80_SF | Z80_ZF | Z80_PF);   /* S Z P/V unaffected */
    if (((hl & 0x0FFF) + (rp & 0x0FFF)) & 0x1000) f |= Z80_HF;
    if (r & 0x10000) f |= Z80_CF;
    uint16_t res = (uint16_t)r;
    f |= (uint8_t)(res >> 8) & (Z80_YF | Z80_XF);
    c->rf[RFP_WZ] = (uint16_t)(hl + 1);
    c->rf[RFP_HL] = res;
    z80_setF(c, f);
}

static void set_prefix_from_ir(z80_t *c)
{
    switch (c->ir) {
        case 0xDD: c->prefix = PFX_DD; break;
        case 0xFD: c->prefix = PFX_FD; break;
        case 0xED: c->prefix = PFX_ED; break;
        case 0xCB:
            c->prefix = (c->prefix == PFX_DD) ? PFX_DDCB :
                        (c->prefix == PFX_FD) ? PFX_FDCB : PFX_CB;
            break;
        default: break;
    }
}

/* convenience for read-byte (the byte latched during the just-finished cycle) */
#define RB (c->tmp8)

void z80_exec_step(z80_t *c)
{
    z80_control_t *ctl = &c->ctl;
    uint8_t m = c->m_cycle;

    switch (ctl->exec) {

    /* ---------- single M-cycle (finish at end of M1) ---------- */
    case EXEC_NOP:
    case EXEC_ILLEGAL:
        finish(c); break;
    case EXEC_PREFIX:
        set_prefix_from_ir(c);
        z80_start_m1(c);            /* continue: fetch the real opcode */
        break;
    case EXEC_DI: c->iff1 = c->iff2 = false; finish(c); break;
    case EXEC_EI: c->iff1 = c->iff2 = true;  finish(c); break;
    case EXEC_HALT: c->halted = true; finish(c); break;

    case EXEC_LD_R_R: setr(c, ctl->rf_dst, getr(c, ctl->rf_src)); finish(c); break;
    case EXEC_ALU_R:  do_alu(c, getr(c, ctl->rf_src)); finish(c); break;
    case EXEC_INC_R: {
        uint8_t res, f = z80_flags_inc8(getr(c, ctl->rf_dst), z80_F(c), &res);
        setr(c, ctl->rf_dst, res); z80_setF(c, f); finish(c); break;
    }
    case EXEC_DEC_R: {
        uint8_t res, f = z80_flags_dec8(getr(c, ctl->rf_dst), z80_F(c), &res);
        setr(c, ctl->rf_dst, res); z80_setF(c, f); finish(c); break;
    }
    case EXEC_ROT_A: {
        uint8_t res, f = z80_flags_rot_a(ctl->rot_op, z80_A(c), z80_F(c), &res);
        z80_setA(c, res); z80_setF(c, f); finish(c); break;
    }
    case EXEC_DAA: { uint8_t res, f = z80_flags_daa(z80_A(c), z80_F(c), &res);
        z80_setA(c, res); z80_setF(c, f); finish(c); break; }
    case EXEC_CPL: { uint8_t res, f = z80_flags_cpl(z80_A(c), z80_F(c), &res);
        z80_setA(c, res); z80_setF(c, f); finish(c); break; }
    case EXEC_SCF: z80_setF(c, z80_flags_scf(z80_A(c), z80_F(c))); finish(c); break;
    case EXEC_CCF: z80_setF(c, z80_flags_ccf(z80_A(c), z80_F(c))); finish(c); break;

    case EXEC_EX_DE_HL: { uint16_t t = c->rf[RFP_DE]; c->rf[RFP_DE] = c->rf[RFP_HL]; c->rf[RFP_HL] = t; finish(c); break; }
    case EXEC_EX_AF:    { uint16_t t = c->rf[RFP_AF]; c->rf[RFP_AF] = c->rf[RFP_AF2]; c->rf[RFP_AF2] = t; finish(c); break; }
    case EXEC_EXX: {
        uint16_t t;
        t = c->rf[RFP_BC]; c->rf[RFP_BC] = c->rf[RFP_BC2]; c->rf[RFP_BC2] = t;
        t = c->rf[RFP_DE]; c->rf[RFP_DE] = c->rf[RFP_DE2]; c->rf[RFP_DE2] = t;
        t = c->rf[RFP_HL]; c->rf[RFP_HL] = c->rf[RFP_HL2]; c->rf[RFP_HL2] = t;
        finish(c); break;
    }
    case EXEC_JP_HL: z80_setPC(c, c->rf[RFP_HL]); finish(c); break;

    /* ---------- 6T / internal-pad single-cycle ---------- */
    case EXEC_INC_RP:
        if (m == 1) { c->rf[ctl->rp_sel] = (uint16_t)(c->rf[ctl->rp_sel] + 1);
                      z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[ctl->rp_sel], 0, 2); }
        else finish(c);
        break;
    case EXEC_DEC_RP:
        if (m == 1) { c->rf[ctl->rp_sel] = (uint16_t)(c->rf[ctl->rp_sel] - 1);
                      z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[ctl->rp_sel], 0, 2); }
        else finish(c);
        break;
    case EXEC_LD_SP_HL:
        if (m == 1) { z80_setSP(c, c->rf[RFP_HL]);
                      z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[RFP_HL], 0, 2); }
        else finish(c);
        break;
    case EXEC_ADD_HL_RP:
        if (m == 1) { do_add16(c); z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[RFP_HL], 0, 7); }
        else finish(c);
        break;

    /* ---------- immediate-8 ---------- */
    case EXEC_LD_R_N:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else { setr(c, ctl->rf_dst, RB); finish(c); }
        break;
    case EXEC_ALU_N:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else { do_alu(c, RB); finish(c); }
        break;

    /* ---------- (HL) read ---------- */
    case EXEC_LD_R_M:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 0);
        else { setr(c, ctl->rf_dst, RB); finish(c); }
        break;
    case EXEC_ALU_M:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 0);
        else { do_alu(c, RB); finish(c); }
        break;

    /* ---------- (HL) write ---------- */
    case EXEC_ST_M_R:
        if (m == 1) z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_HL], getr(c, ctl->rf_src), 0);
        else finish(c);
        break;
    case EXEC_LD_M_N:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_HL], RB, 0);
        else finish(c);
        break;

    /* ---------- (HL) read-modify-write ---------- */
    case EXEC_INC_M:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 1); /* 4T read */
        else if (m == 2) { uint8_t res, f = z80_flags_inc8(RB, z80_F(c), &res);
                           z80_setF(c, f); z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_HL], res, 0); }
        else finish(c);
        break;
    case EXEC_DEC_M:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 1);
        else if (m == 2) { uint8_t res, f = z80_flags_dec8(RB, z80_F(c), &res);
                           z80_setF(c, f); z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_HL], res, 0); }
        else finish(c);
        break;

    /* ---------- immediate-16 ---------- */
    case EXEC_LD_RP_NN:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0); }
        else { c->rf[ctl->rp_sel] = z80_pair_hi_lo(RB, c->tmpl); finish(c); }
        break;
    case EXEC_JP:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0); }
        else { uint16_t nn = z80_pair_hi_lo(RB, c->tmpl);
               z80_setPC(c, nn); c->rf[RFP_WZ] = nn; finish(c); }
        break;
    case EXEC_JP_CC:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0); }
        else { uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); c->rf[RFP_WZ] = nn;
               if (z80_cc_true(z80_F(c), ctl->cc)) z80_setPC(c, nn); finish(c); }
        break;
    case EXEC_LD_A_NN:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0); }
        else if (m == 3) { uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); c->tmp16 = nn;
                           c->rf[RFP_WZ] = (uint16_t)(nn + 1);
                           z80_start_mcycle(c, BUSOP_MRD, nn, 0, 0); }
        else { z80_setA(c, RB); finish(c); }
        break;
    case EXEC_LD_NN_A:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0); }
        else if (m == 3) { uint16_t nn = z80_pair_hi_lo(RB, c->tmpl);
                           c->rf[RFP_WZ] = (uint16_t)(((uint16_t)z80_A(c) << 8) | ((nn + 1) & 0xFF));
                           z80_start_mcycle(c, BUSOP_MWR, nn, z80_A(c), 0); }
        else finish(c);
        break;
    case EXEC_LD_HL_NN:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0); }
        else if (m == 3) { uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); c->tmp16 = nn;
                           c->rf[RFP_WZ] = (uint16_t)(nn + 1);
                           z80_start_mcycle(c, BUSOP_MRD, nn, 0, 0); }
        else if (m == 4) { c->rf[RFP_HL] = (uint16_t)((c->rf[RFP_HL] & 0xFF00) | RB);
                           z80_start_mcycle(c, BUSOP_MRD, (uint16_t)(c->tmp16 + 1), 0, 0); }
        else { c->rf[RFP_HL] = (uint16_t)((c->rf[RFP_HL] & 0x00FF) | ((uint16_t)RB << 8)); finish(c); }
        break;
    case EXEC_LD_NN_HL:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0); }
        else if (m == 3) { uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); c->tmp16 = nn;
                           c->rf[RFP_WZ] = (uint16_t)(nn + 1);
                           z80_start_mcycle(c, BUSOP_MWR, nn, (uint8_t)(c->rf[RFP_HL] & 0xFF), 0); }
        else if (m == 4) z80_start_mcycle(c, BUSOP_MWR, (uint16_t)(c->tmp16 + 1),
                                          (uint8_t)(c->rf[RFP_HL] >> 8), 0);
        else finish(c);
        break;

    /* ---------- (BC)/(DE) <-> A ---------- */
    case EXEC_LD_A_RP:
        if (m == 1) { uint16_t a = c->rf[ctl->rp_sel]; c->rf[RFP_WZ] = (uint16_t)(a + 1);
                      z80_start_mcycle(c, BUSOP_MRD, a, 0, 0); }
        else { z80_setA(c, RB); finish(c); }
        break;
    case EXEC_LD_RP_A:
        if (m == 1) { uint16_t a = c->rf[ctl->rp_sel];
                      c->rf[RFP_WZ] = (uint16_t)(((uint16_t)z80_A(c) << 8) | ((a + 1) & 0xFF));
                      z80_start_mcycle(c, BUSOP_MWR, a, z80_A(c), 0); }
        else finish(c);
        break;

    /* ---------- relative jumps ---------- */
    case EXEC_JR:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { int8_t e = (int8_t)RB; z80_setPC(c, (uint16_t)(z80_PC(c) + e));
                           c->rf[RFP_WZ] = z80_PC(c);
                           z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 5); }
        else finish(c);
        break;
    case EXEC_JR_CC:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) {
            if (z80_cc_true(z80_F(c), ctl->cc)) {
                int8_t e = (int8_t)RB; z80_setPC(c, (uint16_t)(z80_PC(c) + e));
                c->rf[RFP_WZ] = z80_PC(c);
                z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 5);
            } else finish(c);
        } else finish(c);
        break;
    case EXEC_DJNZ:
        if (m == 1) { /* extra M1 T-state */ z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1); }
        else if (m == 2) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 3) {
            uint8_t b = (uint8_t)(getr(c, REG_B) - 1); setr(c, REG_B, b);
            if (b != 0) { int8_t e = (int8_t)RB; z80_setPC(c, (uint16_t)(z80_PC(c) + e));
                          c->rf[RFP_WZ] = z80_PC(c);
                          z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 5); }
            else finish(c);
        } else finish(c);
        break;

    /* ---------- call / ret / rst / push / pop ---------- */
    case EXEC_CALL:
    case EXEC_CALL_CC:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0); }
        else if (m == 3) {
            uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); c->tmp16 = nn; c->rf[RFP_WZ] = nn;
            bool taken = (ctl->exec == EXEC_CALL) || z80_cc_true(z80_F(c), ctl->cc);
            if (taken) z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1);
            else finish(c);
        }
        else if (m == 4) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));
                           z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(z80_PC(c) >> 8), 0); }
        else if (m == 5) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));
                           z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(z80_PC(c) & 0xFF), 0); }
        else { z80_setPC(c, c->tmp16); finish(c); }
        break;
    case EXEC_RET:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
                           z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0); }
        else { z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
               uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); z80_setPC(c, nn); c->rf[RFP_WZ] = nn; finish(c); }
        break;
    case EXEC_RET_CC:
        if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1); /* extra M1 T */
        else if (m == 2) { if (z80_cc_true(z80_F(c), ctl->cc))
                               z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0);
                           else finish(c); }
        else if (m == 3) { c->tmpl = RB; z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
                           z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0); }
        else { z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
               uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); z80_setPC(c, nn); c->rf[RFP_WZ] = nn; finish(c); }
        break;
    case EXEC_RST:
        if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1); /* extra M1 T */
        else if (m == 2) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));
                           z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(z80_PC(c) >> 8), 0); }
        else if (m == 3) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));
                           z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(z80_PC(c) & 0xFF), 0); }
        else { z80_setPC(c, ctl->rst_addr); c->rf[RFP_WZ] = ctl->rst_addr; finish(c); }
        break;
    case EXEC_PUSH:
        if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1);
        else if (m == 2) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));
                           z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(c->rf[ctl->rp_sel] >> 8), 0); }
        else if (m == 3) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));
                           z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(c->rf[ctl->rp_sel] & 0xFF), 0); }
        else finish(c);
        break;
    case EXEC_POP:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
                           z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0); }
        else { z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
               c->rf[ctl->rp_sel] = z80_pair_hi_lo(RB, c->tmpl); finish(c); }
        break;

    /* ---------- EX (SP),HL ---------- */
    case EXEC_EX_SP_HL:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, (uint16_t)(z80_SP(c) + 1), 0, 1); }
        else if (m == 3) { c->tmph = RB;
                           z80_start_mcycle(c, BUSOP_MWR, (uint16_t)(z80_SP(c) + 1), (uint8_t)(c->rf[RFP_HL] >> 8), 0); }
        else if (m == 4) z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(c->rf[RFP_HL] & 0xFF), 2);
        else { c->rf[RFP_HL] = z80_pair_hi_lo(c->tmph, c->tmpl); c->rf[RFP_WZ] = c->rf[RFP_HL]; finish(c); }
        break;

    /* ---------- I/O ---------- */
    case EXEC_IN_A_N:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { uint16_t port = (uint16_t)(((uint16_t)z80_A(c) << 8) | RB);
                           c->rf[RFP_WZ] = (uint16_t)(port + 1);
                           z80_start_mcycle(c, BUSOP_IORD, port, 0, 0); }
        else { z80_setA(c, RB); finish(c); }
        break;
    case EXEC_OUT_N_A:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { uint16_t port = (uint16_t)(((uint16_t)z80_A(c) << 8) | RB);
                           c->rf[RFP_WZ] = (uint16_t)(((uint16_t)z80_A(c) << 8) | ((RB + 1) & 0xFF));
                           z80_start_mcycle(c, BUSOP_IOWR, port, z80_A(c), 0); }
        else finish(c);
        break;

    default:
        finish(c);
        break;
    }
}
