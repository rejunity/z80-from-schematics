/* chips_z80test_runner.c — run Patrik Rak's z80test through floooh's
 * chips/z80.h emulator (scripts/refs/chips_z80.h). Tick-based; we run
 * z80_tick() until z80_opdone() is true to step one instruction.
 *
 *   chips_z80test_runner <variant.tap> [max_instr]
 */
#define _POSIX_C_SOURCE 200809L
#define CHIPS_IMPL                /* make z80.h define the implementation */
#include "refs/chips_z80.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

static uint8_t mem[65536];
static uint8_t io_table[65536];

static int load_tap(const char *path, uint8_t mem[65536],
                    uint16_t *out_addr, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END); long total = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *tap = malloc((size_t)total);
    if (!tap) { fclose(f); return 1; }
    if (fread(tap, 1, (size_t)total, f) != (size_t)total) {
        fclose(f); free(tap); return 1;
    }
    fclose(f);
    long off = 0; int found = 0; uint16_t code_addr = 0;
    while (off < total - 2) {
        unsigned blen = (unsigned)tap[off] | ((unsigned)tap[off+1] << 8);
        if (off + 2 + (long)blen > total) break;
        uint8_t flag = tap[off + 2];
        uint8_t *body = &tap[off + 3];
        long body_len = (long)blen - 2;
        if (flag == 0x00 && body_len >= 14) {
            if (body[0] == 3) { code_addr = (uint16_t)(body[13] | (body[14] << 8)); found = 1; }
        } else if (flag == 0xff && found) {
            size_t copy = (body_len < 0) ? 0 : (size_t)body_len;
            if ((unsigned)code_addr + copy > 0x10000) copy = 0x10000 - code_addr;
            memcpy(&mem[code_addr], body, copy);
            *out_addr = code_addr; *out_len = copy;
            free(tap); return 0;
        }
        off += 2 + (long)blen;
    }
    free(tap); return 1;
}

/* Tick once + service whatever bus op was requested. Inline the data-bus
 * bit ops because chips's Z80_SET_DATA is a brace-block macro that can't
 * sit in an else-if chain (matches the style of scripts/lockstep_quad.c). */
static inline uint64_t one_tick(z80_t *cpu, uint64_t pins) {
    pins = z80_tick(cpu, pins);
    uint16_t a = Z80_GET_ADDR(pins);
    if (pins & Z80_MREQ) {
        if (pins & Z80_RD) {
            pins = (pins & ~0xFF0000ULL) | ((uint64_t)mem[a] << 16);
        } else if (pins & Z80_WR) {
            mem[a] = (uint8_t)((pins >> 16) & 0xFF);
        }
    } else if (pins & Z80_IORQ) {
        if (pins & Z80_RD) {
            pins = (pins & ~0xFF0000ULL) | ((uint64_t)io_table[a] << 16);
        } else if (pins & Z80_WR) {
            io_table[a] = (uint8_t)((pins >> 16) & 0xFF);
        }
    }
    return pins;
}
/* Tick until next opdone — one full instruction. */
static uint64_t step_instr(z80_t *cpu, uint64_t pins) {
    pins = one_tick(cpu, pins);
    /* skip any leading ticks where opdone is true (prior M1/RD overlap) */
    while (z80_opdone(cpu)) pins = one_tick(cpu, pins);
    while (!z80_opdone(cpu)) pins = one_tick(cpu, pins);
    return pins;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <variant.tap> [max_instr]\n", argv[0]);
        return 2;
    }
    long long max_instr = (argc > 2) ? atoll(argv[2]) : 8000000000LL;

    uint16_t addr = 0; size_t n = 0;
    if (load_tap(argv[1], mem, &addr, &n) != 0) {
        fprintf(stderr, "tap load failed\n"); return 2;
    }

    /* ROM stubs + port-FE convention identical to scripts/z80test_runner.c. */
    mem[0x0010] = 0xC9;
    mem[0x1601] = 0xC9;
    memset(io_table, 0xFF, sizeof io_table);
    for (int hi = 0; hi < 256; hi++)
        io_table[(hi << 8) | 0xFE] = 0xBF;

    z80_t cpu;
    uint64_t pins = z80_init(&cpu);
    pins = z80_prefetch(&cpu, addr);
    /* match the reset-state our runners poke for determinism — set AFTER
     * prefetch since prefetch may rewrite some internal state */
    cpu.af = 0xFFFF; cpu.bc = 0xFFFF; cpu.de = 0xFFFF; cpu.hl = 0xFFFF;
    cpu.ix = 0xFFFF; cpu.iy = 0xFFFF; cpu.wz = 0xFFFF;
    cpu.af2 = 0xFFFF; cpu.bc2 = 0xFFFF; cpu.de2 = 0xFFFF; cpu.hl2 = 0xFFFF;
    cpu.sp = 0xFFEE;
    mem[0xFFEE] = 0x00; mem[0xFFEF] = 0x00;

    long long count = 0;
    char *captured = malloc(1 << 20);
    size_t cap_used = 0, cap_sz = 1 << 20;
    if (!captured) return 2;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* chips uses M1+RD overlap: after step_instr completes the RST 10
     * (which jumps to 0x0010, sets the return pushed), the NEXT byte has
     * been prefetched at the start of the next instruction's M1. So at
     * opdone-time pc reads as 0x0011 (one past the RET byte at 0x0010),
     * not 0x0010. Detect pc==0x0011 + sp value matching the push to
     * recognise the print trap distinct from any other transition
     * through 0x0011. */
    int prev_was_in_rst10 = 0;
    for (;;) {
        /* Detect "we just finished the RET at 0x0010 ROM stub" via the
         * sp-decreased-then-increased pattern: after RST 10 we push 2
         * bytes (sp -= 2), then RET pops (sp += 2). At the moment opdone
         * fires post-RST-10, pc == 0x0011 (overlap-prefetched the RET
         * byte). A still holds the original character. */
        if (cpu.pc == 0x0011 && mem[0x0010] == 0xC9) {
            unsigned char ch = cpu.a;
            putchar((int)ch);
            if (cap_used < cap_sz - 1) captured[cap_used++] = (char)ch;
            fflush(stdout);
        }
        if (cpu.pc == 0x0000) break;
        pins = step_instr(&cpu, pins);
        if (++count >= max_instr) break;
        (void)prev_was_in_rst10;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    captured[cap_used] = 0;

    printf("\n=== chips_z80test_runner: %s ===\n", argv[1]);
    printf("instructions executed: %lld  elapsed: %.2f s  (%.2f Minstr/s)\n",
           count, secs, (double)count / secs / 1e6);
    const char *p = strstr(captured, "Result: ");
    if (p) printf("verdict from captured output: %s\n", p);
    else if (strstr(captured, "all tests passed")) printf("verdict: ALL TESTS PASSED\n");
    else printf("verdict: INCONCLUSIVE\n");

    free(captured);
    return 0;
}
