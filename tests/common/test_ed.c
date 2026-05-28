/* ED-prefix (non-block) tests: ADC/SBC HL, NEG, IM, LD I/R, LD (nn),rp,
   IN/OUT (C), RRD/RLD. */
#include "z80_sim.h"
#include "test_util.h"

static int measure_instr(z80_system_t *s)
{
    uint64_t ic = s->cpu.instr_count, c0 = s->cpu.cycle;
    while (s->cpu.instr_count == ic) z80_sys_phase(s);
    return (int)(s->cpu.cycle - c0);
}

static void test_adc_sbc16(void)
{
    static const uint8_t prog[] = {
        0x21, 0x34, 0x12,  /* LD HL,0x1234 */
        0x11, 0x11, 0x11,  /* LD DE,0x1111 */
        0xB7,              /* OR A  (CF=0) */
        0xED, 0x52,        /* SBC HL,DE -> 0x0123 */
        0xED, 0x5A         /* ADC HL,DE -> 0x1234 */
    };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0, prog, sizeof(prog));
    z80_sys_run_instrs(&s, 4);                  /* through SBC */
    CHECK_EQ_U(s.cpu.rf[RFP_HL], 0x0123, "SBC HL,DE");
    z80_sys_run_instrs(&s, 1);                  /* ADC */
    CHECK_EQ_U(s.cpu.rf[RFP_HL], 0x1234, "ADC HL,DE");
}

static void test_neg(void)
{
    static const uint8_t prog[] = { 0x3E, 0x01, 0xED, 0x44, 0x76 };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0, prog, sizeof(prog));
    z80_sys_run_instrs(&s, 2);
    CHECK_EQ_U(s.cpu.rf[RFP_AF] >> 8, 0xFF, "NEG 1 -> FF");
    /* result 0xFF also sets undocumented X/Y (bits 5,3) */
    CHECK_EQ_U(s.cpu.rf[RFP_AF] & 0xFF,
               (Z80_SF | Z80_YF | Z80_HF | Z80_XF | Z80_NF | Z80_CF), "NEG 1 flags");
}

static void test_misc(void)
{
    static const uint8_t prog[] = {
        0xED, 0x5E,        /* IM 2          */
        0x3E, 0x55,        /* LD A,0x55     */
        0xED, 0x47,        /* LD I,A        */
        0xAF,              /* XOR A         */
        0xED, 0x57,        /* LD A,I -> 0x55*/
        0x01, 0xEF, 0xBE,  /* LD BC,0xBEEF  */
        0xED, 0x43, 0x00, 0x90, /* LD (0x9000),BC */
        0xED, 0x5B, 0x00, 0x90, /* LD DE,(0x9000) */
        0x21, 0x10, 0x90,  /* LD HL,0x9010  */
        0x36, 0x12,        /* LD (HL),0x12  */
        0x3E, 0x34,        /* LD A,0x34     */
        0xED, 0x67,        /* RRD           */
        0x76               /* HALT          */
    };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0, prog, sizeof(prog));
    z80_sys_run_instrs(&s, 14);
    CHECK_EQ_U(s.cpu.im, 2, "IM 2");
    CHECK_EQ_U(s.cpu.reg_i, 0x55, "LD I,A");
    CHECK_EQ_U(s.cpu.rf[RFP_DE], 0xBEEF, "LD DE,(nn) <- LD (nn),BC");
    CHECK_EQ_U(s.mem[0x9000], 0xEF, "LD (nn),BC low");
    CHECK_EQ_U(s.mem[0x9001], 0xBE, "LD (nn),BC high");
    CHECK_EQ_U(s.cpu.rf[RFP_AF] >> 8, 0x32, "RRD A");
    CHECK_EQ_U(s.mem[0x9010], 0x41, "RRD (HL)");
}

static void test_inout_c(void)
{
    static const uint8_t prog[] = {
        0x01, 0x34, 0x00,  /* LD BC,0x0034 (port 0x34) */
        0x3E, 0xAB,        /* LD A,0xAB    */
        0xED, 0x79,        /* OUT (C),A    */
        0xED, 0x40,        /* IN B,(C)     */
        0x76
    };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0, prog, sizeof(prog));
    z80_sys_run_instrs(&s, 4);
    CHECK_EQ_U(s.io[0x0034], 0xAB, "OUT (C),A");
    CHECK_EQ_U(z80_get_r8(&s.cpu, REG_B), 0xAB, "IN B,(C)");
}

static void test_timing(void)
{
    static const uint8_t prog[] = {
        0x00,                   /* NOP warm-up */
        0xED, 0x52,             /* SBC HL,DE  15T -> 30 */
        0xED, 0x44,             /* NEG         8T -> 16 */
        0xED, 0x57,             /* LD A,I      9T -> 18 */
        0xED, 0x43, 0x00, 0x90, /* LD (nn),BC 20T -> 40 */
        0x76
    };
    static const int expect[] = { 30, 16, 18, 40 };
    const char *names[] = { "SBC HL,DE", "NEG", "LD A,I", "LD (nn),BC" };
    z80_system_t s; z80_sys_init(&s);
    z80_sys_load(&s, 0, prog, sizeof(prog));
    (void)measure_instr(&s);
    for (int i = 0; i < 4; i++)
        CHECK_EQ_U(measure_instr(&s), expect[i], names[i]);
}

int main(void)
{
    test_adc_sbc16();
    test_neg();
    test_misc();
    test_inout_c();
    test_timing();
    TEST_SUMMARY();
}
