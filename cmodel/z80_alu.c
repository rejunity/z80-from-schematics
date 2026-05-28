/* ============================================================================
 * z80_alu.c - 4-bit-nibble ALU primitives (docs/alu.md).
 *
 * The Z80 ALU is a 4-bit unit applied twice per 8-bit operation, with a carry
 * chain between the low-nibble and high-nibble passes. We model that structure
 * explicitly: the half-carry falls out of the low pass, the carry out of the
 * high pass. Add and subtract share the adder; subtract uses borrow nibbles.
 * ==========================================================================*/
#include "z80.h"

/* one 4-bit add: returns sum nibble, *cout = carry out of bit 3 */
static uint8_t add4(uint8_t a, uint8_t b, uint8_t cin, uint8_t *cout)
{
    uint8_t s = (uint8_t)((a & 0xF) + (b & 0xF) + (cin & 1));
    *cout = (uint8_t)((s >> 4) & 1);
    return (uint8_t)(s & 0xF);
}

/* one 4-bit subtract: returns difference nibble, *bout = borrow out of bit 3 */
static uint8_t sub4(uint8_t a, uint8_t b, uint8_t bin, uint8_t *bout)
{
    int s = (int)(a & 0xF) - (int)(b & 0xF) - (int)(bin & 1);
    *bout = (s < 0) ? 1u : 0u;
    return (uint8_t)(s & 0xF);
}

z80_addsub_t z80_alu_add8(uint8_t a, uint8_t b, uint8_t cin)
{
    z80_addsub_t r;
    uint8_t c0, c1;
    uint8_t lo = add4(a, b, (uint8_t)(cin & 1), &c0);          /* low nibble  */
    uint8_t hi = add4((uint8_t)(a >> 4), (uint8_t)(b >> 4), c0, &c1); /* high  */
    r.res  = (uint8_t)((hi << 4) | lo);
    r.half = c0;   /* carry from bit3 -> HF */
    r.carry = c1;  /* carry from bit7 -> CF */
    return r;
}

z80_addsub_t z80_alu_sub8(uint8_t a, uint8_t b, uint8_t bin)
{
    z80_addsub_t r;
    uint8_t b0, b1;
    uint8_t lo = sub4(a, b, (uint8_t)(bin & 1), &b0);
    uint8_t hi = sub4((uint8_t)(a >> 4), (uint8_t)(b >> 4), b0, &b1);
    r.res  = (uint8_t)((hi << 4) | lo);
    r.half = b0;   /* borrow from bit4 -> HF (sub sense) */
    r.carry = b1;  /* borrow from bit8 -> CF (sub sense) */
    return r;
}

uint8_t z80_alu_logic(z80_alu_op_t op, uint8_t a, uint8_t b)
{
    switch (op) {
        case ALU_AND: return (uint8_t)(a & b);
        case ALU_OR:  return (uint8_t)(a | b);
        case ALU_XOR: return (uint8_t)(a ^ b);
        default:      return a;
    }
}

uint8_t z80_parity(uint8_t x)
{
    /* 1 = even parity (PF set) */
    x ^= (uint8_t)(x >> 4);
    x ^= (uint8_t)(x >> 2);
    x ^= (uint8_t)(x >> 1);
    return (uint8_t)((~x) & 1);
}
