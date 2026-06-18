/* redcode_fuse_runner.c -- run the FUSE Z80 test suite (1356 cases)
 * through redcode/Z80 instead of our C model. Mirrors the structure
 * of scripts/fuse_runner.c so the pass/fail tallies are directly
 * comparable. Final-state mode only (no per-cycle bus-event check).
 *
 * Build (alongside the prebuilt redcode_Z80.o):
 *   cc  -DZ80_STATIC -DZ80_WITH_LOCAL_HEADER -DZ80_WITH_EXECUTE \
 *       -DZ80_WITH_Q -DZ80_WITH_PARITY_COMPUTATION \
 *       -DZ80_WITH_SPECIAL_RESET -DZ80_WITH_FULL_IM0 \
 *       -DZ80_WITH_UNOFFICIAL_RETI -O2 \
 *       -Iscripts/refs/redcode_z80 -Iscripts/refs/redcode_z80/Zeta \
 *       scripts/redcode_fuse_runner.c build/redcode_Z80.o \
 *       -o build/bin/redcode_fuse_runner
 *
 * Run:
 *   build/bin/redcode_fuse_runner tests/fuse/tests.in \
 *                                 tests/fuse/tests.expected [max_show]
 */
#define _POSIX_C_SOURCE 200809L

#include "Z80.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

static uint8_t mem[65536];

typedef struct { uint16_t addr; int n; uint8_t bytes[256]; } memblk_t;
typedef struct {
    char name[64];
    uint16_t af, bc, de, hl, af2, bc2, de2, hl2, ix, iy, sp, pc, memptr;
    uint8_t i, r, iff1, iff2, im, halted;
    int end_tstates;
    memblk_t mem[16]; int nmem;
} test_t;

/* ------------------------------------------------------------ Parsing ---- */
static int parse_mem_line(const char* line, memblk_t* mb) {
    while (*line == ' ' || *line == '\t') line++;
    if (line[0] == '-' && line[1] == '1') return 0;
    unsigned v; int off;
    if (sscanf(line, "%x%n", &v, &off) != 1) return -1;
    mb->addr = (uint16_t)v; mb->n = 0;
    const char* p = line + off;
    while (1) {
        const char* q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (q[0] == '-' && q[1] == '1') break;
        if (sscanf(p, "%x%n", &v, &off) != 1) break;
        mb->bytes[mb->n++] = (uint8_t)v;
        p += off;
        if (mb->n >= (int)sizeof mb->bytes) break;
    }
    return 1;
}
static int skip_blank_lines(FILE* f, char* line, size_t sz) {
    while (fgets(line, (int)sz, f)) {
        if (line[0] != '\n' && line[0] != '\r' && line[0] != 0) return 1;
    }
    return 0;
}
static int read_test_in(FILE* f, test_t* t) {
    char line[512];
    if (!skip_blank_lines(f, line, sizeof line)) return 0;
    sscanf(line, "%63s", t->name);
    if (!fgets(line, sizeof line, f)) return 0;
    unsigned af,bc,de,hl,af2,bc2,de2,hl2,ix,iy,sp,pc,mp;
    sscanf(line, "%x %x %x %x %x %x %x %x %x %x %x %x %x",
        &af,&bc,&de,&hl,&af2,&bc2,&de2,&hl2,&ix,&iy,&sp,&pc,&mp);
    t->af=(uint16_t)af; t->bc=(uint16_t)bc; t->de=(uint16_t)de; t->hl=(uint16_t)hl;
    t->af2=(uint16_t)af2; t->bc2=(uint16_t)bc2; t->de2=(uint16_t)de2; t->hl2=(uint16_t)hl2;
    t->ix=(uint16_t)ix; t->iy=(uint16_t)iy; t->sp=(uint16_t)sp; t->pc=(uint16_t)pc;
    t->memptr=(uint16_t)mp;
    if (!fgets(line, sizeof line, f)) return 0;
    unsigned i,r,iff1,iff2,im,halted; int ts;
    sscanf(line, "%x %x %u %u %u %u %d", &i,&r,&iff1,&iff2,&im,&halted,&ts);
    t->i=(uint8_t)i; t->r=(uint8_t)r; t->iff1=(uint8_t)iff1; t->iff2=(uint8_t)iff2;
    t->im=(uint8_t)im; t->halted=(uint8_t)halted; t->end_tstates=ts;
    t->nmem = 0;
    while (fgets(line, sizeof line, f)) {
        memblk_t mb;
        int r2 = parse_mem_line(line, &mb);
        if (r2 == 0) break;
        if (r2 < 0) break;
        if (t->nmem < (int)(sizeof t->mem / sizeof t->mem[0])) t->mem[t->nmem++] = mb;
    }
    return 1;
}
static int read_test_exp(FILE* f, test_t* t) {
    char line[512];
    if (!skip_blank_lines(f, line, sizeof line)) return 0;
    sscanf(line, "%63s", t->name);
    while (1) {
        if (!fgets(line, sizeof line, f)) return 1;
        if (line[0] != ' ' && line[0] != '\t') break;
    }
    unsigned af,bc,de,hl,af2,bc2,de2,hl2,ix,iy,sp,pc,mp;
    sscanf(line, "%x %x %x %x %x %x %x %x %x %x %x %x %x",
        &af,&bc,&de,&hl,&af2,&bc2,&de2,&hl2,&ix,&iy,&sp,&pc,&mp);
    t->af=(uint16_t)af; t->bc=(uint16_t)bc; t->de=(uint16_t)de; t->hl=(uint16_t)hl;
    t->af2=(uint16_t)af2; t->bc2=(uint16_t)bc2; t->de2=(uint16_t)de2; t->hl2=(uint16_t)hl2;
    t->ix=(uint16_t)ix; t->iy=(uint16_t)iy; t->sp=(uint16_t)sp; t->pc=(uint16_t)pc;
    t->memptr=(uint16_t)mp;
    if (!fgets(line, sizeof line, f)) return 0;
    unsigned i,r,iff1,iff2,im,halted; int ts;
    sscanf(line, "%x %x %u %u %u %u %d", &i,&r,&iff1,&iff2,&im,&halted,&ts);
    t->i=(uint8_t)i; t->r=(uint8_t)r; t->iff1=(uint8_t)iff1; t->iff2=(uint8_t)iff2;
    t->im=(uint8_t)im; t->halted=(uint8_t)halted; t->end_tstates=ts;
    t->nmem = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '\n' || line[0] == '\r' || line[0] == 0) break;
        memblk_t mb;
        int r2 = parse_mem_line(line, &mb);
        if (r2 == 0) break;
        if (r2 < 0) break;
        if (t->nmem < (int)(sizeof t->mem / sizeof t->mem[0])) t->mem[t->nmem++] = mb;
    }
    return 1;
}

/* ----------------------------------------------------- redcode callbacks - */
/* FUSE convention: every port read returns the HIGH byte of the address. */
static uint8_t cb_fetch_opcode(void *ctx, uint16_t a) { (void)ctx; return mem[a]; }
static uint8_t cb_fetch(void *ctx, uint16_t a)        { (void)ctx; return mem[a]; }
static uint8_t cb_read (void *ctx, uint16_t a)        { (void)ctx; return mem[a]; }
static void    cb_write(void *ctx, uint16_t a, uint8_t v) { (void)ctx; mem[a] = v; }
static uint8_t cb_in   (void *ctx, uint16_t a)        { (void)ctx; return (uint8_t)(a >> 8); }
static void    cb_out  (void *ctx, uint16_t a, uint8_t v) { (void)ctx; (void)a; (void)v; }

static void wire_callbacks(Z80 *cpu) {
    memset(cpu, 0, sizeof *cpu);
    cpu->context      = cpu;
    cpu->fetch_opcode = cb_fetch_opcode;
    cpu->fetch        = cb_fetch;
    cpu->read         = cb_read;
    cpu->write        = cb_write;
    cpu->in           = cb_in;
    cpu->out          = cb_out;
    /* All optional callbacks default to NULL (set by memset). */
    cpu->options      = 0;
}

int main(int argc, char** argv) {
    const char* in_path  = (argc>1) ? argv[1] : "tests/fuse/tests.in";
    const char* exp_path = (argc>2) ? argv[2] : "tests/fuse/tests.expected";
    int  max_show        = (argc>3) ? atoi(argv[3]) : 12;
    FILE* fi = fopen(in_path, "r");  if (!fi){perror(in_path); return 2;}
    FILE* fe = fopen(exp_path, "r"); if (!fe){perror(exp_path); return 2;}

    test_t tin, texp;
    int npass=0, nfail=0, ntotal=0;
    char buf[65536] = ""; int shown=0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (read_test_in(fi, &tin) && read_test_exp(fe, &texp)) {
        ntotal++;
        Z80 cpu;
        wire_callbacks(&cpu);
        z80_power(&cpu, 1);
        z80_instant_reset(&cpu);

        /* Seed register state from the .in test. */
        Z80_AF(cpu)  = tin.af;  Z80_BC(cpu)  = tin.bc;
        Z80_DE(cpu)  = tin.de;  Z80_HL(cpu)  = tin.hl;
        Z80_AF_(cpu) = tin.af2; Z80_BC_(cpu) = tin.bc2;
        Z80_DE_(cpu) = tin.de2; Z80_HL_(cpu) = tin.hl2;
        Z80_IX(cpu)  = tin.ix;  Z80_IY(cpu)  = tin.iy;
        Z80_SP(cpu)  = tin.sp;  Z80_PC(cpu)  = tin.pc;
        Z80_WZ(cpu)  = tin.memptr;
        cpu.i        = tin.i;
        cpu.r        = tin.r;
        cpu.r7       = tin.r;        /* preserve R7 across z80_execute */
        cpu.iff1     = tin.iff1;
        cpu.iff2     = tin.iff2;
        cpu.im       = tin.im;
        cpu.halt_line= tin.halted;

        /* Reset memory then apply per-test blocks. The "all 65536 bytes
         * matter" semantics of FUSE include reads from arbitrary
         * addresses, so initialise with a deterministic pattern. */
        memset(mem, 0, sizeof mem);
        for (int i=0; i<tin.nmem; i++)
            for (int j=0; j<tin.mem[i].n; j++)
                mem[(tin.mem[i].addr + j) & 0xFFFF] = tin.mem[i].bytes[j];

        /* Run for the expected T-state count. redcode counts in cycles
         * (= T-states) inside cpu.cycles. z80_execute may overshoot by
         * one instruction; the FUSE final-state check is forgiving of
         * minor T-state differences -- we still compare TS exactly for
         * info but tolerate it as a separate column. */
        cpu.cycles = 0;
        long target = texp.end_tstates;
        if (target < 1) target = 1;
        /* Some tests start in HALT state (tin.halted=1). The HALT
         * instruction itself was already executed before the test
         * starts; we just need redcode to keep executing internal NOPs
         * until target. Setting halt_line=1 isn't quite enough — we
         * also need cpu.options |= Z80_OPTION_HALT_SKIP so the engine
         * advances cycles in the halted state instead of stalling. */
        if (tin.halted) {
            #ifdef Z80_OPTION_HALT_SKIP
                cpu.options |= Z80_OPTION_HALT_SKIP;
            #endif
        }
        z80_execute(&cpu, (zusize)target);

        int ok = 1; char diff[1024] = "";
        #define CHK(field, e, a, fmt) \
            if ((unsigned)(e) != (unsigned)(a)) { ok=0; \
                int L=(int)strlen(diff); \
                snprintf(diff+L, sizeof diff-L, " " field "=" fmt "/" fmt, (unsigned)(e), (unsigned)(a)); }
        CHK("AF",   texp.af,  Z80_AF(cpu),  "%04x");
        CHK("BC",   texp.bc,  Z80_BC(cpu),  "%04x");
        CHK("DE",   texp.de,  Z80_DE(cpu),  "%04x");
        CHK("HL",   texp.hl,  Z80_HL(cpu),  "%04x");
        CHK("AF'",  texp.af2, Z80_AF_(cpu), "%04x");
        CHK("BC'",  texp.bc2, Z80_BC_(cpu), "%04x");
        CHK("DE'",  texp.de2, Z80_DE_(cpu), "%04x");
        CHK("HL'",  texp.hl2, Z80_HL_(cpu), "%04x");
        CHK("IX",   texp.ix,  Z80_IX(cpu),  "%04x");
        CHK("IY",   texp.iy,  Z80_IY(cpu),  "%04x");
        CHK("SP",   texp.sp,  Z80_SP(cpu),  "%04x");
        CHK("PC",   texp.pc,  Z80_PC(cpu),  "%04x");
        CHK("WZ",   texp.memptr, Z80_WZ(cpu), "%04x");
        CHK("I",    texp.i,   cpu.i,        "%02x");
        /* Reconstruct full R = (r & 0x7F) | (r7 & 0x80) per redcode's
         * internal convention (z80_r() in Z80.c). */
        uint8_t r_full = (uint8_t)((cpu.r & 0x7F) | (cpu.r7 & 0x80));
        CHK("R",    texp.r,   r_full,       "%02x");
        CHK("IFF1", texp.iff1, cpu.iff1,    "%u");
        CHK("IFF2", texp.iff2, cpu.iff2,    "%u");
        CHK("IM",   texp.im,  cpu.im,       "%u");
        /* T-states comparison: redcode's cpu.cycles after z80_execute
         * is the actual count. */
        CHK("TS", texp.end_tstates, (unsigned)cpu.cycles, "%u");
        for (int i=0; i<texp.nmem; i++)
            for (int j=0; j<texp.mem[i].n; j++) {
                uint16_t a = (uint16_t)(texp.mem[i].addr + j);
                if (mem[a] != texp.mem[i].bytes[j]) {
                    ok=0; int L=(int)strlen(diff);
                    snprintf(diff+L, sizeof diff-L, " M[%04x]=%02x/%02x",
                        a, texp.mem[i].bytes[j], mem[a]);
                }
            }
        #undef CHK
        if (ok) npass++;
        else {
            nfail++;
            if (shown < max_show) {
                int L = (int)strlen(buf);
                snprintf(buf+L, sizeof buf-L, "FAIL %-12s exp/got:%s\n", tin.name, diff);
                shown++;
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec)
                + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("=== redcode FUSE final-state mode ===\n");
    printf("%d tests: %d PASS, %d FAIL  (%.2f s)\n", ntotal, npass, nfail, secs);
    if (nfail) printf("\nFirst %d failures:\n%s", shown, buf);
    fclose(fi); fclose(fe);
    return nfail ? 1 : 0;
}
