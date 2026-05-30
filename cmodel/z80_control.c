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

/* active "HL" pair for the current instruction (IX/IY under DD/FD) */
static uint8_t hl_pair(const z80_t *c)
{
    return (c->ctl.idx == 1) ? RFP_IX : (c->ctl.idx == 2) ? RFP_IY : RFP_HL;
}

/* index-aware 8-bit read/write: H/L -> IXH/IXL only when DD/FD active and the
   instruction does NOT use a memory operand (docs/undocumented.md). */
static uint8_t getri(z80_t *c, uint8_t r)
{
    if (c->ctl.idx && !c->ctl.use_disp) {
        if (r == REG_H) return (uint8_t)(c->rf[hl_pair(c)] >> 8);
        if (r == REG_L) return (uint8_t)(c->rf[hl_pair(c)] & 0xFF);
    }
    return getr(c, r);
}
static void setri(z80_t *c, uint8_t r, uint8_t v)
{
    if (c->ctl.idx && !c->ctl.use_disp) {
        uint8_t p = hl_pair(c);
        if (r == REG_H) { c->rf[p] = (uint16_t)((c->rf[p] & 0x00FF) | ((uint16_t)v << 8)); return; }
        if (r == REG_L) { c->rf[p] = (uint16_t)((c->rf[p] & 0xFF00) | v); return; }
    }
    setr(c, r, v);
}

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
    uint8_t  dp = hl_pair(c);               /* HL / IX / IY */
    uint16_t hl = c->rf[dp];
    uint16_t rp = c->rf[c->ctl.rp_sel];
    uint32_t r  = (uint32_t)hl + rp;
    uint8_t f = z80_F(c) & (Z80_SF | Z80_ZF | Z80_PF);   /* S Z P/V unaffected */
    if (((hl & 0x0FFF) + (rp & 0x0FFF)) & 0x1000) f |= Z80_HF;
    if (r & 0x10000) f |= Z80_CF;
    uint16_t res = (uint16_t)r;
    f |= (uint8_t)(res >> 8) & (Z80_YF | Z80_XF);
    c->rf[RFP_WZ] = (uint16_t)(hl + 1);
    c->rf[dp] = res;
    z80_setF(c, f);
}

static void do_adc16(z80_t *c, bool sub)
{
    uint16_t hl = c->rf[RFP_HL];
    uint16_t rp = c->rf[c->ctl.rp_sel];
    uint8_t  cin = (z80_F(c) & Z80_CF) ? 1u : 0u;
    c->rf[RFP_WZ] = (uint16_t)(hl + 1);
    uint8_t f = 0;
    uint16_t r16;
    if (!sub) {
        uint32_t res = (uint32_t)hl + rp + cin;
        if (((hl & 0x0FFF) + (rp & 0x0FFF) + cin) & 0x1000) f |= Z80_HF;
        if ((~(hl ^ rp) & (hl ^ (uint16_t)res) & 0x8000)) f |= Z80_PF;
        if (res & 0x10000) f |= Z80_CF;
        r16 = (uint16_t)res;
    } else {
        int hc = (int)(hl & 0x0FFF) - (int)(rp & 0x0FFF) - (int)cin;
        if (hc < 0) f |= Z80_HF;
        uint32_t res = (uint32_t)hl - rp - cin;
        if ((hl ^ rp) & (hl ^ (uint16_t)res) & 0x8000) f |= Z80_PF;
        if (res & 0x10000) f |= Z80_CF;
        f |= Z80_NF;
        r16 = (uint16_t)res;
    }
    if (r16 & 0x8000) f |= Z80_SF;
    if (r16 == 0)     f |= Z80_ZF;
    f |= (uint8_t)(r16 >> 8) & (Z80_YF | Z80_XF);
    c->rf[RFP_HL] = r16;
    z80_setF(c, f);
}

/* ---- block-instruction flag helpers (docs/flags.md) ---- */
static uint8_t block_ld_flags(uint8_t oldF, uint8_t a, uint8_t val, uint16_t bc_after)
{
    uint8_t n = (uint8_t)(a + val);
    uint8_t f = oldF & (Z80_SF | Z80_ZF | Z80_CF);  /* S,Z,C unchanged; H=N=0 */
    if (n & 0x02) f |= Z80_YF;                      /* YF = bit1 of (A+byte) */
    if (n & 0x08) f |= Z80_XF;                      /* XF = bit3 of (A+byte) */
    if (bc_after) f |= Z80_PF;
    return f;
}
static uint8_t block_cp_flags(uint8_t oldF, uint8_t a, uint8_t val, uint16_t bc_after)
{
    uint8_t res = (uint8_t)(a - val);
    uint8_t hf = (((int)(a & 0xF) - (int)(val & 0xF)) < 0) ? Z80_HF : 0;
    uint8_t n  = (uint8_t)(res - (hf ? 1u : 0u));
    uint8_t f  = (oldF & Z80_CF) | Z80_NF;
    if (res & 0x80) f |= Z80_SF;
    if (res == 0)   f |= Z80_ZF;
    f |= hf;
    if (n & 0x02) f |= Z80_YF;
    if (n & 0x08) f |= Z80_XF;
    if (bc_after) f |= Z80_PF;
    return f;
}
static uint8_t block_io_flags(uint8_t data, uint8_t newB, uint16_t k)
{
    uint8_t f = 0;
    if (data & 0x80) f |= Z80_NF;
    if (k > 0xFF)    f |= (Z80_HF | Z80_CF);
    if (z80_parity((uint8_t)((k & 7) ^ newB))) f |= Z80_PF;
    if (newB & 0x80) f |= Z80_SF;
    if (newB == 0)   f |= Z80_ZF;
    f |= newB & (Z80_YF | Z80_XF);
    return f;
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
    uint8_t hlp = hl_pair(c);          /* HL / IX / IY for 16-bit "HL" ops */

    /* (IX+d)/(IY+d) displacement preamble: fetch d (M2), then a 5T address calc
       cycle (M3) for most ops; LD (IX+d),n folds the 2T IX+d compute into its
       N immediate read instead, saving one M-cycle (spec timing 19T not 22T). */
    uint8_t  mm = m;
    uint16_t memaddr = c->rf[hlp];
    if (ctl->idx && ctl->use_disp) {
        if (m == 1) { z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0); return; }
        if (m == 2) {
            int8_t d = (int8_t)c->tmp8;
            c->rf[RFP_WZ] = (uint16_t)(c->rf[hlp] + d);
        }
        if (ctl->exec == EXEC_LD_M_N) {
            mm = (uint8_t)(m - 1);          /* no separate internal cycle */
            memaddr = c->rf[RFP_WZ];
        } else {
            if (m == 2) {
                z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[RFP_WZ], 0, 5);
                return;
            }
            mm = (uint8_t)(m - 2);
            memaddr = c->rf[RFP_WZ];
        }
    }

    switch (ctl->exec) {

    /* ---------- single M-cycle (finish at end of M1) ---------- */
    case EXEC_NOP:
    case EXEC_ILLEGAL:
        finish(c); break;
    case EXEC_PREFIX:
        set_prefix_from_ir(c);
        if (c->prefix == PFX_DDCB || c->prefix == PFX_FDCB) {
            /* DD/FD CB d op: d and op are operand reads, not M1 fetches */
            c->ctl.exec = EXEC_DDCB;
            c->ctl.idx  = (c->prefix == PFX_DDCB) ? 1u : 2u;
            z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);  /* read d */
        } else {
            z80_start_m1(c);        /* continue: fetch the real opcode */
        }
        break;
    case EXEC_DDCB:
        if (m == 2) { c->tmpl = RB;  /* displacement d */
                      z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0); } /* read op */
        else if (m == 3) { c->tmph = RB;  /* CB opcode */
                      c->rf[RFP_WZ] = (uint16_t)(c->rf[hlp] + (int8_t)c->tmpl);
                      z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[RFP_WZ], 0, 2); }
        else if (m == 4) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_WZ], 0, 1); /* read (IX+d) 4T */
        else if (m == 5) {
            uint8_t op = c->tmph, val = RB, res = val;
            uint8_t x = (uint8_t)(op >> 6), yy = (uint8_t)((op >> 3) & 7), zz = (uint8_t)(op & 7);
            if (x == CB_BIT) {
                uint8_t xy = (uint8_t)(c->rf[RFP_WZ] >> 8);
                z80_setF(c, z80_flags_bit(yy, val, xy, z80_F(c)));
                finish(c);
            } else {
                if (x == CB_ROT) { uint8_t f = z80_flags_rot(yy, val, z80_F(c), &res); z80_setF(c, f); }
                else if (x == CB_RES) res = (uint8_t)(val & ~(1u << yy));
                else                  res = (uint8_t)(val | (1u << yy));
                if (zz != REG_iHL) setr(c, zz, res);   /* undocumented register copy */
                z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_WZ], res, 0);
            }
        }
        else finish(c);
        break;
    case EXEC_DI: c->iff1 = c->iff2 = false; finish(c); break;
    case EXEC_EI: c->iff1 = c->iff2 = true; c->ei_delay = true; finish(c); break;
    case EXEC_HALT:
        /* Real Z80: PC stays at the HALT byte (re-fetched each M1 until
           interrupt). Our M1 already incremented PC; back it up so external
           observers see PC at the HALT opcode. begin_next() re-advances PC
           by 1 when an NMI/INT exits the halted state. */
        c->halted = true;
        c->rf[RFP_PC] = (uint16_t)(c->rf[RFP_PC] - 1);
        finish(c);
        break;

    case EXEC_LD_R_R: setri(c, ctl->rf_dst, getri(c, ctl->rf_src)); finish(c); break;
    case EXEC_ALU_R:  do_alu(c, getri(c, ctl->rf_src)); finish(c); break;
    case EXEC_INC_R: {
        uint8_t res, f = z80_flags_inc8(getri(c, ctl->rf_dst), z80_F(c), &res);
        setri(c, ctl->rf_dst, res); z80_setF(c, f); finish(c); break;
    }
    case EXEC_DEC_R: {
        uint8_t res, f = z80_flags_dec8(getri(c, ctl->rf_dst), z80_F(c), &res);
        setri(c, ctl->rf_dst, res); z80_setF(c, f); finish(c); break;
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
    case EXEC_JP_HL: z80_setPC(c, c->rf[hlp]); finish(c); break;

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
        if (m == 1) { z80_setSP(c, c->rf[hlp]);
                      z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[hlp], 0, 2); }
        else finish(c);
        break;
    case EXEC_ADD_HL_RP:
        if (m == 1) { do_add16(c); z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[hlp], 0, 7); }
        else finish(c);
        break;

    /* ---------- immediate-8 ---------- */
    case EXEC_LD_R_N:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else { setri(c, ctl->rf_dst, RB); finish(c); }
        break;
    case EXEC_ALU_N:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else { do_alu(c, RB); finish(c); }
        break;

    /* ---------- (HL) / (IX+d) read ---------- */
    case EXEC_LD_R_M:
        if (mm == 1) z80_start_mcycle(c, BUSOP_MRD, memaddr, 0, 0);
        else { setri(c, ctl->rf_dst, RB); finish(c); }
        break;
    case EXEC_ALU_M:
        if (mm == 1) z80_start_mcycle(c, BUSOP_MRD, memaddr, 0, 0);
        else { do_alu(c, RB); finish(c); }
        break;

    /* ---------- (HL) / (IX+d) write ---------- */
    case EXEC_ST_M_R:
        if (mm == 1) z80_start_mcycle(c, BUSOP_MWR, memaddr, getri(c, ctl->rf_src), 0);
        else finish(c);
        break;
    case EXEC_LD_M_N:
        if (mm == 1) {
            /* For LD (IX+d),n we skipped the preamble's internal 5T cycle and
               fold the 2T IX+d compute into this N read (becomes 5T). */
            uint8_t extra = (ctl->idx && ctl->use_disp) ? 2u : 0u;
            z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, extra);
        }
        else if (mm == 2) z80_start_mcycle(c, BUSOP_MWR, memaddr, RB, 0);
        else finish(c);
        break;

    /* ---------- (HL) / (IX+d) read-modify-write ---------- */
    case EXEC_INC_M:
        if (mm == 1) z80_start_mcycle(c, BUSOP_MRD, memaddr, 0, 1); /* 4T read */
        else if (mm == 2) { uint8_t res, f = z80_flags_inc8(RB, z80_F(c), &res);
                           z80_setF(c, f); z80_start_mcycle(c, BUSOP_MWR, memaddr, res, 0); }
        else finish(c);
        break;
    case EXEC_DEC_M:
        if (mm == 1) z80_start_mcycle(c, BUSOP_MRD, memaddr, 0, 1);
        else if (mm == 2) { uint8_t res, f = z80_flags_dec8(RB, z80_F(c), &res);
                           z80_setF(c, f); z80_start_mcycle(c, BUSOP_MWR, memaddr, res, 0); }
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
        else if (m == 4) { c->rf[hlp] = (uint16_t)((c->rf[hlp] & 0xFF00) | RB);
                           z80_start_mcycle(c, BUSOP_MRD, (uint16_t)(c->tmp16 + 1), 0, 0); }
        else { c->rf[hlp] = (uint16_t)((c->rf[hlp] & 0x00FF) | ((uint16_t)RB << 8)); finish(c); }
        break;
    case EXEC_LD_NN_HL:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0); }
        else if (m == 3) { uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); c->tmp16 = nn;
                           c->rf[RFP_WZ] = (uint16_t)(nn + 1);
                           z80_start_mcycle(c, BUSOP_MWR, nn, (uint8_t)(c->rf[hlp] & 0xFF), 0); }
        else if (m == 4) z80_start_mcycle(c, BUSOP_MWR, (uint16_t)(c->tmp16 + 1),
                                          (uint8_t)(c->rf[hlp] >> 8), 0);
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
                           z80_start_mcycle(c, BUSOP_MWR, (uint16_t)(z80_SP(c) + 1), (uint8_t)(c->rf[hlp] >> 8), 0); }
        else if (m == 4) z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(c->rf[hlp] & 0xFF), 2);
        else { c->rf[hlp] = z80_pair_hi_lo(c->tmph, c->tmpl); c->rf[RFP_WZ] = c->rf[hlp]; finish(c); }
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

    /* ---------- CB: rotates/shifts + BIT/RES/SET ---------- */
    case EXEC_CB_R: {
        uint8_t val = getr(c, ctl->rf_src), res;
        switch (ctl->cb_kind) {
            case CB_ROT: { uint8_t f = z80_flags_rot(ctl->rot_op, val, z80_F(c), &res);
                           setr(c, ctl->rf_dst, res); z80_setF(c, f); } break;
            case CB_BIT: z80_setF(c, z80_flags_bit(ctl->bit_index, val, val, z80_F(c))); break;
            case CB_RES: setr(c, ctl->rf_dst, (uint8_t)(val & ~(1u << ctl->bit_index))); break;
            case CB_SET: setr(c, ctl->rf_dst, (uint8_t)(val | (1u << ctl->bit_index))); break;
        }
        finish(c); break;
    }
    case EXEC_CB_M:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 1); /* 4T read */
        else if (ctl->cb_kind == CB_BIT) {
            uint8_t xy = (uint8_t)(c->rf[RFP_WZ] >> 8);   /* MEMPTR high byte */
            z80_setF(c, z80_flags_bit(ctl->bit_index, RB, xy, z80_F(c)));
            finish(c);
        } else if (m == 2) {
            uint8_t val = RB, res = val;
            switch (ctl->cb_kind) {
                case CB_ROT: { uint8_t f = z80_flags_rot(ctl->rot_op, val, z80_F(c), &res);
                               z80_setF(c, f); } break;
                case CB_RES: res = (uint8_t)(val & ~(1u << ctl->bit_index)); break;
                case CB_SET: res = (uint8_t)(val | (1u << ctl->bit_index)); break;
                default: break;
            }
            z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_HL], res, 0);
        } else finish(c);
        break;

    /* ---------- ED page (non-block) ---------- */
    case EXEC_ADC16:
        if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[RFP_HL], 0, 7);
        else { do_adc16(c, false); finish(c); }
        break;
    case EXEC_SBC16:
        if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[RFP_HL], 0, 7);
        else { do_adc16(c, true); finish(c); }
        break;
    case EXEC_NEG: {
        uint8_t res, f = z80_flags_sub8(0, z80_A(c), 0, false, z80_F(c), &res);
        z80_setA(c, res); z80_setF(c, f); finish(c); break;
    }
    case EXEC_IM: c->im = ctl->aux; finish(c); break;
    case EXEC_RETN:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
                           z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0); }
        else { z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
               uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); z80_setPC(c, nn); c->rf[RFP_WZ] = nn;
               c->iff1 = c->iff2; finish(c); }
        break;
    case EXEC_LD_I_A:
        if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1);
        else { c->reg_i = z80_A(c); finish(c); }
        break;
    case EXEC_LD_R_A:
        if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1);
        else { c->reg_r = z80_A(c); finish(c); }
        break;
    case EXEC_LD_A_IR:
        if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1);
        else {
            uint8_t v = ctl->aux ? c->reg_r : c->reg_i;
            z80_setA(c, v);
            uint8_t f = z80_F(c) & Z80_CF;
            if (v & 0x80) f |= Z80_SF;
            if (v == 0)   f |= Z80_ZF;
            f |= v & (Z80_YF | Z80_XF);
            if (c->iff2)  f |= Z80_PF;
            z80_setF(c, f); finish(c);
        }
        break;
    case EXEC_LD_NNA_RP:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0); }
        else if (m == 3) { uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); c->tmp16 = nn;
                           c->rf[RFP_WZ] = (uint16_t)(nn + 1);
                           z80_start_mcycle(c, BUSOP_MWR, nn, (uint8_t)(c->rf[ctl->rp_sel] & 0xFF), 0); }
        else if (m == 4) z80_start_mcycle(c, BUSOP_MWR, (uint16_t)(c->tmp16 + 1),
                                          (uint8_t)(c->rf[ctl->rp_sel] >> 8), 0);
        else finish(c);
        break;
    case EXEC_LD_RP_NNA:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0); }
        else if (m == 3) { uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); c->tmp16 = nn;
                           c->rf[RFP_WZ] = (uint16_t)(nn + 1);
                           z80_start_mcycle(c, BUSOP_MRD, nn, 0, 0); }
        else if (m == 4) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, (uint16_t)(c->tmp16 + 1), 0, 0); }
        else { c->rf[ctl->rp_sel] = z80_pair_hi_lo(RB, c->tmpl); finish(c); }
        break;
    case EXEC_IN_C:
        if (m == 1) { c->rf[RFP_WZ] = (uint16_t)(c->rf[RFP_BC] + 1);
                      z80_start_mcycle(c, BUSOP_IORD, c->rf[RFP_BC], 0, 0); }
        else {
            uint8_t d = RB;
            if (ctl->rf_dst != REG_iHL) setr(c, ctl->rf_dst, d);
            uint8_t f = z80_F(c) & Z80_CF;
            if (d & 0x80) f |= Z80_SF;
            if (d == 0)   f |= Z80_ZF;
            f |= d & (Z80_YF | Z80_XF);
            if (z80_parity(d)) f |= Z80_PF;
            z80_setF(c, f); finish(c);
        }
        break;
    case EXEC_OUT_C:
        if (m == 1) { c->rf[RFP_WZ] = (uint16_t)(c->rf[RFP_BC] + 1);
                      uint8_t v = (ctl->rf_src == REG_iHL) ? 0 : getr(c, ctl->rf_src);
                      z80_start_mcycle(c, BUSOP_IOWR, c->rf[RFP_BC], v, 0); }
        else finish(c);
        break;
    case EXEC_RRD:
    case EXEC_RLD:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 0);
        else if (m == 2) {
            uint8_t mem = RB, a = z80_A(c), newA, newMem;
            if (ctl->exec == EXEC_RRD) {
                newA   = (uint8_t)((a & 0xF0) | (mem & 0x0F));
                newMem = (uint8_t)((mem >> 4) | ((a & 0x0F) << 4));
            } else {
                newA   = (uint8_t)((a & 0xF0) | ((mem >> 4) & 0x0F));
                newMem = (uint8_t)(((mem << 4) & 0xF0) | (a & 0x0F));
            }
            z80_setA(c, newA);
            uint8_t f = z80_F(c) & Z80_CF;
            if (newA & 0x80) f |= Z80_SF;
            if (newA == 0)   f |= Z80_ZF;
            f |= newA & (Z80_YF | Z80_XF);
            if (z80_parity(newA)) f |= Z80_PF;
            z80_setF(c, f);
            c->rf[RFP_WZ] = (uint16_t)(c->rf[RFP_HL] + 1);
            c->tmpl = newMem;
            z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[RFP_HL], 0, 4);
        }
        else if (m == 3) z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_HL], c->tmpl, 0);
        else finish(c);
        break;

    /* ---------- ED block instructions ---------- */
    case EXEC_BLOCK: {
        uint8_t id  = (uint8_t)(ctl->aux & 7);
        bool    rep = (ctl->aux & BLK_REPEAT) != 0;
        bool    dec = (id & 1) != 0;
        uint8_t cat = (uint8_t)(id >> 1);   /* 0 LD, 1 CP, 2 IN, 3 OUT */

        if (cat == 0) {                     /* LDI/LDD/LDIR/LDDR */
            if (m == 1) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 0);
            else if (m == 2) z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_DE], RB, 2); /* 5T write */
            else if (m == 3) {
                uint8_t val = c->tmp8;
                uint16_t bc = (uint16_t)(c->rf[RFP_BC] - 1); c->rf[RFP_BC] = bc;
                c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + (dec ? -1 : 1));
                c->rf[RFP_DE] = (uint16_t)(c->rf[RFP_DE] + (dec ? -1 : 1));
                z80_setF(c, block_ld_flags(z80_F(c), z80_A(c), val, bc));
                if (rep && bc != 0) { z80_setPC(c, (uint16_t)(z80_PC(c) - 2));
                    c->rf[RFP_WZ] = (uint16_t)(z80_PC(c) + 1);
                    z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 5); }
                else finish(c);
            }
            else finish(c);
        }
        else if (cat == 1) {                /* CPI/CPD/CPIR/CPDR */
            if (m == 1) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 0);
            else if (m == 2) {
                uint8_t val = c->tmp8;
                uint16_t bc = (uint16_t)(c->rf[RFP_BC] - 1); c->rf[RFP_BC] = bc;
                c->rf[RFP_WZ] = (uint16_t)(c->rf[RFP_WZ] + (dec ? -1 : 1));
                c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + (dec ? -1 : 1));
                z80_setF(c, block_cp_flags(z80_F(c), z80_A(c), val, bc));
                z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[RFP_HL], 0, 5);
            }
            else if (m == 3) {
                if (rep && (c->rf[RFP_BC] != 0) && !(z80_F(c) & Z80_ZF)) {
                    z80_setPC(c, (uint16_t)(z80_PC(c) - 2));
                    c->rf[RFP_WZ] = (uint16_t)(z80_PC(c) + 1);
                    z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 5);
                } else finish(c);
            }
            else finish(c);
        }
        else if (cat == 2) {                /* INI/IND/INIR/INDR */
            if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1);
            else if (m == 2) { c->rf[RFP_WZ] = (uint16_t)(c->rf[RFP_BC] + (dec ? -1 : 1));
                               z80_start_mcycle(c, BUSOP_IORD, c->rf[RFP_BC], 0, 0); }
            else if (m == 3) { c->tmpl = RB;   /* data */
                               z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_HL], RB, 0); }
            else if (m == 4) {
                uint8_t data = c->tmpl;
                uint8_t creg = getr(c, REG_C);
                uint8_t newB = (uint8_t)(getr(c, REG_B) - 1); setr(c, REG_B, newB);
                c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + (dec ? -1 : 1));
                uint16_t k = (uint16_t)(data + (uint8_t)(dec ? (creg - 1) : (creg + 1)));
                z80_setF(c, block_io_flags(data, newB, k));
                if (rep && newB != 0) { z80_setPC(c, (uint16_t)(z80_PC(c) - 2));
                    z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 5); }
                else finish(c);
            }
            else finish(c);
        }
        else {                              /* OUTI/OUTD/OTIR/OTDR */
            if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1);
            else if (m == 2) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 0);
            else if (m == 3) {
                c->tmpl = RB;                                  /* data from (HL) */
                uint8_t newB = (uint8_t)(getr(c, REG_B) - 1); setr(c, REG_B, newB);
                c->rf[RFP_WZ] = (uint16_t)(c->rf[RFP_BC] + (dec ? -1 : 1));
                z80_start_mcycle(c, BUSOP_IOWR, c->rf[RFP_BC], c->tmpl, 0); /* BC has new B */
            }
            else if (m == 4) {
                uint8_t data = c->tmpl;
                c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + (dec ? -1 : 1));
                uint8_t l = getr(c, REG_L);
                uint16_t k = (uint16_t)(data + l);
                uint8_t newB = getr(c, REG_B);
                z80_setF(c, block_io_flags(data, newB, k));
                if (rep && newB != 0) { z80_setPC(c, (uint16_t)(z80_PC(c) - 2));
                    z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 5); }
                else finish(c);
            }
            else finish(c);
        }
        break;
    }

    /* ---------- interrupt acceptance sequences ---------- */
    case EXEC_NMI:
        /* m1 was the 5T M1 ack (opcode discarded). push PC, jump to 0x0066. */
        if (m == 1) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));
                      z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(z80_PC(c) >> 8), 0); }
        else if (m == 2) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));
                      z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(z80_PC(c) & 0xFF), 0); }
        else { z80_setPC(c, 0x0066); c->rf[RFP_WZ] = 0x0066; finish(c); }
        break;
    case EXEC_INT:
        /* m1 was the INTA ack (bus byte in tmp8). push PC, then dispatch per IM. */
        if (m == 1) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));
                      z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(z80_PC(c) >> 8), 0); }
        else if (m == 2) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));
                      z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(z80_PC(c) & 0xFF), 0); }
        else if (m == 3) {
            if (c->im == 2) {
                c->tmp16 = (uint16_t)(((uint16_t)c->reg_i << 8) | c->tmp8); /* vector table addr */
                z80_start_mcycle(c, BUSOP_MRD, c->tmp16, 0, 0);
            } else {
                uint16_t target = (c->im == 1) ? 0x0038u
                                  : (uint16_t)(c->tmp8 & 0x38u);  /* IM0: RST from bus opcode */
                z80_setPC(c, target); c->rf[RFP_WZ] = target; finish(c);
            }
        }
        else if (m == 4) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, (uint16_t)(c->tmp16 + 1), 0, 0); }
        else { uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); z80_setPC(c, nn); c->rf[RFP_WZ] = nn; finish(c); }
        break;

    default:
        finish(c);
        break;
    }
}
