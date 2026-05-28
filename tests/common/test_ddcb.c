/* DDCB/FDCB tests: rotate/BIT/RES/SET on (IX+d), incl. undocumented reg copy. */
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
        0xDD, 0x36, 0x03, 0x80,  /* LD (IX+3),0x80    */
        0xDD, 0xCB, 0x03, 0x06,  /* RLC (IX+3) -> 0x01 (z=6, no copy) */
        0xDD, 0xCB, 0x03, 0x00,  /* RLC (IX+3) -> 0x02, copy to B     */
        0xDD, 0xCB, 0x03, 0xC6,  /* SET 0,(IX+3) -> 0x03              */
        0xDD, 0xCB, 0x03, 0x46,  /* BIT 0,(IX+3) -> Z clear           */
        0x76
    };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0, prog, sizeof(prog));
    int n = 0; while (!s.cpu.halted && n < 100000) { z80_sys_step_instr(&s); n++; }

    CHECK_EQ_U(s.mem[0x9003], 0x03, "(IX+3) after RLC,RLC,SET");
    CHECK_EQ_U(z80_get_r8(&s.cpu, REG_B), 0x02, "DDCB undoc copy to B");
    CHECK_EQ_U(s.cpu.rf[RFP_AF] & Z80_ZF, 0, "BIT 0,(IX+3) of 0x03 -> Z clear");
    CHECK(s.cpu.halted, "halted");
}

static void test_timing(void)
{
    static const uint8_t prog[] = {
        0x00,                         /* NOP warm-up          */
        0xDD, 0x21, 0x00, 0x90,       /* LD IX,nn   14T -> 28 */
        0xDD, 0xCB, 0x00, 0x46,       /* BIT 0,(IX+0) 20T -> 40 */
        0xDD, 0xCB, 0x00, 0xC6,       /* SET 0,(IX+0) 23T -> 46 */
        0x76
    };
    static const int expect[] = { 28, 40, 46 };
    const char *names[] = { "LD IX,nn", "BIT 0,(IX+d)", "SET 0,(IX+d)" };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0, prog, sizeof(prog));
    (void)measure_instr(&s);
    for (int i = 0; i < 3; i++)
        CHECK_EQ_U(measure_instr(&s), expect[i], names[i]);
}

int main(void)
{
    test_functional();
    test_timing();
    TEST_SUMMARY();
}
