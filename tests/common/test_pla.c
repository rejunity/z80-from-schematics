/* PLA decode row-matching tests: representative opcodes -> control word. */
#include "z80.h"
#include "test_util.h"

static z80_control_t D(uint8_t op) { return z80_pla_decode(PFX_NONE, op); }

int main(void)
{
    z80_control_t c;

    c = D(0x00); CHECK_EQ_U(c.exec, EXEC_NOP, "00 NOP");
    c = D(0x76); CHECK_EQ_U(c.exec, EXEC_HALT, "76 HALT");

    c = D(0x41); /* LD B,C */
    CHECK_EQ_U(c.exec, EXEC_LD_R_R, "41 exec");
    CHECK_EQ_U(c.rf_dst, REG_B, "41 dst B"); CHECK_EQ_U(c.rf_src, REG_C, "41 src C");

    c = D(0x46); CHECK_EQ_U(c.exec, EXEC_LD_R_M, "46 LD B,(HL)");
    c = D(0x70); CHECK_EQ_U(c.exec, EXEC_ST_M_R, "70 LD (HL),B");

    c = D(0x3E); CHECK_EQ_U(c.exec, EXEC_LD_R_N, "3E LD A,n");
    CHECK_EQ_U(c.rf_dst, REG_A, "3E dst A");

    c = D(0x80); /* ADD A,B */
    CHECK_EQ_U(c.exec, EXEC_ALU_R, "80 exec");
    CHECK_EQ_U(c.alu_op, ALU_ADD, "80 op ADD"); CHECK_EQ_U(c.rf_src, REG_B, "80 src");
    c = D(0xB8); CHECK_EQ_U(c.alu_op, ALU_CP, "B8 CP B"); /* CP B */
    c = D(0xC6); CHECK_EQ_U(c.exec, EXEC_ALU_N, "C6 ADD A,n");
    c = D(0x86); CHECK_EQ_U(c.exec, EXEC_ALU_M, "86 ADD A,(HL)");

    c = D(0x04); CHECK_EQ_U(c.exec, EXEC_INC_R, "04 INC B");
    c = D(0x34); CHECK_EQ_U(c.exec, EXEC_INC_M, "34 INC (HL)");
    c = D(0x35); CHECK_EQ_U(c.exec, EXEC_DEC_M, "35 DEC (HL)");

    c = D(0x21); CHECK_EQ_U(c.exec, EXEC_LD_RP_NN, "21 LD HL,nn");
    CHECK_EQ_U(c.rp_sel, RFP_HL, "21 rp HL");
    c = D(0x33); CHECK_EQ_U(c.exec, EXEC_INC_RP, "33 INC SP");
    CHECK_EQ_U(c.rp_sel, RFP_SP, "33 rp SP");
    c = D(0x09); CHECK_EQ_U(c.exec, EXEC_ADD_HL_RP, "09 ADD HL,BC");

    c = D(0xC3); CHECK_EQ_U(c.exec, EXEC_JP, "C3 JP nn");
    c = D(0xCA); CHECK_EQ_U(c.exec, EXEC_JP_CC, "CA JP Z,nn");
    CHECK_EQ_U(c.cc, 1, "CA cc=Z");
    c = D(0x18); CHECK_EQ_U(c.exec, EXEC_JR, "18 JR");
    c = D(0x20); CHECK_EQ_U(c.exec, EXEC_JR_CC, "20 JR NZ");
    c = D(0x10); CHECK_EQ_U(c.exec, EXEC_DJNZ, "10 DJNZ");

    c = D(0xCD); CHECK_EQ_U(c.exec, EXEC_CALL, "CD CALL");
    c = D(0xC4); CHECK_EQ_U(c.exec, EXEC_CALL_CC, "C4 CALL NZ");
    c = D(0xC9); CHECK_EQ_U(c.exec, EXEC_RET, "C9 RET");
    c = D(0xC0); CHECK_EQ_U(c.exec, EXEC_RET_CC, "C0 RET NZ");
    c = D(0xFF); CHECK_EQ_U(c.exec, EXEC_RST, "FF RST"); CHECK_EQ_U(c.rst_addr, 0x38, "FF rst=38");
    c = D(0xC7); CHECK_EQ_U(c.rst_addr, 0x00, "C7 rst=00");

    c = D(0xC5); CHECK_EQ_U(c.exec, EXEC_PUSH, "C5 PUSH BC"); CHECK_EQ_U(c.rp_sel, RFP_BC, "C5 rp");
    c = D(0xF5); CHECK_EQ_U(c.exec, EXEC_PUSH, "F5 PUSH AF"); CHECK_EQ_U(c.rp_sel, RFP_AF, "F5 rp AF");
    c = D(0xF1); CHECK_EQ_U(c.exec, EXEC_POP, "F1 POP AF");

    c = D(0x07); CHECK_EQ_U(c.exec, EXEC_ROT_A, "07 RLCA"); CHECK_EQ_U(c.rot_op, 0, "07 rot");
    c = D(0x27); CHECK_EQ_U(c.exec, EXEC_DAA, "27 DAA");
    c = D(0x2F); CHECK_EQ_U(c.exec, EXEC_CPL, "2F CPL");
    c = D(0x37); CHECK_EQ_U(c.exec, EXEC_SCF, "37 SCF");
    c = D(0x3F); CHECK_EQ_U(c.exec, EXEC_CCF, "3F CCF");

    c = D(0xEB); CHECK_EQ_U(c.exec, EXEC_EX_DE_HL, "EB EX DE,HL");
    c = D(0x08); CHECK_EQ_U(c.exec, EXEC_EX_AF, "08 EX AF,AF'");
    c = D(0xD9); CHECK_EQ_U(c.exec, EXEC_EXX, "D9 EXX");
    c = D(0xE3); CHECK_EQ_U(c.exec, EXEC_EX_SP_HL, "E3 EX (SP),HL");
    c = D(0xE9); CHECK_EQ_U(c.exec, EXEC_JP_HL, "E9 JP (HL)");
    c = D(0xF9); CHECK_EQ_U(c.exec, EXEC_LD_SP_HL, "F9 LD SP,HL");

    c = D(0x0A); CHECK_EQ_U(c.exec, EXEC_LD_A_RP, "0A LD A,(BC)"); CHECK_EQ_U(c.rp_sel, RFP_BC, "0A rp");
    c = D(0x12); CHECK_EQ_U(c.exec, EXEC_LD_RP_A, "12 LD (DE),A"); CHECK_EQ_U(c.rp_sel, RFP_DE, "12 rp");
    c = D(0x3A); CHECK_EQ_U(c.exec, EXEC_LD_A_NN, "3A LD A,(nn)");
    c = D(0x32); CHECK_EQ_U(c.exec, EXEC_LD_NN_A, "32 LD (nn),A");
    c = D(0x2A); CHECK_EQ_U(c.exec, EXEC_LD_HL_NN, "2A LD HL,(nn)");
    c = D(0x22); CHECK_EQ_U(c.exec, EXEC_LD_NN_HL, "22 LD (nn),HL");

    c = D(0xD3); CHECK_EQ_U(c.exec, EXEC_OUT_N_A, "D3 OUT (n),A");
    c = D(0xDB); CHECK_EQ_U(c.exec, EXEC_IN_A_N, "DB IN A,(n)");
    c = D(0xF3); CHECK_EQ_U(c.exec, EXEC_DI, "F3 DI");
    c = D(0xFB); CHECK_EQ_U(c.exec, EXEC_EI, "FB EI");

    c = D(0xCB); CHECK_EQ_U(c.exec, EXEC_PREFIX, "CB prefix");
    c = D(0xDD); CHECK_EQ_U(c.exec, EXEC_PREFIX, "DD prefix");
    c = D(0xED); CHECK_EQ_U(c.exec, EXEC_PREFIX, "ED prefix");
    c = D(0xFD); CHECK_EQ_U(c.exec, EXEC_PREFIX, "FD prefix");

    TEST_SUMMARY();
}
