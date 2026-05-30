/* lockstep_triple.c - triangulation: run our C model, the superzazu C Z80
   reference, and the chips/z80.h header-only reference in instruction-level
   lockstep on a .com image. Compares the public regs after every instruction;
   stops at the first divergence with the offending program counter + opcode. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>

/* ---- chips/z80 (rename to cz*) -------------------------------------------*/
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

/* ---- superzazu (rename to sz*) -------------------------------------------*/
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

/* ---- our model -----------------------------------------------------------*/
extern "C" {
#include "z80_sim.h"
}

/* memories */
static uint8_t  MMEM[65536];    /* mine */
static uint8_t  SMEM[65536];    /* superzazu */
static uint8_t  CMEM[65536];    /* chips */

/* superzazu memory callbacks */
static uint8_t sz_rd(void*u,uint16_t a){(void)u;return SMEM[a];}
static void    sz_wr(void*u,uint16_t a,uint8_t v){(void)u;SMEM[a]=v;}
static uint8_t sz_pin(sz80*z,uint8_t p){(void)z;(void)p;return 0;}
static void    sz_pout(sz80*z,uint8_t p,uint8_t v){(void)z;(void)p;(void)v;}
static uint8_t sz_reff(sz80*z){return (z->sf<<7)|(z->zf<<6)|(z->yf<<5)|(z->hf<<4)|(z->xf<<3)|(z->pf<<2)|(z->nf<<1)|z->cf;}

/* Run chips/z80 for one full instruction. chips's opdone semantics: true at
   the M1/RD tick of the NEXT instruction (overlap). So we tick once first
   (leaving the opdone state), then tick until opdone is true again. */
static inline uint64_t cz_one_tick(cz80_t* c, uint64_t pins) {
    pins = cz80_tick_(c, pins);
    uint16_t a = Z80_GET_ADDR(pins);
    if (pins & Z80_MREQ) {
        if (pins & Z80_RD)
            pins = (pins & ~0xFF0000ULL) | ((uint64_t)CMEM[a] << 16);
        else if (pins & Z80_WR)
            CMEM[a] = (uint8_t)((pins >> 16) & 0xFF);
    } else if ((pins & Z80_IORQ) && (pins & Z80_RD)) {
        pins = pins & ~0xFF0000ULL;
    }
    return pins;
}
static uint64_t cz_step_one(cz80_t* c, uint64_t pins) {
    /* tick until opdone transitions false->true (one instruction completed) */
    /* skip past any leading ticks where opdone is true (the prior M1/RD) */
    pins = cz_one_tick(c, pins);
    while (cz80_opdone_(c)) pins = cz_one_tick(c, pins);
    while (!cz80_opdone_(c)) pins = cz_one_tick(c, pins);
    return pins;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr,"usage: %s prog.com [max_instr]\n", argv[0]); return 2; }
    long long cap = (argc>2) ? atoll(argv[2]) : 20000000LL;
    FILE* f = fopen(argv[1],"rb"); if (!f){perror(argv[1]);return 2;}
    static uint8_t img[65536]; size_t sz = fread(&img[0x100],1,0x10000-0x100,f); fclose(f);
    (void)sz;

    /* prep each memory */
    memcpy(MMEM, img, 65536); MMEM[0]=0xC3;MMEM[1]=0;MMEM[2]=0;MMEM[5]=0xC9;
    memcpy(SMEM, img, 65536); SMEM[0]=0xC3;SMEM[1]=0;SMEM[2]=0;SMEM[5]=0xC9;
    memcpy(CMEM, img, 65536); CMEM[0]=0xC3;CMEM[1]=0;CMEM[2]=0;CMEM[5]=0xC9;

    /* mine */
    z80_system_t* M = (z80_system_t*)malloc(sizeof *M);
    z80_sys_init(M);
    memcpy(M->mem, MMEM, 65536);
    z80_set_pc(&M->cpu, 0x100); M->cpu.rf[RFP_SP] = 0xFFFE;

    /* superzazu */
    sz80 S; sz80_init(&S);
    S.read_byte=sz_rd; S.write_byte=sz_wr; S.port_in=sz_pin; S.port_out=sz_pout;
    S.a=0xff; S.sf=S.zf=S.yf=S.hf=S.xf=S.pf=S.nf=S.cf=1;
    S.b=S.c=S.d=S.e=S.h=S.l=0xff; S.ix=S.iy=0xffff;
    S.a_=S.b_=S.c_=S.d_=S.e_=S.h_=S.l_=S.f_=0xff;
    S.pc=0x100; S.sp=0xFFFE;

    /* chips */
    cz80_t C; uint64_t cpins = cz80_init_(&C);
    /* chips reset registers to known undefined; match SP and prefetch PC */
    C.af = 0xFFFF; C.bc = 0xFFFF; C.de = 0xFFFF; C.hl = 0xFFFF;
    C.ix = 0xFFFF; C.iy = 0xFFFF; C.wz = 0xFFFF;
    C.af2 = 0xFFFF; C.bc2 = 0xFFFF; C.de2 = 0xFFFF; C.hl2 = 0xFFFF;
    C.sp = 0xFFFE;
    cpins = cz80_prefetch_(&C, 0x100);

    long long n = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (; n < cap; n++) {
        /* harvest pre-state for the culprit print */
        uint16_t mpc = M->cpu.rf[RFP_PC];
        uint8_t  op0 = MMEM[mpc], op1 = MMEM[(mpc+1)&0xFFFF], op2 = MMEM[(mpc+2)&0xFFFF], op3 = MMEM[(mpc+3)&0xFFFF];
        if (mpc == 0) break;
        /* step each */
        z80_sys_step_instr(M);
        sz80_step(&S);
        cpins = cz_step_one(&C, cpins);

        /* compare main regs (PC, AF, BC, DE, HL, IX, IY, SP) across all three */
        uint16_t maf = M->cpu.rf[RFP_AF], mbc = M->cpu.rf[RFP_BC], mde = M->cpu.rf[RFP_DE];
        uint16_t mhl = M->cpu.rf[RFP_HL], mix = M->cpu.rf[RFP_IX], miy = M->cpu.rf[RFP_IY];
        uint16_t msp = M->cpu.rf[RFP_SP], mpc2 = M->cpu.rf[RFP_PC];
        uint16_t saf = (S.a<<8) | sz_reff(&S), sbc=(S.b<<8)|S.c, sde=(S.d<<8)|S.e;
        uint16_t shl = (S.h<<8)|S.l, six = S.ix, siy = S.iy, ssp = S.sp, spc = S.pc;
        /* chips's PC is one past the logical end-of-instruction PC (overlap M1) */
        uint16_t cpc = (uint16_t)(C.pc - 1);
        bool diverge = (mpc2 != spc || mpc2 != cpc || maf != saf || maf != C.af ||
                        mbc != sbc || mbc != C.bc || mde != sde || mde != C.de ||
                        mhl != shl || mhl != C.hl || mix != six || mix != C.ix ||
                        miy != siy || miy != C.iy || msp != ssp || msp != C.sp);
        if (diverge) {
            printf("DIVERGENCE at step %lld after @%04x: %02x %02x %02x %02x\n",
                n, mpc, op0,op1,op2,op3);
            printf("  MINE:  pc%04x af%04x bc%04x de%04x hl%04x ix%04x iy%04x sp%04x\n",
                mpc2,maf,mbc,mde,mhl,mix,miy,msp);
            printf("  SUPZ:  pc%04x af%04x bc%04x de%04x hl%04x ix%04x iy%04x sp%04x\n",
                spc,saf,sbc,sde,shl,six,siy,ssp);
            printf("  CHIPS: pc%04x af%04x bc%04x de%04x hl%04x ix%04x iy%04x sp%04x\n",
                cpc,C.af,C.bc,C.de,C.hl,C.ix,C.iy,C.sp);
            return 1;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    printf("triple-lockstep clean: %lld instructions in %.2f s (%.2f Minstr/s, three emulators)\n",
        n, secs, n/secs/1e6);
    free(M);
    return 0;
}
