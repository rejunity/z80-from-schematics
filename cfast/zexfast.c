/* ============================================================================
 * zexfast.c - the ZEX CP/M harness (scripts/zexrunner.c) on cfast's run
 * frame: the memory / I/O of z80_sys_phase inlined between the generated
 * phase steps (z80f_run), BDOS console and the exit trap at instruction
 * boundaries.
 *
 *   zexfast <prog.com> [max_instructions]
 *
 * Built -DZEX_SINGLE it takes everything from the one-header build
 * (build/cfast/z80_cfast.h, cfast/amalgamate.py) instead.
 * ==========================================================================*/
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef ZEX_SINGLE
#include "z80_cfast.h"
#else
#include "z80_fast.c"
#endif

static uint8_t ram[65536], ports[65536];
static long long count, max_instr;
static int hit_limit;

static void bdos(const z80_t *c)
{
    uint8_t fn = (uint8_t)(c->rf[RFP_BC] & 0xFF);
    if (fn == 2) putchar((int)(c->rf[RFP_DE] & 0xFF));
    else if (fn == 9) {
        uint16_t de = c->rf[RFP_DE];
        for (int g = 0; g < 65536 && ram[de] != '$'; g++) putchar(ram[de++]);
    }
    fflush(stdout);
}

/* the next instruction is about to be fetched: count, trap, maybe stop */
static int boundary(const z80_t *c)
{
    if (++count >= max_instr) { hit_limit = 1; return 1; }
    if (c->rf[RFP_PC] == 0x0000) return 1;
    if (c->rf[RFP_PC] == 0x0005) bdos(z80_arch(c));   /* the writes of the next M1 made */
    return 0;
}

/* z80_sys_phase's memory and I/O; an undriven bus reads 0 */
#define ZF_BUS(p) do {                                              \
        uint16_t a_ = Z80_GET_ADDR(p);                              \
        uint8_t  d_ = 0;                                            \
        if ((p) & Z80_MREQ) {                                       \
            if ((p) & Z80_WR)      ram[a_] = Z80_GET_DATA(p);       \
            else if ((p) & Z80_RD) d_ = ram[a_];                    \
        } else if ((p) & Z80_IORQ) {                                \
            if ((p) & Z80_WR)      ports[a_] = Z80_GET_DATA(p);     \
            else if ((p) & Z80_RD) d_ = ports[a_];                  \
        }                                                           \
        Z80_SET_DATA(p, d_);                                        \
    } while (0)
#define ZF_BOUNDARY(c) boundary(c)
#ifdef ZEX_SINGLE
#define Z80_IMPL
#include "z80_cfast.h"
#else
#include "z80_fast_run.h"
#endif

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <prog.com> [max_instr]\n", argv[0]); return 2; }
    max_instr = (argc > 2) ? atoll(argv[2]) : 4000000000LL;
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    size_t n = fread(&ram[0x0100], 1, 0x10000 - 0x0100, fp);
    fclose(fp);
    ram[0x0000] = 0xC3; ram[0x0001] = 0x00; ram[0x0002] = 0x00;   /* JP 0000 (exit trap) */
    ram[0x0005] = 0xC9;                                            /* RET (BDOS trap) */

    static z80_t cpu;
    z80_init(&cpu);
    z80_set_pc(&cpu, 0x0100);
    cpu.rf[RFP_SP] = 0xFFFE;
    z80f_t f;
    z80f_init(&f, &cpu);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    z80f_run(&f, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("\n=== zexfast: %s ===\n", argv[1]);
    printf("instructions executed: %lld%s\n", count, hit_limit ? " (LIMIT REACHED)" : "");
    printf("final PC=%04X  loaded %zu bytes\n", cpu.rf[RFP_PC], n);
    printf("elapsed: %.2f s  (%.2f Minstr/s, %llu phases, %.1f Mphases/s)\n",
           secs, (double)count / secs / 1e6, (unsigned long long)cpu.cycle,
           (double)cpu.cycle / secs / 1e6);
    return hit_limit ? 3 : 0;
}
