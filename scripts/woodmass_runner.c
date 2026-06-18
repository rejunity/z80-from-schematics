/* woodmass_runner.c -- driver for Mark Woodmass's Z80 Test Suite
 * (2008, plus successor HALT2INT / Super HALT Invaders). These are
 * ZX Spectrum .tap files like Patrik Rak's z80test, but with two
 * complications:
 *
 *   1. They reference Spectrum ROM addresses (e.g. LD HL,0x2758),
 *      so a full run requires the 16 KiB Spectrum 48K ROM at 0x0000.
 *   2. The 2008 suite presents an interactive menu (CP '1'/'2'/'3').
 *      redcode's harness drives it from precomputed start addresses
 *      (0x8049 / 0x8057) and validates results via FNV-1 hash + line
 *      / column / cycle counts.
 *
 * For CI use we cannot redistribute the Sinclair / Amstrad ROM by
 * default. This runner therefore supports two modes:
 *
 *   - With --rom <path>: load the 16 KiB ROM, set up RST 0x10 print
 *     hook + 0x0D6B CLS RET stub like redcode, run until HALT, dump
 *     captured output + FNV-1.
 *   - Without --rom: zero ROM area, set RST 0x10 = RET so the print
 *     stub never traps, run with a hard cycle limit. The test will
 *     misbehave at the first 0x0xxx ROM read, but we can still
 *     observe that it (a) reaches code at 0x8057 and (b) terminates
 *     within a sensible time. CI declares the no-ROM run as a smoke
 *     check, NOT a pass/fail oracle.
 *
 *   woodmass_runner [--rom <path>] [--start <hex>] [--exit <hex>]
 *                   [--cycles <N>] <tap>
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "z80_sim.h"

static int load_rom(const char *path, uint8_t *mem) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    size_t n = fread(mem, 1, 16384, f);
    fclose(f);
    if (n != 16384) {
        fprintf(stderr, "ROM size %zu != 16384, refusing to use\n", n);
        return 1;
    }
    return 0;
}

static int load_tap(const char *path, uint8_t *mem,
                    uint16_t *out_addr, size_t *out_len) {
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
            if (body[0] == 3) {     /* CODE header */
                code_addr = (uint16_t)(body[13] | (body[14] << 8));
                found = 1;
            }
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

int main(int argc, char **argv) {
    const char *rom_path = NULL;
    const char *tap_path = NULL;
    uint16_t start_addr  = 0x8057;
    uint16_t exit_addr   = 0x80E6;
    long long max_cycles = 3000000000LL; /* ~10 min on real Spectrum */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            rom_path = argv[++i];
        } else if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
            start_addr = (uint16_t)strtoul(argv[++i], NULL, 16);
        } else if (strcmp(argv[i], "--exit") == 0 && i + 1 < argc) {
            exit_addr = (uint16_t)strtoul(argv[++i], NULL, 16);
        } else if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) {
            max_cycles = atoll(argv[++i]);
        } else if (argv[i][0] != '-') {
            tap_path = argv[i];
        }
    }
    if (!tap_path) {
        fprintf(stderr, "usage: %s [--rom <path>] [--start <hex>] [--exit <hex>] [--cycles <N>] <tap>\n", argv[0]);
        return 2;
    }

    z80_system_t *s = malloc(sizeof *s);
    z80_sys_init(s);
    s->io_ula_idle = 1;   /* match Rak runner port-parity convention */

    if (rom_path) {
        if (load_rom(rom_path, s->mem) != 0) { free(s); return 2; }
        printf("loaded ROM: %s\n", rom_path);
    } else {
        /* No ROM: RST 0x10 stub returns immediately, CLS = RET. */
        memset(s->mem, 0x00, 16384);
    }

    uint16_t load_addr = 0; size_t code_len = 0;
    if (load_tap(tap_path, s->mem, &load_addr, &code_len) != 0) {
        fprintf(stderr, "tap load failed\n"); free(s); return 2;
    }
    printf("loaded %zu bytes at 0x%04X, start=0x%04X exit=0x%04X\n",
           code_len, load_addr, start_addr, exit_addr);

    /* RST 0x10 print hook: if ROM is loaded, redcode patches
     * 0x0010 -> JP 0x70F2 and intercepts at 0x70F2. Without ROM we
     * patch 0x0010 = RET so prints become no-ops -- runner traps via
     * PC==0x0010 in the loop, like the Rak runner. */
    s->mem[0x0010] = 0xC9;        /* RET */
    s->mem[0x0D6B] = 0xC9;        /* CLS = RET stub (redcode convention) */

    /* HALT at exit address -- the test ends with HALT to signal completion. */
    s->mem[exit_addr] = 0x76;     /* HALT */

    z80_set_pc(&s->cpu, start_addr);
    s->cpu.rf[RFP_SP] = 0x7FE8;   /* redcode value */
    s->cpu.rf[RFP_AF] = 0x3222;   /* redcode value */
    s->cpu.reg_i = 0x3F;

    char *captured = malloc(1 << 20);
    size_t cap_used = 0, cap_sz = 1 << 20;
    if (!captured) { free(s); return 2; }

    /* FNV-1 32-bit hash, redcode initial value. */
    uint32_t hash = 0x811c9dc5u;

    long long count = 0;
    int halted_at_exit = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        uint16_t pc = s->cpu.rf[RFP_PC];
        if (pc == 0x0010) {
            unsigned char ch = (unsigned char)(s->cpu.rf[RFP_AF] >> 8);
            putchar((int)ch);
            if (cap_used < cap_sz - 1) captured[cap_used++] = (char)ch;
            hash ^= (uint32_t)ch;
            hash *= 0x01000193u;
            fflush(stdout);
        }
        if (pc == exit_addr || s->cpu.halted) {
            halted_at_exit = 1; break;
        }
        z80_sys_step_instr(s);
        if (++count >= max_cycles) break;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) +
                  (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    captured[cap_used] = 0;
    printf("\n=== woodmass_runner: %s ===\n", tap_path);
    printf("instructions executed: %lld %s\n", count,
           halted_at_exit ? "(HALT reached)" : "(LIMIT REACHED)");
    printf("elapsed: %.2f s  (%.2f Minstr/s)\n", secs, (double)count / secs / 1e6);
    printf("FNV-1 hash of printed output: 0x%08x\n", hash);
    printf("characters printed: %zu\n", cap_used);

    int ok = halted_at_exit && cap_used > 0;
    if (!rom_path) {
        printf("MODE: smoke (no ZX Spectrum ROM provided -- this is a sanity\n");
        printf("      check that the test code runs and halts, NOT a CRC oracle.\n");
        printf("verdict: %s\n", ok ? "SMOKE-OK" : "SMOKE-FAIL");
    } else {
        printf("MODE: full (ROM provided). Compare hash against expected.\n");
        printf("verdict: %s (hash check requires Mark Woodmass's expected FNV-1)\n",
               ok ? "RAN-TO-COMPLETION" : "DID-NOT-COMPLETE");
    }

    free(captured);
    free(s);
    return ok ? 0 : 1;
}
