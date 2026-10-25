/* xy_oracles.cpp - one FUSE-format case file through our C model and the four
   oracle emulators of scripts/refs (floooh/chips, superzazu, suzukiplan,
   redcode/Z80 as the Zilog NMOS model), each from the case's state for the
   case's T-states (whole instructions until at least that many have run, as
   FUSE's tests.in means it), and one line per case and emulator with what
   it ends with:

     <case> <emu> AF BC DE HL AF' BC' DE' HL' IX IY SP PC WZ I R TS IFF1 IFF2 IM HALTED

   emu: rz80 (ours), chips, superzazu, suzukiplan, redcode. Every emulator
   starts with no flags written by the previous instruction (Q = 0) where it
   has the notion (ours, redcode); IN reads the port address's high byte, as
   FUSE's corpus and fuse_pz80 have it. scripts/xy_study.py generates the
   cases, compares the lines and replays the cases on the die (fuse_pz80).

     xy_oracles <tests.in> [--only rz80] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "z80_sim.h"
}

#define z80_t       cz80_t
#define z80_init    cz80_init_
#define z80_reset   cz80_reset_
#define z80_tick    cz80_tick_
#define z80_prefetch cz80_prefetch_
#define z80_opdone  cz80_opdone_
#define CHIPS_IMPL
extern "C" {
#include "refs/chips_z80.h"
}
#undef z80_t
#undef z80_init
#undef z80_reset
#undef z80_tick
#undef z80_prefetch
#undef z80_opdone

#define z80               sz80
#define z80_init          sz80_init
#define z80_step          sz80_step
#define z80_gen_nmi       sz80_gen_nmi
#define z80_gen_int       sz80_gen_int
#define z80_debug_output  sz80_dbg
extern "C" {
#include "refs/superzazu_z80.h"
#include "refs/superzazu_z80.c"
}
#undef z80
#undef z80_init
#undef z80_step
#undef z80_gen_nmi
#undef z80_gen_int
#undef z80_debug_output

#include "refs/suzukiplan_z80.hpp"

#define Z80 Z80_rc
#ifdef Z80_H
#  define XY_SAVED_Z80_H 1
#  undef Z80_H
#endif
extern "C" {
#include "refs/redcode_z80/Z80.h"
}
#ifdef XY_SAVED_Z80_H
#  ifndef Z80_H
#    define Z80_H
#  endif
#endif
#undef Z80

typedef struct { uint16_t addr; int n; uint8_t bytes[256]; } memblk_t;
typedef struct {
    char name[64];
    uint16_t af, bc, de, hl, af2, bc2, de2, hl2, ix, iy, sp, pc, wz;
    uint8_t i, r, iff1, iff2, im, halted;
    int ts;
    memblk_t mem[16]; int nmem;
} case_t;
typedef struct {
    uint16_t af, bc, de, hl, af2, bc2, de2, hl2, ix, iy, sp, pc, wz;
    uint8_t i, r; long ts;
    uint8_t iff1, iff2, im, halted;
} out_t;

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

static int read_case(FILE *f, case_t *t) {
    char line[512];
    do { if (!fgets(line, sizeof line, f)) return 0; }
    while (line[0] == '\n' || line[0] == '\r' || line[0] == '#');
    memset(t, 0, sizeof *t);
    sscanf(line, "%63s", t->name);
    if (!fgets(line, sizeof line, f)) return 0;
    unsigned v[13];
    sscanf(line, "%x %x %x %x %x %x %x %x %x %x %x %x %x",
           &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7],&v[8],&v[9],&v[10],&v[11],&v[12]);
    t->af=v[0]; t->bc=v[1]; t->de=v[2]; t->hl=v[3]; t->af2=v[4]; t->bc2=v[5];
    t->de2=v[6]; t->hl2=v[7]; t->ix=v[8]; t->iy=v[9]; t->sp=v[10]; t->pc=v[11]; t->wz=v[12];
    if (!fgets(line, sizeof line, f)) return 0;
    unsigned i, r, f1, f2, im, h;
    sscanf(line, "%x %x %u %u %u %u %d", &i, &r, &f1, &f2, &im, &h, &t->ts);
    t->i=i; t->r=r; t->iff1=f1; t->iff2=f2; t->im=im; t->halted=h;
    while (fgets(line, sizeof line, f)) {
        memblk_t mb;
        int k = parse_mem_line(line, &mb);
        if (k <= 0) break;
        if (t->nmem < 16) t->mem[t->nmem++] = mb;
    }
    return 1;
}

static uint8_t MEM[65536];
static void load_mem(const case_t *t) {
    memset(MEM, 0, sizeof MEM);
    for (int k = 0; k < t->nmem; k++)
        for (int j = 0; j < t->mem[k].n; j++) MEM[(t->mem[k].addr + j) & 0xFFFF] = t->mem[k].bytes[j];
}

/* ---- ours ---------------------------------------------------------------- */
static z80_system_t *S;
static out_t run_rz80(const case_t *t) {
    z80_sys_init(S);
    memcpy(S->mem, MEM, 65536);
    for (int a = 0; a < 65536; a++) S->io[a] = (uint8_t)(a >> 8);
    S->cpu.rf[RFP_AF]=t->af; S->cpu.rf[RFP_BC]=t->bc; S->cpu.rf[RFP_DE]=t->de; S->cpu.rf[RFP_HL]=t->hl;
    S->cpu.rf[RFP_AF2]=t->af2; S->cpu.rf[RFP_BC2]=t->bc2; S->cpu.rf[RFP_DE2]=t->de2; S->cpu.rf[RFP_HL2]=t->hl2;
    S->cpu.rf[RFP_IX]=t->ix; S->cpu.rf[RFP_IY]=t->iy; S->cpu.rf[RFP_SP]=t->sp; S->cpu.rf[RFP_WZ]=t->wz;
    z80_set_pc(&S->cpu, t->pc);
    S->cpu.reg_i=t->i; S->cpu.reg_r=t->r; S->cpu.iff1=t->iff1; S->cpu.iff2=t->iff2;
    S->cpu.im=t->im; S->cpu.halted=t->halted; S->cpu.q = 0; S->cpu.cycle = 0;
    uint64_t target = (uint64_t)t->ts * 2;
    for (int g = 0; S->cpu.cycle < target && g < 200; g++) z80_sys_step_instr(S);
    const z80_t *a = z80_arch(&S->cpu);
    out_t o = { a->rf[RFP_AF], a->rf[RFP_BC], a->rf[RFP_DE], a->rf[RFP_HL], a->rf[RFP_AF2], a->rf[RFP_BC2],
                a->rf[RFP_DE2], a->rf[RFP_HL2], a->rf[RFP_IX], a->rf[RFP_IY], a->rf[RFP_SP], a->rf[RFP_PC],
                a->rf[RFP_WZ], a->reg_i, a->reg_r, (long)(S->cpu.cycle / 2),
                a->iff1, a->iff2, a->im, a->halted };
    return o;
}

/* ---- floooh/chips --------------------------------------------------------- */
static uint8_t CM[65536];
static uint64_t cz_tick(cz80_t *c, uint64_t pins) {
    pins = cz80_tick_(c, pins);
    uint16_t a = Z80_GET_ADDR(pins);
    if (pins & Z80_MREQ) {
        if (pins & Z80_RD) pins = (pins & ~0xFF0000ULL) | ((uint64_t)CM[a] << 16);
        else if (pins & Z80_WR) CM[a] = (uint8_t)((pins >> 16) & 0xFF);
    } else if ((pins & Z80_IORQ) && (pins & Z80_RD)) {
        pins = (pins & ~0xFF0000ULL) | ((uint64_t)(a >> 8) << 16);
    }
    return pins;
}
static out_t run_chips(const case_t *t) {
    memcpy(CM, MEM, 65536);
    cz80_t C; uint64_t pins = cz80_init_(&C);
    C.af=t->af; C.bc=t->bc; C.de=t->de; C.hl=t->hl; C.af2=t->af2; C.bc2=t->bc2; C.de2=t->de2; C.hl2=t->hl2;
    C.ix=t->ix; C.iy=t->iy; C.sp=t->sp; C.wz=t->wz; C.i=t->i; C.r=t->r; C.im=t->im;
    C.iff1=t->iff1; C.iff2=t->iff2;
    pins = cz80_prefetch_(&C, t->pc);
    long ts = -1;                         /* the tick after the prefetch is not a T-state of the case */
    for (int g = 0; ts < t->ts && g < 200; g++) {
        pins = cz_tick(&C, pins); ts++;
        while (cz80_opdone_(&C)) { pins = cz_tick(&C, pins); ts++; }
        while (!cz80_opdone_(&C)) { pins = cz_tick(&C, pins); ts++; }
    }
    out_t o = { C.af, C.bc, C.de, C.hl, C.af2, C.bc2, C.de2, C.hl2, C.ix, C.iy, C.sp,
                (uint16_t)(C.pc - 1), C.wz, C.i, C.r, ts, C.iff1, C.iff2, C.im, 0 };
    return o;
}

/* ---- superzazu ------------------------------------------------------------ */
static uint8_t ZM[65536];
static uint8_t sz_rd(void *u, uint16_t a) { (void)u; return ZM[a]; }
static void    sz_wr(void *u, uint16_t a, uint8_t v) { (void)u; ZM[a] = v; }
/* superzazu passes the port's low byte only: the high byte is B for the ED
   group (IN r,(C), INI ...: B before INI's decrement, as it calls) and A for
   IN A,(n). */
static uint8_t sz_in(sz80 *z, uint8_t p) { (void)p; return ZM[(uint16_t)(z->pc - 2)] == 0xED ? z->b : z->a; }
static void    sz_out(sz80 *z, uint8_t p, uint8_t v) { (void)z; (void)p; (void)v; }
static out_t run_superzazu(const case_t *t) {
    memcpy(ZM, MEM, 65536);
    sz80 z; sz80_init(&z);
    z.read_byte = sz_rd; z.write_byte = sz_wr; z.port_in = sz_in; z.port_out = sz_out;
    z.a = t->af >> 8; uint8_t f = t->af & 0xFF;
    z.sf = f >> 7 & 1; z.zf = f >> 6 & 1; z.yf = f >> 5 & 1; z.hf = f >> 4 & 1;
    z.xf = f >> 3 & 1; z.pf = f >> 2 & 1; z.nf = f >> 1 & 1; z.cf = f & 1;
    z.b = t->bc >> 8; z.c = t->bc & 0xFF; z.d = t->de >> 8; z.e = t->de & 0xFF;
    z.h = t->hl >> 8; z.l = t->hl & 0xFF;
    z.a_ = t->af2 >> 8; z.f_ = t->af2 & 0xFF; z.b_ = t->bc2 >> 8; z.c_ = t->bc2 & 0xFF;
    z.d_ = t->de2 >> 8; z.e_ = t->de2 & 0xFF; z.h_ = t->hl2 >> 8; z.l_ = t->hl2 & 0xFF;
    z.ix = t->ix; z.iy = t->iy; z.sp = t->sp; z.pc = t->pc; z.mem_ptr = t->wz;
    z.i = t->i; z.r = t->r; z.interrupt_mode = t->im; z.iff1 = t->iff1; z.iff2 = t->iff2;
    z.halted = t->halted; z.iff_delay = 0; z.cyc = 0;
    for (int g = 0; (long)z.cyc < t->ts && g < 200; g++) sz80_step(&z);
    uint8_t ff = (uint8_t)(z.sf << 7 | z.zf << 6 | z.yf << 5 | z.hf << 4 | z.xf << 3 | z.pf << 2 | z.nf << 1 | z.cf);
    out_t o = { (uint16_t)(z.a << 8 | ff), (uint16_t)(z.b << 8 | z.c), (uint16_t)(z.d << 8 | z.e),
                (uint16_t)(z.h << 8 | z.l), (uint16_t)(z.a_ << 8 | z.f_), (uint16_t)(z.b_ << 8 | z.c_),
                (uint16_t)(z.d_ << 8 | z.e_), (uint16_t)(z.h_ << 8 | z.l_), z.ix, z.iy, z.sp, z.pc,
                z.mem_ptr, z.i, z.r, (long)z.cyc, z.iff1, z.iff2, z.interrupt_mode, z.halted };
    return o;
}

/* ---- suzukiplan ----------------------------------------------------------- */
static uint8_t PM[65536];
static out_t run_suzukiplan(const case_t *t) {
    memcpy(PM, MEM, 65536);
    Z80 P([](void *, unsigned short a) -> unsigned char { return PM[a]; },
          [](void *, unsigned short a, unsigned char v) { PM[a] = v; },
          [](void *, unsigned short p) -> unsigned char { return (unsigned char)(p >> 8); },
          [](void *, unsigned short, unsigned char) {},
          nullptr, true);
    P.reg.pair.A = t->af >> 8; P.reg.pair.F = t->af & 0xFF; P.reg.pair.B = t->bc >> 8; P.reg.pair.C = t->bc & 0xFF;
    P.reg.pair.D = t->de >> 8; P.reg.pair.E = t->de & 0xFF; P.reg.pair.H = t->hl >> 8; P.reg.pair.L = t->hl & 0xFF;
    P.reg.back.A = t->af2 >> 8; P.reg.back.F = t->af2 & 0xFF; P.reg.back.B = t->bc2 >> 8; P.reg.back.C = t->bc2 & 0xFF;
    P.reg.back.D = t->de2 >> 8; P.reg.back.E = t->de2 & 0xFF; P.reg.back.H = t->hl2 >> 8; P.reg.back.L = t->hl2 & 0xFF;
    P.reg.IX = t->ix; P.reg.IY = t->iy; P.reg.SP = t->sp; P.reg.PC = t->pc; P.reg.WZ = t->wz;
    P.reg.I = t->i; P.reg.R = t->r;
    P.reg.IFF = 0;
    if (t->iff1) P.reg.IFF |= 1;          /* IFF_1 */
    if (t->iff2) P.reg.IFF |= 4;          /* IFF_2 */
    P.reg.interrupt = (unsigned char)(t->im & 3);
    long ts = 0;
    try {
        for (int g = 0; ts < t->ts && g < 200; g++) ts += P.execute(1);
    } catch (const std::exception &) {
        ts = -1;                          /* suzukiplan refuses an opcode (DD 00 ...) */
    }
    out_t o = { (uint16_t)(P.reg.pair.A << 8 | P.reg.pair.F), (uint16_t)(P.reg.pair.B << 8 | P.reg.pair.C),
                (uint16_t)(P.reg.pair.D << 8 | P.reg.pair.E), (uint16_t)(P.reg.pair.H << 8 | P.reg.pair.L),
                (uint16_t)(P.reg.back.A << 8 | P.reg.back.F), (uint16_t)(P.reg.back.B << 8 | P.reg.back.C),
                (uint16_t)(P.reg.back.D << 8 | P.reg.back.E), (uint16_t)(P.reg.back.H << 8 | P.reg.back.L),
                P.reg.IX, P.reg.IY, P.reg.SP, P.reg.PC, P.reg.WZ, P.reg.I, P.reg.R, ts,
                (uint8_t)(P.reg.IFF & 1), (uint8_t)(P.reg.IFF >> 2 & 1), (uint8_t)(P.reg.interrupt & 3),
                (uint8_t)(P.reg.IFF >> 7 & 1) };
    return o;
}

/* ---- redcode/Z80 ------------------------------------------------------------ */
static uint8_t RM[65536];
static zuint8 rc_rd(void *, zuint16 a) { return RM[a]; }
static void   rc_wr(void *, zuint16 a, zuint8 v) { RM[a] = v; }
static zuint8 rc_in(void *, zuint16 a) { return (zuint8)(a >> 8); }
static void   rc_out(void *, zuint16, zuint8) {}
static out_t run_redcode(const case_t *t, long target) {
    memcpy(RM, MEM, 65536);
    Z80_rc cpu; memset(&cpu, 0, sizeof cpu);
    cpu.context = &cpu;
    cpu.fetch_opcode = rc_rd; cpu.fetch = rc_rd; cpu.read = rc_rd; cpu.write = rc_wr;
    cpu.in = rc_in; cpu.out = rc_out;
    cpu.options = Z80_MODEL_ZILOG_NMOS;
    z80_power(&cpu, 1);
    z80_instant_reset(&cpu);
    Z80_AF(cpu) = t->af; Z80_BC(cpu) = t->bc; Z80_DE(cpu) = t->de; Z80_HL(cpu) = t->hl;
    Z80_AF_(cpu) = t->af2; Z80_BC_(cpu) = t->bc2; Z80_DE_(cpu) = t->de2; Z80_HL_(cpu) = t->hl2;
    Z80_IX(cpu) = t->ix; Z80_IY(cpu) = t->iy; Z80_SP(cpu) = t->sp; Z80_PC(cpu) = t->pc;
    Z80_WZ(cpu) = t->wz; cpu.i = t->i; cpu.r = t->r; cpu.r7 = t->r;
    cpu.iff1 = t->iff1; cpu.iff2 = t->iff2; cpu.im = t->im; cpu.halt_line = t->halted; cpu.q = 0;
    cpu.cycles = 0;
    /* z80_execute stops between a prefix and its opcode once the budget is
       spent, so it runs for the T-states our model took: whole instructions */
    z80_execute(&cpu, (zusize)(target < 1 ? 1 : target));
    out_t o = { Z80_AF(cpu), Z80_BC(cpu), Z80_DE(cpu), Z80_HL(cpu), Z80_AF_(cpu), Z80_BC_(cpu),
                Z80_DE_(cpu), Z80_HL_(cpu), Z80_IX(cpu), Z80_IY(cpu), Z80_SP(cpu), Z80_PC(cpu),
                Z80_WZ(cpu), cpu.i, (uint8_t)((cpu.r & 0x7F) | (cpu.r7 & 0x80)), (long)cpu.cycles,
                cpu.iff1, cpu.iff2, cpu.im, (uint8_t)(cpu.halt_line ? 1 : 0) };
    return o;
}

static void print(const char *name, const char *emu, const out_t &o) {
    printf("%s %s %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %02x %02x %ld %u %u %u %u\n",
           name, emu, o.af, o.bc, o.de, o.hl, o.af2, o.bc2, o.de2, o.hl2, o.ix, o.iy, o.sp, o.pc, o.wz,
           o.i, o.r, o.ts, o.iff1, o.iff2, o.im, o.halted);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <tests.in> [--only rz80]\n", argv[0]); return 2; }
    int only_ours = argc > 3 && !strcmp(argv[2], "--only") && !strcmp(argv[3], "rz80");
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 2; }
    S = (z80_system_t *)malloc(sizeof *S);
    static case_t c;
    while (read_case(f, &c)) {
        load_mem(&c);
        out_t ours = run_rz80(&c);
        print(c.name, "rz80", ours);
        if (only_ours) continue;
        print(c.name, "chips", run_chips(&c));
        print(c.name, "superzazu", run_superzazu(&c));
        print(c.name, "suzukiplan", run_suzukiplan(&c));
        print(c.name, "redcode", run_redcode(&c, ours.ts));
    }
    fclose(f);
    return 0;
}
