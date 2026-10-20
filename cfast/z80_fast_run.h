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
 * Include in the unit that includes cfast/z80_fast.c, after defining
 *   ZF_BUS(pins)     a statement serving the bus for the phase whose pins are
 *                    in the uint64_t lvalue `pins` (as after z80f_tick), and
 *                    setting D0-D7 and the requests for the next phase; it
 *                    may set `zf_exit = 1` to return after this phase
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

static uint64_t z80f_run(z80f_t *f, uint64_t pins)
{
    z80_t   *c = f->cpu;
    uint64_t bus = f->bus, ic = c->instr_count;
    int      zf_exit = 0;
    static void *const step_label[FS_COUNT] = {
        [FS_NONE] = &&zf_bad,
#define ZF_STEP_LABELS
#include "z80_fast_steps.h"
#undef ZF_STEP_LABELS
    };
    if (!f->fast || (pins & ZF_SLOW_IN))
        goto zf_slow;
    goto *step_label[f->step];

#define ZF_LABEL(s)        s
#define ZF_PHASE(next)     do { ZF_BUS(pins); \
        if (zf_exit) { f->step = (next); return pins; } \
        if (pins & ZF_SLOW_IN) { f->step = (next); goto zf_slow; } } while (0)
#define ZF_PHASE_T1P(next) do { ZF_BUS(pins); \
        if (zf_exit) { f->step = (next); return pins; } \
        if (c->instr_count != ic) { ic = c->instr_count; \
            if (ZF_BOUNDARY(c)) { f->step = (next); return pins; } } \
        if (pins & ZF_SLOW_IN) { f->step = (next); goto zf_slow; } } while (0)
#define ZF_PHASE_CHK(next) do { ZF_BUS(pins); \
        if (zf_exit) { f->step = (next); return pins; } \
        if ((pins & ZF_SLOW_IN) || !f->fast) { f->step = (next); goto zf_slow; } } while (0)
#define ZF_SLOW_AT(s)      goto zf_slow
#define ZF_GOTO_T1P()      goto *step_label[zf_template[(c->bus_op & 7) << 4 | (c->m_len & 15)]]
#include "z80_fast_steps.h"
#undef ZF_LABEL
#undef ZF_PHASE
#undef ZF_PHASE_T1P
#undef ZF_PHASE_CHK
#undef ZF_SLOW_AT
#undef ZF_GOTO_T1P

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
            bus = f->bus;
            goto *step_label[f->step];
        }
    }
zf_bad:
    fprintf(stderr, "z80f_run: no step for bus_op %u m_len %u\n", c->bus_op, c->m_len);
    abort();
}
