/* ============================================================================
 * z80_alu.c - the Z80 ALU module: combinational arithmetic + flag assembly.
 *
 * Mirrors rtl/z80_alu.v one-to-one. The Verilog module has a single port-list
 * (mode, alu_op, rot_op, bit_idx, xy_src, a, b, oldf, q) -> (res, fout); this
 * file exposes the same shape as z80_alu(...) at the bottom.
 *
 * The implementation is layered:
 *
 *   1. 4-bit nibble primitives (add4/sub4) — the explicit nibble-twice
 *      structure the silicon uses, with the half-carry falling out of the
 *      low pass and the carry out of the high pass.
 *   2. 8-bit ALU primitives (z80_alu_add8 / sub8 / logic / parity) built on
 *      the nibble primitives. Public so tests/common/test_alu.c can drive
 *      them directly.
 *   3. Per-class flag computers (z80_flags_add8 / sub8 / logic / inc8 /
 *      dec8 / rot_a / rot / bit / daa / scf / ccf / cpl) — each assembles
 *      the F register for one operation class, including undocumented X/Y
 *      behaviour. Public so tests/common/test_flags.c can drive them
 *      directly.
 *   4. The combinational top-level z80_alu() — the module entry point.
 *      Dispatches on `mode` and produces (res, fout). This is what the
 *      sequencer in z80_control.c calls; it is the C twin of the Verilog
 *      module's output mux.
 *
 * Layers 1-3 are not "modules" in the RTL sense — they are the internal
 * structure of the single z80_alu module. The RTL inlines them as wires
 * and case statements; C keeps them as named functions so flag bugs can be
 * isolated by the unit tests.
 * ==========================================================================*/
#include "z80.h"

#define XY (Z80_YF | Z80_XF)

/* ===========================================================================
 * 1. 4-bit nibble primitives (docs/alu.md)
 * ========================================================================*/

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

/* ===========================================================================
 * 2. 8-bit ALU primitives
 * ========================================================================*/

z80_addsub_t z80_alu_add8(uint8_t a, uint8_t b, uint8_t cin)
{
    z80_addsub_t r;
    uint8_t c0, c1;
    uint8_t lo = add4(a, b, (uint8_t)(cin & 1), &c0);                       /* low nibble  */
    uint8_t hi = add4((uint8_t)(a >> 4), (uint8_t)(b >> 4), c0, &c1);       /* high nibble */
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

/* ===========================================================================
 * 3. Per-class flag computers  (docs/flags.md)
 * ========================================================================*/

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

/* ===========================================================================
 * 4. Top-level combinational module — z80_alu()
 *
 * One C parameter per Verilog port of `rtl/z80_alu.v`. The sequencer calls
 * this once whenever the ALU fires; it dispatches on `mode` and returns the
 * (res, fout) pair the Verilog output mux produces.
 *
 * Arguments unused by the selected mode are ignored — call sites pass 0,
 * matching the Verilog convention of leaving unused inputs tied low.
 * ========================================================================*/
void z80_alu(uint8_t mode,      /* FLAG_*                                  */
             uint8_t alu_op,    /* ALU_* (selects ADD vs ADC, SUB vs SBC)  */
             uint8_t rot_op,    /* RLCA/RRCA/RLA/RRA or CB rot[y]          */
             uint8_t bit_idx,   /* CB BIT/RES/SET bit index                */
             uint8_t xy_src,    /* X/Y source for BIT n,(HL) etc.          */
             uint8_t a,         /* operand a (accumulator side)            */
             uint8_t b,         /* operand b                                */
             uint8_t oldf,      /* incoming F                              */
             uint8_t q,         /* NMOS Q register (SCF/CCF X/Y source)    */
             uint16_t a16,      /* 16-bit operand a (ADD16/ADC16/SBC16)    */
             uint16_t b16,      /* 16-bit operand b OR block-aux value      */
             bool iff2_in,      /* IFF2 (LD_A_I)                            */
             uint8_t *res,
             uint8_t *fout,
             uint16_t *res16,   /* 16-bit result; NULL ok                   */
             uint8_t *mem_byte) /* RRD/RLD second result; NULL ok           */
{
    uint8_t r = b, f = oldf;
    uint8_t cin = (oldf & Z80_CF) ? 1u : 0u;
    uint16_t r16 = 0;
    uint8_t  mb  = 0;

    switch (mode) {
    case FLAG_ADD8: {
        uint8_t c_in = (alu_op == ALU_ADC) ? cin : 0u;
        f = z80_flags_add8(a, b, c_in, oldf, &r);
        break;
    }
    case FLAG_SUB8: {
        uint8_t c_in = (alu_op == ALU_SBC) ? cin : 0u;
        f = z80_flags_sub8(a, b, c_in, false, oldf, &r);
        break;
    }
    case FLAG_CP8:
        f = z80_flags_sub8(a, b, 0, true, oldf, &r);
        break;
    case FLAG_LOGIC:
        f = z80_flags_logic((z80_alu_op_t)alu_op, a, b, &r);
        break;
    case FLAG_INC8:
        f = z80_flags_inc8(b, oldf, &r);
        break;
    case FLAG_DEC8:
        f = z80_flags_dec8(b, oldf, &r);
        break;
    case FLAG_ROT_A:
        f = z80_flags_rot_a(rot_op, a, oldf, &r);
        break;
    case FLAG_ROT:
        f = z80_flags_rot(rot_op, b, oldf, &r);
        break;
    case FLAG_BIT:
        f = z80_flags_bit(bit_idx, b, xy_src, oldf);
        r = b;
        break;
    case FLAG_DAA:
        f = z80_flags_daa(a, oldf, &r);
        break;
    case FLAG_CPL:
        f = z80_flags_cpl(a, oldf, &r);
        break;
    case FLAG_SCF:
        f = z80_flags_scf(a, oldf, q);
        r = a;
        break;
    case FLAG_CCF:
        f = z80_flags_ccf(a, oldf, q);
        r = a;
        break;

    /* ---- 16-bit ADD/ADC/SBC HL,rp ---- (byte-at-a-time on real silicon;
       implementation is one combinational add here for simplicity) ---- */
    case FLAG_ADD16: {
        uint32_t s = (uint32_t)a16 + (uint32_t)b16;
        r16 = (uint16_t)s;
        f = oldf & (Z80_SF | Z80_ZF | Z80_PF);
        if (((a16 & 0x0FFF) + (b16 & 0x0FFF)) & 0x1000) f |= Z80_HF;
        if (s & 0x10000) f |= Z80_CF;
        f |= (uint8_t)(r16 >> 8) & (Z80_YF | Z80_XF);
        break;
    }
    case FLAG_ADC16: {
        uint32_t s = (uint32_t)a16 + b16 + cin;
        r16 = (uint16_t)s;
        f = 0;
        if (((a16 & 0x0FFF) + (b16 & 0x0FFF) + cin) & 0x1000) f |= Z80_HF;
        if ((~(a16 ^ b16) & (a16 ^ r16) & 0x8000)) f |= Z80_PF;
        if (s & 0x10000) f |= Z80_CF;
        if (r16 & 0x8000) f |= Z80_SF;
        if (r16 == 0)     f |= Z80_ZF;
        f |= (uint8_t)(r16 >> 8) & (Z80_YF | Z80_XF);
        break;
    }
    case FLAG_SBC16: {
        int hc = (int)(a16 & 0x0FFF) - (int)(b16 & 0x0FFF) - (int)cin;
        uint32_t s = (uint32_t)a16 - b16 - cin;
        r16 = (uint16_t)s;
        f = Z80_NF;
        if (hc < 0) f |= Z80_HF;
        if ((a16 ^ b16) & (a16 ^ r16) & 0x8000) f |= Z80_PF;
        if (s & 0x10000) f |= Z80_CF;
        if (r16 & 0x8000) f |= Z80_SF;
        if (r16 == 0)     f |= Z80_ZF;
        f |= (uint8_t)(r16 >> 8) & (Z80_YF | Z80_XF);
        break;
    }

    /* ---- NEG: 0 - A ---- */
    case FLAG_NEG: {
        r = (uint8_t)(0u - a);
        f = Z80_NF;
        if (r & 0x80) f |= Z80_SF;
        if (r == 0)   f |= Z80_ZF;
        if (a & 0x0F) f |= Z80_HF;
        if (a == 0x80) f |= Z80_PF;
        if (a != 0)    f |= Z80_CF;
        f |= r & XY;
        break;
    }

    /* ---- LD A,I / LD A,R: PF = IFF2 ---- (a is the loaded byte) */
    case FLAG_LD_A_I: {
        r = a;
        f = oldf & Z80_CF;
        if (a & 0x80) f |= Z80_SF;
        if (a == 0)   f |= Z80_ZF;
        f |= a & XY;
        if (iff2_in)  f |= Z80_PF;
        break;
    }

    /* ---- IN r,(C): standard S/Z/P/X/Y from data, HF=0, NF=0 ---- */
    case FLAG_IN: {
        r = b;                            /* the byte from the port    */
        f = oldf & Z80_CF;
        if (b & 0x80) f |= Z80_SF;
        if (b == 0)   f |= Z80_ZF;
        f |= b & XY;
        if (z80_parity(b)) f |= Z80_PF;
        break;
    }

    /* ---- RRD: A = {A[7:4], (HL)[3:0]}; (HL) = {A[3:0], (HL)[7:4]} ---- */
    case FLAG_RRD: {
        uint8_t mem = b;
        uint8_t newA = (uint8_t)((a & 0xF0) | (mem & 0x0F));
        mb = (uint8_t)((mem >> 4) | ((a & 0x0F) << 4));
        r = newA;
        f = oldf & Z80_CF;
        if (newA & 0x80) f |= Z80_SF;
        if (newA == 0)   f |= Z80_ZF;
        f |= newA & XY;
        if (z80_parity(newA)) f |= Z80_PF;
        break;
    }
    case FLAG_RLD: {
        uint8_t mem = b;
        uint8_t newA = (uint8_t)((a & 0xF0) | ((mem >> 4) & 0x0F));
        mb = (uint8_t)(((mem << 4) & 0xF0) | (a & 0x0F));
        r = newA;
        f = oldf & Z80_CF;
        if (newA & 0x80) f |= Z80_SF;
        if (newA == 0)   f |= Z80_ZF;
        f |= newA & XY;
        if (z80_parity(newA)) f |= Z80_PF;
        break;
    }

    /* ---- Block-op flag formulas. b16 carries the block-specific aux:
           BLOCK_LD: bc_after.   BLOCK_CP: bc_after.   BLOCK_IO: k (9 bits in low). */
    case FLAG_BLOCK_LD: {
        /* a = A reg; b = transferred byte; b16 = BC after decrement. */
        uint8_t n = (uint8_t)(a + b);
        f = oldf & (Z80_SF | Z80_ZF | Z80_CF);
        if (n & 0x02) f |= Z80_YF;
        if (n & 0x08) f |= Z80_XF;
        if (b16) f |= Z80_PF;
        r = a;
        break;
    }
    case FLAG_BLOCK_CP: {
        /* a = A reg; b = (HL) byte; b16 = BC after decrement. */
        uint8_t sub = (uint8_t)(a - b);
        uint8_t hf = (((int)(a & 0xF) - (int)(b & 0xF)) < 0) ? Z80_HF : 0;
        uint8_t n  = (uint8_t)(sub - (hf ? 1u : 0u));
        f = (oldf & Z80_CF) | Z80_NF;
        if (sub & 0x80) f |= Z80_SF;
        if (sub == 0)   f |= Z80_ZF;
        f |= hf;
        if (n & 0x02) f |= Z80_YF;
        if (n & 0x08) f |= Z80_XF;
        if (b16) f |= Z80_PF;
        r = a;
        break;
    }
    case FLAG_BLOCK_IO: {
        /* a = port-data byte; b = new B value (B after decrement);
           b16 = k (low 9 bits matter: bk = data + (C +/- 1) or + L). */
        uint8_t data = a;
        uint8_t newB = b;
        uint16_t bk  = b16;
        f = 0;
        if (data & 0x80)        f |= Z80_NF;
        if (bk & 0x100)         f |= (Z80_HF | Z80_CF);
        if (z80_parity((uint8_t)((bk & 7) ^ newB))) f |= Z80_PF;
        if (newB & 0x80)        f |= Z80_SF;
        if (newB == 0)          f |= Z80_ZF;
        f |= newB & XY;
        r = newB;
        break;
    }

    default:
        /* FLAG_NONE: pass through. */
        r = b;
        f = oldf;
        break;
    }

    *res  = r;
    *fout = f;
    if (res16)    *res16    = r16;
    if (mem_byte) *mem_byte = mb;
}
