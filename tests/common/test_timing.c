/* test_timing.c — per-T-state external-pin sequence assertions.
 *
 * Inspired by floooh/chips-test/tests/z80-timing.c (MIT) — the structure
 * (per-opcode UTEST-style assertions of the M1 / MREAD / MWRITE / IOREAD /
 * IOWRITE / refresh cycle sequence) is the same. The assertions themselves
 * use OUR per-half-cycle pin conventions, not chips/z80.h's "tick = T-state"
 * simplification — see docs/timing.md for the silicon-faithful contract our
 * model implements.
 *
 * Per-T-state convention used by the cycle helpers below: each helper
 * advances 2 z80_sys_phase calls (one full T-state = .P then .N) and then
 * checks the pin state AT T_n.N (the end of the T-state). At T_n.N each
 * cycle's pins are stable enough to assert against; transitions happen at
 * the next T_{n+1}.P or T_n.N->T_n+1.P boundary.
 *
 * Active-low pins, sampled at the .N (end) phase of each T-state:
 *
 *   M1 cycle (4 T-states)
 *     T1.N  m1=A  mreq=A rd=A  rfsh=-   (opcode-fetch window)
 *     T2.N  m1=A  mreq=A rd=A  rfsh=-   (data-in latch happens here)
 *     T3.N  m1=-  mreq=A rd=-  rfsh=A   (refresh MREQ active)
 *     T4.N  m1=-  mreq=- rd=-  rfsh=A   (refresh MREQ released, rfsh held)
 *
 *   MREAD cycle (3 T-states; m1 stays high throughout, rfsh inactive)
 *     T1.N  mreq=A rd=A
 *     T2.N  mreq=A rd=A     (read data latched at T_last.P)
 *     T3.N  mreq=- rd=-     (released at T_last.N)
 *
 *   MWRITE cycle (3 T-states)
 *     T1.N  mreq=A wr=-
 *     T2.N  mreq=A wr=A     (wr drops at T2.N)
 *     T3.N  mreq=- wr=-     (released at T_last.N)
 *
 * (A = asserted/low, - = deasserted/high)
 */
#include "z80_sim.h"
#include "test_util.h"

static z80_system_t S;

#define P(s) z80_sys_phase(s)

static void step_t(int n) {
    for (int i = 0; i < n; i++) { P(&S); P(&S); }
}
static void load(const uint8_t *bytes, size_t n) {
    z80_sys_init(&S);
    for (size_t i = 0; i < n; i++) S.mem[i] = bytes[i];
    /* the model is at reset state: M1 about to fetch at PC=0. The first
       z80_sys_phase advances to (T1, phi=0). After the first step_t(1)
       we'll be at the end of T1 (phi=1). */
}

/* Active-low predicates — return true when the named pin is ASSERTED. */
static int m1(void)    { return !S.cpu.pins.m1_n; }
static int mreq(void)  { return !S.cpu.pins.mreq_n; }
static int iorq(void)  { return !S.cpu.pins.iorq_n; }
static int rd(void)    { return !S.cpu.pins.rd_n; }
static int wr(void)    { return !S.cpu.pins.wr_n; }
static int rfsh(void)  { return !S.cpu.pins.rfsh_n; }

/* === Per-T-state pin predicates (sampled at the .N phase). === */
static int pins_m1_fetch(void) {
    /* T1.N / T2.N of M1: m1 + mreq + rd asserted, rfsh not yet */
    return m1() && mreq() && rd() && !rfsh();
}
static int pins_m1_refresh(void) {
    /* T3.N of M1: m1+rd released, mreq+rfsh asserted */
    return !m1() && mreq() && !rd() && rfsh();
}
static int pins_m1_end(void) {
    /* T4.N of M1: mreq released, rfsh STILL held (drops at next T1.P) */
    return !m1() && !mreq() && !iorq() && !rd() && !wr() && rfsh();
}
static int pins_mread_active(void) {
    /* T1.N / T2.N of MREAD: mreq + rd asserted */
    return !m1() && mreq() && rd() && !wr() && !rfsh();
}
static int pins_mread_end(void) {
    /* T3.N of MREAD: released */
    return !m1() && !mreq() && !iorq() && !rd() && !wr() && !rfsh();
}
static int pins_mwrite_t1(void) {
    /* T1.N of MWRITE: mreq asserted, wr NOT yet (drops at T2.N) */
    return !m1() && mreq() && !wr() && !rd() && !rfsh();
}
static int pins_mwrite_t2(void) {
    /* T2.N: mreq + wr both asserted */
    return !m1() && mreq() && wr() && !rd() && !rfsh();
}
static int pins_mwrite_end(void) {
    /* T3.N: released */
    return !m1() && !mreq() && !wr() && !rd() && !rfsh();
}

/* Walk an M1 (4-T) cycle and assert the pin sequence at each T_N. */
static int walk_m1_cycle(void) {
    int ok = 1;
    step_t(1); ok &= pins_m1_fetch();   /* T1.N */
    step_t(1); ok &= pins_m1_fetch();   /* T2.N — fetch still active */
    step_t(1); ok &= pins_m1_refresh(); /* T3.N — refresh */
    step_t(1); ok &= pins_m1_end();     /* T4.N — mreq released, rfsh held */
    return ok;
}
static int walk_mread_cycle(void) {
    int ok = 1;
    step_t(1); ok &= pins_mread_active(); /* T1.N */
    step_t(1); ok &= pins_mread_active(); /* T2.N */
    step_t(1); ok &= pins_mread_end();    /* T3.N — released */
    return ok;
}
static int walk_mwrite_cycle(void) {
    int ok = 1;
    step_t(1); ok &= pins_mwrite_t1();   /* T1.N — wr not yet asserted */
    step_t(1); ok &= pins_mwrite_t2();   /* T2.N — wr active */
    step_t(1); ok &= pins_mwrite_end();  /* T3.N — released */
    return ok;
}

/* === The actual per-opcode tests. Inspired by floooh's z80-timing.c
       UTEST blocks; ported to our framework. === */

static void test_nop(void) {
    /* NOP runs a single M1 cycle (4 T-states), no operand fetch. */
    uint8_t prog[] = { 0x00, 0x00, 0x00 };
    load(prog, sizeof prog);
    CHECK(walk_m1_cycle(), "NOP M1 sequence");
    /* second NOP for symmetry */
    CHECK(walk_m1_cycle(), "second NOP M1 sequence");
}

static void test_ld_r_n(void) {
    /* LD A,7C: M1 fetch + MREAD for the immediate byte. */
    uint8_t prog[] = { 0x3E, 0x7C, 0x00, 0x00 };
    load(prog, sizeof prog);
    CHECK(walk_m1_cycle(),  "LD A,n  : M1");
    CHECK(walk_mread_cycle(), "LD A,n : MREAD for n");
    /* Trailing NOP completes the trace deterministically. */
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(z80_get_r8(&S.cpu, REG_A), 0x7C, "A holds the immediate byte");
}

static void test_ld_hl_n(void) {
    /* LD (HL),7C : M1 + MREAD (immediate) + MWRITE (to (HL)). */
    uint8_t prog[] = { 0x36, 0x7C, 0x00, 0x00 };
    load(prog, sizeof prog);
    /* point HL somewhere benign */
    S.cpu.rf[RFP_HL] = 0x0100;
    CHECK(walk_m1_cycle(),    "LD (HL),n : M1");
    CHECK(walk_mread_cycle(), "LD (HL),n : MREAD for n");
    CHECK(walk_mwrite_cycle(),"LD (HL),n : MWRITE to (HL)");
    /* Trailing NOP */
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(S.mem[0x0100], 0x7C, "memory at HL holds the written byte");
}

static void test_ld_a_hl(void) {
    /* LD A,(HL) : M1 + MREAD. */
    uint8_t prog[] = { 0x7E, 0x00, 0x00 };
    load(prog, sizeof prog);
    S.cpu.rf[RFP_HL] = 0x0080;
    S.mem[0x0080] = 0xA5;
    CHECK(walk_m1_cycle(),    "LD A,(HL) : M1");
    CHECK(walk_mread_cycle(), "LD A,(HL) : MREAD");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(z80_get_r8(&S.cpu, REG_A), 0xA5, "A holds the loaded byte");
}

static void test_ld_rp_nn(void) {
    /* LD BC,1234 : M1 + MREAD (low) + MREAD (high). */
    uint8_t prog[] = { 0x01, 0x34, 0x12, 0x00 };
    load(prog, sizeof prog);
    CHECK(walk_m1_cycle(),     "LD BC,nn : M1");
    CHECK(walk_mread_cycle(),  "LD BC,nn : MREAD low");
    CHECK(walk_mread_cycle(),  "LD BC,nn : MREAD high");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(S.cpu.rf[RFP_BC], 0x1234, "BC loaded from immediate");
}

static void test_ld_nn_a(void) {
    /* LD (1234),A : M1 + MREAD (low) + MREAD (high) + MWRITE. */
    uint8_t prog[] = { 0x32, 0x34, 0x12, 0x00 };
    load(prog, sizeof prog);
    z80_set_r8(&S.cpu, REG_A, 0x9E);
    CHECK(walk_m1_cycle(),     "LD (nn),A : M1");
    CHECK(walk_mread_cycle(),  "LD (nn),A : MREAD low");
    CHECK(walk_mread_cycle(),  "LD (nn),A : MREAD high");
    CHECK(walk_mwrite_cycle(), "LD (nn),A : MWRITE");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(S.mem[0x1234], 0x9E, "memory at nn holds A");
}

static void test_alu_r(void) {
    /* ADD A,B : single M1 cycle (no memory operand). */
    uint8_t prog[] = { 0x80, 0x00, 0x00 };
    load(prog, sizeof prog);
    z80_set_r8(&S.cpu, REG_A, 0x10);
    z80_set_r8(&S.cpu, REG_B, 0x20);
    CHECK(walk_m1_cycle(), "ADD A,B : M1 (no memory operand)");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(z80_get_r8(&S.cpu, REG_A), 0x30, "A = 0x10 + 0x20");
}

int main(void) {
    test_nop();
    test_ld_r_n();
    test_ld_hl_n();
    test_ld_a_hl();
    test_ld_rp_nn();
    test_ld_nn_a();
    test_alu_r();
    TEST_SUMMARY();
}
