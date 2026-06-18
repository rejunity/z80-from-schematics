/* redcode_z80test_runner.c -- run Patrik Rak z80test through
 * redcode/Z80 (Manuel Sainz de Baranda, github.com/redcode/Z80).
 *
 * Why redcode? It explicitly models the Q flag-register (the
 * SCF/CCF "Q-leak" mechanism) and offers MEMPTR/WZ access -- useful
 * for triangulating against superzazu/chips/suzukiplan on:
 *
 *   - z80memptr (INI INIR IND INDR OUTI OTIR OUTD OTDR WZ trace tests)
 *   - z80full   (SCF / CCF / LDIR-into-NOP-prime / LDDR-into-NOP-prime)
 *
 *   redcode_z80test_runner <variant.tap> [max_instr]
 */
#define _POSIX_C_SOURCE 200809L

#include "refs/redcode_z80/Z80.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

static uint8_t mem[65536];
static uint8_t io_table[65536];

/* Capture buffer for the RST 10 character stream — same convention as
 * the other runners. */
static char *captured = NULL;
static size_t cap_used = 0, cap_sz = 1 << 20;

/* Track instruction count: count fetch_opcode invocations. Each fetch
 * begins one new instruction (or one new prefix byte), but for our
 * purposes "treat each fetch as one tick" is close enough — we just
 * want an upper bound to break runaway loops. */
static long long opcode_fetches = 0;
static long long max_instr_g  = 0;
static int       stop_flag    = 0;

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
            if (body[0] == 3) {
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

/* Callbacks. The third arg of fetch_opcode / fetch / read is the
 * address. context is a Z80* (we set ::context to point at the cpu). */
static uint8_t cb_fetch_opcode(void *ctx, uint16_t addr) {
    (void)ctx;
    opcode_fetches++;
    /* When CPU jumps to 0x0010 (Patrik's print stub: just a RET there
     * because we patched 0x0010 = 0xC9 below), print A. We detect at
     * fetch_opcode time so we don't depend on post-instruction PC. */
    if (addr == 0x0010) {
        Z80 *cpu = (Z80*)ctx;
        unsigned char ch = Z80_A(*cpu);
        putchar((int)ch);
        if (cap_used < cap_sz - 1) captured[cap_used++] = (char)ch;
        fflush(stdout);
    }
    /* Stop conditions:
     *   - PC reached 0x0000 (Rak's "all tests done — JP 0" sentinel)
     *   - hit max_instr ceiling
     */
    if (addr == 0x0000) stop_flag = 1;
    if (max_instr_g && opcode_fetches >= max_instr_g) stop_flag = 1;
    return mem[addr];
}
static uint8_t cb_fetch(void *ctx, uint16_t addr)    { (void)ctx; return mem[addr]; }
static uint8_t cb_read (void *ctx, uint16_t addr)    { (void)ctx; return mem[addr]; }
static void    cb_write(void *ctx, uint16_t addr, uint8_t v) { (void)ctx; mem[addr] = v; }
static uint8_t cb_in   (void *ctx, uint16_t addr)    { (void)ctx; return io_table[addr]; }
static void    cb_out  (void *ctx, uint16_t addr, uint8_t v) { (void)ctx; io_table[addr] = v; }

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <variant.tap> [max_instr]\n", argv[0]);
        return 2;
    }
    max_instr_g = (argc > 2) ? atoll(argv[2]) : 8000000000LL;

    uint16_t addr = 0; size_t n = 0;
    if (load_tap(argv[1], mem, &addr, &n) != 0) {
        fprintf(stderr, "tap load failed\n"); return 2;
    }

    /* Same ROM stubs + port-FE convention as scripts/z80test_runner.c. */
    mem[0x0010] = 0xC9;
    mem[0x1601] = 0xC9;
    /* Spectrum ULA-style port semantics for Patrik Rak's tests: tests
     * 098 INI and 099 IND have an `incheck` guard (raxoft/z80test
     * src/tests.asm:10, :1008-1022) that runs `IN A,(0xFE)` first and
     * aborts the test if the byte != 0xBF (the ULA idle-state return on
     * a real 48K Spectrum). Empirically the INI/IND test sequences only
     * issue INs to port 0xFE so the existing setup (high byte 0xFF
     * elsewhere, 0xBF on (?<<8)|0xFE) satisfies incheck. We tried
     * pinning every port to 0xBF -- CRC was unchanged, confirming the
     * CRC formula gap is NOT IO-data-byte-driven. */
    memset(io_table, 0xFF, sizeof io_table);
    for (int hi = 0; hi < 256; hi++)
        io_table[(hi << 8) | 0xFE] = 0xBF;

    captured = malloc(cap_sz);
    if (!captured) return 2;
    captured[0] = 0;

    Z80 cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.context      = &cpu;
    cpu.fetch_opcode = cb_fetch_opcode;
    cpu.fetch        = cb_fetch;
    cpu.read         = cb_read;
    cpu.write        = cb_write;
    cpu.in           = cb_in;
    cpu.out          = cb_out;
    cpu.halt         = NULL;
    cpu.nop          = NULL;
    cpu.nmia         = NULL;
    cpu.inta         = NULL;
    cpu.int_fetch    = NULL;
    cpu.ld_i_a       = NULL;
    cpu.ld_r_a       = NULL;
    cpu.reti         = NULL;
    cpu.retn         = NULL;
    cpu.hook         = NULL;
    cpu.illegal      = NULL;
    /* Zilog NMOS model preset: LD_A_IR_BUG + XQ + YQ. The Q (X/Y leak)
     * factors are essential for matching Patrik Rak's z80full SCF/CCF
     * expectations on Zilog silicon. Without them redcode behaves like
     * NEC and z80full 1/2 (SCF/CCF) fail while 3/4 (SCF NEC/CCF NEC)
     * pass -- the opposite of what we want for a Zilog Z80 reference. */
    cpu.options      = Z80_MODEL_ZILOG_NMOS;
    cpu.int_line     = 0;

    z80_power(&cpu, 1);
    z80_instant_reset(&cpu);

    /* Seed PC + SP to match the other Rak runners. After
     * z80_instant_reset, PC=0. We poke it manually. */
    Z80_PC(cpu) = addr;
    Z80_SP(cpu) = 0xFFEE;
    mem[0xFFEE] = 0x00; mem[0xFFEF] = 0x00;

    /* Patrik's tests use AF/BC/DE/HL etc. as 0xFFFF on entry. */
    Z80_AF(cpu) = 0xFFFF; Z80_BC(cpu) = 0xFFFF;
    Z80_DE(cpu) = 0xFFFF; Z80_HL(cpu) = 0xFFFF;
    Z80_AF_(cpu) = 0xFFFF; Z80_BC_(cpu) = 0xFFFF;
    Z80_DE_(cpu) = 0xFFFF; Z80_HL_(cpu) = 0xFFFF;
    Z80_IX(cpu) = 0xFFFF; Z80_IY(cpu) = 0xFFFF;
    Z80_WZ(cpu) = 0xFFFF;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Run in chunks. z80_execute returns the number of cycles actually
     * executed. We pump it in 100k-cycle slices and let the
     * fetch_opcode callback set stop_flag on PC=0 or max_instr. */
    const zusize chunk = 100000;
    while (!stop_flag) {
        z80_execute(&cpu, chunk);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    captured[cap_used] = 0;

    printf("\n=== redcode_z80test_runner: %s ===\n", argv[1]);
    printf("opcode-fetches: %lld  elapsed: %.2f s  (%.2f Mf/s)\n",
           opcode_fetches, secs, (double)opcode_fetches / secs / 1e6);
    const char *p = strstr(captured, "Result: ");
    if (p) printf("verdict from captured output: %s\n", p);
    else if (strstr(captured, "all tests passed")) printf("verdict: ALL TESTS PASSED\n");
    else printf("verdict: INCONCLUSIVE\n");

    free(captured);
    return 0;
}
