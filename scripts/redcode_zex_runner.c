/* redcode_zex_runner.c - a CP/M .com (prelim, ZEXDOC, ZEXALL) through
   redcode/Z80, the reference emulator in scripts/refs/redcode_z80, built as
   Zilog NMOS (REDCODE_FLAGS in the Makefile; make redcode_zex).

   Usage: redcode_zex_runner <prog.com> [max_instr]

   The program loads at 0100 with SP = FFFE, as build/bin/zexrunner runs
   it; a CALL 0005 is BDOS: C = 2
   prints E, C = 9 prints the string at DE up to '$', then the call returns.
   A jump to 0000 ends the run. Prints what the program printed and, at the
   limit, "(LIMIT REACHED)" -- scripts/run_zex.py --runner reads both, like
   it reads build/bin/zexrunner. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Z80.h"

static zuint8 mem[0x10000];
static zuint8 rd(void *u, zuint16 a) { (void)u; return mem[a]; }
static void wr(void *u, zuint16 a, zuint8 v) { (void)u; mem[a] = v; }
static zuint8 in_(void *u, zuint16 a) { (void)u; (void)a; return 0xff; }
static void out_(void *u, zuint16 a, zuint8 v) { (void)u; (void)a; (void)v; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s prog.com [max_instr]\n", argv[0]); return 2; }
    long long max_instr = argc > 2 ? atoll(argv[2]) : 8000000000LL;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    size_t n = fread(mem + 0x100, 1, 0x10000 - 0x100, f);
    fclose(f);
    printf("== redcode/Z80 (Zilog NMOS): %s, %zu bytes ==\n", argv[1], n);
    static Z80 cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.fetch_opcode = rd; cpu.fetch = rd; cpu.read = rd; cpu.write = wr;
    cpu.in = in_; cpu.out = out_;
    cpu.options = Z80_MODEL_ZILOG_NMOS;
    z80_power(&cpu, 1);
    mem[0x0000] = 0xc3; mem[0x0001] = 0x00; mem[0x0002] = 0x00;   /* JP 0000: the end */
    mem[0x0005] = 0xc9;                                  /* BDOS: RET after the trap */
    cpu.pc.uint16_value = 0x0100;
    cpu.sp.uint16_value = 0xfffe;
    long long i = 0;
    unsigned long long t = 0;
    for (; i < max_instr; i++) {
        zuint16 pc = cpu.pc.uint16_value;
        if (pc == 0x0000) break;
        if (pc == 0x0005) {
            zuint8 c = cpu.bc.uint8_values.at_0;
            zuint16 de = cpu.de.uint16_value;
            if (c == 2) putchar(cpu.de.uint8_values.at_0);
            else if (c == 9) for (int g = 0; g < 65536 && mem[de] != '$'; g++) putchar(mem[de++]);
            fflush(stdout);
        }
        t += z80_execute(&cpu, 1);
    }
    printf("\n== %lld instructions, %llu T-states%s ==\n", i, t, i >= max_instr ? " (LIMIT REACHED)" : "");
    return 0;
}
