/* ============================================================================
 * z80_fast_run.h - cfast's run frame: the generated phase steps back to
 * back, with the system's bus inlined between phases.
 *
 * z80f_tick() returns after every phase, so every phase pays a call, a
 * dispatch on f->step and the bus logic behind it. Here the caller's bus
 * logic is a macro the steps expand after each phase, so within an M-cycle
 * the steps fall through one into the next with no dispatch at all; only an
 * M-cycle end jumps (to the T1.P of the cycle the sequencer set up). Same
 * steps, same pins, phase by phase, as z80f_tick().
 *
 * The phase bookkeeping -- the model's cycle, t_state and phi -- is implicit
 * in the step the frame is at: it is written back to the z80_t when the
 * frame returns, hands a phase to the reference engine, or reaches an
 * instruction boundary (before ZF_BOUNDARY), not after every phase.
 *
 * Include in the unit that includes cfast/z80_fast.c, after defining
 *   ZF_BUS(pins)     a statement serving the bus for the phase whose pins are
 *                    in the uint64_t lvalue `pins` (as after z80f_tick), and
 *                    setting D0-D7 and the requests for the next phase; it
 *                    may set `zf_exit = 1` to return after this phase. Bus
 *                    code that reads the model's cycle, t_state or phi
 *                    calls ZF_SYNC_STATE() first.
 *   ZF_BOUNDARY(c)   an expression evaluated at every instruction boundary
 *                    (z80_t *c, the next instruction about to be fetched);
 *                    nonzero returns
 * It defines static uint64_t z80f_run(z80f_t *f, uint64_t pins): pins as
 * for z80f_tick; returns the last phase's pins after ZF_BUS; call it again
 * to go on.
 * ==========================================================================*/
#if !ZF_GNU
#error "z80_fast_run.h needs computed goto (GCC / Clang)"
#endif

/* bits of `bus`: A0-A15, D0-D7 (stated so the compiler folds the bus
   logic's tests of the strobes each step sets) */
#define ZF_BUS_BITS 0xFFFFFFull

static uint64_t z80f_run(z80f_t *f, uint64_t pins)
{
    z80_t   *c = f->cpu;
    uint64_t bus = f->bus & ZF_BUS_BITS, ic = c->instr_count;
    uint64_t zf_cyc0;     /* c->cycle at the T1.P of the current M-cycle */
    int      zf_exit = 0;
    static void *const step_label[FS_COUNT] = {
        [FS_NONE] = &&zf_bad,
#include "z80_fast_labels.h"
    };
    static void *const t1p_label[256 + ZK_COUNT] = {
#include "z80_fast_t1p_labels.h"
    };
    if (!f->fast || (pins & ZF_SLOW_IN))
        goto zf_slow;
    zf_cyc0 = c->cycle - zf_step_off[f->step] + 1;
    goto *step_label[f->step];

/* write the bookkeeping of the phase step s produced back to the model */
#define ZF_SYNC(s) do { c->cycle = zf_cyc0 + zf_step_off[s]; \
        c->t_state = zf_step_t[s]; c->phi = zf_step_phi[s]; } while (0)
#define ZF_SYNC_STATE()    ZF_SYNC(zf_cur)
#define ZF_LEAVE(next)     do { ZF_SYNC(zf_cur); f->step = (next); f->bus = bus; } while (0)
#define ZF_LABEL(s)        s
#define ZF_CLOCK(t, ph)    ((void)0)
#define ZF_MEND(n)         (zf_cyc0 += 2u * (n))
#define ZF_SAVE_BUS()      ((void)0)
#define ZF_PHASE(next)     do { const unsigned zf_cur = (next) - 1; (void)zf_cur; \
        ZF_BUS(pins); \
        if (__builtin_expect(zf_exit != 0, 0)) { ZF_LEAVE(next); return pins; } \
        if (__builtin_expect((pins & ZF_SLOW_IN) != 0, 0)) { ZF_LEAVE(next); goto zf_slow; } } while (0)
#define ZF_PHASE_T1P(next) do { const unsigned zf_cur = (next) - 1; (void)zf_cur; \
        ZF_BUS(pins); \
        if (__builtin_expect(zf_exit != 0, 0)) { ZF_LEAVE(next); return pins; } \
        if (c->instr_count != ic) { ic = c->instr_count; ZF_SYNC(zf_cur); \
            if (ZF_BOUNDARY(c)) { ZF_LEAVE(next); return pins; } } \
        if (__builtin_expect((pins & ZF_SLOW_IN) != 0, 0)) { ZF_LEAVE(next); goto zf_slow; } } while (0)
#define ZF_PHASE_CHK(next) do { const unsigned zf_cur = (next) - 1; (void)zf_cur; \
        ZF_BUS(pins); \
        if (__builtin_expect(zf_exit != 0, 0)) { ZF_LEAVE(next); return pins; } \
        if (__builtin_expect((pins & ZF_SLOW_IN) != 0 || !f->fast, 0)) { ZF_LEAVE(next); goto zf_slow; } } while (0)
#define ZF_SLOW_AT(s)      do { ZF_SYNC((s) - 1); goto zf_slow; } while (0)
#define ZF_GOTO_T1P(k)     goto *t1p_label[k]
#define ZF_JUMP(s)         goto s
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
#undef ZF_LEAVE
#undef ZF_SYNC_STATE
/* the reference engine keeps the bookkeeping current itself */
#define ZF_SYNC_STATE()    ((void)0)

zf_slow:    /* phases through the reference engine until fast mode holds */
    for (;;) {
        pins = slow_tick(f, pins);
        ZF_BUS(pins);
        if (zf_exit)
            return pins;
        if (c->instr_count != ic) {
            ic = c->instr_count;
            if (ZF_BOUNDARY(c))
                return pins;
        }
        if (f->fast && !(pins & ZF_SLOW_IN)) {
            bus = f->bus & ZF_BUS_BITS;
            zf_cyc0 = c->cycle - zf_step_off[f->step] + 1;
            goto *step_label[f->step];
        }
    }
zf_bad:
    fprintf(stderr, "z80f_run: no step for bus_op %u m_len %u\n", c->bus_op, c->m_len);
    abort();
}
#undef ZF_SYNC_STATE
#undef ZF_SYNC
