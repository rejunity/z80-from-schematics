/* ============================================================================
 * z80_flags.c - explicit flags subsystem (docs/flags.md).
 *
 * Each helper assembles the F register for one operation class, using the ALU
 * primitives in z80_alu.c. Undocumented X (bit3) / Y (bit5) behavior included.
 * Kept independent of the sequencer so it is unit-testable in isolation.
 * ==========================================================================*/
#include "z80.h"

#define XY (Z80_YF | Z80_XF)

uint8_t z80_flags_add8(uint8_t a, uint8_t b, uint8_t cin, uint8_t oldF, uint8_t *res)
{
    (void)oldF;
    z80_addsub_t r = z80_alu_add8(a, b, cin);
    uint8_t f = 0;
    if (r.res & 0x80) f |= Z80_SF;
    if (r.res == 0)   f |= Z80_ZF;
    f |= r.res & XY;
    if (r.half)       f |= Z80_HF;
    if ((~(a ^ b) & (a ^ r.res) & 0x80)) f |= Z80_PF; /* signed overflow */
    if (r.carry)      f |= Z80_CF;
    *res = r.res;
    return f;
}

uint8_t z80_flags_sub8(uint8_t a, uint8_t b, uint8_t cin, bool is_cp, uint8_t oldF, uint8_t *res)
{
    (void)oldF;
    z80_addsub_t r = z80_alu_sub8(a, b, cin);
    uint8_t f = Z80_NF;
    if (r.res & 0x80) f |= Z80_SF;
    if (r.res == 0)   f |= Z80_ZF;
    if (r.half)       f |= Z80_HF;
    if (((a ^ b) & (a ^ r.res) & 0x80)) f |= Z80_PF;
    if (r.carry)      f |= Z80_CF;
    f |= (is_cp ? b : r.res) & XY;   /* CP: X/Y from operand */
    *res = r.res;
    return f;
}

uint8_t z80_flags_logic(z80_alu_op_t op, uint8_t a, uint8_t b, uint8_t *res)
{
    uint8_t r = z80_alu_logic(op, a, b);
    uint8_t f = 0;
    if (r & 0x80) f |= Z80_SF;
    if (r == 0)   f |= Z80_ZF;
    f |= r & XY;
    if (op == ALU_AND) f |= Z80_HF;
    if (z80_parity(r)) f |= Z80_PF;
    *res = r;
    return f;
}

uint8_t z80_flags_inc8(uint8_t v, uint8_t oldF, uint8_t *res)
{
    uint8_t r = (uint8_t)(v + 1);
    uint8_t f = oldF & Z80_CF;            /* CF unchanged */
    if (r & 0x80) f |= Z80_SF;
    if (r == 0)   f |= Z80_ZF;
    f |= r & XY;
    if ((v & 0x0F) == 0x0F) f |= Z80_HF;
    if (v == 0x7F)          f |= Z80_PF;  /* overflow */
    *res = r;
    return f;
}

uint8_t z80_flags_dec8(uint8_t v, uint8_t oldF, uint8_t *res)
{
    uint8_t r = (uint8_t)(v - 1);
    uint8_t f = (oldF & Z80_CF) | Z80_NF;
    if (r & 0x80) f |= Z80_SF;
    if (r == 0)   f |= Z80_ZF;
    f |= r & XY;
    if ((v & 0x0F) == 0x00) f |= Z80_HF;
    if (v == 0x80)          f |= Z80_PF;
    *res = r;
    return f;
}

uint8_t z80_flags_rot_a(uint8_t op, uint8_t a, uint8_t oldF, uint8_t *res)
{
    uint8_t r = 0, cf = 0;
    switch (op) {
        case 0: cf = (a >> 7) & 1; r = (uint8_t)((a << 1) | cf); break;          /* RLCA */
        case 1: cf = a & 1;        r = (uint8_t)((a >> 1) | (cf << 7)); break;    /* RRCA */
        case 2: cf = (a >> 7) & 1; r = (uint8_t)((a << 1) | ((oldF & Z80_CF) ? 1 : 0)); break; /* RLA */
        case 3: cf = a & 1;        r = (uint8_t)((a >> 1) | ((oldF & Z80_CF) ? 0x80 : 0)); break; /* RRA */
        default: r = a; break;
    }
    uint8_t f = oldF & (Z80_SF | Z80_ZF | Z80_PF); /* unchanged */
    f |= r & XY;
    if (cf) f |= Z80_CF;
    *res = r;
    return f;
}

uint8_t z80_flags_rot(uint8_t op, uint8_t v, uint8_t oldF, uint8_t *res)
{
    uint8_t r = 0, cf = 0;
    switch (op) {
        case 0: cf = (v >> 7) & 1; r = (uint8_t)((v << 1) | cf); break;                 /* RLC */
        case 1: cf = v & 1;        r = (uint8_t)((v >> 1) | (cf << 7)); break;           /* RRC */
        case 2: cf = (v >> 7) & 1; r = (uint8_t)((v << 1) | ((oldF & Z80_CF) ? 1 : 0)); break; /* RL */
        case 3: cf = v & 1;        r = (uint8_t)((v >> 1) | ((oldF & Z80_CF) ? 0x80 : 0)); break; /* RR */
        case 4: cf = (v >> 7) & 1; r = (uint8_t)(v << 1); break;                         /* SLA */
        case 5: cf = v & 1;        r = (uint8_t)((v >> 1) | (v & 0x80)); break;          /* SRA */
        case 6: cf = (v >> 7) & 1; r = (uint8_t)((v << 1) | 1); break;                   /* SLL/SL1 (undoc) */
        case 7: cf = v & 1;        r = (uint8_t)(v >> 1); break;                         /* SRL */
        default: r = v; break;
    }
    uint8_t f = 0;
    if (r & 0x80) f |= Z80_SF;
    if (r == 0)   f |= Z80_ZF;
    f |= r & XY;
    if (z80_parity(r)) f |= Z80_PF;
    if (cf) f |= Z80_CF;
    *res = r;
    return f;
}

uint8_t z80_flags_bit(uint8_t b, uint8_t src, uint8_t xy_src, uint8_t oldF)
{
    uint8_t t = (uint8_t)(src & (1u << b));
    uint8_t f = oldF & Z80_CF;       /* CF unchanged */
    f |= Z80_HF;                     /* HF = 1 */
    if (t == 0) f |= (Z80_ZF | Z80_PF);
    if (t & 0x80) f |= Z80_SF;       /* set only when testing bit7 and it's 1 */
    f |= xy_src & XY;
    return f;
}

uint8_t z80_flags_daa(uint8_t a, uint8_t oldF, uint8_t *res)
{
    bool nf = (oldF & Z80_NF) != 0;
    bool hf = (oldF & Z80_HF) != 0;
    bool cf = (oldF & Z80_CF) != 0;
    uint8_t corr = 0;
    bool cf_out = cf;

    if (hf || (a & 0x0F) > 0x09) corr |= 0x06;
    if (cf || a > 0x99)          { corr |= 0x60; cf_out = true; }

    uint8_t r = nf ? (uint8_t)(a - corr) : (uint8_t)(a + corr);

    uint8_t f = nf ? Z80_NF : 0;
    if (r & 0x80) f |= Z80_SF;
    if (r == 0)   f |= Z80_ZF;
    f |= r & XY;
    if (z80_parity(r)) f |= Z80_PF;
    if (cf_out) f |= Z80_CF;
    if (nf ? (hf && (a & 0x0F) < 0x06) : ((a & 0x0F) > 0x09)) f |= Z80_HF;

    *res = r;
    return f;
}

uint8_t z80_flags_scf(uint8_t a, uint8_t oldF, uint8_t q)
{
    uint8_t f = oldF & (Z80_SF | Z80_ZF | Z80_PF);
    f |= Z80_CF;                /* HF=0, NF=0 */
    f |= (uint8_t)((a | q) & XY); /* NMOS Q-variant: X/Y from (A | Q) */
    return f;
}

uint8_t z80_flags_ccf(uint8_t a, uint8_t oldF, uint8_t q)
{
    bool oldc = (oldF & Z80_CF) != 0;
    uint8_t f = oldF & (Z80_SF | Z80_ZF | Z80_PF);
    if (oldc) f |= Z80_HF;      /* HF = old CF */
    else      f |= Z80_CF;      /* CF = ~old CF */
    f |= (uint8_t)((a | q) & XY); /* NMOS Q-variant: X/Y from (A | Q) */
    return f;
}

uint8_t z80_flags_cpl(uint8_t a, uint8_t oldF, uint8_t *res)
{
    uint8_t r = (uint8_t)(~a);
    uint8_t f = oldF & (Z80_SF | Z80_ZF | Z80_PF | Z80_CF);
    f |= Z80_HF | Z80_NF;
    f |= r & XY;
    *res = r;
    return f;
}
