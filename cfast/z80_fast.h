/* ============================================================================
 * z80_fast.h - cfast: a generated phase engine for the C model (prototype).
 *
 * z80f_tick() is z80_tick() -- one phase per call, the same pin word, the
 * same pins in every phase -- run by straight-line code generated from the
 * model's timing tables (cfast/gen_fast.py) for the common case, and by the
 * reference z80_phase_step for the rest (WAIT, BUSREQ, RESET, NMI, interrupt
 * acknowledge, HALT). Both work on the same z80_t.
 * ==========================================================================*/
#ifndef Z80_FAST_H
#define Z80_FAST_H

#include "z80.h"

typedef struct {
    z80_t   *cpu;    /* the model's state, shared with z80_phase_step      */
    uint16_t step;   /* fast mode: the phase the next tick produces         */
    bool     fast;   /* the next tick may take the generated path           */
    uint64_t bus;    /* fast mode: m_addr | m_wdata << 16 of the M-cycle    */
} z80f_t;

/* Attach to a model; again after changing its state between ticks. */
void     z80f_init(z80f_t *f, z80_t *cpu);
uint64_t z80f_tick(z80f_t *f, uint64_t pins);

#endif /* Z80_FAST_H */
