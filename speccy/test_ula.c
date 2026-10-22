/* ============================================================================
 * test_ula.c - the ULA against the published 48K timings (make speccy_test).
 *
 * Programs run from uncontended RAM (0x8000) on the whole machine (cfast,
 * the ULA, the bus), started so a chosen T-state of the frame falls where
 * it should; what the CPU actually took is measured:
 *   contention  LD A,(0x4000) with its read's T1 at each T-state t of the
 *               frame: the extra T-states, summed as FUSE's unit test does
 *               (delay(t) * (t + 1), mod 2^32);
 *   floating    IN A,(0xFF) with its T3 at each t, screen RAM 0x4000-0x5FFF
 *               holding offset % 256: the byte read, summed the same way;
 *   I/O         the four I/O contention patterns, an I/O cycle's T1 rising
 *               into C[3:0] = 3 of a paper line.
 * T-states count FUSE's way (early timing): t starts at the rising edge
 * entering pixel clock P(t) = (248 * 448 + 5 + 2t) mod 139776.
 * ==========================================================================*/
#include "zx.h"
#include <stdio.h>
#include <string.h>

#define FRAME_P (ULA_C * ULA_V)

static zx_t z;

static int t0_pixel = 5;                       /* early timing; late: 3 */
static unsigned p_of(long t) { return (unsigned)(((248L * ULA_C + t0_pixel + 2 * t) % FRAME_P + FRAME_P) % FRAME_P); }
static unsigned pos(void) { return z.ula.v * ULA_C + z.ula.c; }

/* run code at 0x8000 with A = a, its first T-state at t; stop at the M1 of
   `end`; return the T-states taken */
static unsigned run_at(long t, uint8_t a, uint16_t end)
{
    z80_init(&z.cpu);
    z80_set_pc(&z.cpu, 0x8000);
    z.cpu.rf[RFP_AF] = (uint16_t)(a << 8 | 0xFF);
    z80f_init(&z.fast, &z.cpu);
    z.in = 0;
    unsigned p0 = p_of(t), before = (p0 + FRAME_P - 1) % FRAME_P;
    z.ula.v = (uint16_t)(before / ULA_C);
    z.ula.c = (uint16_t)(before % ULA_C);
    z.ula.mreq_t2 = z.ula.ioreq_t2 = false;
    zx_sync(&z);                                 /* into P(t): the clock rises */
    unsigned start = pos();
    for (int phases = 0; phases < 400; phases++) {
        unsigned at = pos();                     /* the period this phase starts */
        zx_phase(&z);
        if ((z.pins & Z80_M1) && Z80_GET_ADDR(z.pins) == end)   /* the next T1 */
            return ((at + FRAME_P - start) % FRAME_P) / 2;
    }
    return 0;
}

/* the published per-T-state contention (early timing) */
static unsigned delay_ref(long t)
{
    if (t < 14335)
        return 0;
    long line = (t - 14335) / 224, at = (t - 14335) % 224;
    static const unsigned d[8] = { 6, 5, 4, 3, 2, 1, 0, 0 };
    return line < 192 && at < 128 ? d[at % 8] : 0;
}

int main(void)
{
    static uint8_t rom[16384];
    zx_init(&z, rom);
    z.ula.draw = false;
    int fails = 0;

    /* contention: LD A,(0x4000) = 3A 00 40: M1 4, operands 3 + 3, then the
       read whose T1 is at t + 10 */
    memcpy(&z.mem[0x8000], "\x3A\x00\x40\x00", 4);
    uint32_t sum = 0;
    int mism = 0;
    for (long t = 0; t < ULA_FRAME_T; t++) {
        unsigned took = run_at(t - 10, 0, 0x8003), d = took - 13;
        sum += d * (uint32_t)(t + 1);
        if (d != delay_ref(t) && mism++ < 5)
            printf("  contention at %ld: %u, expected %u\n", t, d, delay_ref(t));
    }
    printf("contention  checksum %10u  (FUSE 48K early: 2308862976)  %d T-states differ  %s\n",
           sum, mism, sum == 2308862976u && !mism ? "OK" : "FAIL");
    fails += sum != 2308862976u || mism;

    /* floating bus: IN A,(0xFF) = DB FF with A = 0: M1 4, operand 3, the
       I/O cycle T1 T2 TW T3, T3 at t; screen RAM holds offset % 256 */
    for (int i = 0x4000; i < 0x5B00; i++)
        z.mem[i] = (uint8_t)(i - 0x4000);
    memcpy(&z.mem[0x8000], "\xDB\xFF\x00", 3);
    sum = 0;
    for (long t = 0; t < ULA_FRAME_T; t++) {
        run_at(t - 10, 0, 0x8002);
        uint8_t got = (uint8_t)(z80_arch(&z.cpu)->rf[RFP_AF] >> 8);
        sum += got * (uint32_t)(t + 1);
    }
    /* FUSE's sum runs to 80000 with 0xFF past the frame */
    for (long t = ULA_FRAME_T; t < 80000; t++)
        sum += 0xFFu * (uint32_t)(t + 1);
    printf("floating    checksum %10u  (FUSE 48K early: 3427723200)  %s\n", sum, sum == 3427723200u ? "OK" : "FAIL");
    fails += sum != 3427723200u;

    /* late timing: INT for C 0..63, T-state 0 a pixel clock pair earlier */
    ula_set_late(&z.ula, true);
    t0_pixel = 3;
    memcpy(&z.mem[0x8000], "\x3A\x00\x40\x00", 4);
    sum = 0;
    for (long t = 0; t < ULA_FRAME_T; t++)
        sum += (run_at(t - 10, 0, 0x8003) - 13) * (uint32_t)(t + 1);
    printf("late contention checksum %10u  (FUSE 48K late: 2308927488)  %s\n", sum, sum == 2308927488u ? "OK" : "FAIL");
    fails += sum != 2308927488u;
    memcpy(&z.mem[0x8000], "\xDB\xFF\x00", 3);
    sum = 0;
    for (long t = 0; t < ULA_FRAME_T; t++) {
        run_at(t - 10, 0, 0x8002);
        sum += (uint8_t)(z80_arch(&z.cpu)->rf[RFP_AF] >> 8) * (uint32_t)(t + 1);
    }
    for (long t = ULA_FRAME_T; t < 80000; t++)
        sum += 0xFFu * (uint32_t)(t + 1);
    printf("late floating   checksum %10u  (FUSE 48K late: 3426156480)  %s\n", sum, sum == 3426156480u ? "OK" : "FAIL");
    fails += sum != 3426156480u;
    ula_set_late(&z.ula, false);
    t0_pixel = 5;

    /* I/O contention: IN A,(n) with port (A << 8) | n; its I/O cycle's T1
       at t + 7 rising into C[3:0] = 3 of line 0 (FUSE t = 14335) */
    static const struct { uint8_t a, n; unsigned io; const char *name; } io[4] = {
        { 0x00, 0xFF, 4, "N:4" }, { 0x00, 0xFE, 9, "N:1 C:3" },
        { 0x40, 0xFE, 10, "C:1 C:3" }, { 0x40, 0xFF, 16, "C:1 C:1 C:1 C:1" } };
    for (int i = 0; i < 4; i++) {
        z.mem[0x8001] = io[i].n;
        unsigned took = run_at(14335 - 7, io[i].a, 0x8002) - 7;
        printf("I/O %-16s port %02X%02X: %2u T-states (expected %2u)  %s\n", io[i].name, io[i].a, io[i].n,
               took, io[i].io, took == io[i].io ? "OK" : "FAIL");
        fails += took != io[i].io;
    }
    /* instructions: each run at every T-state of the frame, its length
       against the published pattern (the Sinclair wiki's / FUSE's form,
       contended accesses at 0x4000-0x7FFF and IR when I is 0x40-0x7F),
       code at pc, HL = 0x4000, DE = 0x5000, SP = 0x6000 */
    enum { PC, PC1, PC2, HL, DE, SP, SPM1, SPM2, SP1, IR };
    static const struct {
        const char *name, *code;
        int size;
        uint16_t pc;
        uint8_t i;
        int steps;
        uint8_t at[12], len[12];
    } ins[] = {
        { "NOP (code in 0x6000)", "\x00", 1, 0x6000, 0, 1, { PC }, { 4 } },
        { "LD A,(HL)", "\x7E", 1, 0x8000, 0, 2, { PC, HL }, { 4, 3 } },
        { "INC (HL)", "\x34", 1, 0x8000, 0, 4, { PC, HL, HL, HL }, { 4, 3, 1, 3 } },
        { "LD (HL),n", "\x36\x55", 2, 0x8000, 0, 3, { PC, PC1, HL }, { 4, 3, 3 } },
        { "PUSH BC (I = 0x40)", "\xC5", 1, 0x8000, 0x40, 4, { PC, IR, SPM1, SPM2 }, { 4, 1, 3, 3 } },
        { "POP BC", "\xC1", 1, 0x8000, 0, 3, { PC, SP, SP1 }, { 4, 3, 3 } },
        { "JR $+2 (code in 0x6000)", "\x18\x00", 2, 0x6000, 0, 7, { PC, PC1, PC1, PC1, PC1, PC1, PC1 }, { 4, 3, 1, 1, 1, 1, 1 } },
        { "INC BC (I = 0x40)", "\x03", 1, 0x8000, 0x40, 3, { PC, IR, IR }, { 4, 1, 1 } },
        { "LD A,I (I = 0x40)", "\xED\x57", 2, 0x8000, 0x40, 3, { PC, PC1, IR }, { 4, 4, 1 } },
        { "LDI", "\xED\xA0", 2, 0x8000, 0, 6, { PC, PC1, HL, DE, DE, DE }, { 4, 4, 3, 3, 1, 1 } },
        { "CALL nn (code in 0x6000)", "\xCD\x03\x60", 3, 0x6000, 0, 6, { PC, PC1, PC2, PC2, SPM1, SPM2 }, { 4, 3, 3, 1, 3, 3 } },
        { "EX (SP),HL", "\xE3", 1, 0x8000, 0, 8, { PC, SP, SP1, SP1, SP1, SP, SP, SP }, { 4, 3, 3, 1, 3, 3, 1, 1 } },
    };
    for (unsigned n = 0; n < sizeof ins / sizeof ins[0]; n++) {
        int bad = 0, len = ins[n].size;
        unsigned total = 0;
        for (int i = 0; i < ins[n].steps; i++)
            total += ins[n].len[i];
        for (long t = 0; t < ULA_FRAME_T; t++) {
            memset(&z.mem[0x4000], 0, 0xC000);
            memcpy(&z.mem[ins[n].pc], ins[n].code, (size_t)len);
            uint16_t end = (uint16_t)(ins[n].pc + len);
            if (ins[n].code[0] == '\xCD')
                end = 0x6003;                        /* CALL $+3 */
            /* the expected length: each access waits for its delay */
            long tc = t;
            for (int i = 0; i < ins[n].steps; i++) {
                uint16_t ad = 0;
                switch (ins[n].at[i]) {
                case PC: ad = ins[n].pc; break;
                case PC1: ad = (uint16_t)(ins[n].pc + 1); break;
                case PC2: ad = (uint16_t)(ins[n].pc + 2); break;
                case HL: ad = 0x4000; break;
                case DE: ad = 0x5000; break;
                case SP: ad = 0x6000; break;
                case SPM1: ad = 0x5FFF; break;
                case SPM2: ad = 0x5FFE; break;
                case SP1: ad = 0x6001; break;
                case IR: ad = (uint16_t)(ins[n].i << 8); break;
                }
                if ((ad & 0xC000) == 0x4000)
                    tc += delay_ref(tc % ULA_FRAME_T);
                tc += ins[n].len[i];
            }
            unsigned want = (unsigned)(tc - t);
            /* run it: registers set after the reset run_at does */
            z80_init(&z.cpu);
            z80_set_pc(&z.cpu, ins[n].pc);
            z.cpu.rf[RFP_HL] = 0x4000;
            z.cpu.rf[RFP_DE] = 0x5000;
            z.cpu.rf[RFP_SP] = 0x6000;
            z.cpu.rf[RFP_BC] = 0x1234;
            z.cpu.reg_i = ins[n].i;
            z.cpu.reg_r = 0;
            z80f_init(&z.fast, &z.cpu);
            z.in = 0;
            unsigned p0 = p_of(t), before = (p0 + FRAME_P - 1) % FRAME_P;
            z.ula.v = (uint16_t)(before / ULA_C);
            z.ula.c = (uint16_t)(before % ULA_C);
            z.ula.mreq_t2 = z.ula.ioreq_t2 = false;
            zx_sync(&z);
            unsigned start = pos(), got = 0;
            for (int ph = 0; ph < 600; ph++) {
                unsigned at = pos();                 /* the period this phase starts */
                zx_phase(&z);
                if ((z.pins & Z80_M1) && Z80_GET_ADDR(z.pins) == end && ph > 1) {
                    got = ((at + FRAME_P - start) % FRAME_P) / 2;
                    break;
                }
            }
            if (got != want && bad++ < 3)
                printf("  %s at %ld: %u T-states, the pattern says %u\n", ins[n].name, t, got, want);
        }
        printf("pattern %-26s %2u T-states + contention, every T-state of the frame: %s\n", ins[n].name, total,
               bad ? "FAIL" : "OK");
        fails += bad != 0;
    }
    /* INT: NOPs with interrupts on (IM 1), the first INT sample at the
       end of a NOP started at b - 4: taken at boundary b iff 0 <= b <= 31 */
    memset(&z.mem[0x8000], 0, 64);
    int int_bad = 0;
    for (long b = -6; b <= 36; b++) {
        z80_init(&z.cpu);
        z80_set_pc(&z.cpu, 0x8000);
        z.cpu.iff1 = z.cpu.iff2 = 1;
        z.cpu.im = 1;
        z80f_init(&z.fast, &z.cpu);
        z.in = 0;
        unsigned before = (p_of(b - 4) + FRAME_P - 1) % FRAME_P;
        z.ula.v = (uint16_t)(before / ULA_C);
        z.ula.c = (uint16_t)(before % ULA_C);
        z.ula.mreq_t2 = z.ula.ioreq_t2 = false;
        zx_sync(&z);
        bool taken = false;
        for (int ph = 0; ph < 8 + 2; ph++) {          /* the NOP, and the next T1 */
            zx_phase(&z);
            if ((z.pins & Z80_M1) && (z.pins & Z80_IORQ))
                taken = true;
        }
        for (int ph = 0; ph < 6 && !taken; ph++) {      /* INTA's IORQ comes in T3 */
            zx_phase(&z);
            if ((z.pins & Z80_M1) && (z.pins & Z80_IORQ))
                taken = true;
        }
        if (taken != (b >= 0 && b <= 31) && int_bad++ < 4)
            printf("  INT at boundary %ld: %s\n", b, taken ? "taken" : "not taken");
    }
    printf("INT taken at boundaries 0..31 only                              %s\n", int_bad ? "FAIL" : "OK");
    fails += int_bad != 0;

    /* the border: OUT (0xFE),A at s writes the port latch in T2-low (FUSE:
       s + 8); the picture changes at the first 4-T-state cell (C = 5 mod 8)
       after it, here on border line 300 (above the paper) */
    z.ula.draw = true;
    memcpy(&z.mem[0x8000], "\xD3\xFE\x00\x00\x00\x00\x00\x00", 8);
    int border_bad = 0;
    for (long s0 = 0; s0 < 32; s0++) {
        long base = (long)((300L * ULA_C + 101 - 111109 + FRAME_P) % FRAME_P) / 2;   /* T-state of C 101, V 300 */
        long s = base + s0;
        z.ula.port_fe = 7;
        z.ula.ao = 7 << 3;
        z80_init(&z.cpu);
        z80_set_pc(&z.cpu, 0x8000);
        z.cpu.rf[RFP_AF] = 0x02FF;                       /* red */
        z80f_init(&z.fast, &z.cpu);
        z.in = 0;
        unsigned before = (p_of(s) + FRAME_P - 1) % FRAME_P;
        z.ula.v = (uint16_t)(before / ULA_C);
        z.ula.c = (uint16_t)(before % ULA_C);
        z.ula.mreq_t2 = z.ula.ioreq_t2 = false;
        zx_sync(&z);
        for (int ph = 0; ph < 80; ph++)
            zx_phase(&z);
        long tw = s + 8, cell = 4 * (tw / 4) + 4;      /* the first cell after the write */
        unsigned pc = p_of(cell), pp = (pc + FRAME_P - 1) % FRAME_P;
        unsigned xc = (pc + 35) % ULA_C, yc = ((pc + 35) / ULA_C + 48) % ULA_V;
        unsigned xp = (pp + 35) % ULA_C, yp = ((pp + 35) / ULA_C + 48) % ULA_V;
        if ((z.ula.fb[yc][xc] != 2 || z.ula.fb[yp][xp] != 7) && border_bad++ < 4)
            printf("  border: OUT at %ld: pixel at the cell %u, before it %u (want 2 and 7)\n", s,
                   z.ula.fb[yc][xc], z.ula.fb[yp][xp]);
    }
    printf("border changes at the first 4-T-state cell after OUT's T2       %s\n", border_bad ? "FAIL" : "OK");
    fails += border_bad != 0;
    return fails ? 1 : 0;
}
