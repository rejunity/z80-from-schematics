/* DD/FD (IX/IY) tests: index 16-bit ops, (IX+d) memory, IXH/IXL half regs. */
#include "z80_sim.h"
#include "test_util.h"

static int measure_instr(z80_system_t *s)
{
    uint64_t ic = s->cpu.instr_count, c0 = s->cpu.cycle;
    while (s->cpu.instr_count == ic) z80_sys_phase(s);
    return (int)(s->cpu.cycle - c0);
}

static void test_functional(void)
{
    static const uint8_t prog[] = {
        0xDD, 0x21, 0x00, 0x90,  /* LD IX,0x9000      */
        0xDD, 0x36, 0x05, 0xAA,  /* LD (IX+5),0xAA    */
        0xDD, 0x7E, 0x05,        /* LD A,(IX+5) -> AA */
        0xDD, 0x34, 0x05,        /* INC (IX+5) -> AB  */
        0xDD, 0x86, 0x05,        /* ADD A,(IX+5)->0x55*/
        0xDD, 0x23,              /* INC IX -> 0x9001  */
        0xDD, 0x26, 0x12,        /* LD IXH,0x12       */
        0xDD, 0x2E, 0x34,        /* LD IXL,0x34 ->1234*/
        0x01, 0x11, 0x00,        /* LD BC,0x0011      */
        0xDD, 0x09,              /* ADD IX,BC -> 1245 */
        0xDD, 0xE5,              /* PUSH IX           */
        0xE1,                    /* POP HL -> 0x1245  */
        0x76                     /* HALT              */
    };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0, prog, sizeof(prog));
    int n = 0; while (!s.cpu.halted && n < 100000) { z80_sys_step_instr(&s); n++; }

    CHECK_EQ_U(s.mem[0x9005], 0xAB, "(IX+5) after INC");
    CHECK_EQ_U(s.cpu.rf[RFP_AF] >> 8, 0x55, "A after ADD A,(IX+5)");
    CHECK_EQ_U(s.cpu.rf[RFP_IX], 0x1245, "IX after IXH/IXL/ADD");
    CHECK_EQ_U(s.cpu.rf[RFP_HL], 0x1245, "HL after PUSH IX/POP HL");
    CHECK(s.cpu.halted, "halted");
}

static void test_iy(void)
{
    static const uint8_t prog[] = {
        0xFD, 0x21, 0x34, 0x12,  /* LD IY,0x1234 */
        0xFD, 0x23,              /* INC IY       */
        0x76
    };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0, prog, sizeof(prog));
    int n = 0; while (!s.cpu.halted && n < 100000) { z80_sys_step_instr(&s); n++; }
    CHECK_EQ_U(s.cpu.rf[RFP_IY], 0x1235, "IY after LD/INC");
}

static void test_timing(void)
{
    static const uint8_t prog[] = {
        0x00,                    /* NOP warm-up           */
        0xDD, 0x21, 0x00, 0x90,  /* LD IX,nn   14T -> 28  */
        0xDD, 0x23,              /* INC IX     10T -> 20  */
        0x01, 0x11, 0x00,        /* LD BC,nn   10T -> 20  */
        0xDD, 0x09,              /* ADD IX,BC  15T -> 30  */
        0xDD, 0x7E, 0x05,        /* LD A,(IX+5)19T -> 38  */
        0x76
    };
    static const int expect[] = { 28, 20, 20, 30, 38 };
    const char *names[] = { "LD IX,nn", "INC IX", "LD BC,nn", "ADD IX,BC", "LD A,(IX+d)" };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0, prog, sizeof(prog));
    (void)measure_instr(&s);
    for (int i = 0; i < 5; i++)
        CHECK_EQ_U(measure_instr(&s), expect[i], names[i]);
}

int main(void)
{
    test_functional();
    test_iy();
    test_timing();
    TEST_SUMMARY();
}
