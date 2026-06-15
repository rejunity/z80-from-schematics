/* ============================================================================
 * zexrunner.c - CP/M harness for the ZEX exercisers (prelim/zexdoc/zexall)
 * against the C model. Loads a .com at 0x0100, provides a minimal BDOS shim
 * (console functions 2 and 9), and runs to completion.
 *
 *   zexrunner <prog.com> [max_instructions]
 *
 * BDOS is trapped by placing a RET at 0x0005 and acting whenever the core is
 * about to fetch there; program exit is detected when PC returns to 0x0000.
 * ==========================================================================*/
/* Expose clock_gettime / CLOCK_MONOTONIC / struct timespec from <time.h>:
   Linux glibc hides POSIX.1-2008 extensions at -std=c99 unless asked;
   macOS libc exposes them unconditionally. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "z80_sim.h"

static void bdos(z80_system_t *s)
{
    uint8_t c = (uint8_t)(s->cpu.rf[RFP_BC] & 0xFF);   /* function number in C */
    if (c == 2) {                                       /* console out: char in E */
        putchar((int)(s->cpu.rf[RFP_DE] & 0xFF));
    } else if (c == 9) {                                /* print '$'-terminated string at DE */
        uint16_t de = s->cpu.rf[RFP_DE];
        for (int guard = 0; guard < 65536; guard++) {
            uint8_t ch = s->mem[de++];
            if (ch == '$') break;
            putchar((int)ch);
        }
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <prog.com> [max_instr]\n", argv[0]); return 2; }
    long long max_instr = (argc > 2) ? atoll(argv[2]) : 4000000000LL;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }

    z80_system_t *s = malloc(sizeof(*s));
    z80_sys_init(s);

    size_t n = fread(&s->mem[0x0100], 1, 0x10000 - 0x0100, f);
    fclose(f);

    /* CP/M low-memory setup: warm-boot vector and BDOS entry */
    s->mem[0x0000] = 0xC3; s->mem[0x0001] = 0x00; s->mem[0x0002] = 0x00; /* JP 0000 (exit trap) */
    s->mem[0x0005] = 0xC9;                                               /* RET (BDOS trap) */
    z80_set_pc(&s->cpu, 0x0100);
    s->cpu.rf[RFP_SP] = 0xFFFE;

    long long count = 0;
    int hit_limit = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        uint16_t pc = s->cpu.rf[RFP_PC];
        if (pc == 0x0000) break;            /* warm boot -> done */
        if (pc == 0x0005) bdos(s);          /* service before executing the RET at 5 */
        z80_sys_step_instr(s);
        if (++count >= max_instr) { hit_limit = 1; break; }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec)
                + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("\n=== zexrunner: %s ===\n", argv[1]);
    printf("instructions executed: %lld%s\n", count, hit_limit ? " (LIMIT REACHED)" : "");
    printf("final PC=%04X  loaded %zu bytes\n", s->cpu.rf[RFP_PC], n);
    printf("elapsed: %.2f s  (%.2f Minstr/s)\n", secs, (double)count / secs / 1e6);
    free(s);
    return hit_limit ? 3 : 0;
}
