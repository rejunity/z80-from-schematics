/* End-to-end execution + sub-cycle timing tests for the engine. */
#include "z80_sim.h"
#include "test_util.h"

/* run until instr_count advances; return phases elapsed for that instruction.
   (call after a warm-up instruction to avoid the post-reset priming offset) */
static int measure_instr(z80_system_t *s)
{
    uint64_t ic = s->cpu.instr_count;
    uint64_t c0 = s->cpu.cycle;
    while (s->cpu.instr_count == ic) z80_sys_phase(s);
    return (int)(s->cpu.cycle - c0);
}

static void test_functional(void)
{
    static const uint8_t prog[] = {
        0x3E, 0x05,        /* LD A,5         */
        0x06, 0x03,        /* LD B,3         */
        0x80,              /* ADD A,B  -> 8  */
        0x21, 0x00, 0x40,  /* LD HL,0x4000   */
        0x77,              /* LD (HL),A      */
        0x23,              /* INC HL         */
        0x36, 0xAA,        /* LD (HL),0xAA   */
        0x4E,              /* LD C,(HL)      */
        0xAF,              /* XOR A    -> 0  */
        0x76               /* HALT           */
    };
    z80_system_t s;
    z80_sys_init(&s);
    z80_sys_load(&s, 0x0000, prog, sizeof(prog));

    z80_sys_run_instrs(&s, 10); /* 9 ops + HALT */

    CHECK_EQ_U(s.cpu.rf[RFP_AF] >> 8, 0x00, "A after XOR A");
    CHECK_EQ_U(z80_get_r8(&s.cpu, REG_B), 0x03, "B");
    CHECK_EQ_U(z80_get_r8(&s.cpu, REG_C), 0xAA, "C = (HL)");
    CHECK_EQ_U(s.cpu.rf[RFP_HL], 0x4001, "HL after INC");
    CHECK_EQ_U(s.cpu.rf[RFP_AF] & 0xFF, (Z80_ZF | Z80_PF), "F after XOR A");
    CHECK_EQ_U(s.mem[0x4000], 0x08, "mem[4000] = A");
    CHECK_EQ_U(s.mem[0x4001], 0xAA, "mem[4001] = AA");
    CHECK(s.cpu.halted, "halted after HALT");
}

static void test_timing(void)
{
    static const uint8_t prog[] = {
        0x00,              /* NOP  (warm-up, discarded) */
        0x3E, 0x12,        /* LD A,0x12       14 */
        0x47,              /* LD B,A           8 */
        0x80,              /* ADD A,B          8 */
        0x21, 0x34, 0x12,  /* LD HL,0x1234    20 */
        0x23,              /* INC HL          12 */
        0x77,              /* LD (HL),A       14 */
        0x7E,              /* LD A,(HL)       14 */
        0x34,              /* INC (HL)        22 */
        0x29,              /* ADD HL,HL       22 */
        0xC3, 0x00, 0x00   /* JP 0x0000       20 */
    };
    static const int expect[] = { 14, 8, 8, 20, 12, 14, 14, 22, 22, 20 };
    const char *names[] = { "LD A,n", "LD B,A", "ADD A,B", "LD HL,nn", "INC HL",
                            "LD (HL),A", "LD A,(HL)", "INC (HL)", "ADD HL,HL", "JP nn" };

    z80_system_t s;
    z80_sys_init(&s);
    z80_sys_load(&s, 0x0000, prog, sizeof(prog));

    (void)measure_instr(&s); /* warm-up NOP */
    for (int i = 0; i < 10; i++) {
        int ph = measure_instr(&s);
        CHECK_EQ_U(ph, expect[i], names[i]);
    }
}

/* sanity-check the M1 bus pattern of a single fetch */
static void test_m1_trace(void)
{
    static const uint8_t prog[] = { 0x00 }; /* NOP */
    z80_system_t s;
    z80_sys_init(&s);
    z80_sys_load(&s, 0x0000, prog, sizeof(prog));

    int saw_m1_low = 0, saw_rfsh_low = 0, saw_opcode_read = 0;
    for (int i = 0; i < 8; i++) {        /* one M1 = 8 phases */
        z80_sys_phase(&s);
        if (s.cpu.pins.m1_n == 0) saw_m1_low = 1;
        if (s.cpu.pins.rfsh_n == 0) saw_rfsh_low = 1;
        if (!s.cpu.pins.mreq_n && !s.cpu.pins.rd_n && s.cpu.pins.addr == 0x0000)
            saw_opcode_read = 1;
    }
    CHECK(saw_m1_low, "M1 asserted during fetch");
    CHECK(saw_rfsh_low, "RFSH asserted during M1 refresh");
    CHECK(saw_opcode_read, "MREQ+RD asserted at PC during fetch");
}

int main(void)
{
    test_functional();
    test_timing();
    test_m1_trace();
    TEST_SUMMARY();
}
