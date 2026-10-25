/* bench_common.h - the frame every C emulator runs ZEXDOC in for make
   cfast_bench (scripts/cfast_bench.py): the .com at 0100, JP 0000 as the
   exit, RET at 0005 for BDOS, whose console calls the harness prints when
   PC reaches 0005; N instructions, then the same three lines as
   scripts/zexrunner.c prints, so one script reads every emulator. The
   timing is the script's: the process's own CPU time. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static uint8_t MEM[65536];

static void load_com(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    size_t n = fread(&MEM[0x100], 1, 0x10000 - 0x100, f);
    fclose(f);
    (void)n;
    MEM[0] = 0xC3; MEM[1] = 0; MEM[2] = 0;      /* JP 0000: the end */
    MEM[5] = 0xC9;                              /* BDOS: RET */
}

static void bdos(uint8_t c, uint16_t de, uint8_t e)
{
    if (c == 2) putchar(e);
    else if (c == 9) for (int g = 0; g < 65536 && MEM[de] != '$'; g++) putchar(MEM[de++]);
}

/* n instructions, the T-states they took, the PC of the next one */
static void report(const char *name, long long n, long long limit, double tstates, unsigned pc)
{
    printf("\n=== %s ===\n", name);
    printf("instructions executed: %lld%s\n", n, n >= limit ? " (LIMIT REACHED)" : "");
    printf("final PC=%04X\n", pc & 0xFFFF);
    printf("T-states: %.0f\n", tstates);
}

static long long args(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <prog.com> [max_instr]\n", argv[0]); exit(2); }
    load_com(argv[1]);
    return argc > 2 ? atoll(argv[2]) : 4000000000LL;
}
