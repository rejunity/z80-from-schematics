/* fuse_runner.c - run the Frank-D-Cringle / FUSE Z80 test suite against the C
   model. Reads tests/fuse/tests.in (~1356 cases) and tests/fuse/tests.expected,
   compares final state (regs+IFFs+IM+HALTED+T-states+memory) per case.
   Per-cycle bus-event verification (the MC/MR/MW lines) is deferred. */
/* Expose clock_gettime / CLOCK_MONOTONIC / struct timespec from <time.h>:
   Linux glibc hides POSIX.1-2008 extensions at -std=c99 unless asked;
   macOS libc exposes them unconditionally. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "z80_sim.h"

typedef struct { uint16_t addr; int n; uint8_t bytes[256]; } memblk_t;
typedef struct {
    char name[64];
    uint16_t af, bc, de, hl, af2, bc2, de2, hl2, ix, iy, sp, pc, memptr;
    uint8_t i, r, iff1, iff2, im, halted;
    int end_tstates;
    memblk_t mem[16]; int nmem;
} test_t;

/* Read a memory line of the form "addr b0 b1 ... -1" (addr and bytes both hex)
   or a terminating "-1" alone. Returns 1=block read, 0=terminator, -1=malformed. */
static int parse_mem_line(const char* line, memblk_t* mb) {
    while (*line == ' ' || *line == '\t') line++;
    if (line[0] == '-' && line[1] == '1') return 0;     /* terminator */
    unsigned v; int off;
    if (sscanf(line, "%x%n", &v, &off) != 1) return -1;
    mb->addr = (uint16_t)v;
    mb->n = 0;
    const char* p = line + off;
    while (1) {
        const char* q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (q[0] == '-' && q[1] == '1') break;          /* inline -1 ends bytes */
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
        if (r2 == 0) break;       /* "-1" terminator */
        if (r2 < 0) break;        /* malformed -> stop */
        if (t->nmem < (int)(sizeof t->mem / sizeof t->mem[0])) t->mem[t->nmem++] = mb;
    }
    return 1;
}

static int read_test_exp(FILE* f, test_t* t) {
    char line[512];
    if (!skip_blank_lines(f, line, sizeof line)) return 0;
    sscanf(line, "%63s", t->name);
    /* skip event lines (leading whitespace) until the registers line */
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

int main(int argc, char** argv) {
    const char* in_path  = (argc>1) ? argv[1] : "tests/fuse/tests.in";
    const char* exp_path = (argc>2) ? argv[2] : "tests/fuse/tests.expected";
    FILE* fi = fopen(in_path, "r");  if (!fi){perror(in_path); return 2;}
    FILE* fe = fopen(exp_path, "r"); if (!fe){perror(exp_path); return 2;}

    z80_system_t* s = malloc(sizeof *s);
    test_t tin, texp;
    int npass=0, nfail=0, ntotal=0;
    char buf[65536] = ""; int shown=0;
    int max_show = (argc>3) ? atoi(argv[3]) : 12;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (read_test_in(fi, &tin) && read_test_exp(fe, &texp)) {
        ntotal++;
        z80_sys_init(s);
        /* FUSE convention: port read returns the HIGH byte of the address bus.
           Pre-fill s->io so any port read at (B<<8)|C / (A<<8)|N returns B/A. */
        for (int a = 0; a < 65536; a++) s->io[a] = (uint8_t)(a >> 8);
        s->cpu.rf[RFP_AF]=tin.af; s->cpu.rf[RFP_BC]=tin.bc;
        s->cpu.rf[RFP_DE]=tin.de; s->cpu.rf[RFP_HL]=tin.hl;
        s->cpu.rf[RFP_AF2]=tin.af2; s->cpu.rf[RFP_BC2]=tin.bc2;
        s->cpu.rf[RFP_DE2]=tin.de2; s->cpu.rf[RFP_HL2]=tin.hl2;
        s->cpu.rf[RFP_IX]=tin.ix; s->cpu.rf[RFP_IY]=tin.iy;
        s->cpu.rf[RFP_SP]=tin.sp; s->cpu.rf[RFP_WZ]=tin.memptr;
        z80_set_pc(&s->cpu, tin.pc);
        s->cpu.reg_i=tin.i; s->cpu.reg_r=tin.r;
        s->cpu.iff1=tin.iff1; s->cpu.iff2=tin.iff2;
        s->cpu.im=tin.im; s->cpu.halted=tin.halted;
        /* FUSE convention: Q (the F value left by the "previous" instruction
           used by SCF/CCF X/Y) is taken to be the initial F. */
        s->cpu.q = (uint8_t)(tin.af & 0xFF);
        s->cpu.f_modified = false;
        s->cpu.cycle = 0;
        for (int i=0; i<tin.nmem; i++)
            for (int j=0; j<tin.mem[i].n; j++)
                s->mem[(tin.mem[i].addr + j) & 0xFFFF] = tin.mem[i].bytes[j];

        uint64_t target = (uint64_t)texp.end_tstates * 2;
        int guard = 0;
        while (s->cpu.cycle < target && guard++ < 200) {
            z80_sys_step_instr(s);
        }

        int ok = 1; char diff[1024] = "";
        #define CHK(field, e, a, fmt) \
            if ((unsigned)(e) != (unsigned)(a)) { ok=0; \
                int L=(int)strlen(diff); \
                snprintf(diff+L, sizeof diff-L, " " field "=" fmt "/" fmt, (unsigned)(e), (unsigned)(a)); }
        CHK("AF", texp.af, s->cpu.rf[RFP_AF], "%04x");
        CHK("BC", texp.bc, s->cpu.rf[RFP_BC], "%04x");
        CHK("DE", texp.de, s->cpu.rf[RFP_DE], "%04x");
        CHK("HL", texp.hl, s->cpu.rf[RFP_HL], "%04x");
        CHK("AF'", texp.af2, s->cpu.rf[RFP_AF2], "%04x");
        CHK("BC'", texp.bc2, s->cpu.rf[RFP_BC2], "%04x");
        CHK("DE'", texp.de2, s->cpu.rf[RFP_DE2], "%04x");
        CHK("HL'", texp.hl2, s->cpu.rf[RFP_HL2], "%04x");
        CHK("IX", texp.ix, s->cpu.rf[RFP_IX], "%04x");
        CHK("IY", texp.iy, s->cpu.rf[RFP_IY], "%04x");
        CHK("SP", texp.sp, s->cpu.rf[RFP_SP], "%04x");
        CHK("PC", texp.pc, s->cpu.rf[RFP_PC], "%04x");
        CHK("WZ", texp.memptr, s->cpu.rf[RFP_WZ], "%04x");
        CHK("I", texp.i, s->cpu.reg_i, "%02x");
        CHK("R", texp.r, s->cpu.reg_r, "%02x");
        CHK("IFF1", texp.iff1, s->cpu.iff1, "%u");
        CHK("IFF2", texp.iff2, s->cpu.iff2, "%u");
        CHK("IM", texp.im, s->cpu.im, "%u");
        CHK("HALTED", texp.halted, s->cpu.halted, "%u");
        CHK("TS", texp.end_tstates, s->cpu.cycle / 2, "%u");
        for (int i=0; i<texp.nmem; i++)
            for (int j=0; j<texp.mem[i].n; j++) {
                uint16_t a = (uint16_t)(texp.mem[i].addr + j);
                if (s->mem[a] != texp.mem[i].bytes[j]) {
                    ok=0; int L=(int)strlen(diff);
                    snprintf(diff+L, sizeof diff-L, " M[%04x]=%02x/%02x",
                        a, texp.mem[i].bytes[j], s->mem[a]);
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
    double secs = (t1.tv_sec-t0.tv_sec) + (t1.tv_nsec-t0.tv_nsec)/1e9;

    printf("=== FUSE z80-test (final-state mode) ===\n");
    printf("%d tests: %d PASS, %d FAIL  (%.2f s)\n", ntotal, npass, nfail, secs);
    if (nfail) printf("\nFirst %d failures:\n%s", shown, buf);
    free(s); fclose(fi); fclose(fe);
    return nfail ? 1 : 0;
}
