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
 *   M1 cycle (4 T-states; PUSH / RST / DJNZ / block-I/O extend to 5)
 *     T1.N  m1=A  mreq=A rd=A  rfsh=-   (opcode-fetch window)
 *     T2.N  m1=A  mreq=A rd=A  rfsh=-   (data-in latch happens here)
 *     T3.N  m1=-  mreq=A rd=-  rfsh=A   (refresh MREQ active)
 *     T4.N  m1=-  mreq=- rd=-  rfsh=A   (refresh MREQ released, rfsh held)
 *     T5.N  m1=-  mreq=- rd=-  rfsh=-   (extra T; only for 5-T M1)
 *
 *   MREAD cycle (3 T-states; m1 stays high, rfsh inactive)
 *     T1.N  mreq=A rd=A
 *     T2.N  mreq=A rd=A     (read data latched at T_last.P)
 *     T3.N  mreq=- rd=-     (released at T_last.N)
 *   Extra T-states (m_len > 3, used by CB-RMW (HL) data read, CALL high-byte
 *   read, DDCB CB-opcode fetch): silent — all pins high.
 *
 *   MWRITE cycle (3 T-states)
 *     T1.N  mreq=A wr=-
 *     T2.N  mreq=A wr=A     (wr drops at T2.N — one phase later than mreq)
 *     T3.N  mreq=- wr=-     (released at T_last.N)
 *
 *   I/O read cycle (4 T-states: T1, T2, Tw, T_last; iorq+rd asserted at T2.P)
 *     T1.N  -- all high     (addr set at T1.P)
 *     T2.N  iorq=A rd=A
 *     Tw.N  iorq=A rd=A     (the automatic wait state)
 *     T_last.N  -- all high (released at T_last.N)
 *
 *   I/O write cycle (4 T-states)
 *     T1.N  -- all high
 *     T2.N  iorq=A wr=A     (iorq at T2.P, wr at T2.N — both asserted by .N)
 *     Tw.N  iorq=A wr=A
 *     T_last.N  -- all high
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
    /* The model is at reset state: M1 about to fetch at PC=0. The first
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
static int halt_p(void){ return !S.cpu.pins.halt_n; }

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
static int pins_all_idle(void) {
    /* Silent T-state — every bus pin released. */
    return !m1() && !mreq() && !iorq() && !rd() && !wr() && !rfsh();
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
static int pins_ioread_t1_idle(void) {
    /* T1.N of IORD: addr set at T1.P but iorq/rd don't drop until T2.P */
    return !m1() && !mreq() && !iorq() && !rd() && !wr() && !rfsh();
}
static int pins_ioread_active(void) {
    /* T2.N / Tw.N of IORD: iorq + rd asserted */
    return !m1() && !mreq() && iorq() && rd() && !wr() && !rfsh();
}
static int pins_ioread_end(void) {
    /* T_last.N of IORD: released */
    return !m1() && !mreq() && !iorq() && !rd() && !wr() && !rfsh();
}
static int pins_iowrite_t1_idle(void) {
    /* T1.N of IOWR: addr + data set at T1.P, iorq+wr not yet */
    return !m1() && !mreq() && !iorq() && !rd() && !wr() && !rfsh();
}
static int pins_iowrite_active(void) {
    /* T2.N / Tw.N of IOWR: iorq + wr asserted */
    return !m1() && !mreq() && iorq() && !rd() && wr() && !rfsh();
}
static int pins_iowrite_end(void) {
    /* T_last.N of IOWR: released */
    return !m1() && !mreq() && !iorq() && !rd() && !wr() && !rfsh();
}

/* === Walkers — each consumes one full M-cycle worth of T-states. === */

/* 4-T M1 (normal opcode / prefix fetch). */
static int walk_m1_cycle(void) {
    int ok = 1;
    step_t(1); ok &= pins_m1_fetch();   /* T1.N */
    step_t(1); ok &= pins_m1_fetch();   /* T2.N — fetch still active */
    step_t(1); ok &= pins_m1_refresh(); /* T3.N — refresh */
    step_t(1); ok &= pins_m1_end();     /* T4.N — mreq released, rfsh held */
    return ok;
}
/* n-T M1 (n >= 4). 5-T M1 used by PUSH / RST / DJNZ / block-I/O.
 * Extra T-states after T4 are silent. */
static int walk_m1_n(int n_t) {
    int ok = walk_m1_cycle();
    for (int i = 4; i < n_t; i++) {
        step_t(1); ok &= pins_all_idle();
    }
    return ok;
}
/* 3-T MREAD. */
static int walk_mread_cycle(void) {
    int ok = 1;
    step_t(1); ok &= pins_mread_active(); /* T1.N */
    step_t(1); ok &= pins_mread_active(); /* T2.N */
    step_t(1); ok &= pins_mread_end();    /* T3.N — released */
    return ok;
}
/* n-T MREAD (n >= 3). Extra T-states past T3 are silent. */
static int walk_mread_n(int n_t) {
    int ok = walk_mread_cycle();
    for (int i = 3; i < n_t; i++) {
        step_t(1); ok &= pins_all_idle();
    }
    return ok;
}
/* 3-T MWRITE. */
static int walk_mwrite_cycle(void) {
    int ok = 1;
    step_t(1); ok &= pins_mwrite_t1();   /* T1.N — wr not yet asserted */
    step_t(1); ok &= pins_mwrite_t2();   /* T2.N — wr active */
    step_t(1); ok &= pins_mwrite_end();  /* T3.N — released */
    return ok;
}
/* 4-T I/O read (T1, T2, Tw, T_last). */
static int walk_ioread_cycle(void) {
    int ok = 1;
    step_t(1); ok &= pins_ioread_t1_idle(); /* T1.N — addr set at T1.P */
    step_t(1); ok &= pins_ioread_active();  /* T2.N — iorq + rd asserted */
    step_t(1); ok &= pins_ioread_active();  /* Tw.N — automatic wait */
    step_t(1); ok &= pins_ioread_end();     /* T_last.N — released */
    return ok;
}
/* 4-T I/O write (T1, T2, Tw, T_last). */
static int walk_iowrite_cycle(void) {
    int ok = 1;
    step_t(1); ok &= pins_iowrite_t1_idle(); /* T1.N */
    step_t(1); ok &= pins_iowrite_active();  /* T2.N — iorq + wr asserted */
    step_t(1); ok &= pins_iowrite_active();  /* Tw.N */
    step_t(1); ok &= pins_iowrite_end();     /* T_last.N — released */
    return ok;
}
/* n silent T-states — used by internal-compute M-cycles (JR taken delay,
 * ADC/SBC HL,rp 7T internal, DD (IX+d) 5T compute). */
static int walk_silent_t(int n) {
    int ok = 1;
    for (int i = 0; i < n; i++) {
        step_t(1); ok &= pins_all_idle();
    }
    return ok;
}

/* === Per-opcode tests. === */

static void test_nop(void) {
    /* NOP runs a single 4-T M1 cycle, no operand fetch. */
    uint8_t prog[] = { 0x00, 0x00, 0x00 };
    load(prog, sizeof prog);
    CHECK(walk_m1_cycle(), "NOP M1 sequence");
    CHECK(walk_m1_cycle(), "second NOP M1 sequence");
}

static void test_ld_r_n(void) {
    /* LD A,7C: M1 + MREAD for the immediate byte. */
    uint8_t prog[] = { 0x3E, 0x7C, 0x00, 0x00 };
    load(prog, sizeof prog);
    CHECK(walk_m1_cycle(),  "LD A,n  : M1");
    CHECK(walk_mread_cycle(), "LD A,n : MREAD for n");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(z80_get_r8(&S.cpu, REG_A), 0x7C, "A holds the immediate byte");
}

static void test_ld_hl_n(void) {
    /* LD (HL),7C : M1 + MREAD (immediate) + MWRITE (to (HL)). */
    uint8_t prog[] = { 0x36, 0x7C, 0x00, 0x00 };
    load(prog, sizeof prog);
    S.cpu.rf[RFP_HL] = 0x0100;
    CHECK(walk_m1_cycle(),    "LD (HL),n : M1");
    CHECK(walk_mread_cycle(), "LD (HL),n : MREAD for n");
    CHECK(walk_mwrite_cycle(),"LD (HL),n : MWRITE to (HL)");
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

static void test_ld_hl_r(void) {
    /* LD (HL),B : M1 + MWRITE. */
    uint8_t prog[] = { 0x70, 0x00, 0x00 };
    load(prog, sizeof prog);
    S.cpu.rf[RFP_HL] = 0x0080;
    z80_set_r8(&S.cpu, REG_B, 0xAB);
    CHECK(walk_m1_cycle(),     "LD (HL),B : M1");
    CHECK(walk_mwrite_cycle(), "LD (HL),B : MWRITE");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(S.mem[0x0080], 0xAB, "memory at HL holds B");
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

static void test_halt(void) {
    /* HALT : the model loops fetching HALT and asserting halt_n.
     * First M1 fetches HALT normally; subsequent M1s loop with halt_n=0. */
    uint8_t prog[] = { 0x76, 0x00, 0x00 };
    load(prog, sizeof prog);
    CHECK(walk_m1_cycle(), "HALT : first M1");
    /* After the first M1 the HALT state is entered. Subsequent re-fetches
       run normal M1 shapes but with halt_n asserted. */
    CHECK(walk_m1_cycle(), "HALT : looping M1");
    CHECK(halt_p(), "HALT : halt_n asserted in the loop");
}

static void test_push(void) {
    /* PUSH BC : 5-T M1 + MWRITE (high) + MWRITE (low). */
    uint8_t prog[] = { 0xC5, 0x00, 0x00 };
    load(prog, sizeof prog);
    S.cpu.rf[RFP_BC] = 0x1234;
    S.cpu.rf[RFP_SP] = 0xFFF0;
    CHECK(walk_m1_n(5),        "PUSH BC : 5-T M1");
    CHECK(walk_mwrite_cycle(), "PUSH BC : MWRITE high");
    CHECK(walk_mwrite_cycle(), "PUSH BC : MWRITE low");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(S.mem[0xFFEF], 0x12, "stack[SP-1] = BC high");
    CHECK_EQ_U(S.mem[0xFFEE], 0x34, "stack[SP-2] = BC low");
}

static void test_pop(void) {
    /* POP BC : M1 + MREAD (low) + MREAD (high). */
    uint8_t prog[] = { 0xC1, 0x00, 0x00 };
    load(prog, sizeof prog);
    S.cpu.rf[RFP_SP] = 0xFFF0;
    S.mem[0xFFF0] = 0x34; S.mem[0xFFF1] = 0x12;
    CHECK(walk_m1_cycle(),    "POP BC : M1");
    CHECK(walk_mread_cycle(), "POP BC : MREAD low");
    CHECK(walk_mread_cycle(), "POP BC : MREAD high");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(S.cpu.rf[RFP_BC], 0x1234, "BC popped from stack");
}

static void test_jp_nn(void) {
    /* JP 0x0010 : M1 + MREAD (low) + MREAD (high). */
    uint8_t prog[] = { 0xC3, 0x10, 0x00, 0x00, 0x00 };
    load(prog, sizeof prog);
    CHECK(walk_m1_cycle(),    "JP nn : M1");
    CHECK(walk_mread_cycle(), "JP nn : MREAD low");
    CHECK(walk_mread_cycle(), "JP nn : MREAD high");
    /* After JP, PC=0x0010. We can't easily check trailing M1 without memory
       at 0x0010 — but the JP itself is enough for the cycle-shape gate. */
}

static void test_jr_taken(void) {
    /* JR +2 (18 02) : M1 + MREAD (displacement) + 5-T internal (taken). */
    uint8_t prog[] = { 0x18, 0x02, 0x00, 0x00, 0x00 };
    load(prog, sizeof prog);
    CHECK(walk_m1_cycle(),     "JR e : M1");
    CHECK(walk_mread_cycle(),  "JR e : MREAD displacement");
    CHECK(walk_silent_t(5),    "JR e : 5-T internal (taken)");
    CHECK(walk_m1_cycle(), "trailing NOP M1 at target");
    /* JR +2 from PC=2 (after the displacement read) lands at 4. */
    CHECK_EQ_U(S.cpu.rf[RFP_PC], 0x0005, "PC advanced by JR + trailing NOP");
}

static void test_call_nn(void) {
    /* CALL 0x0010 : M1 + MREAD (low) + MREAD (high, m_len=4) +
     *               MWRITE (PCH) + MWRITE (PCL). */
    uint8_t prog[] = { 0xCD, 0x10, 0x00, 0x00 };
    load(prog, sizeof prog);
    S.cpu.rf[RFP_SP] = 0xFFF0;
    S.mem[0x0010] = 0x00;   /* target lands here */
    CHECK(walk_m1_cycle(),     "CALL nn : M1");
    CHECK(walk_mread_cycle(),  "CALL nn : MREAD low");
    CHECK(walk_mread_n(4),     "CALL nn : MREAD high (4-T with compute)");
    CHECK(walk_mwrite_cycle(), "CALL nn : MWRITE PC high");
    CHECK(walk_mwrite_cycle(), "CALL nn : MWRITE PC low");
    CHECK(walk_m1_cycle(), "M1 at call target");
    CHECK_EQ_U(S.mem[0xFFEE], 0x03, "stack[SP-2] = return PCL = 3");
    CHECK_EQ_U(S.mem[0xFFEF], 0x00, "stack[SP-1] = return PCH = 0");
}

static void test_ret(void) {
    /* RET : M1 + MREAD (low) + MREAD (high). */
    uint8_t prog[] = { 0xC9, 0x00 };
    load(prog, sizeof prog);
    S.cpu.rf[RFP_SP] = 0xFFF0;
    S.mem[0xFFF0] = 0x10; S.mem[0xFFF1] = 0x00;
    S.mem[0x0010] = 0x00;   /* NOP at the return target so the trailing M1
                               doesn't re-execute RET in a loop. */
    CHECK(walk_m1_cycle(),    "RET : M1");
    CHECK(walk_mread_cycle(), "RET : MREAD PCL");
    CHECK(walk_mread_cycle(), "RET : MREAD PCH");
    /* RET's internal model holds the popped target in WZ during its
       3rd M-cycle; PC is updated to WZ at the start of the next M1.
       Walk a trailing M1 to commit the new PC. */
    CHECK(walk_m1_cycle(), "M1 at RET target");
    CHECK_EQ_U(S.cpu.rf[RFP_PC], 0x0011, "PC at RET target + 1 after trailing NOP");
}

static void test_rst(void) {
    /* RST 18h : 5-T M1 + MWRITE (high) + MWRITE (low). */
    uint8_t prog[] = { 0xDF, 0x00 };
    load(prog, sizeof prog);
    S.cpu.rf[RFP_SP] = 0xFFF0;
    S.mem[0x0018] = 0x00;
    CHECK(walk_m1_n(5),        "RST 18h : 5-T M1");
    CHECK(walk_mwrite_cycle(), "RST 18h : MWRITE PCH");
    CHECK(walk_mwrite_cycle(), "RST 18h : MWRITE PCL");
    CHECK(walk_m1_cycle(), "M1 at RST vector");
    CHECK_EQ_U(S.mem[0xFFEE], 0x01, "stack[SP-2] = return PCL = 1");
    CHECK_EQ_U(S.cpu.rf[RFP_PC], 0x0019, "PC at RST vector + 1 after trailing NOP");
}

static void test_in_a_n(void) {
    /* IN A,(0x10) : M1 + MREAD (port n) + IORD. */
    uint8_t prog[] = { 0xDB, 0x10, 0x00 };
    load(prog, sizeof prog);
    /* High byte of port = A. Explicitly set A=0xFF so port = 0xFF10
     * (independent of reset register-init value, which since
     * commit 54f9173 is 0x5555 rather than 0xFFFF). */
    z80_set_r8(&S.cpu, REG_A, 0xFF);
    S.io[0xFF10] = 0xA5;
    CHECK(walk_m1_cycle(),     "IN A,(n) : M1");
    CHECK(walk_mread_cycle(),  "IN A,(n) : MREAD for n");
    CHECK(walk_ioread_cycle(), "IN A,(n) : IORD");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(z80_get_r8(&S.cpu, REG_A), 0xA5, "A holds the I/O input byte");
}

static void test_out_n_a(void) {
    /* OUT (0x20),A : M1 + MREAD (port n) + IOWR. */
    uint8_t prog[] = { 0xD3, 0x20, 0x00 };
    load(prog, sizeof prog);
    z80_set_r8(&S.cpu, REG_A, 0x77);
    CHECK(walk_m1_cycle(),      "OUT (n),A : M1");
    CHECK(walk_mread_cycle(),   "OUT (n),A : MREAD for n");
    CHECK(walk_iowrite_cycle(), "OUT (n),A : IOWR");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    /* Port address = {A=0x77, n=0x20} = 0x7720. */
    CHECK_EQ_U(S.io[0x7720], 0x77, "I/O port holds A");
}

static void test_cb_r(void) {
    /* RLC B (CB 00) : M1 (CB prefix) + M1 (opcode). */
    uint8_t prog[] = { 0xCB, 0x00, 0x00 };
    load(prog, sizeof prog);
    z80_set_r8(&S.cpu, REG_B, 0x81);
    CHECK(walk_m1_cycle(), "RLC B : M1 (CB prefix)");
    CHECK(walk_m1_cycle(), "RLC B : M1 (opcode)");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(z80_get_r8(&S.cpu, REG_B), 0x03, "B = RLC(0x81)");
}

static void test_cb_hl(void) {
    /* RLC (HL) (CB 06) : M1 + M1 + MREAD (4-T, read+compute) + MWRITE. */
    uint8_t prog[] = { 0xCB, 0x06, 0x00 };
    load(prog, sizeof prog);
    S.cpu.rf[RFP_HL] = 0x0080;
    S.mem[0x0080] = 0x42;
    CHECK(walk_m1_cycle(),     "RLC (HL) : M1 (CB prefix)");
    CHECK(walk_m1_cycle(),     "RLC (HL) : M1 (opcode)");
    CHECK(walk_mread_n(4),     "RLC (HL) : MREAD (4-T RMW data read)");
    CHECK(walk_mwrite_cycle(), "RLC (HL) : MWRITE (RMW data write)");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(S.mem[0x0080], 0x84, "(HL) = RLC(0x42)");
}

static void test_ed_neg(void) {
    /* NEG (ED 44) : M1 (ED prefix) + M1 (opcode). */
    uint8_t prog[] = { 0xED, 0x44, 0x00 };
    load(prog, sizeof prog);
    z80_set_r8(&S.cpu, REG_A, 0x01);
    CHECK(walk_m1_cycle(), "NEG : M1 (ED prefix)");
    CHECK(walk_m1_cycle(), "NEG : M1 (opcode)");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(z80_get_r8(&S.cpu, REG_A), 0xFF, "A = NEG(0x01)");
}

static void test_ed_in_r_c(void) {
    /* IN B,(C) (ED 40) : M1 + M1 + IORD. */
    uint8_t prog[] = { 0xED, 0x40, 0x00 };
    load(prog, sizeof prog);
    S.cpu.rf[RFP_BC] = 0x1234;
    S.io[0x1234] = 0xCC;
    CHECK(walk_m1_cycle(),     "IN B,(C) : M1 (ED prefix)");
    CHECK(walk_m1_cycle(),     "IN B,(C) : M1 (opcode)");
    CHECK(walk_ioread_cycle(), "IN B,(C) : IORD");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(z80_get_r8(&S.cpu, REG_B), 0xCC, "B holds the I/O input byte");
}

static void test_ed_ld_rp_nn(void) {
    /* LD HL,(0x0100) (ED 6B 00 01) :
     *   M1 + M1 + MREAD (addr low) + MREAD (addr high)
     *           + MREAD (data low) + MREAD (data high). */
    uint8_t prog[] = { 0xED, 0x6B, 0x00, 0x01, 0x00 };
    load(prog, sizeof prog);
    S.mem[0x0100] = 0x34; S.mem[0x0101] = 0x12;
    CHECK(walk_m1_cycle(),    "LD HL,(nn) : M1 (ED prefix)");
    CHECK(walk_m1_cycle(),    "LD HL,(nn) : M1 (opcode)");
    CHECK(walk_mread_cycle(), "LD HL,(nn) : MREAD addr low");
    CHECK(walk_mread_cycle(), "LD HL,(nn) : MREAD addr high");
    CHECK(walk_mread_cycle(), "LD HL,(nn) : MREAD data low");
    CHECK(walk_mread_cycle(), "LD HL,(nn) : MREAD data high");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(S.cpu.rf[RFP_HL], 0x1234, "HL loaded from (nn)");
}

static void test_ed_adc_hl_rp(void) {
    /* ADC HL,DE (ED 5A) : M1 + M1 + 7-T internal (no bus traffic). */
    uint8_t prog[] = { 0xED, 0x5A, 0x00 };
    load(prog, sizeof prog);
    S.cpu.rf[RFP_HL] = 0x1000;
    S.cpu.rf[RFP_DE] = 0x0234;
    /* Reset state has F=0xFF (CF=1); clear CF for a clean ADC. */
    S.cpu.rf[RFP_AF] &= 0xFFFE;
    CHECK(walk_m1_cycle(),  "ADC HL,DE : M1 (ED prefix)");
    CHECK(walk_m1_cycle(),  "ADC HL,DE : M1 (opcode)");
    CHECK(walk_silent_t(7), "ADC HL,DE : 7-T internal");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(S.cpu.rf[RFP_HL], 0x1234, "HL = HL + DE (carry clear)");
}

static void test_dd_ld_ix_nn(void) {
    /* LD IX,0x1234 (DD 21 34 12) : M1 + M1 + MREAD (low) + MREAD (high). */
    uint8_t prog[] = { 0xDD, 0x21, 0x34, 0x12, 0x00 };
    load(prog, sizeof prog);
    CHECK(walk_m1_cycle(),    "LD IX,nn : M1 (DD prefix)");
    CHECK(walk_m1_cycle(),    "LD IX,nn : M1 (opcode)");
    CHECK(walk_mread_cycle(), "LD IX,nn : MREAD low");
    CHECK(walk_mread_cycle(), "LD IX,nn : MREAD high");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(S.cpu.rf[RFP_IX], 0x1234, "IX loaded from immediate");
}

static void test_dd_ld_a_ixd(void) {
    /* LD A,(IX+1) (DD 7E 01) :
     *   M1 + M1 + MREAD (displ) + 5-T internal + MREAD (data). */
    uint8_t prog[] = { 0xDD, 0x7E, 0x01, 0x00 };
    load(prog, sizeof prog);
    S.cpu.rf[RFP_IX] = 0x0080;
    S.mem[0x0081] = 0xEE;
    CHECK(walk_m1_cycle(),     "LD A,(IX+d) : M1 (DD prefix)");
    CHECK(walk_m1_cycle(),     "LD A,(IX+d) : M1 (opcode)");
    CHECK(walk_mread_cycle(),  "LD A,(IX+d) : MREAD displacement");
    CHECK(walk_silent_t(5),    "LD A,(IX+d) : 5-T internal IX+d compute");
    CHECK(walk_mread_cycle(),  "LD A,(IX+d) : MREAD data");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
    CHECK_EQ_U(z80_get_r8(&S.cpu, REG_A), 0xEE, "A holds data from (IX+d)");
}

static void test_ddcb_bit(void) {
    /* BIT 0,(IX+1) (DD CB 01 46) :
     *   M1 + M1 + MREAD (displ) + MREAD (5-T CB-opcode fetch w/ compute)
     *           + MREAD (4-T data read, no write). */
    uint8_t prog[] = { 0xDD, 0xCB, 0x01, 0x46, 0x00 };
    load(prog, sizeof prog);
    S.cpu.rf[RFP_IX] = 0x0080;
    S.mem[0x0081] = 0xFF;
    CHECK(walk_m1_cycle(),    "BIT 0,(IX+d) : M1 (DD prefix)");
    CHECK(walk_m1_cycle(),    "BIT 0,(IX+d) : M1 (CB prefix)");
    CHECK(walk_mread_cycle(), "BIT 0,(IX+d) : MREAD displacement");
    CHECK(walk_mread_n(5),    "BIT 0,(IX+d) : MREAD CB-opcode (5-T)");
    CHECK(walk_mread_n(4),    "BIT 0,(IX+d) : MREAD data (4-T)");
    CHECK(walk_m1_cycle(), "trailing NOP M1");
}

int main(void) {
    test_nop();
    test_ld_r_n();
    test_ld_hl_n();
    test_ld_a_hl();
    test_ld_hl_r();
    test_ld_rp_nn();
    test_ld_nn_a();
    test_alu_r();
    test_halt();
    test_push();
    test_pop();
    test_jp_nn();
    test_jr_taken();
    test_call_nn();
    test_ret();
    test_rst();
    test_in_a_n();
    test_out_n_a();
    test_cb_r();
    test_cb_hl();
    test_ed_neg();
    test_ed_in_r_c();
    test_ed_ld_rp_nn();
    test_ed_adc_hl_rp();
    test_dd_ld_ix_nn();
    test_dd_ld_a_ixd();
    test_ddcb_bit();
    TEST_SUMMARY();
}
