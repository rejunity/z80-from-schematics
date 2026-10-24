/* ============================================================================
 * SPDX-FileCopyrightText: 2026 ReJ aka Renaldas Zioma
 * SPDX-License-Identifier: MIT
 *
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
static inline void z80_setF(z80_t *c, uint8_t v){ c->rf[RFP_AF] = (uint16_t)((c->rf[RFP_AF] & 0xFF00) | v); }
static inline uint16_t z80_PC(const z80_t *c){ return c->rf[RFP_PC]; }
static inline uint16_t z80_SP(const z80_t *c){ return c->rf[RFP_SP]; }
static inline void z80_setPC(z80_t *c, uint16_t v){ c->rf[RFP_PC] = v; }
static inline void z80_setSP(z80_t *c, uint16_t v){ c->rf[RFP_SP] = v; }

/* fetch byte at PC and post-increment PC (in IM0 the device supplies the
   operands at PC, which stays: perfectz80) */
static inline uint16_t z80_pc_inc(z80_t *c){
    uint16_t a = c->rf[RFP_PC];
    if (!c->im0_ins) c->rf[RFP_PC] = (uint16_t)(a + 1);
    return a;
}

/* always inlined: the sequencer's step, into end_mcycle() and cfast's
   zf_end_mcycle_in(), which the phase engine and cfast's steps run at every
   M-cycle's end, and the small helpers that step calls there -- inside
   cfast's z80f_run, a function of tens of KB, GCC would call them */
#if defined(__GNUC__)
#define Z80_STEP_INLINE inline __attribute__((always_inline))
#else
#define Z80_STEP_INLINE inline
#endif

/* condition-code evaluation cc[y]: NZ Z NC C PO PE P M */
static Z80_STEP_INLINE bool z80_cc_true(uint8_t f, uint8_t cc){
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
static Z80_STEP_INLINE void z80_exec_step(z80_t *c);
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
        case BUSOP_INTA: return 6; /* M1-like + 2 wait states */
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

static void finish(z80_t *c) { c->instr_done = true; c->im0_ins = false; }

/* ---- register write-back on the die's edges (docs/timing.md "Register
   write-back") ----
   perfectz80 under RESET at every T-state: the die writes a result in the
   low half of a T-state, and a cut ending on that T-state blocks it, so the
   write is kept from the cut ending on the T-state after. Its ALU takes
   the B operand in the instruction's last half-cycle and forms the result
   in the next M1's (INTA's) T1 (low nibble) and T2 (high nibble): the
   register takes it in T2.N, F in T3.N; EX AF,AF' flips in T1.N, EXX and
   EX DE,HL in T2.N. The next instruction first reads a register in its
   own T4. This model ends such an instruction with its writes pending
   (wbp) and makes them on the edge that ends that T-state (wb_fire, from
   advance()):
     - WBP_ALU, the next M1's T2: the 8-bit loads, the ALU, INC / DEC, the
       rotates, NEG, CB and DDCB, IN r,(C), RRD / RLD, LD A,I / R; F of
       INC / DEC (HL), of LDI, and of INI / OUTI when they end. A cut
       ending on that T2 blocks the register write; F takes what the flag
       latches had formed (f_cut);
     - WBP_F1, its T1: F of CPI, and of INI / OUTI when they repeat;
     - WBP_F16, its T2: F of ADD / ADC / SBC HL (pf_val; pf_cut after a cut);
     - WBP_EXAF at T1, WBP_EXX and WBP_EXDH at T2.
   The writes inside an instruction land on the die's edge in the exec step
   (or wb_hooks): a read straight into its register in that read's T3.N --
   LD A,(nn), LD A,(rr), IN A,(n), the 16-bit loads, POP -- which a cut
   ending on that T3 blocks (cut_now); INC / DEC rr, LD SP,HL in M1's T5.N;
   ADD / ADC / SBC HL's L in the first internal M-cycle's T3.N, H in the
   second's; the block pointers in their bus cycle's T2.N, B in its T1.N,
   LDI / CPI's BC in the third M-cycle's T4.N. */
enum { WBP_ALU = 1, WBP_F1 = 2, WBP_F16 = 4, WBP_EXAF = 8, WBP_EXX = 16, WBP_EXDH = 32,
       WBP_DEC = 64 };     /* an interrupt response's decode stands in ctl: theirs in wb_* */

/* F as the die writes it after a cut that ends on the next M1's T2: its
   flag latches as they stand in that M1's T1 (perfectz80). S and Z take
   the result in T2 (Z preset to 1 in the T-state before the M1), P/V there
   too -- in T1 it holds the parity of the result's bits 0-2 --, H and N are
   latched, C per instruction; X and Y come from the internal bus, which
   still carries the ALU's operand. fo / fn: F before / after the step. */
static uint8_t f_cut(const z80_t *c, uint8_t fo, uint8_t fn, uint8_t a, uint8_t b, uint8_t res)
{
    const uint8_t XYF = Z80_YF | Z80_XF;
    uint8_t p3 = (uint8_t)(((res ^ (res >> 1) ^ (res >> 2)) & 1) ? 0 : Z80_PF);
    switch (c->ctl.exec) {
    case EXEC_ALU_R: case EXEC_ALU_N: case EXEC_ALU_M:
        return (uint8_t)((fo & (Z80_SF | Z80_CF)) | Z80_ZF | p3 | (fn & (Z80_HF | Z80_NF)) | (b & XYF));
    case EXEC_INC_R: case EXEC_DEC_R: case EXEC_CB_R:
    case EXEC_INC_M: case EXEC_DEC_M: case EXEC_CB_M: case EXEC_DDCB: {
        if (c->ctl.flag_mode == FLAG_BIT || (c->ctl.exec == EXEC_DDCB && (c->tmph >> 6) == CB_BIT)) {
            uint8_t bit = (uint8_t)(1u << ((c->ctl.exec == EXEC_DDCB ? c->tmph >> 3 : c->ctl.bit_index) & 7));
            uint8_t pb = (uint8_t)((b & bit & 7) ? 0 : Z80_PF);        /* the masked byte's bits 0-2 */
            return (uint8_t)((fo & Z80_SF) | Z80_ZF | pb | (fn & ~(Z80_SF | Z80_ZF | Z80_PF)));
        }
        /* the bus: a register's old value, or (memory) the result written back */
        bool mem = c->ctl.exec == EXEC_INC_M || c->ctl.exec == EXEC_DEC_M ||
                   c->ctl.exec == EXEC_CB_M || c->ctl.exec == EXEC_DDCB;
        return (uint8_t)((fo & Z80_SF) | Z80_ZF | p3 | (fn & (Z80_HF | Z80_NF | Z80_CF)) | ((mem ? res : b) & XYF));
    }
    case EXEC_ROT_A:                           /* S Z P/V untouched */
        return (uint8_t)((fn & ~XYF) | (a & XYF));
    case EXEC_DAA: {                           /* the bus: the correction, 00 06 60 66 */
        uint8_t corr = (uint8_t)((fn & Z80_NF) ? a - res : res - a);
        return (uint8_t)((fo & Z80_SF) | Z80_ZF | p3 | (fn & (Z80_HF | Z80_NF | Z80_CF)) | (corr & XYF));
    }
    case EXEC_CPL:
        return (uint8_t)((fn & ~XYF) | ((a | fo) & XYF));
    case EXEC_SCF: case EXEC_CCF:              /* C waits for T2; H: A's bit 3 */
        return (uint8_t)((fn & ~(Z80_HF | Z80_CF)) | ((a & 0x08) ? Z80_HF : 0) | (fo & Z80_CF));
    case EXEC_IN_C:
        return (uint8_t)((fo & Z80_SF) | Z80_ZF | p3 | (fn & (XYF | Z80_HF | Z80_NF | Z80_CF)));
    case EXEC_NEG:                             /* the bus: A and F's X / Y, as CPL's */
        return (uint8_t)((fo & (Z80_SF | Z80_CF)) | Z80_ZF | p3 | (fn & (Z80_HF | Z80_NF)) | ((a | fo) & XYF));
    case EXEC_LD_A_IR:                         /* the bus: the opcode, 57 / 5F */
        return (uint8_t)((fo & Z80_SF) | Z80_ZF | p3 | (fn & (Z80_HF | Z80_NF | Z80_CF)) | (c->ctl.aux ? Z80_XF : 0));
    case EXEC_BLOCK: {                         /* every flag latched before the next M1 but H: */
        uint8_t cat = (uint8_t)((c->ctl.aux >> 1) & 3);   /* the step's end puts F around it */
        if (cat == 0) return (uint8_t)((((a & 0x0F) + (c->tmp8 & 0x0F)) & 0x10) ? Z80_HF : 0);  /* A + data */
        if (cat == 2) return (uint8_t)((b & 0x0F) ? 0 : Z80_HF);    /* B-1 once more */
        if (cat == 3)                          /* OUTI: k's carry where B-1 borrowed from bit 4 */
            return (uint8_t)(((b & 0x0F) == 0x0F && ((c->tmpl + (c->rf[RFP_HL] & 0xFF)) & 0x100)) ? Z80_HF : 0);
        return 0;
    }
    case EXEC_RRD: case EXEC_RLD: {            /* the bus: the byte written back */
        uint8_t mem = (uint8_t)(c->ctl.exec == EXEC_RRD ? (b >> 4) | (a << 4) : (b << 4) | (a & 0x0F));
        return (uint8_t)((fo & Z80_SF) | Z80_ZF | p3 | (fn & (Z80_HF | Z80_NF | Z80_CF)) | (mem & XYF));
    }
    default:
        return fo;
    }
}

/* the M-cycles with a write wb_hooks() makes before their last edge: EX
   (SP),HL's first write (SP+1 in T1), DJNZ's offset read (B-1 in T2), the
   third M-cycle of LDI (DE in its write's T3), INI and OUTI (B in T2) --
   the rest of wb_hooks()'s writes fall on an M-cycle's last edge
   (end_mcycle). cfast runs these M-cycles in step variants that call
   wb_hooks() on those edges (cfast/dump_tables.c lists them). */
static const uint8_t wb_mid_m[EXEC_ILLEGAL + 1] = {
    [EXEC_EX_SP_HL] = 4, [EXEC_DJNZ] = 3, [EXEC_BLOCK] = 3,
};
static Z80_STEP_INLINE bool wb_mid(const z80_t *c)
{
    return c->m_cycle == wb_mid_m[c->ctl.exec] &&
           (c->ctl.exec != EXEC_BLOCK || ((c->ctl.aux >> 1) & 3) != 1);
}

/* the die's writes inside an M-cycle, on the edge ending T-state t: a
   write in T_k's low half on the edge ending T_k+1 -- in a bus cycle the T2
   writes on the edge after T2, which WAIT can make a Tw's. Those on an
   M-cycle's last edge the exec step makes too, at its start (marked "on
   this edge"); this makes them for a RESET cut that ends inside a WAIT
   (advance) and the ones before the last edge (wb_mid). */
static inline void wb_hooks(z80_t *c, uint8_t t)
{
    uint8_t m = c->m_cycle;
    if (t < 2) {                               /* T1's: EX (SP),HL only */
        if (c->ctl.exec == EXEC_EX_SP_HL && m == 4) z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
        return;
    }
    switch (c->ctl.exec) {
    case EXEC_DJNZ:                            /* B-1 in the offset read's T1.N */
        if (m == 3 && t == 2) setr(c, REG_B, (uint8_t)(getr(c, REG_B) - 1));
        break;
    case EXEC_EX_SP_HL:                        /* SP steps up for the read of (SP+1) (above) and back */
        if (m == 4 && t == 3) z80_setSP(c, (uint16_t)(z80_SP(c) - 1));
        break;
    /* a push's second SP-1 in the first write's T2.N, a pop's SP+1 in each read's */
    case EXEC_PUSH: case EXEC_RST:
        if (m == 3 && t == 3) z80_setSP(c, (uint16_t)(z80_SP(c) - 1));
        break;
    case EXEC_CALL: case EXEC_CALL_CC:
        if (m == 5 && t == 3) z80_setSP(c, (uint16_t)(z80_SP(c) - 1));
        break;
    case EXEC_NMI: case EXEC_INT:
        if (m == 2 && t == 3) z80_setSP(c, (uint16_t)(z80_SP(c) - 1));
        break;
    case EXEC_POP: case EXEC_RET: case EXEC_RETN:
        if ((m == 2 || m == 3) && t == 3) z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
        break;
    case EXEC_RET_CC:
        if ((m == 3 || m == 4) && t == 3) z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
        break;
    case EXEC_BLOCK: {
        uint8_t cat = (uint8_t)((c->ctl.aux >> 1) & 3);
        uint16_t d = (c->ctl.aux & 1) ? 0xFFFFu : 1u;
        if ((cat == 0 || cat == 1) && m == 2 && t == 3)      /* LDI / CPI: HL in the read's T2.N */
            c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + d);
        else if (cat == 0 && m == 3 && t == 3)               /* LDI: DE in the write's */
            c->rf[RFP_DE] = (uint16_t)(c->rf[RFP_DE] + d);
        else if ((cat == 2 || cat == 3) && m == 3 && t == 2) /* INI / OUTI: B in T1.N */
            setr(c, REG_B, (uint8_t)(getr(c, REG_B) - 1));
        else if (cat == 3 && m == 3 && t == 3)               /* OUTI: HL in the read's T2.N */
            c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + d);
        else if (cat == 2 && m == 4 && t == 3)               /* INI: HL in the write's */
            c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + d);
    } break;
    default: break;
    }
}

/* ADD / ADC / SBC HL's sum, at the first internal M-cycle's end: L there,
   H held for the second's end (ph_val), F for the next M1 (pf_val; after
   a cut that ends on its T2, pf_cut) */
static void a16_hold(z80_t *c, uint8_t dp, uint16_t res, uint8_t f, uint8_t fcut)
{
    c->rf[dp] = (uint16_t)((c->rf[dp] & 0xFF00) | (res & 0xFF));
    c->ph_val = (uint8_t)(res >> 8);
    c->pf_val = f;
    c->pf_cut = fcut;
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
    /* a cut on the next M1's T2: C still from the low bytes' sum */
    a16_hold(c, dp, res, f, (uint8_t)((f & ~Z80_CF) | ((((hl & 0xFF) + (rp & 0xFF)) >> 8) ? Z80_CF : 0)));
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
    /* a cut on the next M1's T2: S Z C still from the low bytes, P/V their
       overflow XOR the high byte's bits 0-2 */
    uint8_t l = (uint8_t)hl, r = (uint8_t)rp, hi = (uint8_t)(r16 >> 8);
    uint16_t lo = (uint16_t)(sub ? l - r - cin : l + r + cin);
    uint8_t lr = (uint8_t)lo;
    bool v = sub ? ((l ^ r) & (l ^ lr) & 0x80) : (~(l ^ r) & (l ^ lr) & 0x80);
    bool pv = v != (((hi ^ (hi >> 1) ^ (hi >> 2)) & 1) != 0);
    a16_hold(c, RFP_HL, r16, f, (uint8_t)((lr & Z80_SF) | (lr ? 0 : Z80_ZF) | (pv ? Z80_PF : 0) |
                                          ((lo & 0x100) ? Z80_CF : 0) | (f & (Z80_YF | Z80_HF | Z80_XF | Z80_NF))));
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

/* INI / OUTI's k: the data plus C+1 / C-1 (INI), or plus L as stepped */
static inline uint16_t blk_k(const z80_t *c)
{
    uint8_t v = (((c->ctl.aux >> 1) & 3) == 3) ? (uint8_t)c->rf[RFP_HL]
              : (uint8_t)(c->rf[RFP_BC] + ((c->ctl.aux & 1) ? -1 : 1));
    return (uint16_t)(c->tmpl + v);
}

/* a push's first SP-1: in the T.N that ends the M-cycle before its first
   write (M1's T5, CALL's extra T, the NMI / INT response's first cycle),
   which a RESET cut ending on that T-state blocks (cut_now; the RTL's
   push_cut). The write is set up at SP-1 either way. */
static inline uint16_t push_sp(z80_t *c)
{
    uint16_t sp = (uint16_t)(z80_SP(c) - 1);
    if (!c->cut_now) z80_setSP(c, sp);
    return sp;
}

/* a block instruction repeats: PC back to its first opcode byte, WZ =
   PC+1, an internal M-cycle of 5 T at the old PC */
static inline void blk_repeat(z80_t *c)
{
    z80_setPC(c, (uint16_t)(z80_PC(c) - 2));
    c->rf[RFP_WZ] = (uint16_t)(z80_PC(c) + 1);
    z80_start_mcycle(c, BUSOP_INTERNAL, (uint16_t)(z80_PC(c) + 2), 0, 5);
}

/* =====================================================================
 * ALU input mux and ALU  (mirror rtl/z80_core.v's always @* alu-input mux
 * and its u_alu instance).
 *
 * The mux selects the ALU's inputs: defaults track the PLA control word;
 * per-exec overrides match V's case (exec_w). For EXEC_BLOCK the active
 * inputs depend on (cat, m_cycle); the alu output is ignored at M-cycles
 * where the formula is not committed. Both are combinational, so the
 * sequencer evaluates them when it first reads an output (alu_res() /
 * alu_fout() in z80_exec_step), from the datapath as the M-cycle ended:
 * a step that updates an input before reading the outputs evaluates them
 * first (ALU_EVAL).
 * =================================================================== */
static void alu_eval(z80_t *c, uint8_t *alu_r, uint8_t *alu_fo,
                     uint8_t *alu_ia, uint8_t *alu_ib)
{
    const z80_control_t *ctl = &c->ctl;
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
            /* NMOS race: an INT accepted at the end of LD A,I / LD A,R
               clears IFF2 while the instruction writes P/V from it, so P/V
               reads 0 (perfectz80; an NMI there leaves P/V = IFF2): the
               boundary's decision (wb_pv, end_mcycle) */
            alu_bit = c->wb_pv;
            break;
        case EXEC_IN_C:    alu_md = (uint8_t)FLAG_IN; alu_b = c->tmp8; break;
        case EXEC_RRD: case EXEC_RLD:
            alu_md = (ctl->exec == EXEC_RRD) ? (uint8_t)FLAG_RRD : (uint8_t)FLAG_RLD;
            alu_b  = c->tmp8;
            break;
        case EXEC_BLOCK: {
            /* (cat = aux >> 1): at wb_fire, in the next M1 -- BC, B and HL
               stepped, the byte read in tmp8 (INI / OUTI's data in tmpl) */
            uint8_t cat = (uint8_t)((ctl->aux >> 1) & 0x3);
            if (cat <= 1) {
                alu_md  = cat ? (uint8_t)FLAG_BLOCK_CP : (uint8_t)FLAG_BLOCK_LD;
                alu_b   = c->tmp8;
                alu_bit = c->rf[RFP_BC] ? 1u : 0u;
            } else {
                uint16_t bk = blk_k(c);
                alu_md = (uint8_t)FLAG_BLOCK_IO;
                alu_a  = c->tmpl;
                alu_b  = getr(c, REG_B);
                alu_xy = (uint8_t)((bk & 0x7u) | ((bk & 0x100u) ? 0x8u : 0u));
            }
        } break;
        default: break;
    }

    z80_alu(alu_md, (uint8_t)ctl->alu_op, alu_rot, alu_bit, alu_xy,
            alu_a, alu_b, z80_F(c), c->q, alu_r, alu_fo);
    *alu_ia = alu_a; *alu_ib = alu_b;
}

static Z80_STEP_INLINE void z80_exec_step(z80_t *c)
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
        uint16_t wz0 = c->rf[RFP_WZ];
        if (m == 2) {
            int8_t d = (int8_t)c->tmp8;
            c->rf[RFP_WZ] = (uint16_t)(c->rf[hlp] + d);
        }
        if (ctl->exec == EXEC_LD_M_N) {
            mm = (uint8_t)(m - 1);          /* no separate internal cycle */
            memaddr = c->rf[RFP_WZ];
        } else {
            if (m == 2) {                   /* the address latch holds WZ from before */
                z80_start_mcycle(c, BUSOP_INTERNAL, wz0, 0, 5);
                return;
            }
            mm = (uint8_t)(m - 2);
            memaddr = c->rf[RFP_WZ];
        }
    }

    /* The ALU's outputs (alu_eval above), evaluated on first use: the
       M-cycles that read neither skip the mux and the ALU. Its operands
       (alu_ia / alu_ib) are for f_cut(). */
    uint8_t alu_r = 0, alu_fo = 0, alu_ia = 0, alu_ib = 0;
    bool    alu_valid = false;
#define ALU_EVAL()  ((void)(alu_valid || (alu_eval(c, &alu_r, &alu_fo, &alu_ia, &alu_ib), alu_valid = true)))
#define alu_res()   (ALU_EVAL(), alu_r)
#define alu_fout()  (ALU_EVAL(), alu_fo)

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
        } else if (c->im0_ins) {
            /* IM0: the device hands over the opcode after a prefix, in
               another INTA cycle (perfectz80) */
            z80_start_m1(c);
            c->bus_op = BUSOP_INTA;
            c->m_len  = 6;
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
            uint8_t x = (uint8_t)(op >> 6), yy = (uint8_t)((op >> 3) & 7);
            if (x == CB_BIT) finish(c);              /* F in the next M1 (wb_fire) */
            else {                                   /* ... and the copy to r[z] */
                if      (x == CB_ROT) res = alu_res();
                else if (x == CB_RES) res = (uint8_t)(val & ~(1u << yy));
                else                  res = (uint8_t)(val |  (1u << yy));
                z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_WZ], res, 0);
            }
        }
        else finish(c);
        break;
    case EXEC_DI: c->iff1 = c->iff2 = false; finish(c); break;
    case EXEC_EI: c->iff1 = c->iff2 = true; c->ei_delay = true; finish(c); break;
    case EXEC_HALT:
        /* Silicon-faithful per Brewer 2014 ("Z80 Special Reset") and
         * Woodmass 2021 HALT2INT: PC is incremented past the HALT byte
         * during the HALT M1's normal fetch and STAYS there during the
         * HALT NOP loop. Each HALT-internal NOP re-fetches at PC (now
         * post-HALT-byte) without further increment. INT/NMI then
         * accept that PC as the return address — RETN from an IM 1
         * service routine returns to PC = HALT_addr + 1, which is the
         * documented Z80 HALT-then-INT semantic.
         *
         * Our earlier convention decremented PC back to the HALT byte
         * (matching FUSE test `76`'s pre-Brewer expectation) and then
         * re-incremented in begin_next() on NMI/INT acceptance. That
         * gave the correct final state but the *intermediate* PC + halt
         * pin trace diverged from perfectz80 by 145 / 200 phases on
         * prog13_halt_int. The silicon-faithful convention agrees with
         * perfectz80 exactly. FUSE's test `76` expects PC at the HALT
         * byte; tests/fuse/silicon.expected has the die's value (make
         * fuse_pz80 runs the case on perfectz80). */
        c->halted = true;
        finish(c);
        break;

    /* the 8-bit loads and ALU ops write in the next M1 (wb_fire) */
    case EXEC_LD_R_R: case EXEC_ALU_R: case EXEC_INC_R: case EXEC_DEC_R:
    case EXEC_ROT_A: case EXEC_DAA: case EXEC_CPL: case EXEC_SCF: case EXEC_CCF:
        finish(c); break;

    /* the exchanges flip in the next M1 (wb_fire); EX AF,AF' swaps F in --
       counts as modifying F: Q is the new F (Sean Young §4.1) */
    case EXEC_EX_DE_HL: c->wbp |= WBP_EXDH; finish(c); break;
    case EXEC_EX_AF:    c->wbp |= WBP_EXAF; finish(c); break;
    case EXEC_EXX:      c->wbp |= WBP_EXX;  finish(c); break;
    case EXEC_JP_HL: z80_setPC(c, c->rf[hlp]); finish(c); break;

    /* ---------- 6T / internal-pad single-cycle ---------- */
    /* INC / DEC rr, LD SP,HL: the die writes the pair in M1's T5.N, kept by
       a cut ending on T6 -- the extension's end */
    case EXEC_INC_RP:
    case EXEC_DEC_RP: {
        uint16_t v = (uint16_t)(c->rf[ctl->rp_sel] + (ctl->exec == EXEC_INC_RP ? 1 : -1));
        if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, v, 0, 2);
        else { c->rf[ctl->rp_sel] = v; finish(c); }
        break;
    }
    case EXEC_LD_SP_HL:
        if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[hlp], 0, 2);
        else { z80_setSP(c, c->rf[hlp]); finish(c); }
        break;
    case EXEC_ADD_HL_RP:
        /* the die's M2 and M3: internal M-cycles of 4 and 3 T (BUSREQ can
           take the bus between them; perfectz80) */
        if (m == 1) {                        /* the latch holds HL from before the sum */
            c->rf[RFP_WZ] = (uint16_t)(c->rf[hlp] + 1);
            z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[hlp], 0, 4);
        }
        else if (m == 2) {                   /* L in this M-cycle's T3.N; H and F held */
            do_add16(c);
            z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[RFP_WZ], 0, 3);   /* the latch: WZ = HL+1 */
        }
        else {                               /* H in this one's (cut_now: a cut blocks it) */
            if (!c->cut_now) c->rf[hlp] = (uint16_t)((c->rf[hlp] & 0x00FF) | (uint16_t)c->ph_val << 8);
            c->wbp |= WBP_F16; finish(c);    /* F in the next M1 (wb_fire) */
        }
        break;

    /* ---------- immediate-8 ---------- */
    case EXEC_LD_R_N:
    case EXEC_ALU_N:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else finish(c);                      /* the writes in the next M1 (wb_fire) */
        break;

    /* ---------- (HL) / (IX+d) read ---------- */
    case EXEC_LD_R_M:
    case EXEC_ALU_M:
        if (mm == 1) z80_start_mcycle(c, BUSOP_MRD, memaddr, 0, 0);
        else finish(c);                      /* the writes in the next M1 (wb_fire) */
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
        else if (mm == 2) z80_start_mcycle(c, BUSOP_MWR, memaddr, alu_res(), 0);   /* F in the next M1 */
        else finish(c);
        break;
    case EXEC_DEC_M:
        if (mm == 1) z80_start_mcycle(c, BUSOP_MRD, memaddr, 0, 1);
        else if (mm == 2) z80_start_mcycle(c, BUSOP_MWR, memaddr, alu_res(), 0);
        else finish(c);
        break;

    /* ---------- immediate-16 ---------- */
    case EXEC_LD_RP_NN:                       /* each byte into its register at its read */
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { c->tmpl = RB;
                           if (!c->cut_now) c->rf[ctl->rp_sel] = (uint16_t)((c->rf[ctl->rp_sel] & 0xFF00) | RB);
                           z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0); }
        else { if (!c->cut_now) c->rf[ctl->rp_sel] = (uint16_t)((c->rf[ctl->rp_sel] & 0x00FF) | ((uint16_t)RB << 8));
               finish(c); }
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
        else { if (!c->cut_now) z80_setA(c, RB); finish(c); }
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
        else if (m == 4) { if (!c->cut_now) c->rf[hlp] = (uint16_t)((c->rf[hlp] & 0xFF00) | RB);
                           z80_start_mcycle(c, BUSOP_MRD, (uint16_t)(c->tmp16 + 1), 0, 0); }
        else { if (!c->cut_now) c->rf[hlp] = (uint16_t)((c->rf[hlp] & 0x00FF) | ((uint16_t)RB << 8));
               finish(c); }
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
        else { if (!c->cut_now) z80_setA(c, RB); finish(c); }
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
        else if (m == 2) { int8_t e = (int8_t)RB; uint16_t wz0 = c->rf[RFP_WZ];
                           z80_setPC(c, (uint16_t)(z80_PC(c) + e));
                           c->rf[RFP_WZ] = z80_PC(c);
                           z80_start_mcycle(c, BUSOP_INTERNAL, wz0, 0, 5); }   /* the latch: WZ from before */
        else finish(c);
        break;
    case EXEC_JR_CC:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) {
            if (z80_cc_true(z80_F(c), ctl->cc)) {
                int8_t e = (int8_t)RB; uint16_t wz0 = c->rf[RFP_WZ];
                z80_setPC(c, (uint16_t)(z80_PC(c) + e));
                c->rf[RFP_WZ] = z80_PC(c);
                z80_start_mcycle(c, BUSOP_INTERNAL, wz0, 0, 5);   /* the latch: WZ from before */
            } else finish(c);
        } else finish(c);
        break;
    case EXEC_DJNZ:
        if (m == 1) { /* extra M1 T-state */ z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1); }
        else if (m == 2) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 3) {
            uint8_t b = getr(c, REG_B);         /* B-1 came in this read's T2 (wb_hooks) */
            if (b != 0) { int8_t e = (int8_t)RB; uint16_t wz0 = c->rf[RFP_WZ];
                          z80_setPC(c, (uint16_t)(z80_PC(c) + e));
                          c->rf[RFP_WZ] = z80_PC(c);
                          z80_start_mcycle(c, BUSOP_INTERNAL, wz0, 0, 5); }   /* the latch: WZ from before */
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
        else if (m == 4) z80_start_mcycle(c, BUSOP_MWR, push_sp(c), (uint8_t)(z80_PC(c) >> 8), 0);
        else if (m == 5) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));   /* in this write's T2.N: on this edge */
                           z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(z80_PC(c) & 0xFF), 0); }
        else { z80_setPC(c, c->tmp16); finish(c); }
        break;
    case EXEC_RET:                            /* SP+1 in each read's T2.N: on its last edge */
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0);
        else if (m == 2) { z80_setSP(c, (uint16_t)(z80_SP(c) + 1)); c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0); }
        else { z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
               uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); z80_setPC(c, nn); c->rf[RFP_WZ] = nn; finish(c); }
        break;
    case EXEC_RET_CC:
        if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1); /* extra M1 T */
        else if (m == 2) { if (z80_cc_true(z80_F(c), ctl->cc))
                               z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0);
                           else finish(c); }
        else if (m == 3) { z80_setSP(c, (uint16_t)(z80_SP(c) + 1));   /* SP+1 in each read's T2.N: on its last edge */
                           c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0); }
        else { z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
               uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); z80_setPC(c, nn); c->rf[RFP_WZ] = nn; finish(c); }
        break;
    case EXEC_RST:
        if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1); /* extra M1 T */
        else if (m == 2) z80_start_mcycle(c, BUSOP_MWR, push_sp(c), (uint8_t)(z80_PC(c) >> 8), 0);
        else if (m == 3) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));   /* in this write's T2.N: on this edge */
                           z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(z80_PC(c) & 0xFF), 0); }
        else { z80_setPC(c, ctl->rst_addr); c->rf[RFP_WZ] = ctl->rst_addr; finish(c); }
        break;
    case EXEC_PUSH:
        if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1);
        else if (m == 2) z80_start_mcycle(c, BUSOP_MWR, push_sp(c), (uint8_t)(c->rf[ctl->rp_sel] >> 8), 0);
        else if (m == 3) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));   /* in this write's T2.N: on this edge */
                           z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(c->rf[ctl->rp_sel] & 0xFF), 0); }
        else finish(c);
        break;
    case EXEC_POP:                            /* each byte into its register at its read; */
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0);   /* SP+1 in its T2.N */
        else if (m == 2) { z80_setSP(c, (uint16_t)(z80_SP(c) + 1));   /* on this edge */
                           c->tmpl = RB;
                           if (!c->cut_now) c->rf[ctl->rp_sel] = (uint16_t)((c->rf[ctl->rp_sel] & 0xFF00) | RB);
                           z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0); }
        else { z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
               if (!c->cut_now) c->rf[ctl->rp_sel] = (uint16_t)((c->rf[ctl->rp_sel] & 0x00FF) | ((uint16_t)RB << 8));
               finish(c); }
        break;

    /* ---------- EX (SP),HL ---------- */
    case EXEC_EX_SP_HL:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0);
        else if (m == 2) { c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, (uint16_t)(z80_SP(c) + 1), 0, 1); }
        else if (m == 3) { c->tmph = RB;
                           z80_start_mcycle(c, BUSOP_MWR, (uint16_t)(z80_SP(c) + 1), (uint8_t)(c->rf[hlp] >> 8), 0); }
        else if (m == 4) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));   /* back, in this write's T2.N: on this edge */
                           z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(c->rf[hlp] & 0xFF), 2); }
        else { c->rf[hlp] = z80_pair_hi_lo(c->tmph, c->tmpl); c->rf[RFP_WZ] = c->rf[hlp]; finish(c); }
        break;

    /* ---------- I/O ---------- */
    case EXEC_IN_A_N:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_pc_inc(c), 0, 0);
        else if (m == 2) { uint16_t port = (uint16_t)(((uint16_t)z80_A(c) << 8) | RB);
                           c->rf[RFP_WZ] = (uint16_t)(port + 1);
                           z80_start_mcycle(c, BUSOP_IORD, port, 0, 0); }
        else { if (!c->cut_now) z80_setA(c, RB); finish(c); }
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
    case EXEC_CB_R: finish(c); break;        /* the writes in the next M1 (wb_fire) */
    case EXEC_CB_M:
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 1); /* 4T read */
        else if (ctl->cb_kind == CB_BIT) finish(c);     /* F in the next M1 (wb_fire) */
        else if (m == 2) {
            uint8_t val = RB, res = val;
            switch (ctl->cb_kind) {
                case CB_ROT: res = alu_res(); break;     /* F in the next M1 (wb_fire) */
                case CB_RES: res = (uint8_t)(val & ~(1u << ctl->bit_index)); break;
                case CB_SET: res = (uint8_t)(val |  (1u << ctl->bit_index)); break;
                default: break;
            }
            z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_HL], res, 0);
        } else finish(c);
        break;

    /* ---------- ED page (non-block) ---------- */
    case EXEC_ADC16:                          /* internal M-cycles of 4 and 3 T, as ADD; the */
    case EXEC_SBC16:                          /* sum at the first one's end, where the die has L */
        if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[RFP_HL], 0, 4);
        else if (m == 2) {                   /* L in this M-cycle's T3.N; H and F held */
            do_adc16(c, ctl->exec == EXEC_SBC16);    /* the latch: WZ = HL+1 */
            z80_start_mcycle(c, BUSOP_INTERNAL, c->rf[RFP_WZ], 0, 3);
        }
        else {                               /* H in this one's (cut_now: a cut blocks it) */
            if (!c->cut_now) c->rf[RFP_HL] = (uint16_t)((c->rf[RFP_HL] & 0x00FF) | (uint16_t)c->ph_val << 8);
            c->wbp |= WBP_F16; finish(c);    /* F in the next M1 (wb_fire) */
        }
        break;
    case EXEC_NEG: finish(c); break;        /* the writes in the next M1 (wb_fire) */
    case EXEC_IM: c->im = ctl->aux; finish(c); break;
    case EXEC_RETN:                           /* SP+1 in each read's T2.N: on its last edge */
        if (m == 1) z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0);
        else if (m == 2) { z80_setSP(c, (uint16_t)(z80_SP(c) + 1)); c->tmpl = RB; z80_start_mcycle(c, BUSOP_MRD, z80_SP(c), 0, 0); }
        else { z80_setSP(c, (uint16_t)(z80_SP(c) + 1));
               uint16_t nn = z80_pair_hi_lo(RB, c->tmpl); z80_setPC(c, nn); c->rf[RFP_WZ] = nn;
               /* IFF2 -> IFF1 after this boundary's INT decision, which still
                  sees the old IFF1: INT held in an NMI handler is taken one
                  instruction after RETN (perfectz80). IFF1 implies IFF2, so
                  that is EI's block whenever RETN raises IFF1. */
               if (!c->iff1) c->ei_delay = true;
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
        else finish(c);                      /* A and F in the next M1 (wb_fire) */
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
        else if (m == 4) { c->tmpl = RB;       /* each byte into its register at its read */
                           if (!c->cut_now) c->rf[ctl->rp_sel] = (uint16_t)((c->rf[ctl->rp_sel] & 0xFF00) | RB);
                           z80_start_mcycle(c, BUSOP_MRD, (uint16_t)(c->tmp16 + 1), 0, 0); }
        else { if (!c->cut_now) c->rf[ctl->rp_sel] = (uint16_t)((c->rf[ctl->rp_sel] & 0x00FF) | ((uint16_t)RB << 8));
               finish(c); }
        break;
    case EXEC_IN_C:
        if (m == 1) { c->rf[RFP_WZ] = (uint16_t)(c->rf[RFP_BC] + 1);
                      z80_start_mcycle(c, BUSOP_IORD, c->rf[RFP_BC], 0, 0); }
        else finish(c);                      /* r and F in the next M1 (wb_fire) */
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
            /* new_mem is pure nibble routing; A and F from z80_alu
               (FLAG_RRD / RLD) in the next M1 (wb_fire) */
            uint8_t newMem = (ctl->exec == EXEC_RRD)
                ? (uint8_t)((mem >> 4) | ((a & 0x0F) << 4))
                : (uint8_t)(((mem << 4) & 0xF0) | (a & 0x0F));
            uint16_t wz0 = c->rf[RFP_WZ];    /* the internal cycle's address latch */
            c->rf[RFP_WZ] = (uint16_t)(c->rf[RFP_HL] + 1);
            c->tmpl = newMem;
            z80_start_mcycle(c, BUSOP_INTERNAL, wz0, 0, 4);
        }
        else if (m == 3) z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_HL], c->tmpl, 0);
        else finish(c);
        break;

    /* ---------- ED block instructions ---------- */
    case EXEC_BLOCK: {
        uint8_t id  = (uint8_t)(ctl->aux & 7);
        bool    rep = (ctl->aux & BLK_REPEAT) != 0;
        uint16_t d  = (id & 1) ? 0xFFFFu : 1u;
        uint8_t cat = (uint8_t)(id >> 1);   /* 0 LD, 1 CP, 2 IN, 3 OUT */

        /* the pointers step in a bus cycle's T2.N, B in INI / OUTI's T1.N
           (wb_hooks), BC in LDI / CPI's third M-cycle's T4.N -- on this
           model's M-cycle's last edge, or before it (wb_hooks); F in the
           next M1 (wb_fire: z80_alu's FLAG_BLOCK_LD / CP / IO, after a
           repeat Banks 2018's X / Y from PC, and INIR / OTIR's H, P/V) */
        if (cat == 0) {                     /* LDI/LDD/LDIR/LDDR */
            if (m == 1) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 0);
            else if (m == 2) { c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + d);   /* in the read's T2.N: on this edge */
                               z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_DE], RB, 2); } /* 5T write */
            else if (m == 3) {              /* DE in this write's T2.N (wb_hooks), BC in its T4.N */
                c->rf[RFP_BC] = (uint16_t)(c->rf[RFP_BC] - 1);
                if (rep && c->rf[RFP_BC] != 0) blk_repeat(c);
                else finish(c);
            }
            else finish(c);
        }
        else if (cat == 1) {                /* CPI/CPD/CPIR/CPDR */
            if (m == 1) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 0);
            else if (m == 2) {
                c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + d);  /* in the read's T2.N: on this edge */
                uint16_t wz0 = c->rf[RFP_WZ];   /* the internal cycle's address latch */
                c->rf[RFP_WZ] = (uint16_t)(wz0 + d);
                z80_start_mcycle(c, BUSOP_INTERNAL, wz0, 0, 5);
            }
            else if (m == 3) {              /* BC in this M-cycle's T4.N; Z: A == the byte */
                c->rf[RFP_BC] = (uint16_t)(c->rf[RFP_BC] - 1);
                if (rep && c->rf[RFP_BC] != 0 && z80_A(c) != RB) blk_repeat(c);
                else finish(c);
            }
            else finish(c);
        }
        else if (cat == 2) {                /* INI/IND/INIR/INDR */
            if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1);
            else if (m == 2) { c->rf[RFP_WZ] = (uint16_t)(c->rf[RFP_BC] + d);
                               z80_start_mcycle(c, BUSOP_IORD, c->rf[RFP_BC], 0, 0); }
            else if (m == 3) { c->tmpl = RB;   /* data; B-1 came in this read's T1.N (wb_hooks) */
                               z80_start_mcycle(c, BUSOP_MWR, c->rf[RFP_HL], RB, 0); }
            else if (m == 4) {
                c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + d);  /* in the write's T2.N: on this edge */
                if (rep && getr(c, REG_B) != 0) blk_repeat(c);
                else finish(c);
            }
            else finish(c);
        }
        else {                              /* OUTI/OUTD/OTIR/OTDR */
            if (m == 1) z80_start_mcycle(c, BUSOP_INTERNAL, z80_PC(c), 0, 1);
            else if (m == 2) z80_start_mcycle(c, BUSOP_MRD, c->rf[RFP_HL], 0, 0);
            else if (m == 3) {
                c->rf[RFP_HL] = (uint16_t)(c->rf[RFP_HL] + d);  /* in this read's T2.N: on this edge */
                c->tmpl = RB;                                  /* data from (HL); B-1 came in its T1.N (wb_hooks) */
                c->rf[RFP_WZ] = (uint16_t)(c->rf[RFP_BC] + d);
                z80_start_mcycle(c, BUSOP_IOWR, c->rf[RFP_BC], c->tmpl, 0); /* BC has new B */
            }
            else if (m == 4) {
                if (rep && getr(c, REG_B) != 0) blk_repeat(c);
                else finish(c);
            }
            else finish(c);
        }
        break;
    }

    /* ---------- interrupt acceptance sequences ---------- */
    case EXEC_NMI:
        /* m1 was the 5T M1 ack (opcode discarded). push PC, jump to 0x0066. */
        if (m == 1) z80_start_mcycle(c, BUSOP_MWR, push_sp(c), (uint8_t)(z80_PC(c) >> 8), 0);
        else if (m == 2) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));   /* in this write's T2.N: on this edge */
                           z80_start_mcycle(c, BUSOP_MWR, z80_SP(c), (uint8_t)(z80_PC(c) & 0xFF), 0); }
        else { z80_setPC(c, 0x0066); c->rf[RFP_WZ] = 0x0066; finish(c); }
        break;
    case EXEC_INT:
        /* m1 was the INTA ack (bus byte in tmp8). push PC, then dispatch per IM. */
        if (m == 1) z80_start_mcycle(c, BUSOP_MWR, push_sp(c), (uint8_t)(z80_PC(c) >> 8), 0);
        else if (m == 2) { z80_setSP(c, (uint16_t)(z80_SP(c) - 1));   /* in this write's T2.N: on this edge */
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
    (void)alu_ia; (void)alu_ib;
#undef alu_res
#undef alu_fout
#undef ALU_EVAL
}

/* a block instruction's F after a repeat (Banks 2018): X / Y from PC, as
   rewound; INIR / OTIR's H and P/V from k and B-1 as well */
static uint8_t blk_rep_f(const z80_t *c, uint8_t f)
{
    uint8_t pch = (uint8_t)(z80_PC(c) >> 8);
    if (((c->ctl.aux >> 1) & 3) <= 1)
        return (uint8_t)((f & ~(Z80_YF | Z80_XF)) | (pch & (Z80_YF | Z80_XF)));
    uint8_t  data = c->tmpl, nb = (uint8_t)(c->rf[RFP_BC] >> 8);
    uint16_t k = blk_k(c);
    uint8_t  nf = (uint8_t)((data >> 6) & Z80_NF);
    uint8_t  p  = (uint8_t)((k & 7) ^ nb);
    f = (uint8_t)((nb & Z80_SF) | (pch & (Z80_YF | Z80_XF)) | nf);
    if (k > 255) {
        f |= Z80_CF;
        if (nf) {
            if ((nb & 0xF) == 0) f |= Z80_HF;
            if (z80_parity((uint8_t)(p ^ ((nb - 1) & 7)))) f |= Z80_PF;
        } else {
            if ((nb & 0xF) == 0xF) f |= Z80_HF;
            if (z80_parity((uint8_t)(p ^ ((nb + 1) & 7)))) f |= Z80_PF;
        }
    } else if (z80_parity((uint8_t)(p ^ (nb & 7)))) f |= Z80_PF;
    return f;
}

/* the pending instruction's writes (WBP_ALU at T2, WBP_F1 at T1): its decode
   (back from wb_exec / wb_idx / wb_disp after an interrupt response's,
   WBP_DEC), the ALU again from the operands as it left them -- the
   registers and F, the byte read in tmp8. A cut ending on this edge blocks
   them, but F at T2: f_cut. Q is the F written, or 0. As z80_core.v's
   fire / fire1. */
static void wb_fire_op(z80_t *c, bool f1, bool cut)
{
    z80_control_t *ctl = &c->ctl;
    z80_exec_t ex = ctl->exec;
    uint8_t ix = ctl->idx;
    bool dp = ctl->use_disp, dec = (c->wbp & WBP_DEC) != 0;
    if (dec) { ctl->exec = (z80_exec_t)c->wb_exec; ctl->idx = c->wb_idx; ctl->use_disp = c->wb_disp; }
    uint8_t r = 0, fn = 0, a = 0, b = 0, fo = z80_F(c);
    bool ld = ctl->exec == EXEC_LD_R_R || ctl->exec == EXEC_LD_R_N || ctl->exec == EXEC_LD_R_M;
    if (!ld) alu_eval(c, &r, &fn, &a, &b);   /* the loads don't use it */
    bool fw = true;
    switch (ctl->exec) {
    case EXEC_LD_R_R:
        if (!cut) setri(c, ctl->rf_dst, getri(c, ctl->rf_src));
        fw = false; break;
    case EXEC_LD_R_N: case EXEC_LD_R_M:
        if (!cut) setri(c, ctl->rf_dst, c->tmp8);
        fw = false; break;
    case EXEC_ALU_R: case EXEC_ALU_N: case EXEC_ALU_M:
        if (!cut && ctl->alu_op != ALU_CP) z80_setA(c, r);
        break;
    case EXEC_INC_R: case EXEC_DEC_R:
        if (!cut) setri(c, ctl->rf_dst, r);
        break;
    case EXEC_SCF: case EXEC_CCF: case EXEC_INC_M: case EXEC_DEC_M:
        break;
    case EXEC_CB_R: {                        /* rotate, RES, SET r: the bit unit */
        uint8_t v = getr(c, ctl->rf_src), bm = (uint8_t)(1u << ctl->bit_index);
        if (!cut && ctl->cb_kind != CB_BIT)
            setr(c, ctl->rf_dst, ctl->cb_kind == CB_ROT ? r : ctl->cb_kind == CB_RES ? (uint8_t)(v & ~bm) : (uint8_t)(v | bm));
        fw = ctl->cb_kind == CB_ROT || ctl->cb_kind == CB_BIT;
    } break;
    case EXEC_CB_M:
        fw = ctl->cb_kind == CB_ROT || ctl->cb_kind == CB_BIT;
        break;
    case EXEC_DDCB: {                        /* F, and the undocumented copy to r[z] */
        uint8_t k = (uint8_t)(c->tmph >> 6), z = (uint8_t)(c->tmph & 7);
        uint8_t bm = (uint8_t)(1u << ((c->tmph >> 3) & 7));
        if (!cut && k != CB_BIT && z != REG_iHL)
            setr(c, z, k == CB_ROT ? r : k == CB_RES ? (uint8_t)(c->tmp8 & ~bm) : (uint8_t)(c->tmp8 | bm));
        fw = k == CB_ROT || k == CB_BIT;
    } break;
    case EXEC_IN_C:
        if (!cut && ctl->rf_dst != REG_iHL) setr(c, ctl->rf_dst, c->tmp8);
        break;
    case EXEC_BLOCK:
        if (c->wb_rep) fn = blk_rep_f(c, fn);
        break;
    default:                                 /* ROT_A DAA CPL NEG LD_A_IR RRD RLD */
        if (!cut) z80_setA(c, r);
        break;
    }
    if (fw && (!cut || !f1)) {
        uint8_t f = fn;
        if (cut) {
            uint8_t fc = f_cut(c, fo, fn, a, b, r);
            f = (ctl->exec == EXEC_BLOCK) ? (uint8_t)((fn & ~Z80_HF) | fc) : fc;
        }
        z80_setF(c, f);
    }
    if (!cut) c->q = fw ? fn : 0;
    if (dec) { ctl->exec = ex; ctl->idx = ix; ctl->use_disp = dp; }
}

/* the pending writes on the edge that ends T-state t (1, 2) of the next M1
   or INTA cycle (advance; cfast's M1 steps); cut: a RESET cut ends on it */
static void wb_fire(z80_t *c, uint8_t t, bool cut)
{
    uint8_t p = c->wbp;
    if (t == 1) {
        if (p & WBP_F1) wb_fire_op(c, true, cut);
        if ((p & WBP_EXAF) && !cut) {
            uint16_t x = c->rf[RFP_AF]; c->rf[RFP_AF] = c->rf[RFP_AF2]; c->rf[RFP_AF2] = x;
        }
        c->wbp = (uint8_t)(p & ~(WBP_F1 | WBP_EXAF));
        if (!(c->wbp & ~WBP_DEC)) c->wbp = 0;
        return;
    }
    if (p & WBP_ALU) wb_fire_op(c, false, cut);
    if (p & WBP_F16) {
        z80_setF(c, cut ? c->pf_cut : c->pf_val);
        if (!cut) c->q = c->pf_val;
    }
    if (!cut && (p & WBP_EXX))
        for (uint8_t i = RFP_BC; i <= RFP_HL; i++) {
            uint16_t x = c->rf[i]; c->rf[i] = c->rf[i + RFP_BC2 - RFP_BC]; c->rf[i + RFP_BC2 - RFP_BC] = x;
        }
    if (!cut && (p & WBP_EXDH)) {
        uint16_t x = c->rf[RFP_DE]; c->rf[RFP_DE] = c->rf[RFP_HL]; c->rf[RFP_HL] = x;
    }
    c->wbp = 0;
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
   (= t_state 3 in our IORD/IOWR which has m_len=4 and counts Tw as T3).
   INTA: second automatic Tw.N, counted as t_state 4. */
static bool is_wait_sample_phase(const z80_t *c)
{
    if (c->phi != 1) return false;
    switch (c->bus_op) {
        case BUSOP_M1:
        case BUSOP_MRD:
        case BUSOP_MWR:  return c->t_state == 2;
        case BUSOP_IORD:
        case BUSOP_IOWR: return c->t_state == 3;
        case BUSOP_INTA: return c->t_state == 4;
        default:         return false;
    }
}

static bool is_latch_phase(const z80_t *c)
{
    /* Read-data latch: the step into the phase named here takes data_in,
       which is still the bus of the half-cycle before it (z80_sys_phase).
       The die (perfectz80; event sweep `latch`, docs/timing.md "The data
       latch") takes
         - an opcode at the rising edge that ends T2 -- or the last Tw --
           from the bus in its low half: the step into T3.P;
         - an INTA byte the same way at the end of the second automatic
           Tw (t_state 4): the step into T3.P, t_state 5;
         - read data at the falling edge of T3 from the bus in T3's high
           half: the step into T3.N (t_state 3 of an MRD, 4 of an IORD,
           whose automatic Tw is t_state 3).
       The RTL's latch_pos / latch_neg (z80_core.v) are the same edges. */
    switch (c->bus_op) {
        case BUSOP_M1:   return c->t_state == 3 && c->phi == 0;
        case BUSOP_INTA: return c->t_state == 5 && c->phi == 0;
        case BUSOP_MRD:  return c->t_state == 3 && c->phi == 1;
        case BUSOP_IORD: return c->t_state == 4 && c->phi == 1;
        default:         return false;
    }
}

/* The rising edge that ends the refresh -- T4 of an M1, the second refresh
   T-state of an INTA: R steps (low 7 bits) there, so the refresh address
   shows I:R from before the step, as on silicon (perfectz80). advance()
   steps it before the execute step, so LD R,A's write wins. */
static bool is_refresh_end(const z80_t *c)
{
    return (c->bus_op == BUSOP_M1 && c->t_state == 4) ||
           (c->bus_op == BUSOP_INTA && c->t_state == 6);
}

/* Internal T-states drive no new address: the bus keeps the last one -- the
   refresh address after an M1, the operand address after a read
   (perfectz80; the Spectrum's contention tables, "ir:1", "hl:1") -- and so
   do the T-states an M1 or INTA runs past its refresh (an M1's T5 / T6, IM1
   / IM2's seventh). m_addr still carries the IDU / EA work. */
static bool is_addr_held(const z80_t *c)
{
    return c->bus_op == BUSOP_INTERNAL ||
           (c->bus_op == BUSOP_M1 && c->t_state >= 5) ||
           (c->bus_op == BUSOP_INTA && c->t_state >= 7);
}

/* ---- the phase's timing ROM entries ----
   The predicates above, like the timing module's pins (z80_timing_rom),
   depend on (bus_op, t_state, phi) alone: tpoint_rom tabulates them under
   the same index (built by z80_reset), so a phase reads one byte for its
   timing points and one ROM entry for its pins. */
enum { TP_WAIT_SAMPLE = 1, TP_LATCH = 2, TP_ADDR_HELD = 4 };
static uint8_t tpoint_rom[Z80_TIMING_ROM_SIZE];
static bool    tpoint_rom_built;

static void tpoint_rom_init(void)
{
    if (tpoint_rom_built) return;
    z80_t t;
    memset(&t, 0, sizeof t);
    for (unsigned op = 0; op < 8; op++)
        for (unsigned ts = 0; ts < 16; ts++)
            for (unsigned phi = 0; phi < 2; phi++) {
                t.bus_op = (uint8_t)op; t.t_state = (uint8_t)ts; t.phi = (uint8_t)phi;
                tpoint_rom[Z80_TIMING_ROM_INDEX(op, ts, phi)] = (uint8_t)
                    ((is_wait_sample_phase(&t) ? TP_WAIT_SAMPLE : 0) |
                     (is_latch_phase(&t) ? TP_LATCH : 0) |
                     (is_addr_held(&t) ? TP_ADDR_HELD : 0));
            }
    tpoint_rom_built = true;
}

static inline unsigned rom_index(const z80_t *c)
{
    return Z80_TIMING_ROM_INDEX(c->bus_op, c->t_state > 15 ? 15 : c->t_state, c->phi);
}

/* The timing module's pins for the phase: z80_timing() of (bus_op, t_state,
   phi, m_addr, m_wdata, I, R), read from its ROM. */
static inline void drive_pins(z80_t *c, unsigned rom)
{
    const z80_timing_rom_t *e = &z80_timing_rom[rom];
    memcpy((uint8_t *)&c->pins + Z80_TIMING_ROM_FIRST, e->lvl, Z80_TIMING_ROM_LEN);
    c->pins.data_out = c->m_wdata;
    c->pins.addr = e->refresh ? (uint16_t)((c->reg_i << 8) | c->reg_r) : c->m_addr;
}

/* The data latch of each bus cycle: the byte on D0-D7 at the cycle's latch
   point (is_latch_phase). do_latch() picks the cycle's own. */
static inline void latch_m1(z80_t *c)
{
    c->ir   = c->pins.data_in;
    c->tmp8 = c->pins.data_in;
    if (c->srst_m1) {
        /* special reset (perfectz80): the fetch after the one whose T2
           rising edge saw RESET runs as a NOP that leaves PC at 0000;
           I, R, the IFFs and IM are kept */
        c->srst_m1 = false;
        c->ir = 0x00;
        c->rf[RFP_PC] = 0x0000;
        if (!c->suppress_decode) {
            c->ctl = z80_pla_rom[c->prefix][c->ir];
            c->decoded = true;
        }
        c->suppress_decode = false;
    } else if (c->suppress_decode) {
        c->suppress_decode = false;      /* ack M1: discard opcode, no PC++/decode */
    } else {
        (void)z80_pc_inc(c);             /* PC advances past opcode */
        c->ctl = z80_pla_rom[c->prefix][c->ir];
        c->decoded = true;
    }
}
static inline void latch_inta(z80_t *c)  /* interrupt-ack: latch bus byte, refresh */
{
    c->tmp8 = c->pins.data_in;
    if (c->im == 0) {
        /* IM0: the device supplies the opcode, decoded without a fetch
           / PC++; until it ends, PC stays (z80_pc_inc, EXEC_PREFIX) */
        c->ir = c->pins.data_in;
        c->ctl = z80_pla_rom[c->prefix][c->ir];
        c->decoded = true; /* supplied opcode consumes no memory byte */
        c->im0_ins = true;
    }
}
static inline void latch_rd(z80_t *c)    /* MRD / IORD */
{
    c->tmp8 = c->pins.data_in;
}

static void do_latch(z80_t *c)
{
    switch (c->bus_op) {
    case BUSOP_M1:   latch_m1(c);   break;
    case BUSOP_INTA: latch_inta(c); break;
    case BUSOP_MRD:
    case BUSOP_IORD: latch_rd(c);   break;
    default: break;
    }
}

/* an interrupt response's (or a halt's) decode replaces the instruction's,
   which the writes it left pending still need (wb_fire_op): it waits in
   wb_exec / wb_idx / wb_disp. The RTL keeps the opcode and prefix it
   decodes from until the fire. */
static inline void wb_keep_decode(z80_t *c)
{
    if (c->wbp && !(c->wbp & WBP_DEC)) {
        c->wb_exec = (uint8_t)c->ctl.exec; c->wb_idx = c->ctl.idx; c->wb_disp = c->ctl.use_disp;
        c->wbp |= WBP_DEC;
    }
}
/* set up a synthetic M1-style cycle for an interrupt/HALT sequence */
static void start_seq_m1(z80_t *c, z80_exec_t exec, uint8_t len, bool suppress)
{
    wb_keep_decode(c);
    c->ctl.exec = exec; c->ctl.idx = 0; c->ctl.use_disp = false;
    c->decoded = true; c->instr_done = false;
    c->bus_op = BUSOP_M1; c->m_addr = c->rf[RFP_PC]; c->m_wdata = 0; c->m_len = len;
    c->t_state = 1; c->phi = 0; c->m_cycle = 1; c->ucode = 0;
    c->suppress_decode = suppress;
}
static void start_seq_inta(z80_t *c, uint8_t len)
{
    wb_keep_decode(c);
    c->ctl.exec = EXEC_INT; c->ctl.idx = 0; c->ctl.use_disp = false;
    c->decoded = true; c->instr_done = false;
    c->bus_op = BUSOP_INTA; c->m_addr = c->rf[RFP_PC]; c->m_wdata = 0; c->m_len = len;
    c->t_state = 1; c->phi = 0; c->m_cycle = 1; c->ucode = 0;
}

/* decide what happens at an instruction boundary: bus grant, NMI, INT, HALT, or
   the next opcode fetch. (docs/timing.md interrupt/HALT/BUSREQ sections) */
static void begin_next(z80_t *c)
{
    c->nmi_resp = false;

    bool allow_int = !c->ei_delay;
    c->ei_delay = false;

    /* Use the silicon sample latches (taken at T_last.P), not the live pin
       state. Silicon's accept decision is locked in one phase before the
       M-cycle boundary; this prevents an edge/level change between T_last.P
       and the boundary from retroactively flipping the decision. */
    if (c->nmi_sampled) {
        c->nmi_sampled = false;
        c->nmi_pending = false;
        /* Silicon-faithful HALT exit: PC is ALREADY past the HALT byte
         * (set when HALT's M1 committed; see EXEC_HALT). Just clear the
         * halted flag and start the NMI ack sequence -- no PC bump. */
        if (c->halted) c->halted = false;
        /* IFF1 off; IFF2 keeps what EI / DI / RETN last set (perfectz80):
           it holds IFF1's value from before the NMI because the two only
           part here -- and a nested NMI leaves IFF2 alone, so the RETNs
           bring back the enable from before the first NMI */
        c->iff1 = false;
        c->nmi_resp = true;
        start_seq_m1(c, EXEC_NMI, 5, true);        /* 5T ack, opcode discarded */
        return;
    }
    if (allow_int && c->int_sampled && c->iff1) {
        c->int_sampled = false;
        /* Same: HALT exit without PC bump. */
        if (c->halted) c->halted = false;
        c->iff1 = c->iff2 = false;
        start_seq_inta(c, c->im == 0 ? 6 : 7);                      /* INTA ack (IM0/1/2) */
        return;
    }
    if (c->halted) {
        start_seq_m1(c, EXEC_NOP, 4, true);        /* HALT: execute NOPs */
        c->srst_m1 = c->sres;
        return;
    }
    z80_start_m1(c);
    c->srst_m1 = c->sres;
}

/* the classes whose writes the die makes in the next M1 (wb_fire); BLOCK's
   by its kind and whether it repeated */
static const uint8_t wb_cls[EXEC_ILLEGAL + 1] = {
    [EXEC_LD_R_R] = WBP_ALU, [EXEC_LD_R_N] = WBP_ALU, [EXEC_LD_R_M] = WBP_ALU,
    [EXEC_ALU_R]  = WBP_ALU, [EXEC_ALU_N]  = WBP_ALU, [EXEC_ALU_M]  = WBP_ALU,
    [EXEC_INC_R]  = WBP_ALU, [EXEC_DEC_R]  = WBP_ALU, [EXEC_ROT_A]  = WBP_ALU,
    [EXEC_DAA]    = WBP_ALU, [EXEC_CPL]    = WBP_ALU, [EXEC_SCF]    = WBP_ALU,
    [EXEC_CCF]    = WBP_ALU, [EXEC_NEG]    = WBP_ALU, [EXEC_CB_R]   = WBP_ALU,
    [EXEC_IN_C]   = WBP_ALU, [EXEC_LD_A_IR] = WBP_ALU,
    [EXEC_INC_M]  = WBP_ALU, [EXEC_DEC_M]  = WBP_ALU, [EXEC_CB_M]   = WBP_ALU,
    [EXEC_DDCB]   = WBP_ALU, [EXEC_RRD]    = WBP_ALU, [EXEC_RLD]    = WBP_ALU,
    [EXEC_BLOCK]  = 0x80,
};

/* An instruction ends: the writes the die makes in the next M1 wait (wbp),
   with the decode they need and Q, which they commit (wb_fire_op). Any
   other instruction commits Q here: SCF / CCF in the next one derive X / Y
   from (A | Q), and Q is F if this instruction wrote F, else 0 -- Sean
   Young's "Undocumented Z80 Documented" §4.1 (ZEXALL's <daa,cpl,scf,ccf>
   CRC; run 27650481834). EX AF,AF' writes the F it swaps in; ADD / ADC /
   SBC HL commit theirs with it (WBP_F16); the rest write none. */
static inline void wb_defer(z80_t *c)
{
    uint8_t k = wb_cls[c->ctl.exec];
    if (k & 0x80) {
        uint8_t cat = (uint8_t)((c->ctl.aux >> 1) & 3);
        c->wb_rep = c->m_cycle == (cat <= 1 ? 4 : 5);        /* after a repeat */
        k = (cat == 1 || (cat >= 2 && c->wb_rep)) ? WBP_F1 : WBP_ALU;
    }
    if (k) {
        c->wbp |= k;
        /* LD A,I / R: the boundary's decision, from the T_last.P samples
           (alu_eval) -- before begin_next takes the INT */
        if (c->ctl.exec == EXEC_LD_A_IR)
            c->wb_pv = (c->iff2 && !(c->int_sampled && c->iff1 && !c->nmi_sampled)) ? 1u : 0u;
    } else if (!(c->wbp & WBP_F16))
        c->q = (c->wbp & WBP_EXAF) ? (uint8_t)c->rf[RFP_AF2] : 0;
}

/* The end of an M-cycle: the sequencer sets up the next one or finishes the
   instruction, and an instruction boundary decides what follows. Returns
   the template of the M-cycle it set up, bus_op << 4 | m_len, with 0x80 when
   wb_hooks() writes inside it (wb_mid) -- for cfast's steps. */
static unsigned end_mcycle(z80_t *c)
{
    z80_exec_step(c);                        /* set up next M-cycle / finish */
    if (c->instr_done) {
        wb_defer(c);                         /* the writes of the next M1; Q */
        c->instr_count++;
        c->prefix = PFX_NONE;
        begin_next(c);
    }
    return (wb_mid(c) ? 0x80u : 0u) | (unsigned)(c->bus_op & 7) << 4 | (c->m_len & 15);
}

/* the work of the rising edge that ends a T-state: a write the die makes
   inside the M-cycle, refresh step, next T-state, and at the end of an
   M-cycle the execute step / next M-cycle */
static void end_tstate(z80_t *c)
{
    if (c->t_state < c->m_len && wb_mid(c))
        wb_hooks(c, c->t_state);
    if (is_refresh_end(c))
        c->reg_r = (uint8_t)((c->reg_r & 0x80u) | ((c->reg_r + 1u) & 0x7Fu));
    c->t_state = (uint8_t)(c->t_state + 1);
    if (c->t_state > c->m_len)
        end_mcycle(c);
}

/* RESET (perfectz80; docs/timing.md "RESET"): RES2 stopped the T-state ring
   after the T-state just ended. What it did stands; the reset clears PC, I,
   R, IFF1 / IFF2, IM and HALT and keeps every other register, WZ and the
   exchange flip-flops -- with what the die had written of them by then:
   the pending writes go (docs/timing.md "Register write-back"). */
static void enter_reset_idle(z80_t *c)
{
    c->rst_idle = true;
    c->wbp = 0;
    c->in_tw = false;
    c->rf[RFP_PC] = 0x0000;
    c->reg_i = 0; c->reg_r = 0;
    c->iff1 = c->iff2 = false;
    c->im = 0;
    c->halted = false;
    c->prefix = PFX_NONE;
    c->nmi_pending = false;
    c->nmi_resp = false;
    c->nmi_sampled = c->int_sampled = false;
    c->ei_delay = false;
    c->suppress_decode = false;
    c->im0_ins = false;
    c->bus_granted = false;
    c->pins.busack_n = 1;
    c->srst_m1 = false;
}

static void advance(z80_t *c)
{
    if (c->phi == 0) {
        c->phi = 1;
        return;
    }
    c->phi = 0;
    /* Rising edge: the end of this T-state. RESET first. RES2 took this
       T-state's pin sample (res_s) on its falling edge, except in the low half
       of an M1's (INTA's) T2, where it holds and the special-reset latch sres
       takes the sample instead; RES2 clears that latch. Then the pin is
       sampled for the next T-state. */
    bool m1_t2 = !c->rst_idle && (c->bus_op == BUSOP_M1 || c->bus_op == BUSOP_INTA) && c->t_state == 2 &&
                 !c->in_tw;               /* T2 itself: RES2 doesn't hold in a Tw (perfectz80) */
    bool res2  = c->res2;                    /* set on the previous falling edge */
    bool ext   = c->bus_op == BUSOP_INTERNAL && c->m_len <= 2;   /* M1 / read extension */
    /* no cut in a bus grant: the bus is the DMA master's (perfectz80) */
    bool cut_t1 = res2 && !c->rst_idle && !c->bus_granted && !ext && c->t_state == 1;
    bool hold  = c->rst_idle && c->rd_hold && !res2;   /* restart right after a T1 cut */
    bool tw    = c->in_tw && c->t_state == 2 && !c->bus_granted;   /* a Tw after a T2 */
    c->res2 = m1_t2 ? c->res2 : c->res_s;
    c->sres = c->res2 ? false : (m1_t2 ? c->res_s : c->sres);
    c->res_s = !c->pins.reset_n;
    bool breq = c->busreq_s;                 /* BUSREQ at this T-state's rising edge */
    c->busreq_s = !c->pins.busreq_n;
    c->rd_hold = hold;
    if (c->rst_idle) {
        /* the ring has stopped: T1 of an M1 at 0000 once RES2 was clear on the
           previous falling edge -- the third rising edge after release */
        if (!res2) {                         /* PC is 0000 since the cut */
            c->rst_idle = false;
            z80_start_m1(c);
        } else {
            enter_reset_idle(c);             /* the clears hold while RES2 does */
        }
        return;
    }
    /* the pending writes land on the edge that ends the next M1's (INTA's)
       T1 or T2, not a Tw after it, and not in a bus grant (wb_fire) */
    if (c->wbp && !c->bus_granted && (c->bus_op == BUSOP_M1 || c->bus_op == BUSOP_INTA) &&
        (c->t_state == 1 || (c->t_state == 2 && !c->in_tw)))
        wb_fire(c, c->t_state, res2);
    if (c->bus_granted) {
        /* bus grant (DMA, perfectz80 / PS0178 Figure 10): the M-cycle after
           the grant is set up and held; BUSREQ seen high on this T-state's
           rising edge releases it here (BUSACK rose on its falling edge) */
        if (!breq) c->bus_granted = false;
    } else if (!c->stalled) {                /* else hold this T-state as a Tw */
        bool last = c->t_state >= c->m_len;
        c->cut_now = res2;                   /* reads straight into a register: blocked */
        end_tstate(c);
        c->cut_now = false;
        /* BUSREQ seen low on the rising edge that started the M-cycle's last
           T-state: the bus goes to the DMA master after it, at any M-cycle
           boundary; the next M-cycle (or instruction, or interrupt
           response) waits for the release. A 1- or 2-T internal cycle
           extends this M-cycle on the die (an M1's T5 / T6, CALL's 4-T
           read): no boundary before it. */
        bool ext_next = c->bus_op == BUSOP_INTERNAL && c->m_len <= 2 && c->t_state == 1;
        if (last && breq && !ext_next) c->bus_granted = true;
    } else if (res2 && c->t_state == 2) {
        /* the cut ends on a T2 that WAIT held, or on a Tw after it: the die's
           writes of T1's low half stand (below: and from a Tw on, T2's) */
        wb_hooks(c, 2);
    }
    if (res2 && tw) wb_hooks(c, 3);
    /* the T-state this edge starts is a Tw: WAIT held the last one */
    c->in_tw = !c->bus_granted && c->stalled;
    if (res2) {                              /* this T-state was the last */
        enter_reset_idle(c);
        c->rd_hold = cut_t1;
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
    c->nmi_resp = false;
    c->nmi_sampled = false;
    c->int_sampled = false;
    c->ei_delay = false;
    c->suppress_decode = false;
    c->im0_ins = false;
    c->bus_granted = false;

    c->t_state = 1; c->phi = 0; c->m_cycle = 1;
    c->bus_op = BUSOP_M1; c->m_len = 4; c->m_addr = 0x0000;
    c->decoded = false; c->instr_done = false; c->ucode = 0;
    c->phase_primed = false; c->stalled = false; c->wait_sampled = false;
    c->q = 0; c->wbp = 0;
    c->res_s = c->res2 = c->sres = c->rst_idle = c->srst_m1 = c->rd_hold = false;
    c->in_tw = false;
    c->busreq_s = false;

    c->pins.m1_n = c->pins.mreq_n = c->pins.iorq_n = 1;
    c->pins.rd_n = c->pins.wr_n = c->pins.rfsh_n = 1;
    c->pins.halt_n = 1; c->pins.busack_n = 1;
    c->pins.data_drive = false;
    c->pins.addr_drive = false; c->pins.ctl_drive = true;   /* the reset idle */
    c->pins.addr = 0x0000;
    c->pins.data_out = 0;
}

void z80_reset(z80_t *c)
{
    /* Programmer-visible registers are undefined on real silicon. We
     * force a deterministic state for C<->RTL compare AND for parity
     * with perfectz80's gate-level Visual-Z80 netlist, which boots
     * with rf = 0x5555 (alternating-bit pattern from die-level
     * reset). Previously this was 0xFFFF; that surfaced as informational
     * bus diffs on prog_rnd_02/03/04 (`make perfectz80`) and on
     * prog19_nmi_in_int (`make pin_scenarios`). Switching to 0x5555
     * closes those without touching any functional behaviour --
     * external test harnesses (FUSE / Rak / lockstep) explicitly poke
     * their start state. See docs/simplifications.md §C1 +
     * docs/known-differences.md row 1. */
    for (int i = 0; i < RFP_COUNT; i++) c->rf[i] = 0x5555;
    reset_state(c);
    z80_timing_rom_init();
    tpoint_rom_init();
    z80_pla_rom_init();
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

const z80_t *z80_arch(const z80_t *c)
{
    static z80_t s;
    if (!c->wbp) return c;
    s = *c;
    wb_fire(&s, 1, false);
    wb_fire(&s, 2, false);
    return &s;
}

/* ---- per-phase engine ---- */

void z80_phase_step(z80_t *c)
{
    c->cycle++;

    /* NMI is edge-triggered: latch a falling edge on nmi_n -- not while the
       full reset (RES2) is set, which still follows the pin, so an edge
       then is lost; once RES2 clears, the last idle T-state before the
       restart takes one (perfectz80) */
    if (c->prev_nmi_n && !c->pins.nmi_n && !c->res2) c->nmi_pending = true;
    c->prev_nmi_n = c->pins.nmi_n;

    /* Tw repeats T2 in memory writes, but its .P half must not regenerate
       the initial T2.P (WR high). Preserve the already asserted strobe. */
    bool hold_write_strobe = c->phase_primed && c->phi == 1 && c->stalled &&
                             c->bus_op == BUSOP_MWR;
    bool in_nmi_resp = c->nmi_resp;    /* as this edge found it */
    if (c->phase_primed)
        advance(c);
    c->phase_primed = true;

    /* NMI edges during the NMI response -- its M1 and the two pushes, up to
       and including the rising edge that ends the last push -- are lost,
       even when NMI then stays low: the chip holds its NMI flip-flop clear
       (perfectz80; event sweep nmir_entry). The response's state as this
       edge found it decides, as in the RTL (irq_seq). */
    if (in_nmi_resp || c->nmi_resp) c->nmi_pending = false;

    /* WAIT sample + latch (UM0080). Sample only at the spec edge; outside
       that single phase the latch falls to 0 so subsequent .N→.P advances
       run normally. This is the silicon's behavior: each Tw is decided by
       one falling-edge sample. */
    unsigned rom = rom_index(c);
    uint8_t  tp  = tpoint_rom[rom];
    if (!c->rst_idle && !c->bus_granted && (tp & TP_WAIT_SAMPLE))
        c->wait_sampled = (c->pins.wait_n == 0);
    else
        c->wait_sampled = false;
    c->stalled = c->wait_sampled;

    if (!c->rst_idle && !c->bus_granted && !c->stalled && (tp & TP_LATCH))
        do_latch(c);

    /* NMI / INT silicon sample point: rising edge of the last T-state of
       the current M-cycle (= T_last.P, the .P sub-phase of t_state == m_len)
       per Zilog UM0080. Sample once per M-cycle here; the last M-cycle's
       sample is the one begin_next() reads. While WAIT is asserted we hold
       the previous sample (T-state didn't advance), matching silicon. */
    if (!c->rst_idle && !c->bus_granted && !c->stalled && c->t_state == c->m_len && c->phi == 0) {
        c->nmi_sampled = c->nmi_pending;
        c->int_sampled = !c->pins.int_n;
    }

    /* the timing ROM's pins; the address the last phase drove stays on the
       bus in the phases is_addr_held() names and in the reset idle */
    uint16_t held_addr = c->pins.addr;
    drive_pins(c, rom);

    if (hold_write_strobe) c->pins.wr_n = 0;
    /* ... except that a RESET cut in a long internal cycle's T1, with RES2
       already set as the T1 starts, forces it into an opcode-fetch-like read
       and the bus drives the address latch, m_addr there: what the die
       loads at that T1 (perfectz80) -- HL in ADD / ADC / SBC HL,rr's first
       internal cycle, WZ (HL+1) in their second, and WZ from before the
       instruction changes it in JR's, DJNZ's, (IX+d)'s, CPI's and RLD's;
       PC before the -2 in a block repeat */
    bool int_cut = c->bus_op == BUSOP_INTERNAL && c->m_len > 2 && c->t_state == 1 && c->res2;
    if (((tp & TP_ADDR_HELD) && !int_cut) || c->rst_idle)
        c->pins.addr = held_addr;

    /* RESET (perfectz80): the idle drives nothing. `res2` -- set on the
       previous falling edge -- makes this T-state the last before the idle:
         - T1 cut, any cycle: the die forces the M-cycle to an opcode fetch
           on that falling edge, so MREQ and RD fall in T1.N even in a write
           or I/O cycle; MREQ rises on the next rising edge, RD on the
           falling edge after it (RD stays low in the idle T.P);
         - T2 cut: MREQ / RD release in T2.N and WR never falls; an I/O
           cycle cut after T2 never asserts IORQ / RD / WR at all;
         - I/O Tw cut: IORQ and RD / WR release in Tw.N; INTA Tw cut: IORQ. */
    bool ext   = c->bus_op == BUSOP_INTERNAL && c->m_len <= 2;
    bool int_g = c->bus_op == BUSOP_INTERNAL && !ext;
    bool m1_t2 = !c->rst_idle && (c->bus_op == BUSOP_M1 || c->bus_op == BUSOP_INTA) && c->t_state == 2 &&
                 !c->in_tw;               /* T2 itself: RES2 doesn't hold in a Tw (perfectz80) */
    /* RES2 / the special-reset latch after this T-state's falling edge */
    bool res2_f = (c->phi == 0) ? c->res2 : (m1_t2 ? c->res2 : c->res_s);
    bool sres_f = (c->phi == 0) ? c->sres : (res2_f ? false : (m1_t2 ? c->res_s : c->sres));
    /* bus grant: strobes idle, the address held; BUSACK low, rising in the
       low half of the granted T-state whose rising edge saw BUSREQ high */
    /* ... and RES2 set in a granted T-state releases it the same way
       (perfectz80): BUSACK rises in its low half, the reset idle follows */
    c->pins.busack_n = c->bus_granted ? ((c->phi == 1 && (!c->busreq_s || c->res2)) ? 1 : 0) : 1;
    if (c->bus_granted) {
        c->pins.m1_n = c->pins.mreq_n = c->pins.iorq_n = 1;
        c->pins.rd_n = c->pins.wr_n = c->pins.rfsh_n = 1;
        c->pins.data_drive = false;
        c->pins.addr = held_addr;
    }
    /* the die's pad drivers (perfectz80's pull-up / pull-down gates,
       scripts/pz80_pads.c): the address floats in a bus grant and in the
       reset idle, MREQ IORQ RD WR in a bus grant only, to the rising edge
       that starts the held M-cycle (half a T-state after BUSACK rises) */
    c->pins.addr_drive = !(c->bus_granted || c->rst_idle);
    c->pins.ctl_drive  = !c->bus_granted;
    if (c->rst_idle) {
        c->pins.m1_n = c->pins.mreq_n = c->pins.iorq_n = 1;
        c->pins.rd_n = c->pins.wr_n = c->pins.rfsh_n = 1;
        c->pins.data_drive = false;
        /* RD from a cut T1: low in the idle T.P, and in its T.N when the
           restart follows at once */
        if (c->rd_hold && (c->phi == 0 || !c->res2)) c->pins.rd_n = 0;
    } else if (c->bus_granted) {
        /* a grant: RESET drives no cut pattern on the DMA master's bus */
    } else if (c->rd_hold && c->phi == 0) {
        c->pins.rd_n = 0;                    /* ... and in the restarted M1's T1.P */
    } else if (!c->res2) {
        /* RES2 rising on a long internal M-cycle's T1.N: MREQ falls there */
        if (int_g && c->t_state == 1 && c->phi == 1 && res2_f) c->pins.mreq_n = 0;
    } else if (ext) {
        /* an M1 / read extension cut: nothing to drive */
    } else {
        bool io = (c->bus_op == BUSOP_IORD || c->bus_op == BUSOP_IOWR);
        if (c->phi == 0) {
            if (io && c->t_state == 2) c->pins.iorq_n = c->pins.rd_n = c->pins.wr_n = 1;
            if (int_g && c->t_state == 2) c->pins.mreq_n = 0;  /* ... and holds through T2.P */
        } else if (c->t_state == 1) {
            c->pins.mreq_n = 0; c->pins.rd_n = 0;
            c->pins.wr_n = 1; c->pins.iorq_n = 1;
            c->pins.data_drive = false;
        } else if (c->t_state == 2) {
            c->pins.mreq_n = c->pins.rd_n = c->pins.wr_n = c->pins.iorq_n = 1;
        } else if (io && c->t_state == 3) {
            c->pins.iorq_n = c->pins.rd_n = c->pins.wr_n = 1;
        } else if (c->bus_op == BUSOP_INTA && (c->t_state == 3 || c->t_state == 4)) {
            c->pins.iorq_n = 1;
        }
    }

    /* HALT pin level (perfectz80's gate-level trace: prog10_halt_nmi,
     * prog13_halt_int, event sweep int_halt / nmi_halt). It changes at
     * T4.N, the falling edge of an instruction's last T-state, to the
     * state the next instruction runs in:
     *   - low from T4.N of the HALT instruction's M1, half a T-state
     *     before instruction-done sets c->halted, which then holds it
     *     through the HALT NOP loop;
     *   - high again from T4.N of the HALT NOP whose end accepts an NMI
     *     or INT (the T4.P sample decided it), so an interrupt pending
     *     at the end of HALT itself never lowers it. */
    bool exec_halt_in_m1 = (c->ctl.exec == EXEC_HALT &&
                            ((c->bus_op == BUSOP_M1 && c->t_state == 4) ||
                             (c->bus_op == BUSOP_INTA && c->t_state == 6)) &&  /* IM0 */
                            c->phi == 1);
    bool halt_exit = (c->t_state == c->m_len && c->phi == 1 &&
                      (c->nmi_sampled || (c->int_sampled && c->iff1)));
    /* RESET: HALT rises on the falling edge where RES2 or the special-reset
       latch sets, and stays high (perfectz80) */
    bool halt_rst = res2_f || sres_f;
    c->pins.halt_n = ((c->halted || exec_halt_in_m1) && !halt_exit && !halt_rst) ? 0 : 1;
}

/* ---- pin word API ----
   z80_phase_step() behind the 64-bit pin word: the requests and the data
   byte of `pins` go to the input side of z80_pins_t, the phase runs, and
   the driven side comes back as active-high bits. */
uint64_t z80_tick(z80_t *c, uint64_t pins)
{
    c->pins.data_in  = Z80_GET_DATA(pins);
    c->pins.wait_n   = !(pins & Z80_WAIT);
    c->pins.int_n    = !(pins & Z80_INT);
    c->pins.nmi_n    = !(pins & Z80_NMI);
    c->pins.busreq_n = !(pins & Z80_BUSREQ);
    c->pins.reset_n  = !(pins & Z80_RES);
    z80_phase_step(c);
    const z80_pins_t *p = &c->pins;
    return (pins & ~Z80_OUT_PIN_MASK)
         | p->addr | ((uint64_t)p->data_out << Z80_PIN_D0)
         | (p->m1_n     ? 0 : Z80_M1)   | (p->mreq_n ? 0 : Z80_MREQ)
         | (p->iorq_n   ? 0 : Z80_IORQ) | (p->rd_n   ? 0 : Z80_RD)
         | (p->wr_n     ? 0 : Z80_WR)   | (p->halt_n ? 0 : Z80_HALT)
         | (p->rfsh_n   ? 0 : Z80_RFSH) | (p->busack_n ? 0 : Z80_BUSACK)
         | (p->data_drive ? Z80_DOE : 0);
}
