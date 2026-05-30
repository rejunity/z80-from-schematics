/* ============================================================================
 * z80_internal.h - cross-module internals shared by the C model engine.
 * Not part of the public API.
 * ==========================================================================*/
#ifndef Z80_INTERNAL_H
#define Z80_INTERNAL_H

#include "z80.h"

/* ---- register-file byte helpers (inline, used by all engine modules) ---- */
static inline uint8_t  z80_A(const z80_t *c) { return (uint8_t)(c->rf[RFP_AF] >> 8); }
static inline uint8_t  z80_F(const z80_t *c) { return (uint8_t)(c->rf[RFP_AF] & 0xFF); }
static inline void z80_setA(z80_t *c, uint8_t v){ c->rf[RFP_AF] = (uint16_t)((c->rf[RFP_AF] & 0x00FF) | ((uint16_t)v << 8)); }
static inline void z80_setF(z80_t *c, uint8_t v){ c->rf[RFP_AF] = (uint16_t)((c->rf[RFP_AF] & 0xFF00) | v); c->f_modified = true; }
static inline uint16_t z80_HL(const z80_t *c){ return c->rf[RFP_HL]; }
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

/* ---- engine internals ---- */
void    z80_drive_pins(z80_t *cpu);                  /* z80_timing.c */
void    z80_exec_step(z80_t *cpu);                   /* z80_control.c */
uint8_t z80_busop_base_len(uint8_t busop);           /* z80_bus.c */
void    z80_start_mcycle(z80_t *cpu, uint8_t busop, uint16_t addr,
                         uint8_t wdata, uint8_t extra_t); /* z80_bus.c */
void    z80_start_m1(z80_t *cpu);                    /* z80_bus.c */

#endif /* Z80_INTERNAL_H */
