/* CB-prefix instruction tests: rotates/shifts + BIT/RES/SET, reg and (HL). */
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
        0x3E, 0x80,        /* LD A,0x80        */
        0xCB, 0x07,        /* RLC A  -> 0x01,C  */
        0x06, 0x01,        /* LD B,0x01        */
        0xCB, 0x00,        /* RLC B  -> 0x02    */
        0xCB, 0x78,        /* BIT 7,B -> Z set  */
        0x21, 0x00, 0x90,  /* LD HL,0x9000     */
        0x36, 0x80,        /* LD (HL),0x80     */
        0xCB, 0x06,        /* RLC (HL) -> 0x01  */
        0xCB, 0xFE,        /* SET 7,(HL)->0x81  */
        0xCB, 0x46,        /* BIT 0,(HL)->Z clr */
        0x76               /* HALT             */
    };
    z80_system_t s;
    z80_sys_init(&s);
    z80_sys_load(&s, 0x0000, prog, sizeof(prog));
    z80_sys_run_instrs(&s, 11);

    CHECK_EQ_U(s.cpu.rf[RFP_AF] >> 8, 0x01, "RLC A result");
    CHECK_EQ_U(z80_get_r8(&s.cpu, REG_B), 0x02, "RLC B result");
    CHECK_EQ_U(s.mem[0x9000], 0x81, "RLC then SET 7 on (HL)");
    CHECK_EQ_U(s.cpu.rf[RFP_AF] & Z80_ZF, 0, "BIT 0,(HL) of 0x81 -> Z clear");
    CHECK(s.cpu.halted, "halted");
}

static void test_bit_flags(void)
{
    /* BIT 7,A on 0x80: Z clear, S set, H set, N clear */
    static const uint8_t prog[] = { 0x3E, 0x80, 0xCB, 0x7F, 0x76 };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0x0000, prog, sizeof(prog));
    z80_sys_run_instrs(&s, 2); /* LD A,0x80 ; BIT 7,A */
    uint8_t f = s.cpu.rf[RFP_AF] & 0xFF;
    CHECK_EQ_U(f & Z80_ZF, 0, "BIT 7 of 0x80: Z clear");
    CHECK_EQ_U(f & Z80_SF, Z80_SF, "BIT 7 of 0x80: S set");
    CHECK_EQ_U(f & Z80_HF, Z80_HF, "BIT: H set");
    CHECK_EQ_U(f & Z80_NF, 0, "BIT: N clear");
}

static void test_timing(void)
{
    static const uint8_t prog[] = {
        0x00,              /* NOP (warm-up)        */
        0xCB, 0x00,        /* RLC B      8T  -> 16  */
        0xCB, 0x40,        /* BIT 0,B    8T  -> 16  */
        0xCB, 0xC0,        /* SET 0,B    8T  -> 16  */
        0xCB, 0x80,        /* RES 0,B    8T  -> 16  */
        0x21, 0x00, 0x90,  /* LD HL,9000 10T -> 20  */
        0xCB, 0x06,        /* RLC (HL)  15T  -> 30  */
        0xCB, 0x46,        /* BIT 0,(HL)12T  -> 24  */
        0xCB, 0xC6,        /* SET 0,(HL)15T  -> 30  */
        0x76
    };
    static const int expect[] = { 16, 16, 16, 16, 20, 30, 24, 30 };
    const char *names[] = { "RLC B", "BIT 0,B", "SET 0,B", "RES 0,B",
                            "LD HL,nn", "RLC (HL)", "BIT 0,(HL)", "SET 0,(HL)" };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0x0000, prog, sizeof(prog));
    (void)measure_instr(&s); /* warm-up NOP */
    for (int i = 0; i < 8; i++)
        CHECK_EQ_U(measure_instr(&s), expect[i], names[i]);
}

int main(void)
{
    test_functional();
    test_bit_flags();
    test_timing();
    TEST_SUMMARY();
}
