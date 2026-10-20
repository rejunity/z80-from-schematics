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

/* inputs only the reference engine handles */
#define ZF_SLOW_IN (Z80_RES | Z80_BUSREQ | Z80_WAIT | Z80_NMI)

#if defined(__GNUC__)
#define ZF_GNU 1          /* computed goto (labels as values) */
#else
#define ZF_GNU 0
#endif

/* The state the generated steps assume: nothing pending that only
   z80_phase_step handles, and a bus cycle with a template. */
static bool fast_ok(const z80_t *c)
{
    return c->phase_primed && !c->in_reset_hold && !c->reset_assert_filter &&
           !c->reset_release_filter && !c->bus_granted && !c->busreq_release_filter &&
           !c->halted && !c->nmi_resp && !c->nmi_pending && !c->nmi_sampled &&
           c->prev_nmi_n && !c->stalled && !c->wait_sampled && !c->suppress_decode &&
           !(c->ctl.exec == EXEC_HALT && c->bus_op == BUSOP_M1) &&
           c->m_len < 16 && zf_template[(c->bus_op & 7) << 4 | c->m_len] &&
           c->t_state >= 1 && c->t_state <= c->m_len;
}

/* the step that produces the phase after (t_state, phi) */
static void resume(z80f_t *f)
{
    const z80_t *c = f->cpu;
    f->fast = fast_ok(c);
    if (f->fast) {
        f->step = (uint16_t)(zf_template[(c->bus_op & 7) << 4 | c->m_len] +
                             2 * (c->t_state - 1) + c->phi + 1);
        f->bus = c->m_addr | (uint64_t)c->m_wdata << Z80_PIN_D0;
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
#define ZF_GOTO_T1P()      ZF_GOTO_STEP(zf_template[(c->bus_op & 7) << 4 | (c->m_len & 15)])
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
