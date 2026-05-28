/* ED block-instruction tests: LDIR, LDDR, CPIR. */
#include "z80_sim.h"
#include "test_util.h"

static void run_to_halt(z80_system_t *s){
    int n=0; while(!s->cpu.halted && n<100000){ z80_sys_step_instr(s); n++; }
}

static void test_ldir(void)
{
    static const uint8_t prog[] = {
        0x21, 0x00, 0x90,  /* LD HL,0x9000 */
        0x11, 0x00, 0x91,  /* LD DE,0x9100 */
        0x01, 0x04, 0x00,  /* LD BC,0x0004 */
        0xED, 0xB0,        /* LDIR         */
        0x76
    };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0, prog, sizeof(prog));
    s.mem[0x9000] = 0x11; s.mem[0x9001] = 0x22;
    s.mem[0x9002] = 0x33; s.mem[0x9003] = 0x44;
    run_to_halt(&s);

    CHECK_EQ_U(s.mem[0x9100], 0x11, "LDIR[0]");
    CHECK_EQ_U(s.mem[0x9101], 0x22, "LDIR[1]");
    CHECK_EQ_U(s.mem[0x9102], 0x33, "LDIR[2]");
    CHECK_EQ_U(s.mem[0x9103], 0x44, "LDIR[3]");
    CHECK_EQ_U(s.cpu.rf[RFP_BC], 0x0000, "LDIR BC=0");
    CHECK_EQ_U(s.cpu.rf[RFP_HL], 0x9004, "LDIR HL");
    CHECK_EQ_U(s.cpu.rf[RFP_DE], 0x9104, "LDIR DE");
    CHECK_EQ_U(s.cpu.rf[RFP_AF] & Z80_PF, 0, "LDIR PV clear (BC=0)");
}

static void test_cpir(void)
{
    static const uint8_t prog[] = {
        0x21, 0x00, 0x90,  /* LD HL,0x9000 */
        0x01, 0x04, 0x00,  /* LD BC,0x0004 */
        0x3E, 0x03,        /* LD A,0x03    */
        0xED, 0xB1,        /* CPIR         */
        0x76
    };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0, prog, sizeof(prog));
    s.mem[0x9000] = 0x01; s.mem[0x9001] = 0x02;
    s.mem[0x9002] = 0x03; s.mem[0x9003] = 0x04;
    run_to_halt(&s);

    CHECK_EQ_U(s.cpu.rf[RFP_HL], 0x9003, "CPIR HL past match");
    CHECK_EQ_U(s.cpu.rf[RFP_BC], 0x0001, "CPIR BC");
    CHECK_EQ_U(s.cpu.rf[RFP_AF] & Z80_ZF, Z80_ZF, "CPIR found -> Z set");
}

static void test_lddr(void)
{
    static const uint8_t prog[] = {
        0x21, 0x03, 0x90,  /* LD HL,0x9003 */
        0x11, 0x03, 0x91,  /* LD DE,0x9103 */
        0x01, 0x04, 0x00,  /* LD BC,0x0004 */
        0xED, 0xB8,        /* LDDR         */
        0x76
    };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0, prog, sizeof(prog));
    for (int i = 0; i < 4; i++) s.mem[0x9000 + i] = (uint8_t)(0xA0 + i);
    run_to_halt(&s);

    for (int i = 0; i < 4; i++)
        CHECK_EQ_U(s.mem[0x9100 + i], (uint8_t)(0xA0 + i), "LDDR copy");
    CHECK_EQ_U(s.cpu.rf[RFP_BC], 0x0000, "LDDR BC=0");
    CHECK_EQ_U(s.cpu.rf[RFP_HL], 0x8FFF, "LDDR HL");
}

int main(void)
{
    test_ldir();
    test_cpir();
    test_lddr();
    TEST_SUMMARY();
}
