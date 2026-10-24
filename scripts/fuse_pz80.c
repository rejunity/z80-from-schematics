/* fuse_pz80.c - the FUSE corpus on the die: every case of tests/fuse/tests.in
   runs on perfectz80 (the transistor netlist of the Zilog NMOS die, Brian
   Silverman et al.; scripts/refs/perfectz80) and the die's registers and
   memory are compared with tests.expected, or with tests/fuse/silicon.expected
   for the cases listed there.

   The die starts from RESET, so a preamble loads each case's registers: it
   ends at the byte before the case's PC and falls into the instruction, with
   the stack, WZ (LD A,(WZ-1)), I and R (LD I,A / LD R,A, counted back over
   the M1s that follow) set as the case gives them; its last instructions are
   POP AF and LD SP,nn, so Q is 0 when the case starts (no flags written by
   the previous instruction). The registers are read from the die's nodes in
   T4 of the M1 that follows the case: the die writes some results in that
   M1's T1-T3 (docs/timing.md), and R steps only at the end of T4. PC is the
   address that M1 fetches, R the one its refresh puts out (the die steps the
   R latch at T3.N, the refresh address keeps the value before). The case's
   T-states are checked too: that M1
   must start end_tstates after the case's own first M1.

   Not checked on the die: IFF1, IFF2 and IM (no reader in perfectz80). EI
   or DI loads IFF2's value into both flip-flops, so the four RETN cases that
   start with IFF1 != IFF2 run with IFF1 = IFF2; no register read here
   depends on IFF1. A case whose memory would collide with the preamble is
   reported as not run (none in the corpus).

     fuse_pz80 <tests.in> <tests.expected> [silicon.expected] [-v] [--only NAME,...]

   Exit 0 when every case matches. -v prints the die's values for every case
   in tests.expected's format (no event lines), as silicon.expected has them;
   '#' lines in the expected files are comments. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "refs/perfectz80/perfectz80.h"

typedef struct { uint16_t addr; int n; uint8_t bytes[256]; } memblk_t;
typedef struct {
    char name[64];
    uint16_t af, bc, de, hl, af2, bc2, de2, hl2, ix, iy, sp, pc, memptr;
    uint8_t i, r, iff1, iff2, im, halted;
    int tstates;
    memblk_t mem[16]; int nmem;
} test_t;

static int parse_mem_line(const char *line, memblk_t *mb) {
    while (*line == ' ' || *line == '\t') line++;
    if (line[0] == '-' && line[1] == '1') return 0;
    unsigned v; int off;
    if (sscanf(line, "%x%n", &v, &off) != 1) return -1;
    mb->addr = (uint16_t)v; mb->n = 0;
    const char *p = line + off;
    for (;;) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (q[0] == '-' && q[1] == '1') break;
        if (sscanf(p, "%x%n", &v, &off) != 1) break;
        mb->bytes[mb->n++] = (uint8_t)v; p += off;
        if (mb->n >= (int)sizeof mb->bytes) break;
    }
    return 1;
}

/* One case of tests.in (expected = 0) or tests.expected (expected = 1: event
   lines skipped). Returns 0 at the end of the file. */
static int read_case(FILE *f, test_t *t, int expected) {
    char line[512];
    do { if (!fgets(line, sizeof line, f)) return 0; }
    while (line[0] == '\n' || line[0] == '\r' || line[0] == '#');
    memset(t, 0, sizeof *t);
    sscanf(line, "%63s", t->name);
    do { if (!fgets(line, sizeof line, f)) return 0; }
    while (expected && (line[0] == ' ' || line[0] == '\t'));
    unsigned v[13];
    sscanf(line, "%x %x %x %x %x %x %x %x %x %x %x %x %x",
           &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7],&v[8],&v[9],&v[10],&v[11],&v[12]);
    t->af=v[0]; t->bc=v[1]; t->de=v[2]; t->hl=v[3]; t->af2=v[4]; t->bc2=v[5];
    t->de2=v[6]; t->hl2=v[7]; t->ix=v[8]; t->iy=v[9]; t->sp=v[10]; t->pc=v[11]; t->memptr=v[12];
    if (!fgets(line, sizeof line, f)) return 0;
    unsigned i, r, f1, f2, im, h;
    sscanf(line, "%x %x %u %u %u %u %d", &i, &r, &f1, &f2, &im, &h, &t->tstates);
    t->i=i; t->r=r; t->iff1=f1; t->iff2=f2; t->im=im; t->halted=h;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '\n' || line[0] == '\r') break;
        memblk_t mb;
        int k = parse_mem_line(line, &mb);
        if (k <= 0) break;
        if (t->nmem < 16) t->mem[t->nmem++] = mb;
    }
    return 1;
}

static test_t *load_all(const char *path, int expected, int *n) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(2); }
    int cap = 2048; test_t *v = malloc(cap * sizeof *v); *n = 0;
    while (*n < cap && read_case(f, &v[*n], expected)) (*n)++;
    fclose(f);
    return v;
}

static const test_t *find(const test_t *v, int n, const char *name) {
    for (int k = 0; k < n; k++) if (!strcmp(v[k].name, name)) return &v[k];
    return NULL;
}

/* ---- the preamble ---------------------------------------------------- */
static uint8_t used[65536];          /* bytes the case's memory occupies */

static int region_free(unsigned a, unsigned n) {
    for (unsigned k = 0; k < n; k++) if (used[(a + k) & 0xFFFF]) return 0;
    return 1;
}

/* Builds mem[] for case t; returns the preamble's start, or -1 (+ why). */
static int build(const test_t *t, uint8_t *mem, const char **why) {
    memset(mem, 0, 65536);
    memset(used, 0, sizeof used);
    for (int k = 0; k < t->nmem; k++)
        for (int j = 0; j < t->mem[k].n; j++) {
            mem[(t->mem[k].addr + j) & 0xFFFF] = t->mem[k].bytes[j];
            used[(t->mem[k].addr + j) & 0xFFFF] = 1;
        }
    /* keep away from the case's PC, SP and WZ too */
    for (int k = -8; k < 24; k++) used[(t->pc + k) & 0xFFFF] |= 2;
    for (int k = -8; k < 8; k++) used[(t->sp + k) & 0xFFFF] |= 2;
    for (int k = -2; k < 2; k++) used[(t->memptr + k) & 0xFFFF] |= 2;
    static const unsigned bases[] = { 0x8000, 0xA000, 0xC000, 0x6000, 0x3000, 0xE000, 0x1000, 0x5000 };
    int d = -1;
    for (unsigned k = 0; k < sizeof bases / sizeof *bases; k++)
        if (region_free(bases[k], 32)) { d = (int)bases[k]; break; }
    if (d < 0) { *why = "no free data area"; return -1; }
    /* stack image at d: I (as A), IX, IY, AF', BC', DE', HL', BC, DE, HL; AF at d+20 */
    uint16_t w[10] = { (uint16_t)(t->i << 8), t->ix, t->iy, t->af2, t->bc2, t->de2, t->hl2,
                       t->bc, t->de, t->hl };
    for (int k = 0; k < 10; k++) { mem[d + 2*k] = w[k] & 0xFF; mem[d + 2*k + 1] = w[k] >> 8; }
    mem[d + 20] = t->af & 0xFF; mem[d + 21] = t->af >> 8;
    uint8_t p[64]; int n = 0;
#define B(x) (p[n++] = (uint8_t)(x))
#define W(x) (B((x) & 0xFF), B(((x) >> 8) & 0xFF))
    B(0x31); W(d);                          /* LD SP,d */
    B(0xF1); B(0xED); B(0x47);              /* POP AF; LD I,A */
    B(0xDD); B(0xE1); B(0xFD); B(0xE1);     /* POP IX; POP IY */
    B(0xF1); B(0xC1); B(0xD1); B(0xE1);     /* POP AF; POP BC; POP DE; POP HL */
    B(0x08); B(0xD9);                       /* EX AF,AF'; EXX */
    B(0xC1); B(0xD1); B(0xE1);              /* POP BC; POP DE; POP HL */
    B(0xED); B(t->im == 0 ? 0x46 : t->im == 1 ? 0x56 : 0x5E);   /* IM n */
    B(t->iff2 ? 0xFB : 0xF3);               /* EI / DI: IFF2 (IFF1 is not read back) */
    /* R: four M1s follow LD R,A before the case's own */
    B(0x3E); B((t->r & 0x80) | ((t->r - 4) & 0x7F));             /* LD A,r */
    B(0xED); B(0x4F);                       /* LD R,A */
    B(0x3A); W((uint16_t)(t->memptr - 1));  /* LD A,(WZ-1): WZ = memptr */
    B(0x31); W(d + 20); B(0xF1);            /* LD SP,d+20; POP AF */
    B(0x31); W(t->sp);                      /* LD SP,sp */
#undef B
#undef W
    unsigned start = (t->pc - n) & 0xFFFF;
    for (int k = 0; k < n; k++) {
        if (used[(start + k) & 0xFFFF] & 1) { *why = "memory collides with the preamble"; return -1; }
        mem[(start + k) & 0xFFFF] = p[k];
    }
    for (int k = 0; k < 22; k++) if (used[d + k] & 1) { *why = "data area"; return -1; }
    return (int)start;
}

/* ---- the run --------------------------------------------------------- */
typedef struct {
    uint16_t af, bc, de, hl, af2, bc2, de2, hl2, ix, iy, sp, pc, wz;
    uint8_t i, r, halted;
    int tstates;
} die_t;

static uint8_t image[65536];

/* Returns 0, or a message. */
static const char *run_die(const test_t *in, int end_ts, die_t *out) {
    const char *why = NULL;
    int start = build(in, image, &why);
    if (start < 0) return why;
    uint8_t keep[3] = { image[0], image[1], image[2] };
    memcpy(cpu_memory, image, 65536);
    if (start > 2 || start == 0) {          /* reset vector: JP start */
        if (start != 0) { cpu_memory[0] = 0xC3; cpu_memory[1] = start & 0xFF; cpu_memory[2] = start >> 8; }
    } else return "preamble at the reset vector";
    void *st = cpu_initAndResetChip();
    cpu_setIntVec(0x00);
    for (unsigned a = 0; a < 65536; a++) cpu_io[a] = (uint8_t)(a >> 8);   /* FUSE: IN reads the high address byte */
    cpu_writeNMI(st, 1); cpu_writeINT(st, 1); cpu_writeWAIT(st, 1); cpu_writeBUSRQ(st, 1);
    int pre = (start == 0), began = -1, m1prev = 1, limit = 200000;
    const char *res = "no M1 after the case";
    for (int ph = 0; ph < limit; ph++) {
        int m1 = cpu_readM1(st) ? 1 : 0;
        if (!m1 && m1prev) {                /* an M1 starts */
            unsigned a = cpu_readAddressBus(st);
            if (!pre && a == (unsigned)start) {
                pre = 1;
                if (start != 0) memcpy(cpu_memory, keep, 3);
            } else if (pre && began < 0 && a == in->pc) {
                began = ph;
            } else if (began >= 0) {
                int t = (ph - began) / 2;
                if (t >= end_ts) {
                    out->tstates = t;
                    out->pc = (uint16_t)a;
                    for (int k = 0; k < 4; k++) cpu_step(st);        /* to T3.P: the refresh */
                    out->r = (uint8_t)cpu_readAddressBus(st);       /* shows I:R, before this M1's step */
                    for (int k = 4; k < 7; k++) cpu_step(st);        /* to T4.N */
                    out->af = (uint16_t)(cpu_readA(st) << 8 | cpu_readF(st));
                    out->bc = (uint16_t)(cpu_readB(st) << 8 | cpu_readC(st));
                    out->de = (uint16_t)(cpu_readD(st) << 8 | cpu_readE(st));
                    out->hl = (uint16_t)(cpu_readH(st) << 8 | cpu_readL(st));
                    out->af2 = (uint16_t)(cpu_readA2(st) << 8 | cpu_readF2(st));
                    out->bc2 = (uint16_t)(cpu_readB2(st) << 8 | cpu_readC2(st));
                    out->de2 = (uint16_t)(cpu_readD2(st) << 8 | cpu_readE2(st));
                    out->hl2 = (uint16_t)(cpu_readH2(st) << 8 | cpu_readL2(st));
                    out->ix = cpu_readIX(st); out->iy = cpu_readIY(st); out->sp = cpu_readSP(st);
                    out->wz = (uint16_t)(cpu_readW(st) << 8 | cpu_readZ(st));
                    out->i = cpu_readI(st);
                    out->halted = cpu_readHALT(st) ? 0 : 1;
                    res = NULL;
                    break;
                }
            }
        }
        m1prev = m1;
        cpu_step(st);
    }
    cpu_destroyChip(st);
    return res;
}

int main(int argc, char **argv) {
    const char *only = NULL; int verbose = 0;
    const char *pos[3] = { 0 }; int np = 0;
    for (int k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "-v")) verbose = 1;
        else if (!strcmp(argv[k], "--only") && k + 1 < argc) only = argv[++k];
        else if (np < 3) pos[np++] = argv[k];
    }
    if (np < 2) {
        fprintf(stderr, "usage: %s <tests.in> <tests.expected> [silicon.expected] [-v] [--only NAME,...]\n", argv[0]);
        return 2;
    }
    int nin, nexp, nfix = 0;
    test_t *tin = load_all(pos[0], 0, &nin), *texp = load_all(pos[1], 1, &nexp), *tfix = NULL;
    if (pos[2]) tfix = load_all(pos[2], 1, &nfix);
    int ok = 0, bad = 0, skipped = 0, fixed_ok = 0;
    for (int k = 0; k < nin; k++) {
        const test_t *in = &tin[k];
        if (only) {
            char key[70]; snprintf(key, sizeof key, ",%s,", in->name);
            char list[4096]; snprintf(list, sizeof list, ",%s,", only);
            if (!strstr(list, key)) continue;
        }
        const test_t *fx = tfix ? find(tfix, nfix, in->name) : NULL;
        const test_t *ex = fx ? fx : find(texp, nexp, in->name);
        if (!ex) { printf("%-10s no expected values\n", in->name); bad++; continue; }
        die_t d;
        const char *why = run_die(in, ex->tstates, &d);
        if (why) { printf("%-10s not run on the die: %s\n", in->name, why); skipped++; continue; }
        char diff[1024] = "";
#define CK(nm, e, g, fmt) if ((unsigned)(e) != (unsigned)(g)) { int L = (int)strlen(diff); \
            snprintf(diff + L, sizeof diff - L, " " nm "=" fmt "/" fmt, (unsigned)(e), (unsigned)(g)); }
        CK("AF", ex->af, d.af, "%04x"); CK("BC", ex->bc, d.bc, "%04x");
        CK("DE", ex->de, d.de, "%04x"); CK("HL", ex->hl, d.hl, "%04x");
        CK("AF'", ex->af2, d.af2, "%04x"); CK("BC'", ex->bc2, d.bc2, "%04x");
        CK("DE'", ex->de2, d.de2, "%04x"); CK("HL'", ex->hl2, d.hl2, "%04x");
        CK("IX", ex->ix, d.ix, "%04x"); CK("IY", ex->iy, d.iy, "%04x");
        CK("SP", ex->sp, d.sp, "%04x"); CK("PC", ex->pc, d.pc, "%04x");
        CK("WZ", ex->memptr, d.wz, "%04x"); CK("I", ex->i, d.i, "%02x");
        CK("R", ex->r, d.r, "%02x"); CK("HALTED", ex->halted, d.halted, "%u");
        CK("TS", ex->tstates, d.tstates, "%u");
        for (int b = 0; b < ex->nmem; b++)
            for (int j = 0; j < ex->mem[b].n; j++) {
                uint16_t a = (uint16_t)(ex->mem[b].addr + j);
                CK("M", ex->mem[b].bytes[j], cpu_memory[a], "%02x");
            }
#undef CK
        if (diff[0]) { printf("%-10s DIFFERS%s (expected/die)%s\n", in->name, fx ? " from silicon.expected" : "", diff); bad++; }
        else { ok++; if (fx) fixed_ok++; }
        if (verbose) {
            printf("%s\n%04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x\n"
                   "%02x %02x %u %u %u %u %d\n",
                   in->name, d.af, d.bc, d.de, d.hl, d.af2, d.bc2, d.de2, d.hl2,
                   d.ix, d.iy, d.sp, d.pc, d.wz, d.i, d.r, ex->iff1, ex->iff2, ex->im, d.halted, d.tstates);
            for (int b = 0; b < ex->nmem; b++) {
                printf("%04x", ex->mem[b].addr);
                for (int j = 0; j < ex->mem[b].n; j++) printf(" %02x", cpu_memory[(ex->mem[b].addr + j) & 0xFFFF]);
                printf(" -1\n");
            }
            printf("\n");
        }
    }
    printf("=== FUSE on the die (perfectz80) ===\n%d cases: %d match, %d differ, %d not run (the preamble does not fit)\n",
           ok + bad + skipped, ok, bad, skipped);
    if (fixed_ok) printf("(%d of the matches against tests/fuse/silicon.expected)\n", fixed_ok);
    return bad ? 1 : 0;
}
