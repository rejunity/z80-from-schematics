/* Unit tests for the flags subsystem (cmodel/z80_flags.c).
   Known-answer vectors for documented + undocumented (X/Y) behavior. */
#include "z80.h"
#include "test_util.h"

int main(void)
{
    uint8_t res, f;

    /* ---- ADD ---- */
    f = z80_flags_add8(0x00, 0x00, 0, 0, &res);
    CHECK_EQ_U(res, 0x00, "ADD 0+0 res"); CHECK_EQ_U(f, Z80_ZF, "ADD 0+0 F");

    f = z80_flags_add8(0x0F, 0x01, 0, 0, &res);
    CHECK_EQ_U(res, 0x10, "ADD 0F+1 res"); CHECK_EQ_U(f, Z80_HF, "ADD 0F+1 F");

    f = z80_flags_add8(0xFF, 0x01, 0, 0, &res);
    CHECK_EQ_U(res, 0x00, "ADD FF+1 res");
    CHECK_EQ_U(f, (Z80_ZF | Z80_HF | Z80_CF), "ADD FF+1 F");

    f = z80_flags_add8(0x7F, 0x01, 0, 0, &res);   /* overflow -> +128 */
    CHECK_EQ_U(res, 0x80, "ADD 7F+1 res");
    CHECK_EQ_U(f, (Z80_SF | Z80_HF | Z80_PF), "ADD 7F+1 F (overflow)");

    /* ADC with carry-in */
    f = z80_flags_add8(0x0F, 0x00, 1, 0, &res);
    CHECK_EQ_U(res, 0x10, "ADC 0F+0+1 res"); CHECK_EQ_U(f, Z80_HF, "ADC 0F+0+1 F");

    /* ---- SUB ---- */
    f = z80_flags_sub8(0x00, 0x01, 0, false, 0, &res);
    CHECK_EQ_U(res, 0xFF, "SUB 0-1 res");
    /* 0xFF: S Y X H N C, no overflow */
    CHECK_EQ_U(f, (Z80_SF | Z80_YF | Z80_XF | Z80_HF | Z80_NF | Z80_CF), "SUB 0-1 F");

    f = z80_flags_sub8(0x80, 0x01, 0, false, 0, &res); /* overflow */
    CHECK_EQ_U(res, 0x7F, "SUB 80-1 res");
    CHECK_EQ_U(f, (Z80_YF | Z80_XF | Z80_HF | Z80_PF | Z80_NF), "SUB 80-1 F (overflow)");

    /* CP takes X/Y from the operand, not the result. 0x28 = bit5|bit3 set. */
    f = z80_flags_sub8(0x00, 0x28, 0, true, 0, &res);
    CHECK_EQ_U(f & (Z80_YF | Z80_XF), (Z80_YF | Z80_XF), "CP X/Y from operand 0x28");
    f = z80_flags_sub8(0x00, 0x20, 0, true, 0, &res); /* operand 0x20 -> only Y */
    CHECK_EQ_U(f & (Z80_YF | Z80_XF), Z80_YF, "CP X/Y from operand 0x20 (Y only)");

    /* ---- LOGIC ---- */
    f = z80_flags_logic(ALU_AND, 0xF0, 0x0F, &res);
    CHECK_EQ_U(res, 0x00, "AND res");
    CHECK_EQ_U(f, (Z80_ZF | Z80_HF | Z80_PF), "AND F (H set, even parity)");

    f = z80_flags_logic(ALU_OR, 0x00, 0x00, &res);
    CHECK_EQ_U(f, (Z80_ZF | Z80_PF), "OR 0 F");

    /* ---- INC / DEC ---- */
    f = z80_flags_inc8(0xFF, Z80_CF, &res);
    CHECK_EQ_U(res, 0x00, "INC FF res");
    CHECK_EQ_U(f, (Z80_ZF | Z80_HF | Z80_CF), "INC FF F (CF preserved)");

    f = z80_flags_inc8(0x7F, 0, &res);
    CHECK_EQ_U(res, 0x80, "INC 7F res");
    CHECK_EQ_U(f, (Z80_SF | Z80_HF | Z80_PF), "INC 7F F (overflow)");

    f = z80_flags_dec8(0x00, 0, &res);
    CHECK_EQ_U(res, 0xFF, "DEC 0 res");
    CHECK_EQ_U(f, (Z80_SF | Z80_YF | Z80_XF | Z80_HF | Z80_NF), "DEC 0 F");

    f = z80_flags_dec8(0x80, 0, &res);
    CHECK_EQ_U(res, 0x7F, "DEC 80 res");
    CHECK_EQ_U(f, (Z80_YF | Z80_XF | Z80_HF | Z80_PF | Z80_NF), "DEC 80 F (overflow)");

    /* ---- rotates (CB) ---- */
    f = z80_flags_rot(0, 0x80, 0, &res);  /* RLC 0x80 -> 0x01, C=1 */
    CHECK_EQ_U(res, 0x01, "RLC 80 res"); CHECK_EQ_U(f & Z80_CF, Z80_CF, "RLC 80 C");
    f = z80_flags_rot(1, 0x01, 0, &res);  /* RRC 0x01 -> 0x80, C=1 */
    CHECK_EQ_U(res, 0x80, "RRC 01 res"); CHECK_EQ_U(f & (Z80_SF|Z80_CF), (Z80_SF|Z80_CF), "RRC 01 SF/CF");
    f = z80_flags_rot(6, 0x00, 0, &res);  /* SLL 0x00 -> 0x01 (undoc) */
    CHECK_EQ_U(res, 0x01, "SLL 00 res");

    /* RLA through carry */
    f = z80_flags_rot_a(2, 0x80, 0, &res);  /* RLA, in C=0 -> res=0x00, C=1 */
    CHECK_EQ_U(res, 0x00, "RLA 80 res"); CHECK_EQ_U(f & Z80_CF, Z80_CF, "RLA 80 C");

    /* ---- BIT ---- */
    f = z80_flags_bit(7, 0x80, 0x80, 0);  /* bit7 set */
    CHECK_EQ_U(f & Z80_ZF, 0, "BIT 7 of 80 not zero");
    CHECK_EQ_U(f & Z80_SF, Z80_SF, "BIT 7 of 80 sets SF");
    CHECK_EQ_U(f & Z80_HF, Z80_HF, "BIT sets HF");
    f = z80_flags_bit(0, 0x00, 0x00, 0);  /* bit0 clear */
    CHECK_EQ_U(f & (Z80_ZF | Z80_PF), (Z80_ZF | Z80_PF), "BIT clear sets Z and PV");

    /* ---- DAA known answers ---- */
    f = z80_flags_daa(0x9A, 0, &res);   /* add path, low>9 & a>0x99 */
    CHECK_EQ_U(res, 0x00, "DAA 9A res");
    CHECK_EQ_U(f & Z80_CF, Z80_CF, "DAA 9A carry");
    f = z80_flags_daa(0x0A, 0, &res);
    CHECK_EQ_U(res, 0x10, "DAA 0A res");
    f = z80_flags_daa(0x1F, Z80_HF, &res); /* low=F>9 plus H -> +6 -> 0x25 */
    CHECK_EQ_U(res, 0x25, "DAA 1F+H res");

    /* ---- SCF / CCF / CPL ---- */
    f = z80_flags_scf(0x28, 0);  /* X/Y from A=0x28 -> both bit5 and bit3 set */
    CHECK_EQ_U(f & Z80_CF, Z80_CF, "SCF sets C");
    CHECK_EQ_U(f & (Z80_YF | Z80_XF), (Z80_YF | Z80_XF), "SCF X/Y from A");
    f = z80_flags_ccf(0x00, Z80_CF); /* C=1 -> C=0, H=old C=1 */
    CHECK_EQ_U(f & Z80_CF, 0, "CCF clears C");
    CHECK_EQ_U(f & Z80_HF, Z80_HF, "CCF H = old C");
    f = z80_flags_cpl(0x0F, 0, &res);
    CHECK_EQ_U(res, 0xF0, "CPL res");
    CHECK_EQ_U(f & (Z80_HF | Z80_NF), (Z80_HF | Z80_NF), "CPL sets H,N");

    TEST_SUMMARY();
}
