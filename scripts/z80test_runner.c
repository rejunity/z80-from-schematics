/* ============================================================================
 * z80test_runner.c — run Patrik Rak's z80test (https://github.com/raxoft/z80test,
 * MIT) against the C model. Reads a ZX Spectrum .tap file, extracts the CODE
 * block, loads it at the address from the TAP header (always 0x8000 for the
 * z80test variants), and runs starting at 0x8000 until either:
 *   - PC returns to a sentinel address (0x0000), or
 *   - max_instr is reached.
 *
 * The Spectrum print path is short-circuited by stubbing two ROM entries
 * with single-byte RET (0xC9):
 *   - 0x10   : RST 0x10. We trap PC==0x10 before stepping to emit register A,
 *              then mem[0x10]=0xC9 returns to the caller.
 *   - 0x1601 : CHAN-OPEN. The test's printinit does JP 0x1601 which lands
 *              here — also stubbed with RET so the caller of printinit gets
 *              control back.
 *
 *   z80test_runner <variant.tap> [max_instr] [max_allowed_failures]
 *
 * Exit code: 0 if observed failures <= max_allowed_failures (default 0 =
 * strict full-pass required), 1 otherwise. The Rak suite surfaces real
 * silicon-faithfulness gaps in our model that we currently document as
 * audit followups; the max_allowed_failures arg lets CI catch *new*
 * regressions without flipping red on every existing one.
 * ==========================================================================*/
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "z80_sim.h"

/* Parse the .tap into mem. .tap layout: a sequence of blocks. Each block is
 *   u16 length(le)  u8 flag(0=header,ff=data)  ... payload ...  u8 checksum
 * The header for a CODE file (type=3) carries the load address in param1. */
static int load_tap(const char *path, uint8_t mem[65536],
                    uint16_t *out_addr, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END); long total = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *tap = malloc((size_t)total);
    if (!tap) { fclose(f); fprintf(stderr, "oom\n"); return 1; }
    if (fread(tap, 1, (size_t)total, f) != (size_t)total) {
        fclose(f); free(tap); fprintf(stderr, "%s: short read\n", path); return 1;
    }
    fclose(f);

    long off = 0;
    int found = 0;
    uint16_t code_addr = 0;
    /* Scan for: (1) a CODE header (type=3) — captures the load address;
       (2) the immediately-following DATA block — that's the binary. */
    while (off < total - 2) {
        unsigned blen = (unsigned)tap[off] | ((unsigned)tap[off+1] << 8);
        if (off + 2 + (long)blen > total) break;
        uint8_t flag = tap[off + 2];
        uint8_t *body = &tap[off + 3];
        long body_len = (long)blen - 2;  /* minus flag + checksum */
        if (flag == 0x00 && body_len >= 14) {
            /* header: type(1) name(10) length(2) param1(2) param2(2) */
            if (body[0] == 3) {            /* CODE */
                code_addr = (uint16_t)(body[13] | (body[14] << 8));
                found = 1;
            }
        } else if (flag == 0xff && found) {
            /* data: body_len bytes of payload */
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
    fprintf(stderr, "%s: no CODE block found\n", path);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,"usage: %s <variant.tap> [max_instr] [max_allowed_failures]\n",argv[0]);
        return 2;
    }
    long long max_instr     = (argc > 2) ? atoll(argv[2]) : 5000000000LL;
    int       max_allowed   = (argc > 3) ? atoi(argv[3])  : 0;

    z80_system_t *s = malloc(sizeof(*s));
    if (!s) return 2;
    z80_sys_init(s);

    uint16_t addr = 0; size_t n = 0;
    if (load_tap(argv[1], s->mem, &addr, &n) != 0) { free(s); return 2; }

    /* ROM stubs: RST 0x10 print + CHAN-OPEN no-op. */
    s->mem[0x0010] = 0xC9;        /* RET */
    s->mem[0x1601] = 0xC9;        /* RET */

    /* Port-input convention. The z80test IN-block subtests target port 0xFE
       (the ZX Spectrum ULA) and check that the read value is 0xBF —
       which is what the ULA returns when no keyboard keys are pressed
       AND the EAR input is high (the test's earlier "ld a,7 / out (fe),a"
       configures the ULA so this is true). Make every address whose
       low byte is 0xFE return 0xBF; everything else returns 0xFF
       (open-bus convention). */
    memset(s->io, 0xFF, 65536);
    for (int hi = 0; hi < 256; hi++)
        s->io[(hi << 8) | 0xFE] = 0xBF;

    /* Exit sentinel: when the test's outer RET pops a return address of 0,
       we'll see PC == 0 and stop. The cleanest way: leave SP pointing at a
       pair of zero bytes that we explicitly install. */
    s->cpu.rf[RFP_SP] = 0xFFEE;
    s->mem[0xFFEE] = 0x00;
    s->mem[0xFFEF] = 0x00;
    z80_set_pc(&s->cpu, addr);

    long long count = 0;
    int hit_limit = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Capture the test output for pass/fail detection. Mirror to stdout
       live (for the CI log) and into a buffer (for the post-run grep). */
    char *captured = malloc(1 << 20);   /* 1 MB is plenty for any variant */
    size_t cap_used = 0, cap_sz = 1 << 20;
    if (!captured) { free(s); return 2; }

    for (;;) {
        uint16_t pc = s->cpu.rf[RFP_PC];
        if (pc == 0x0000) break;
        if (pc == 0x0010) {
            /* RST 0x10 print: emit register A. */
            unsigned char ch = (unsigned char)(s->cpu.rf[RFP_AF] >> 8);
            putchar((int)ch);
            if (cap_used < cap_sz - 1) captured[cap_used++] = (char)ch;
            fflush(stdout);
            /* mem[0x10] = RET; let the CPU execute it normally below. */
        }
        z80_sys_step_instr(s);
        if (++count >= max_instr) { hit_limit = 1; break; }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec)
                + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    captured[cap_used] = 0;
    printf("\n=== z80test_runner: %s ===\n", argv[1]);
    printf("loaded %zu bytes at 0x%04X\n", n, addr);
    printf("instructions executed: %lld%s\n", count, hit_limit ? " (LIMIT REACHED)" : "");
    printf("elapsed: %.2f s  (%.2f Minstr/s)\n", secs, (double)count / secs / 1e6);

    int rc = 2;
    int observed_failures = -1;
    if (strstr(captured, "all tests passed")) {
        observed_failures = 0;
        printf("verdict: PASS (all 160 tests passed)\n");
        rc = 0;
    } else {
        /* Parse "Result: NNN of MMM tests failed." — the trailing line. */
        const char *p = strstr(captured, "Result: ");
        if (p && sscanf(p, "Result: %d of", &observed_failures) == 1) {
            if (observed_failures <= max_allowed) {
                printf("verdict: PASS (%d failure(s); within tolerance %d)\n",
                       observed_failures, max_allowed);
                rc = 0;
            } else {
                printf("verdict: FAIL (%d failure(s); tolerance was %d — REGRESSION)\n",
                       observed_failures, max_allowed);
                rc = 1;
            }
        } else {
            printf("verdict: INCONCLUSIVE — no pass/fail string found\n");
        }
    }
    free(captured); free(s);
    return rc;
}
