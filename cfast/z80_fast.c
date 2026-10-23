/* ============================================================================
 * z80_fast.c - cfast's tick frame: the generated phase steps (build/cfast/
 * z80_fast_*.h, from cfast/gen_fast.py) around the reference model.
 *
 * The model is compiled into this unit so the steps call its own file-static
 * parts: do_latch() at a data latch and end_mcycle() (the sequencer and the
 * instruction boundary) at the end of an M-cycle. What the steps do in
 * between -- the pins, the latch and sample points, R -- is folded from the
 * timing ROM into constants. Phases the generated code does not cover go
 * through z80_tick() (z80_phase_step) unchanged: fast mode holds only while
 * none of the conditions in fast_ok() calls for the reference engine.
 *
 * z80f_tick() returns after every phase and resumes at f->step. The run
 * frame (cfast/z80_fast_run.h) runs the same steps back to back.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include "z80_fast.h"

#include "../cmodel/z80_alu.c"
#include "../cmodel/z80_core.c"
#include "../cmodel/z80_pla.c"
#include "../cmodel/z80_regfile.c"
#include "../cmodel/z80_timing.c"

#include "z80_fast_enum.h"

/* The writes an instruction leaves for the next M1 land on the edges that
   end its T1 and T2 (the model's wb_fire, the die's write-back): the ALU
   runs again from the operands where they stand, the register takes the
   result, F the flags. On the T2 edge (T1's for EX AF,AF'), for the
   classes most code runs -- the 8-bit loads, the ALU ops, INC / DEC, the
   rotates of A, DAA, CPL, NEG, SCF / CCF, the CB ops on a register, INC /
   DEC (HL), ADD / ADC / SBC HL's F, the exchanges -- the steps form the
   writes and store them in one go, the ALU's inputs taken as the model's
   alu_eval() takes them, where wb_fire_op() would dispatch twice more on
   the instruction for the same writes. The rest -- a block instruction's
   F, DD CB and the CB ops on (HL), IN r,(C), LD A,I / R, RRD / RLD, and a
   decode an interrupt response replaced (WBP_DEC) -- goes through
   wb_fire(). Fast mode never sees a RESET cut, which the reference engine
   handles.
   Measured on ZEXDOC (z80f_run): forming the writes at the instruction's
   end and storing them on the edge runs slower (29.5 against 31.0
   Minstr/s), making them there -- with what they replace kept for a phase
   that goes to the reference engine before the edge -- the same (31.1):
   the cost is the dispatch on the instruction, not where it happens.

   So the steps dispatch on it once, in the jump an M-cycle end makes
   anyway: an instruction's end (zf_end_mcycle) goes on in the copy of the
   next M1's T1.P - T3.P steps for what it leaves (its kind, ZK_*, by
   zf_kind[]; gen_fast.py's KINDS), whose steps make those writes on their
   edges with nothing to test -- the copy's T1.P states them in wbp, which
   the model reads should the phase go to it, and commits Q where the
   model's wb_defer() would. ZK_MODEL is wb_defer() itself and the steps
   that test wbp on both edges, for the rare classes and for an M1 the
   reference engine hands over with writes pending (resume). */

#if defined(__GNUC__)
#define ZF_INLINE inline __attribute__((always_inline))
#else
#define ZF_INLINE inline
#endif

/* register r as getr / setr (r) and getri / setri (ri) reach it, inlined
   into the steps (the model's own are calls): its pair and byte */
static ZF_INLINE uint8_t zf_pair(const z80_t *c, uint8_t r, bool ix)
{
    return (ix && c->ctl.idx && !c->ctl.use_disp && (r == REG_H || r == REG_L)) ? hl_pair(c) : r8_pair[r];
}
static ZF_INLINE uint8_t zf_get(const z80_t *c, uint8_t r, bool ix)
{
    uint16_t v = c->rf[zf_pair(c, r, ix)];
    return r8_hi[r] ? (uint8_t)(v >> 8) : (uint8_t)v;
}
static ZF_INLINE void zf_set(z80_t *c, uint8_t r, bool ix, uint8_t v)
{
    uint8_t p = zf_pair(c, r, ix);
    c->rf[p] = r8_hi[r] ? (uint16_t)((c->rf[p] & 0x00FF) | (unsigned)v << 8) : (uint16_t)((c->rf[p] & 0xFF00) | v);
}

/* the ALU on the instruction's operation and flag mode (md): A and b, F and
   Q as they stand, xy the bit ops' X / Y source; the result, F in *f */
static inline uint8_t zf_alu(z80_t *c, uint8_t md, uint8_t b, uint8_t xy, uint8_t *f)
{
    const z80_control_t *ctl = &c->ctl;
    uint8_t r;
    z80_alu(md, (uint8_t)ctl->alu_op, ctl->rot_op, ctl->bit_index, xy, z80_A(c), b, z80_F(c), c->q, &r, f);
    return r;
}
/* What an instruction leaves for the next M1's edges, by its class: the
   copy of the M1's first steps that makes it (gen_fast.py's KINDS). The
   exchanges and ADD / ADC / SBC HL set their wbp bit themselves (the exec
   step); ZK_MODEL: wb_defer() records the writes (a block instruction's F
   at T1 or T2, LD A,I / R's P/V) and the steps test them; ZK_NONE (0):
   nothing pending, Q 0. */
static const uint8_t zf_kind[EXEC_ILLEGAL + 1] = {
    [EXEC_LD_R_R] = ZK_LD_RR,    [EXEC_LD_R_N] = ZK_LD_RN,    [EXEC_LD_R_M] = ZK_LD_RN,
    [EXEC_ALU_R]  = ZK_ALU_R,    [EXEC_ALU_N]  = ZK_ALU_N,    [EXEC_ALU_M]  = ZK_ALU_N,
    [EXEC_INC_R]  = ZK_INCDEC_R, [EXEC_DEC_R]  = ZK_INCDEC_R,
    [EXEC_INC_M]  = ZK_INCDEC_M, [EXEC_DEC_M]  = ZK_INCDEC_M,
    [EXEC_SCF]    = ZK_SCF_CCF,  [EXEC_CCF]    = ZK_SCF_CCF,
    [EXEC_ROT_A]  = ZK_ACC,      [EXEC_DAA]    = ZK_ACC,      [EXEC_CPL]    = ZK_ACC,
    [EXEC_NEG]    = ZK_NEG,      [EXEC_CB_R]   = ZK_CB_R,
    [EXEC_EX_DE_HL] = ZK_EXDH,   [EXEC_EXX]    = ZK_EXX,      [EXEC_EX_AF]  = ZK_EXAF,
    [EXEC_ADD_HL_RP] = ZK_F16,   [EXEC_ADC16]  = ZK_F16,      [EXEC_SBC16]  = ZK_F16,
    [EXEC_CB_M]   = ZK_MODEL,    [EXEC_DDCB]   = ZK_MODEL,    [EXEC_IN_C]   = ZK_MODEL,
    [EXEC_LD_A_IR] = ZK_MODEL,   [EXEC_RRD]    = ZK_MODEL,    [EXEC_RLD]    = ZK_MODEL,
    [EXEC_BLOCK]  = ZK_MODEL,
};

/* The writes on the edge that ends the next M1's T2 (T1: EX AF,AF'), one
   kind each, as wb_fire_op() makes them: the ALU from the operands where
   they stand, the register, F, Q (the F written, or 0) */
static ZF_INLINE void zf_fire_f(z80_t *c, uint8_t f)
{
    z80_setF(c, f);
    c->wbp = 0; c->q = f;
}
static ZF_INLINE void zf_fire_ld_rr(z80_t *c)        /* the loads: the byte, no F */
{
    zf_set(c, c->ctl.rf_dst, true, zf_get(c, c->ctl.rf_src, true));
    c->wbp = 0; c->q = 0;
}
static ZF_INLINE void zf_fire_ld_rn(z80_t *c)
{
    zf_set(c, c->ctl.rf_dst, true, c->tmp8);
    c->wbp = 0; c->q = 0;
}
static ZF_INLINE void zf_fire_alu(z80_t *c, uint8_t b)
{
    uint8_t f, r = zf_alu(c, (uint8_t)c->ctl.flag_mode, b, 0, &f);
    if (c->ctl.alu_op != ALU_CP) z80_setA(c, r);
    zf_fire_f(c, f);
}
static ZF_INLINE void zf_fire_alu_r(z80_t *c) { zf_fire_alu(c, zf_get(c, c->ctl.rf_src, true)); }
static ZF_INLINE void zf_fire_alu_n(z80_t *c) { zf_fire_alu(c, c->tmp8); }
static ZF_INLINE void zf_fire_incdec_r(z80_t *c)
{
    uint8_t f, r = zf_alu(c, (uint8_t)c->ctl.flag_mode, zf_get(c, c->ctl.rf_dst, true), 0, &f);
    zf_set(c, c->ctl.rf_dst, true, r);
    zf_fire_f(c, f);
}
static ZF_INLINE void zf_fire_incdec_m(z80_t *c)     /* F; the byte went to memory */
{
    uint8_t f;
    zf_alu(c, (uint8_t)c->ctl.flag_mode, c->tmp8, 0, &f);
    zf_fire_f(c, f);
}
static ZF_INLINE void zf_fire_scf_ccf(z80_t *c)
{
    uint8_t f;
    zf_alu(c, (uint8_t)c->ctl.flag_mode, 0, 0, &f);
    zf_fire_f(c, f);
}
static ZF_INLINE void zf_fire_acc(z80_t *c)          /* the rotates of A, DAA, CPL */
{
    uint8_t f, r = zf_alu(c, (uint8_t)c->ctl.flag_mode, 0, 0, &f);
    z80_setA(c, r);
    zf_fire_f(c, f);
}
static ZF_INLINE void zf_fire_neg(z80_t *c)
{
    uint8_t f, r = zf_alu(c, FLAG_NEG, 0, 0, &f);
    z80_setA(c, r);
    zf_fire_f(c, f);
}
static ZF_INLINE void zf_fire_cb_r(z80_t *c)         /* the bit unit: r, and F (not RES / SET) */
{
    const z80_control_t *ctl = &c->ctl;
    uint8_t v = zf_get(c, ctl->rf_src, false), bm = (uint8_t)(1u << ctl->bit_index), f, r;
    if (ctl->cb_kind == CB_RES || ctl->cb_kind == CB_SET) {
        zf_set(c, ctl->rf_dst, false, ctl->cb_kind == CB_RES ? (uint8_t)(v & ~bm) : (uint8_t)(v | bm));
        c->wbp = 0; c->q = 0;
        return;
    }
    r = zf_alu(c, (uint8_t)ctl->flag_mode, v, v, &f);
    if (ctl->cb_kind == CB_ROT) zf_set(c, ctl->rf_dst, false, r);
    zf_fire_f(c, f);
}
static ZF_INLINE void zf_fire_exdh(z80_t *c)
{
    uint16_t x = c->rf[RFP_DE]; c->rf[RFP_DE] = c->rf[RFP_HL]; c->rf[RFP_HL] = x;
    c->wbp = 0;
}
static ZF_INLINE void zf_fire_exx(z80_t *c)
{
    for (uint8_t i = RFP_BC; i <= RFP_HL; i++) {
        uint16_t x = c->rf[i]; c->rf[i] = c->rf[i + RFP_BC2 - RFP_BC]; c->rf[i + RFP_BC2 - RFP_BC] = x;
    }
    c->wbp = 0;
}
static ZF_INLINE void zf_fire_exaf(z80_t *c)         /* on the edge that ends T1 */
{
    uint16_t x = c->rf[RFP_AF]; c->rf[RFP_AF] = c->rf[RFP_AF2]; c->rf[RFP_AF2] = x;
    c->wbp = 0;
}
static ZF_INLINE void zf_fire_f16(z80_t *c)          /* ADD / ADC / SBC HL's F */
{
    z80_setF(c, c->pf_val);
    c->wbp = 0; c->q = c->pf_val;
}

/* end_mcycle() for the steps: at an instruction's end, the kind of what it
   leaves for the next M1 picks that M1's steps, 256 + ZK_* (z80_fast_enum.h);
   only ZK_MODEL records it the model's way (wb_defer). In fast mode the
   boundary starts an M1 of 4 T-states: an interrupt to accept goes to the
   reference engine before (the M-cycle end steps), as do an NMI and a halt. */
static unsigned zf_end_mcycle(z80_t *c)
{
    z80_exec_step(c);
    if (!c->instr_done)
        return (wb_mid(c) ? 0x80u : 0u) | (unsigned)(c->bus_op & 7) << 4 | (c->m_len & 15);
    unsigned k = zf_kind[c->ctl.exec];
    if (k == ZK_MODEL)
        wb_defer(c);
    c->instr_count++;
    c->prefix = PFX_NONE;
    begin_next(c);
    return 256u + k;
}

/* inputs only the reference engine handles */
#define ZF_SLOW_IN (Z80_RES | Z80_BUSREQ | Z80_WAIT | Z80_NMI)

#if defined(__GNUC__)
#define ZF_GNU 1          /* computed goto (labels as values) */
#else
#define ZF_GNU 0
#endif

/* the template the generated steps run the current M-cycle in:
   bus_op << 4 | m_len, | 0x80 for an M-cycle with a register write inside
   it (wb_mid) -- as end_mcycle() returns it */
static inline unsigned zf_key(const z80_t *c)
{
    return (wb_mid(c) ? 0x80u : 0u) | (unsigned)(c->bus_op & 7) << 4 | c->m_len;
}

/* The state the generated steps assume: nothing pending that only
   z80_phase_step handles -- RESET's samples and idle, a bus grant or its
   request seen, a wait state (the steps don't update in_tw, which holds
   false while no WAIT is sampled), an IM0 instruction from the interrupting
   device, a halt, an NMI -- and a bus cycle with a template. */
static bool fast_ok(const z80_t *c)
{
    return c->phase_primed && !c->res_s && !c->res2 && !c->sres && !c->rst_idle &&
           !c->srst_m1 && !c->rd_hold && !c->bus_granted && !c->busreq_s && !c->in_tw &&
           !c->im0_ins &&
           !c->halted && !c->nmi_resp && !c->nmi_pending && !c->nmi_sampled &&
           c->prev_nmi_n && !c->stalled && !c->wait_sampled && !c->suppress_decode &&
           !(c->ctl.exec == EXEC_HALT && c->bus_op == BUSOP_M1) &&
           c->m_len < 16 && zf_template[zf_key(c)] &&
           c->t_state >= 1 && c->t_state <= c->m_len;
}

/* the step that produces the phase after (t_state, phi) */
static void resume(z80f_t *f)
{
    z80_t *c = f->cpu;
    f->fast = fast_ok(c);
    if (f->fast) {
        /* the pads are driven in every phase fast mode runs (never a bus
           grant or the reset idle): the steps leave the drivers as set */
        c->pins.addr_drive = c->pins.ctl_drive = true;
        unsigned key = zf_key(c), o = 2u * (c->t_state - 1u) + c->phi + 1u;
        /* an M1 the reference engine leaves with writes pending, before the
           T3.P step: the copy of its first steps that tests them */
        if (c->wbp && key == (BUSOP_M1 << 4 | 4) && o < ZF_M1K_STEPS)
            key = 256u + ZK_MODEL;
        f->step = (uint16_t)(zf_template[key] + o);
        /* the address of the phases to come: the M-cycle's, or the one on
           the bus where the model holds it (is_addr_held) */
        unsigned t = c->t_state + c->phi, phi = !c->phi;
        bool held = t <= c->m_len &&
                    (tpoint_rom[Z80_TIMING_ROM_INDEX(c->bus_op, t, phi)] & TP_ADDR_HELD);
        f->bus = (held ? c->pins.addr : c->m_addr) | (uint64_t)c->m_wdata << Z80_PIN_D0;
    }
}

#ifdef ZF_STATS
/* phases run by the reference engine, and slow -> fast resumptions */
unsigned long long zf_slow_ticks, zf_resumes;
#endif

/* one phase through the reference engine */
static uint64_t slow_tick(z80f_t *f, uint64_t pins)
{
    uint64_t out = z80_tick(f->cpu, pins);
    resume(f);
#ifdef ZF_STATS
    zf_slow_ticks++;
    zf_resumes += f->fast;
#endif
    return out;
}

void z80f_init(z80f_t *f, z80_t *cpu)
{
    f->cpu = cpu;
    resume(f);
}

uint64_t z80f_tick(z80f_t *f, uint64_t pins)
{
    if (!f->fast || (pins & ZF_SLOW_IN))
        return slow_tick(f, pins);
    z80_t *c = f->cpu;
    uint64_t bus = f->bus;
#if ZF_GNU
    static void *const step_label[FS_COUNT] = {
        [FS_NONE] = &&zf_bad,
#include "z80_fast_labels.h"
    };
#define ZF_GOTO_STEP(s) goto *step_label[s]
#else
    unsigned zf_target;
#define ZF_GOTO_STEP(s) do { zf_target = (s); goto zf_dispatch; } while (0)
#endif
    ZF_GOTO_STEP(f->step);

#define ZF_LABEL(s)        s
#define ZF_CLOCK(t, ph)    do { c->cycle++; c->phi = (ph); c->t_state = (t); } while (0)
#define ZF_MEND(n)         do { c->cycle++; c->phi = 0; c->t_state = (n) + 1; } while (0)
#define ZF_SAVE_BUS()      (f->bus = bus)
#define ZF_PHASE(next)     do { f->step = (next); return pins; } while (0)
#define ZF_PHASE_T1P(next) ZF_PHASE(next)
#define ZF_PHASE_CHK(next) ZF_PHASE(next)
#define ZF_SLOW_AT(s)      return slow_tick(f, pins)
#define ZF_GOTO_T1P(k)     ZF_GOTO_STEP(zf_template[k])
#define ZF_JUMP(s)         ((void)0)     /* after ZF_PHASE's return */
#include "z80_fast_steps.h"
#undef ZF_LABEL
#undef ZF_CLOCK
#undef ZF_MEND
#undef ZF_SAVE_BUS
#undef ZF_PHASE
#undef ZF_PHASE_T1P
#undef ZF_PHASE_CHK
#undef ZF_SLOW_AT
#undef ZF_GOTO_T1P
#undef ZF_JUMP

#if !ZF_GNU
zf_dispatch:
    switch (zf_target) {
#include "z80_fast_cases.h"
    default: break;
    }
#endif
zf_bad:
    fprintf(stderr, "z80f_tick: no step for bus_op %u m_len %u\n", c->bus_op, c->m_len);
    abort();
}
#undef ZF_GOTO_STEP
