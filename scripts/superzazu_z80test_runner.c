/* superzazu_z80test_runner.c — run Patrik Rak's z80test through the
 * superzazu/z80 oracle (scripts/refs/superzazu_z80.c). Same TAP loader,
 * same ROM stubs, same port-FE convention as scripts/z80test_runner.c.
 *
 * Used to triangulate the F-block-op gap: if superzazu PASSES the tests
 * our model fails (INI/IND, INIR→NOP', LDIR→NOP', SCF/CCF), the diff
 * between its implementation and ours is the answer. If it FAILS them
 * too, the gap is something other than a single emulator's choice.
 *
 *   superzazu_z80test_runner <variant.tap> [max_instr]
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "refs/superzazu_z80.h"

static uint8_t mem[65536];
static uint8_t io_table[65536];

static uint8_t mem_read (void *ud, uint16_t a) { (void)ud; return mem[a]; }
static void    mem_write(void *ud, uint16_t a, uint8_t v) { (void)ud; mem[a] = v; }
static uint8_t port_in  (z80 *cpu, uint8_t lo)  {
    uint16_t a = ((uint16_t)cpu->b << 8) | lo;
    return io_table[a];
}
static void    port_out (z80 *cpu, uint8_t lo, uint8_t v) {
    uint16_t a = ((uint16_t)cpu->b << 8) | lo;
    io_table[a] = v;
}

/* TAP loader — same as scripts/z80test_runner.c. */
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
    long off = 0;
    int found = 0;
    uint16_t code_addr = 0;
    while (off < total - 2) {
        unsigned blen = (unsigned)tap[off] | ((unsigned)tap[off+1] << 8);
        if (off + 2 + (long)blen > total) break;
        uint8_t flag = tap[off + 2];
        uint8_t *body = &tap[off + 3];
        long body_len = (long)blen - 2;
        if (flag == 0x00 && body_len >= 14) {
            if (body[0] == 3) {
                code_addr = (uint16_t)(body[13] | (body[14] << 8));
                found = 1;
            }
        } else if (flag == 0xff && found) {
            size_t copy = (body_len < 0) ? 0 : (size_t)body_len;
            if ((unsigned)code_addr + copy > 0x10000) copy = 0x10000 - code_addr;
            memcpy(&mem[code_addr], body, copy);
            *out_addr = code_addr;
            *out_len  = copy;
            free(tap);
            return 0;
        }
        off += 2 + (long)blen;
    }
    free(tap);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <variant.tap> [max_instr]\n", argv[0]);
        return 2;
    }
    long long max_instr = (argc > 2) ? atoll(argv[2]) : 8000000000LL;

    uint16_t addr = 0; size_t n = 0;
    if (load_tap(argv[1], mem, &addr, &n) != 0) {
        fprintf(stderr, "tap load failed\n"); return 2;
    }

    /* ROM stubs: RST 0x10 print + CHAN-OPEN no-op. */
    mem[0x0010] = 0xC9;
    mem[0x1601] = 0xC9;

    /* Port-input convention identical to scripts/z80test_runner.c. */
    memset(io_table, 0xFF, sizeof io_table);
    for (int hi = 0; hi < 256; hi++)
        io_table[(hi << 8) | 0xFE] = 0xBF;

    z80 cpu;
    z80_init(&cpu);
    cpu.read_byte  = mem_read;
    cpu.write_byte = mem_write;
    cpu.port_in    = port_in;
    cpu.port_out   = port_out;
    cpu.userdata   = NULL;

    /* Stack sentinel: SP=0xFFEE so RET pops 0x0000 and exit-on-PC-0 fires. */
    cpu.sp = 0xFFEE;
    mem[0xFFEE] = 0x00; mem[0xFFEF] = 0x00;
    cpu.pc = addr;

    long long count = 0;
    char *captured = malloc(1 << 20);
    size_t cap_used = 0, cap_sz = 1 << 20;
    if (!captured) return 2;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        if (cpu.pc == 0x0000) break;
        if (cpu.pc == 0x0010) {
            unsigned char ch = cpu.a;
            putchar((int)ch);
            if (cap_used < cap_sz - 1) captured[cap_used++] = (char)ch;
            fflush(stdout);
        }
        z80_step(&cpu);
        if (++count >= max_instr) break;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec)
                + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    captured[cap_used] = 0;

    printf("\n=== superzazu_z80test_runner: %s ===\n", argv[1]);
    printf("instructions executed: %lld\n", count);
    printf("elapsed: %.2f s  (%.2f Minstr/s)\n", secs, (double)count / secs / 1e6);

    const char *p = strstr(captured, "Result: ");
    if (p) printf("verdict from captured output: %s\n", p);
    else if (strstr(captured, "all tests passed")) printf("verdict: ALL TESTS PASSED\n");
    else printf("verdict: INCONCLUSIVE\n");

    free(captured);
    return 0;
}
