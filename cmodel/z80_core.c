/* ============================================================================
 * z80_core.c - the Z80 CPU module: phase engine, micro-sequencer, bus-cycle
 * setup, and register file accessors.
 *
 * Mirrors rtl/z80_core.v one-to-one. The z80_t struct is this module's
 * registered state (analogous to the Verilog module's internal regs);
 * the public functions below are the only external entry points. The pure
 * combinational submodules called from here — z80_pla(), z80_alu(),
 * z80_timing() — live in their own .c files and match their Verilog
 * counterparts port-for-port.
 *
 * File contents in order:
 *   1. Module-internal inline helpers (formerly z80_internal.h):
 *      register-file accessors, PC post-increment, condition-code eval.
 *   2. Bus-cycle setup (formerly z80_bus.c): z80_busop_base_len(),
 *      z80_start_mcycle(), z80_start_m1(). All file-static.
 *   3. Micro-sequencer (formerly z80_control.c): z80_exec_step() and
 *      its helpers — dispatches on (ctl.exec, m_cycle, phi) to drive the
 *      datapath. File-static.
 *   4. Per-phase engine + reset/init (formerly z80.c): the public API
 *      surface — z80_init, z80_reset, z80_phase_step, z80_set_pc.
 * ==========================================================================*/
#include <string.h>
#include "z80.h"

/* =========================================================================
 * 1. Module-internal helpers (formerly z80_internal.h)
 * =======================================================================*/

static inline uint8_t  z80_A(const z80_t *c) { return (uint8_t)(c->rf[RFP_AF] >> 8); }
static inline uint8_t  z80_F(const z80_t *c) { return (uint8_t)(c->rf[RFP_AF] & 0xFF); }
static inline void z80_setA(z80_t *c, uint8_t v){ c->rf[RFP_AF] = (uint16_t)((c->rf[RFP_AF] & 0x00FF) | ((uint16_t)v << 8)); }
static inline void z80_setF(z80_t *c, uint8_t v){ c->rf[RFP_AF] = (uint16_t)((c->rf[RFP_AF] & 0xFF00) | v); c->f_modified = true; }
static inline uint16_t z80_PC(const z80_t *c){ return c->rf[RFP_PC]; }
static inline uint16_t z80_SP(const z80_t *c){ return c->rf[RFP_SP]; }
static inline void z80_setPC(z80_t *c, uint16_t v){ c->rf[RFP_PC] = v; }
static inline void z80_setSP(z80_t *c, uint16_t v){ c->rf[RFP_SP] = v; }

/* fetch byte at PC and post-increment PC */
static inline uint16_t z80_pc_inc(z80_t *c){
    uint16_t a = c->rf[RFP_PC];
    c->rf[RFP_PC] = (uint16_t)(a + 1);
    return a;
}

/* condition-code evaluation cc[y]: NZ Z NC C PO PE P M */
static inline bool z80_cc_true(uint8_t f, uint8_t cc){
    switch (cc & 7) {
        case 0: return !(f & Z80_ZF);
        case 1: return  (f & Z80_ZF) != 0;
        case 2: return !(f & Z80_CF);
        case 3: return  (f & Z80_CF) != 0;
        case 4: return !(f & Z80_PF);
        case 5: return  (f & Z80_PF) != 0;
        case 6: return !(f & Z80_SF);
        default:return  (f & Z80_SF) != 0;
    }
}

/* forward decls — sequencer / bus helpers are referenced by the phase
   engine below; bus helpers are referenced by the sequencer.            */
static void z80_exec_step(z80_t *c);
static void z80_start_m1(z80_t *cpu);
static void z80_start_mcycle(z80_t *cpu, uint8_t busop, uint16_t addr,
                             uint8_t wdata, uint8_t extra_t);

/* =========================================================================
 * 2. Bus-cycle setup (formerly z80_bus.c)
 * =======================================================================*/


static uint8_t z80_busop_base_len(uint8_t busop)
{
    switch (busop) {
        case BUSOP_M1:   return 4;
        case BUSOP_MRD:  return 3;
        case BUSOP_MWR:  return 3;
        case BUSOP_IORD: return 4; /* T1 T2 Tw T3 */
        case BUSOP_IOWR: return 4;
        case BUSOP_INTA: return 5; /* M1-like + 2 wait states */
        default:         return 0; /* INTERNAL: caller supplies length via extra */
    }
}

static void z80_start_mcycle(z80_t *cpu, uint8_t busop, uint16_t addr,
                      uint8_t wdata, uint8_t extra_t)
{
    cpu->bus_op  = busop;
    cpu->m_addr  = addr;
    cpu->m_wdata = wdata;
    cpu->m_len   = (uint8_t)(z80_busop_base_len(busop) + extra_t);
    cpu->t_state = 1;
    cpu->phi     = 0;
    cpu->m_cycle = (uint8_t)(cpu->m_cycle + 1);
}

/* Begin an opcode-fetch (M1) cycle for the current PC. Used both for a new
   instruction and to continue after a prefix byte. */
static void z80_start_m1(z80_t *cpu)
{
    cpu->bus_op   = BUSOP_M1;
    cpu->m_addr   = cpu->rf[RFP_PC];
    cpu->m_wdata  = 0;
    cpu->m_len    = 4;
    cpu->t_state  = 1;
    cpu->phi      = 0;
    cpu->m_cycle  = 1;
    cpu->ucode    = 0;
    cpu->decoded  = false;
    cpu->instr_done = false;
}

/* =========================================================================
 * 3. Micro-sequencer (formerly z80_control.c)
 *
 * z80_exec_step() is called once per completed M-cycle. Using the decoded
 * control word and the current m_cycle, it performs the datapath action
 * and either sets up the next M-cycle or marks the instruction complete.
 * Structured state machine keyed on (ctl.exec, m_cycle), not a per-opcode
 * behavioral function.
 * =======================================================================*/


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

static void z80_exec_step(z80_t *c)
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

    /* =====================================================================
     * ALU input mux  (mirrors rtl/z80_core.v's always @* alu-input mux).
     *
     * Drives the inputs to the single z80_alu() call below. Defaults track
     * the PLA control word; per-exec overrides match V's case (exec_w).
     * For EXEC_BLOCK the active inputs depend on (cat, m_cycle); the alu
     * output is ignored at M-cycles where the formula is not committed.
     * =================================================================== */
    uint8_t alu_md  = (uint8_t)ctl->flag_mode;
    uint8_t alu_a   = z80_A(c);
    uint8_t alu_b   = 0;
    uint8_t alu_xy  = 0;
    uint8_t alu_bit = ctl->bit_index;
    uint8_t alu_rot = ctl->rot_op;
    switch (ctl->exec) {
        case EXEC_ALU_R:               alu_b = getri(c, ctl->rf_src); break;
        case EXEC_ALU_N: case EXEC_ALU_M: alu_b = c->tmp8; break;
        case EXEC_INC_R: case EXEC_DEC_R: alu_b = getri(c, ctl->rf_dst); break;
        case EXEC_INC_M: case EXEC_DEC_M: alu_b = c->tmp8; break;
        case EXEC_CB_R:  alu_b = getr(c, ctl->rf_src); alu_xy = alu_b; break;
        case EXEC_CB_M:  alu_b = c->tmp8; alu_xy = (uint8_t)(c->rf[RFP_WZ] >> 8); break;
        case EXEC_DDCB: {
            /* mode + rot/bit come from the latched CB opcode in tmph */
            uint8_t cb_op = c->tmph;
            alu_b   = c->tmp8;
            alu_xy  = (uint8_t)(c->rf[RFP_WZ] >> 8);
            alu_md  = ((cb_op >> 6) == CB_BIT) ? (uint8_t)FLAG_BIT : (uint8_t)FLAG_ROT;
            alu_rot = (uint8_t)((cb_op >> 3) & 7);
            alu_bit = (uint8_t)((cb_op >> 3) & 7);
        } break;
        case EXEC_NEG:     alu_md = (uint8_t)FLAG_NEG; break;
        case EXEC_LD_A_IR:
            alu_md  = (uint8_t)FLAG_LD_A_I;
            alu_b   = ctl->aux ? c->reg_r : c->reg_i;
            alu_bit = c->iff2 ? 1u : 0u;
            break;
        case EXEC_IN_C:    alu_md = (uint8_t)FLAG_IN; alu_b = c->tmp8; break;
        case EXEC_RRD: case EXEC_RLD:
            alu_md = (ctl->exec == EXEC_RRD) ? (uint8_t)FLAG_RRD : (uint8_t)FLAG_RLD;
            alu_b  = c->tmp8;
            break;
        case EXEC_BLOCK: {
            /* (cat=aux>>1, dec=aux[0]); mux fires at the M-cycle that
               commits the flag formula. tmp8 holds the byte latched in
               the prior MRD; for BLOCK_CP the current rbyte == tmp8 too. */
            uint8_t cat = (uint8_t)((ctl->aux >> 1) & 0x3);
            bool dec = (ctl->aux & 1) != 0;
            switch (cat) {
                case 0: if (m == 3) {
                    alu_md = (uint8_t)FLAG_BLOCK_LD; alu_b = c->tmp8;
                    alu_bit = ((c->rf[RFP_BC] - 1u) & 0xFFFFu) ? 1u : 0u;
                } break;
                case 1: if (m == 2) {
                    alu_md = (uint8_t)FLAG_BLOCK_CP; alu_b = c->tmp8;
                    alu_bit = ((c->rf[RFP_BC] - 1u) & 0xFFFFu) ? 1u : 0u;
                } break;
                case 2: if (m == 4) {
                    /* INI/IND: data = tmpl, newB = B-1, k = data + C +/- 1 */
                    uint16_t bk = (uint16_t)(c->tmpl +
                                  (uint8_t)(getr(c, REG_C) + (dec ? -1 : 1)));
                    alu_md = (uint8_t)FLAG_BLOCK_IO;
                    alu_a  = c->tmpl;
                    alu_b  = (uint8_t)(getr(c, REG_B) - 1u);
                    alu_xy = (uint8_t)((bk & 0x7u) | ((bk & 0x100u) ? 0x8u : 0u));
                } break;
                default: if (m == 4) { /* cat==3 OUTI/OUTD */
                    /* data = tmpl, newB = current B (already decremented at m=3),
                       k = data + new_L (after HL +/-1) */
                    uint8_t new_l = (uint8_t)((c->rf[RFP_HL] + (dec ? -1 : 1)) & 0xFF);
                    uint16_t bk = (uint16_t)(c->tmpl + new_l);
                    alu_md = (uint8_t)FLAG_BLOCK_IO;
                    alu_a  = c->tmpl;
                    alu_b  = getr(c, REG_B);
                    alu_xy = (uint8_t)((bk & 0x7u) | ((bk & 0x100u) ? 0x8u : 0u));
                } break;
            }
        } break;
        default: break;
    }

    /* Single z80_alu call (mirrors u_alu instance in V). */
    uint8_t alu_res, alu_fout;
    z80_alu(alu_md, (uint8_t)ctl->alu_op, alu_rot, alu_bit, alu_xy,
            alu_a, alu_b, z80_F(c), c->q,
            &alu_res, &alu_fout);

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
            /* mode / rot / bit / xy were set by the alu mux from tmph + WZ */
            uint8_t op = c->tmph, val = RB, res = val;
            uint8_t x = (uint8_t)(op >> 6), yy = (uint8_t)((op >> 3) & 7), zz = (uint8_t)(op & 7);
            if (x == CB_BIT) {
                z80_setF(c, alu_fout);
                finish(c);
            } else {
                if      (x == CB_ROT) { res = alu_res; z80_setF(c, alu_fout); }
                else if (x == CB_RES) res = (uint8_t)(val & ~(1u << yy));
                else                  res = (uint8_t)(val |  (1u << yy));
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
    case EXEC_ALU_R:
        if (ctl->alu_op != ALU_CP) z80_setA(c, alu_res);
        z80_setF(c, alu_fout); finish(c); break;
    case EXEC_INC_R:
        setri(c, ctl->rf_dst, alu_res); z80_setF(c, alu_fout); finish(c); break;
    case EXEC_DEC_R:
        setri(c, ctl->rf_dst, alu_res); z80_setF(c, alu_fout); finish(c); break;
    case EXEC_ROT_A:
        z80_setA(c, alu_res); z80_setF(c, alu_fout); finish(c); break;
    case EXEC_DAA:
        z80_setA(c, alu_res); z80_setF(c, alu_fout); finish(c); break;
    case EXEC_CPL:
        z80_setA(c, alu_res); z80_setF(c, alu_fout); finish(c); break;
    case EXEC_SCF:
        z80_setF(c, alu_fout); finish(c); break;
    case EXEC_CCF:
        z80_setF(c, alu_fout); finish(c); break;

    case EXEC_EX_DE_HL: { uint16_t t = c->rf[RFP_DE]; c->rf[RFP_DE] = c->rf[RFP_HL]; c->rf[RFP_HL] = t; finish(c); break; }
    case EXEC_EX_AF:    {
        /* Swap replaces F with the alternate F — counts as modifying F, so
           Q (= new F) must be committed at instruction end. Sean Young §4.1. */
        uint16_t t = c->rf[RFP_AF]; c->rf[RFP_AF] = c->rf[RFP_AF2]; c->rf[RFP_AF2] = t;
        c->f_modified = true; finish(c); break;
    }
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
        else {
            if (ctl->alu_op != ALU_CP) z80_setA(c, alu_res);
            z80_setF(c, alu_fout); finish(c);
        }
        break;

    /* ---------- (HL) / (IX+d) read ---------- */
    case EXEC_LD_R_M:
        if (mm == 1) z80_start_mcycle(c, BUSOP_MRD, memaddr, 0, 0);
        else { setri(c, ctl->rf_dst, RB); finish(c); }
        break;
    case EXEC_ALU_M:
        if (mm == 1) z80_start_mcycle(c, BUSOP_MRD, memaddr, 0, 0);
        else {
            if (ctl->alu_op != ALU_CP) z80_setA(c, alu_res);
            z80_setF(c, alu_fout); finish(c);
        }
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
        else if (mm == 2) { z80_setF(c, alu_fout);
                           z80_start_mcycle(c, BUSOP_MWR, memaddr, alu_res, 0); }
        else finish(c);
        break;
    case EXEC_DEC_M:
        if (mm == 1) z80_start_mcycle(c, BUSOP_MRD, memaddr, 0, 1);
        else if (mm == 2) { z80_setF(c, alu_fout);
                           z80_start_mcycle(c, BUSOP_MWR, memaddr, alu_res, 0); }
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
        else {
            uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); c->rf[RFP_WZ] = nn;
            if (z80_cc_true(z80_F(c), ctl->cc)) z80_setPC(c, nn);
            finish(c);
        }
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

    /* ---------- CB: rotates/shifts + BIT/RES/SET ----------
       ALU mode (FLAG_ROT for cb_kind=ROT, FLAG_BIT for cb_kind=BIT, FLAG_NONE
       for RES/SET) comes from the PLA via the mux above; for RES/SET the
       sequencer computes the bit-manipulation result directly (alu unused). */
    case EXEC_CB_R: {
        uint8_t val = getr(c, ctl->rf_src), res = val;
        switch (ctl->cb_kind) {
            case CB_ROT: res = alu_res;    setr(c, ctl->rf_dst, res); z80_setF(c, alu_fout); break;
            case CB_BIT:                   z80_setF(c, alu_fout); break;
            case CB_RES: setr(c, ctl->rf_dst, (uint8_t)(val & ~(1u << ctl->bit_index))); break;
            case CB_SET: setr(c, ctl->rf_dst, (uint8_t)(val |  (1u << ctl->bit_index))); break;
        }
        finish(c); break;
    }
    case EXEC_CB_M:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 1); /* 4T read */
        else if (ctl->cb_kind == CB_BIT) {
            z80_setF(c, alu_fout);
            finish(c);
        } else if (m == 2) {
            uint8_t val = RB, res = val;
            switch (ctl->cb_kind) {
                case CB_ROT: res = alu_res; z80_setF(c, alu_fout); break;
                case CB_RES: res = (uint8_t)(val & ~(1u << ctl->bit_index)); break;
                case CB_SET: res = (uint8_t)(val |  (1u << ctl->bit_index)); break;
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
    case EXEC_NEG:
        z80_setA(c, alu_res); z80_setF(c, alu_fout); finish(c); break;
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
        else { z80_setA(c, alu_res); z80_setF(c, alu_fout); finish(c); }
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
            if (ctl->rf_dst != REG_iHL) setr(c, ctl->rf_dst, RB);
            z80_setF(c, alu_fout); finish(c);
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
            uint8_t mem = RB, a = z80_A(c);
            /* new_A and flags from z80_alu (FLAG_RRD/RLD set by the mux).
               new_mem is pure nibble routing — kept here as bus fabric
               until E1 lands the explicit db1/db2 segments. */
            z80_setA(c, alu_res); z80_setF(c, alu_fout);
            uint8_t newMem = (ctl->exec == EXEC_RRD)
                ? (uint8_t)((mem >> 4) | ((a & 0x0F) << 4))
                : (uint8_t)(((mem << 4) & 0xF0) | (a & 0x0F));
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

        /* Pointer/BC updates are IDU work (A1 will move them); flags come
           from z80_alu (FLAG_BLOCK_LD/CP/IO modes; inputs set by the mux). */
        if (cat == 0) {                     /* LDI/LDD/LDIR/LDDR */
            if (m == 1) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 0);
            else if (m == 2) z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_DE], RB, 2); /* 5T write */
            else if (m == 3) {
                uint16_t bc = (uint16_t)(c->rf[RFP_BC] - 1); c->rf[RFP_BC] = bc;
                c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + (dec ? -1 : 1));
                c->rf[RFP_DE] = (uint16_t)(c->rf[RFP_DE] + (dec ? -1 : 1));
                z80_setF(c, alu_fout);
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
                uint16_t bc = (uint16_t)(c->rf[RFP_BC] - 1); c->rf[RFP_BC] = bc;
                c->rf[RFP_WZ] = (uint16_t)(c->rf[RFP_WZ] + (dec ? -1 : 1));
                c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + (dec ? -1 : 1));
                z80_setF(c, alu_fout);
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
                uint8_t newB = (uint8_t)(getr(c, REG_B) - 1); setr(c, REG_B, newB);
                c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + (dec ? -1 : 1));
                z80_setF(c, alu_fout);
                if (rep && newB != 0) { z80_setPC(c, (uint16_t)(z80_PC(c) - 2));
                    /* INIR/INDR repeat: overwrite WZ = PC + 1 during the
                     * 5-T internal M-cycle. Silicon-faithful per
                     * boo-boo et al. 2006 MEMPTR paper + Patrik Rak's
                     * z80memptr (validated on real Spectrum) + Chandler's
                     * NEC + Visual Z80 Remix retest (z80test v1.2a).
                     * Matches chips/z80.h and redcode/Z80. FUSE's
                     * edba_1/edb2_1 expected WZ = BC ± 1 is incorrect vs
                     * silicon — see docs/simplifications.md F1 and
                     * tests/fuse/known-fuse-wrong.txt. */
                    c->rf[RFP_WZ] = (uint16_t)(z80_PC(c) + 1);
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
                c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + (dec ? -1 : 1));
                uint8_t newB = getr(c, REG_B);
                z80_setF(c, alu_fout);
                if (rep && newB != 0) { z80_setPC(c, (uint16_t)(z80_PC(c) - 2));
                    /* OTIR/OTDR repeat: overwrite WZ = PC + 1 during the
                     * 5-T internal M-cycle. Same silicon-faithful basis
                     * as INIR/INDR above. FUSE's edbb_1/edb3_1 expected
                     * WZ = BC ± 1 is incorrect vs silicon. */
                    c->rf[RFP_WZ] = (uint16_t)(z80_PC(c) + 1);
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

/* =========================================================================
 * 4. Per-phase engine, reset, init (formerly z80.c) — public API
 * ========================================================================
 *
 * The engine advances one phase (half T-state) per call. To keep the
 * external pins consistent with (t_state, phi) for tracing, each call:
 *   1) advances from the phase driven last call (handling wait stalls and
 *      end-of-M-cycle dispatch into the micro-sequencer),
 *   2) samples inputs (read-data latch / wait) for the phase about to drive,
 *   3) drives the external pins for that phase.
 * After the call, the pins and (t_state, phi) describe the same phase.
 * =======================================================================*/


/* ---- timing-point predicates ---- */

/* The WAIT *sample edge* — the single phase per (T2 or any inserted Tw) at
   which the silicon latches !wait_n. Memory: T2.N. I/O: the automatic Tw.N
   (= t_state 3 in our IORD/IOWR which has m_len=4 and counts Tw as T3). */
static bool is_wait_sample_phase(const z80_t *c)
{
    if (c->phi != 1) return false;
    switch (c->bus_op) {
        case BUSOP_M1:
        case BUSOP_MRD:
        case BUSOP_MWR:  return c->t_state == 2;
        case BUSOP_IORD:
        case BUSOP_IOWR: return c->t_state == 3;
        default:         return false;
    }
}

static bool is_latch_phase(const z80_t *c)
{
    /* Read-data latch phase: captures the input data bus AT THE FALLING EDGE
       of the last T-state's high half (= our T_last.phi=0 sampling point,
       just before MREQ/IORQ deassert at T_last.phi=1). This matches the
       gate-level transition observed in perfectz80 / Visual Z80 traces. */
    switch (c->bus_op) {
        case BUSOP_M1:   return c->t_state == 2 && c->phi == 1; /* M1 opcode latched at T2.N */
        case BUSOP_INTA: return c->t_state == 2 && c->phi == 1;
        case BUSOP_MRD:  return c->t_state == 3 && c->phi == 0; /* MRD data at T3.P */
        case BUSOP_IORD: return c->t_state == 4 && c->phi == 0; /* IORD data at T4.P */
        default:         return false;
    }
}

static void do_latch(z80_t *c)
{
    switch (c->bus_op) {
    case BUSOP_M1:
        c->ir   = c->pins.data_in;
        c->tmp8 = c->pins.data_in;
        /* refresh counter: only low 7 bits increment */
        c->reg_r = (uint8_t)((c->reg_r & 0x80u) | ((c->reg_r + 1u) & 0x7Fu));
        if (c->suppress_decode) {
            c->suppress_decode = false;      /* ack M1: discard opcode, no PC++/decode */
        } else {
            (void)z80_pc_inc(c);             /* PC advances past opcode */
            c->ctl = z80_pla(c->prefix, c->ir);
            c->decoded = true;
        }
        break;
    case BUSOP_INTA:                          /* interrupt-ack: latch bus byte, refresh */
        c->tmp8 = c->pins.data_in;
        c->reg_r = (uint8_t)((c->reg_r & 0x80u) | ((c->reg_r + 1u) & 0x7Fu));
        break;
    case BUSOP_MRD:
    case BUSOP_IORD:
        c->tmp8 = c->pins.data_in;
        break;
    default: break;
    }
}

/* set up a synthetic M1-style cycle for an interrupt/HALT sequence */
static void start_seq_m1(z80_t *c, z80_exec_t exec, uint8_t len, bool suppress)
{
    c->ctl.exec = exec; c->ctl.idx = 0; c->ctl.use_disp = false;
    c->decoded = true; c->instr_done = false;
    c->bus_op = BUSOP_M1; c->m_addr = c->rf[RFP_PC]; c->m_wdata = 0; c->m_len = len;
    c->t_state = 1; c->phi = 0; c->m_cycle = 1; c->ucode = 0;
    c->suppress_decode = suppress;
}
static void start_seq_inta(z80_t *c, uint8_t len)
{
    c->ctl.exec = EXEC_INT; c->ctl.idx = 0; c->ctl.use_disp = false;
    c->decoded = true; c->instr_done = false;
    c->bus_op = BUSOP_INTA; c->m_addr = c->rf[RFP_PC]; c->m_wdata = 0; c->m_len = len;
    c->t_state = 1; c->phi = 0; c->m_cycle = 1; c->ucode = 0;
}

/* decide what happens at an instruction boundary: bus grant, NMI, INT, HALT, or
   the next opcode fetch. (docs/timing.md interrupt/HALT/BUSREQ sections) */
static void begin_next(z80_t *c)
{
    if (!c->pins.busreq_n) { c->bus_granted = true; return; } /* DMA owns the bus */

    bool allow_int = !c->ei_delay;
    c->ei_delay = false;

    /* Use the silicon sample latches (taken at T_last.P), not the live pin
       state. Silicon's accept decision is locked in one phase before the
       M-cycle boundary; this prevents an edge/level change between T_last.P
       and the boundary from retroactively flipping the decision. */
    if (c->nmi_sampled) {
        c->nmi_sampled = false;
        c->nmi_pending = false;
        if (c->halted) { c->rf[RFP_PC] = (uint16_t)(c->rf[RFP_PC] + 1); c->halted = false; }
        c->iff2 = c->iff1; c->iff1 = false;        /* IFF1->IFF2, disable */
        start_seq_m1(c, EXEC_NMI, 5, true);        /* 5T ack, opcode discarded */
        return;
    }
    if (allow_int && c->int_sampled && c->iff1) {
        c->int_sampled = false;
        if (c->halted) { c->rf[RFP_PC] = (uint16_t)(c->rf[RFP_PC] + 1); c->halted = false; }
        c->iff1 = c->iff2 = false;
        start_seq_inta(c, 7);                      /* INTA ack (IM0/1/2) */
        return;
    }
    if (c->halted) {
        start_seq_m1(c, EXEC_NOP, 4, true);        /* HALT: execute NOPs */
        return;
    }
    z80_start_m1(c);
}

static void advance(z80_t *c)
{
    if (c->phi == 0) {
        c->phi = 1;
        return;
    }
    c->phi = 0;
    if (c->stalled) return;                  /* hold this T-state as a Tw */
    c->t_state = (uint8_t)(c->t_state + 1);
    if (c->t_state > c->m_len) {
        z80_exec_step(c);                    /* set up next M-cycle / finish */
        if (c->instr_done) {
            /* Commit Q: holds F if THIS instruction wrote F, else 0. SCF/CCF
               in the NEXT instruction read it to derive X/Y from (A | Q).
               Per Sean Young's "Undocumented Z80 Documented" §4.1: Q resets
               to 0 after any instruction that does NOT modify F. Verified
               against real-silicon CRC by ZEXALL's <daa,cpl,scf,ccf> test;
               an earlier attempt to make Q persist across non-F-modifying
               instructions broke that CRC (run 27650481834). */
            c->q = c->f_modified ? z80_F(c) : 0;
            c->f_modified = false;
            c->instr_count++;
            c->prefix = PFX_NONE;
            begin_next(c);
        }
    }
}

/* ---- reset ---- */

static void reset_state(z80_t *c)
{
    c->rf[RFP_PC] = 0x0000;
    c->reg_i = 0; c->reg_r = 0; c->ir = 0;
    c->iff1 = c->iff2 = false;
    c->im = 0;
    c->halted = false;
    c->prefix = PFX_NONE;
    c->nmi_pending = false;
    c->prev_nmi_n = true;
    c->nmi_sampled = false;
    c->int_sampled = false;
    c->ei_delay = false;
    c->suppress_decode = false;
    c->bus_granted = false;

    c->t_state = 1; c->phi = 0; c->m_cycle = 1;
    c->bus_op = BUSOP_M1; c->m_len = 4; c->m_addr = 0x0000;
    c->decoded = false; c->instr_done = false; c->ucode = 0;
    c->phase_primed = false; c->stalled = false; c->wait_sampled = false;
    c->q = 0; c->f_modified = false;

    c->pins.m1_n = c->pins.mreq_n = c->pins.iorq_n = 1;
    c->pins.rd_n = c->pins.wr_n = c->pins.rfsh_n = 1;
    c->pins.halt_n = 1; c->pins.busack_n = 1;
    c->pins.data_drive = false;
    c->pins.addr = 0x0000;
    c->pins.data_out = 0;
}

void z80_reset(z80_t *c)
{
    /* programmer-visible registers are undefined on real silicon; force a
       deterministic state for C<->RTL comparison (docs/known-differences.md). */
    for (int i = 0; i < RFP_COUNT; i++) c->rf[i] = 0xFFFF;
    reset_state(c);
}

/* Seed the program counter from outside (e.g. loading a CP/M .com at 0x0100).
   Also updates the pending M1 fetch address so the next opcode is fetched from
   pc rather than the stale reset address. Only valid at an instruction boundary
   (start of an M1 fetch), which is the case right after reset. */
void z80_set_pc(z80_t *c, uint16_t pc)
{
    c->rf[RFP_PC] = pc;
    if (c->bus_op == BUSOP_M1 && c->t_state == 1 && !c->decoded)
        c->m_addr = pc;
}

void z80_init(z80_t *c)
{
    memset(c, 0, sizeof(*c));
    c->pins.reset_n = 1;
    c->pins.wait_n  = 1;
    c->pins.int_n   = 1;
    c->pins.nmi_n   = 1;
    c->pins.busreq_n = 1;
    z80_reset(c);
}

/* ---- per-phase engine ---- */

void z80_phase_step(z80_t *c)
{
    c->cycle++;

    if (!c->pins.reset_n) {
        reset_state(c);
        return;
    }

    /* NMI is edge-triggered: latch a falling edge on nmi_n */
    if (c->prev_nmi_n && !c->pins.nmi_n) c->nmi_pending = true;
    c->prev_nmi_n = c->pins.nmi_n;

    /* bus grant (DMA): while BUSREQ held, release the bus and idle */
    if (c->bus_granted) {
        if (!c->pins.busreq_n) {
            c->pins.busack_n = 0;
            c->pins.m1_n = c->pins.mreq_n = c->pins.iorq_n = 1;
            c->pins.rd_n = c->pins.wr_n = c->pins.rfsh_n = 1;
            c->pins.data_drive = false;
            c->pins.halt_n = c->halted ? 0 : 1;
            return;
        }
        c->bus_granted = false;          /* BUSREQ released: resume */
        c->pins.busack_n = 1;
        z80_start_m1(c);
        c->phase_primed = false;
    }

    if (c->phase_primed)
        advance(c);
    c->phase_primed = true;

    /* WAIT sample + latch (UM0080). Sample only at the spec edge; outside
       that single phase the latch falls to 0 so subsequent .N→.P advances
       run normally. This is the silicon's behavior: each Tw is decided by
       one falling-edge sample. */
    if (is_wait_sample_phase(c))
        c->wait_sampled = (c->pins.wait_n == 0);
    else
        c->wait_sampled = false;
    c->stalled = c->wait_sampled;

    if (!c->stalled && is_latch_phase(c))
        do_latch(c);

    /* NMI / INT silicon sample point: rising edge of the last T-state of
       the current M-cycle (= T_last.P, the .P sub-phase of t_state == m_len)
       per Zilog UM0080. Sample once per M-cycle here; the last M-cycle's
       sample is the one begin_next() reads. While WAIT is asserted we hold
       the previous sample (T-state didn't advance), matching silicon. */
    if (!c->stalled && c->t_state == c->m_len && c->phi == 0) {
        c->nmi_sampled = c->nmi_pending;
        c->int_sampled = !c->pins.int_n;
    }

    z80_timing(c->bus_op, c->t_state, c->phi, c->m_len, c->m_addr, c->m_wdata,
               c->reg_i, c->reg_r,
               &c->pins.addr, &c->pins.data_out, &c->pins.data_drive,
               &c->pins.m1_n, &c->pins.mreq_n, &c->pins.iorq_n,
               &c->pins.rd_n, &c->pins.wr_n,   &c->pins.rfsh_n);

    /* HALT/refresh pin level: halt_n reflects halted state. */
    c->pins.halt_n = c->halted ? 0 : 1;
}
