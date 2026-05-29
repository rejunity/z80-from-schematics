/* Interrupt / NMI / HALT / BUSREQ tests. */
#include "z80_sim.h"
#include "test_util.h"

static void step_instr(z80_system_t *s)
{
    uint64_t ic = s->cpu.instr_count; int n = 0;
    while (s->cpu.instr_count == ic && n < 100000) { z80_sys_phase(s); n++; }
}
static void run_to_halt(z80_system_t *s)
{
    int n = 0; while (!s->cpu.halted && n < 100000) { z80_sys_phase(s); n++; }
}

static void test_nmi(void)
{
    uint8_t prog[] = { 0x31, 0xF0, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00 }; /* LD SP,FFF0; NOPs */
    z80_system_t s; z80_sys_init(&s); z80_sys_load(&s, 0, prog, sizeof prog);
    s.mem[0x0066] = 0x76;                 /* HALT at the NMI vector */
    step_instr(&s);                        /* LD SP,FFF0 */
    s.cpu.pins.nmi_n = 0; z80_sys_phase(&s); s.cpu.pins.nmi_n = 1;  /* NMI edge */
    run_to_halt(&s);
    CHECK(s.cpu.halted, "NMI reached handler (HALT at 0x0066)");
    CHECK_EQ_U(s.cpu.rf[RFP_SP], 0xFFEE, "NMI pushed return PC (SP -= 2)");
    uint16_t ret = (uint16_t)(s.mem[0xFFEE] | (s.mem[0xFFEF] << 8));
    CHECK(ret >= 0x0003 && ret <= 0x0008, "NMI return address is in the NOP run");
    CHECK_EQ_U(s.cpu.iff1, 0, "NMI cleared IFF1");
}

static void test_int_im1(void)
{
    uint8_t prog[] = { 0x31, 0xF0, 0xFF, 0xED, 0x56, 0xFB, 0x00, 0x00, 0x00 };
    /* LD SP,FFF0; IM1; EI; NOP; NOP; NOP */
    z80_system_t s; z80_sys_init(&s); z80_sys_load(&s, 0, prog, sizeof prog);
    s.mem[0x0038] = 0x76;                  /* HALT at IM1 vector */
    step_instr(&s);                         /* LD SP */
    step_instr(&s);                         /* IM1 */
    CHECK_EQ_U(s.cpu.im, 1, "IM 1 set");
    step_instr(&s);                         /* EI (sets ei_delay) */
    s.cpu.pins.int_n = 0;                   /* assert INT (level) */
    /* EI delay: the instruction right after EI must still execute (no INT yet) */
    uint16_t pc_before = s.cpu.rf[RFP_PC];
    step_instr(&s);                         /* the NOP after EI executes */
    CHECK(s.cpu.rf[RFP_PC] != 0x0039 && pc_before <= 0x0008, "EI delays INT by one instruction");
    run_to_halt(&s);
    CHECK(s.cpu.halted, "INT IM1 reached handler (HALT at 0x0038)");
    CHECK_EQ_U(s.cpu.iff1, 0, "INT cleared IFF1");
    CHECK_EQ_U(s.cpu.rf[RFP_SP], 0xFFEE, "INT pushed return PC");
}

static void test_int_im2(void)
{
    uint8_t prog[] = { 0x31, 0xF0, 0xFF, 0xED, 0x5E, 0x3E, 0x80, 0xED, 0x47, 0xFB, 0x00, 0x00 };
    /* LD SP,FFF0; IM2; LD A,0x80; LD I,A; EI; NOP; NOP */
    z80_system_t s; z80_sys_init(&s); z80_sys_load(&s, 0, prog, sizeof prog);
    /* vector table: I=0x80, device supplies vector 0x00 -> addr 0x8000 holds handler ptr */
    s.mem[0x8000] = 0x66; s.mem[0x8001] = 0x00;  /* handler at 0x0066 */
    s.mem[0x0066] = 0x76;                         /* HALT */
    step_instr(&s); step_instr(&s); step_instr(&s); step_instr(&s); /* LD SP,IM2,LD A,LD I,A */
    CHECK_EQ_U(s.cpu.im, 2, "IM 2 set");
    CHECK_EQ_U(s.cpu.reg_i, 0x80, "I = 0x80");
    step_instr(&s);                         /* EI */
    s.cpu.pins.int_n = 0;                   /* device drives vector 0x00 (default data_in) */
    run_to_halt(&s);
    CHECK(s.cpu.halted, "INT IM2 vectored through {I,vec} to handler");
}

static void test_halt_release(void)
{
    uint8_t prog[] = { 0x76, 0x00, 0x00 }; /* HALT; NOP; NOP */
    z80_system_t s; z80_sys_init(&s); z80_sys_load(&s, 0, prog, sizeof prog);
    s.mem[0x0066] = 0x00;                   /* NMI handler: NOP (then runs on) */
    step_instr(&s);                         /* execute HALT */
    CHECK(s.cpu.halted, "HALT sets halted");
    /* halted: halt_n asserted; CPU loops NOPs at the HALT */
    for (int i = 0; i < 20; i++) z80_sys_phase(&s);
    CHECK_EQ_U(s.cpu.pins.halt_n, 0, "halt_n asserted while halted");
    CHECK(s.cpu.halted, "still halted without interrupt");
    s.cpu.pins.nmi_n = 0; z80_sys_phase(&s); s.cpu.pins.nmi_n = 1;  /* NMI releases HALT */
    for (int i = 0; i < 30; i++) z80_sys_phase(&s);
    CHECK_EQ_U(s.cpu.halted, 0, "NMI released HALT");
}

static void test_busreq(void)
{
    uint8_t prog[] = { 0x00, 0x00, 0x00, 0x00 }; /* NOPs */
    z80_system_t s; z80_sys_init(&s); z80_sys_load(&s, 0, prog, sizeof prog);
    step_instr(&s);
    s.cpu.pins.busreq_n = 0;                /* request the bus */
    for (int i = 0; i < 30; i++) z80_sys_phase(&s);
    CHECK_EQ_U(s.cpu.pins.busack_n, 0, "BUSACK asserted on BUSREQ");
    uint64_t ic = s.cpu.instr_count;
    for (int i = 0; i < 30; i++) z80_sys_phase(&s);
    CHECK_EQ_U(s.cpu.instr_count, ic, "no instructions execute while bus granted");
    s.cpu.pins.busreq_n = 1;                /* release */
    for (int i = 0; i < 20; i++) z80_sys_phase(&s);
    CHECK_EQ_U(s.cpu.pins.busack_n, 1, "BUSACK released");
    CHECK(s.cpu.instr_count > ic, "execution resumes after BUSREQ release");
}

static void test_wait(void)
{
    uint8_t prog[] = { 0x00, 0x00 };        /* NOPs */
    z80_system_t s; z80_sys_init(&s); z80_sys_load(&s, 0, prog, sizeof prog);
    s.cpu.pins.wait_n = 0;                   /* hold WAIT asserted */
    for (int i = 0; i < 40; i++) z80_sys_phase(&s);
    CHECK_EQ_U(s.cpu.instr_count, 0, "WAIT stalls the CPU indefinitely");
    s.cpu.pins.wait_n = 1;                   /* release */
    for (int i = 0; i < 20; i++) z80_sys_phase(&s);
    CHECK(s.cpu.instr_count >= 1, "CPU resumes and completes after WAIT released");
}

int main(void)
{
    test_nmi();
    test_int_im1();
    test_int_im2();
    test_halt_release();
    test_busreq();
    test_wait();
    TEST_SUMMARY();
}
