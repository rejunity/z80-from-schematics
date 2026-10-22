/* ============================================================================
 * zexrunner.c - CP/M harness for the ZEX exercisers (prelim/zexdoc/zexall)
 * against the C model. Loads a .com at 0x0100, provides a minimal BDOS shim
 * (console functions 2 and 9), and runs to completion.
 *
 *   zexrunner <prog.com> [max_instructions]
 *
 * BDOS is trapped by placing a RET at 0x0005 and acting whenever the core is
 * about to fetch there; program exit is detected when PC returns to 0x0000.
 *
 * Built with -DZEX_TICK (zexrunner_tick) the bus runs in this file's loop
 * through the pin word API (z80_tick), one phase per tick, instead of
 * z80_sys_step_instr -- same phases, same pins; with -DZ80_FAST
 * (zexrunner_fast, make cfast) through cfast's generated engine
 * (z80f_tick). One path per binary keeps the phase engine inlined in it.
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
#ifdef Z80_FAST
#include "z80_fast.h"
static z80f_t fast_core;
#define ZEX_TICK
#define TICK(cpu, pins) z80f_tick(&fast_core, pins)
#else
#define TICK(cpu, pins) z80_tick(cpu, pins)
#endif

static void bdos(z80_system_t *s)
{
    const z80_t *a = z80_arch(&s->cpu);                 /* the writes of the next M1 made */
    uint8_t c = (uint8_t)(a->rf[RFP_BC] & 0xFF);        /* function number in C */
    if (c == 2) {                                       /* console out: char in E */
        putchar((int)(a->rf[RFP_DE] & 0xFF));
    } else if (c == 9) {                                /* print '$'-terminated string at DE */
        uint16_t de = a->rf[RFP_DE];
        for (int guard = 0; guard < 65536; guard++) {
            uint8_t ch = s->mem[de++];
            if (ch == '$') break;
            putchar((int)ch);
        }
    }
    fflush(stdout);
}

#ifdef ZEX_TICK
/* The tick loop: z80_sys_phase's memory and I/O, on the pin word. Runs to
   the next instruction boundary. */
static void tick_instr(z80_system_t *s, uint64_t *pins_io)
{
    z80_t   *cpu = &s->cpu;
    uint8_t *mem = s->mem, *io = s->io;
    uint64_t pins = *pins_io, ic = cpu->instr_count;
    do {
        pins = TICK(cpu, pins);
        uint16_t a = Z80_GET_ADDR(pins);
        uint8_t  d = 0;                         /* an undriven bus reads 0 */
        if (pins & Z80_MREQ) {
            if (pins & Z80_WR)      mem[a] = Z80_GET_DATA(pins);
            else if (pins & Z80_RD) d = mem[a];
        } else if (pins & Z80_IORQ) {
            if (pins & Z80_WR)      io[a] = Z80_GET_DATA(pins);
            else if (pins & Z80_RD) d = io[a];
        }
        Z80_SET_DATA(pins, d);
    } while (cpu->instr_count == ic);
    *pins_io = pins;
}
#endif

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

#ifdef Z80_FAST
    z80f_init(&fast_core, &s->cpu);
#endif
    long long count = 0;
    int hit_limit = 0;
#ifdef ZEX_TICK
    uint64_t pins = 0;
#endif
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        uint16_t pc = s->cpu.rf[RFP_PC];
        if (pc == 0x0000) break;            /* warm boot -> done */
        if (pc == 0x0005) bdos(s);          /* service before executing the RET at 5 */
#ifdef ZEX_TICK
        tick_instr(s, &pins);
#else
        z80_sys_step_instr(s);
#endif
        if (++count >= max_instr) { hit_limit = 1; break; }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec)
                + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("\n=== zexrunner: %s ===\n", argv[1]);
    printf("instructions executed: %lld%s\n", count, hit_limit ? " (LIMIT REACHED)" : "");
    printf("final PC=%04X  loaded %zu bytes\n", s->cpu.rf[RFP_PC], n);
    printf("elapsed: %.2f s  (%.2f Minstr/s, %llu phases, %.1f Mphases/s)\n",
           secs, (double)count / secs / 1e6, (unsigned long long)s->cpu.cycle,
           (double)s->cpu.cycle / secs / 1e6);
    free(s);
    return hit_limit ? 3 : 0;
}
