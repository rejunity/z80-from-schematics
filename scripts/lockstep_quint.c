/* lockstep_quint.c — extends lockstep_quad with a 5th oracle:
 * redcode/Z80 (Manuel Sainz de Baranda, github.com/redcode/Z80). redcode
 * advertises explicit MEMPTR + Q-factor + special-RESET support — a
 * good triangulation partner for the WZ-during-INIR/INDR question.
 *
 * Same workload as lockstep_quad: a CP/M .com loaded at 0x100, stepped
 * one instruction at a time across all five emulators, compared
 * register-by-register, abort on first divergence.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>

/* ---- our model FIRST -----------------------------------------------------
 * Order matters: redcode/Z80.h uses `#ifndef Z80_H` as its include guard,
 * the exact same name our cmodel/z80.h uses. If redcode is processed first
 * it claims the guard and our header's body gets skipped, leaving z80_t
 * undefined. Process ours first; its own guard then locks redcode out of
 * shadowing it. */
extern "C" {
#include "z80_sim.h"
}

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

/* ---- suzukiplan/z80 (single-header C++) ----------------------------------*/
#include "refs/suzukiplan_z80.hpp"   /* declares class Z80 */

/* ---- redcode/Z80 (rename ONLY the Z80 struct name; suzukiplan's `class
 *      Z80` would otherwise clash. Function names like `z80_power`,
 *      `z80_execute` etc. don't clash — chips/superzazu were already
 *      renamed in their blocks above. We leave them as-is so that
 *      redcode's prebuilt Z80.o object (compiled separately as C)
 *      exports the symbols we'll call. ----------------------------- */
#define Z80           Z80_rc
/* Sidestep guard collision: our cmodel/z80.h already defined Z80_H above,
 * which would cause redcode's `#ifndef Z80_H` to skip its body. Undef
 * around the include and restore afterwards. */
#ifdef Z80_H
#  define LOCKSTEP_QUINT_SAVED_Z80_H 1
#  undef Z80_H
#endif
extern "C" {
#include "refs/redcode_z80/Z80.h"
}
#ifdef LOCKSTEP_QUINT_SAVED_Z80_H
#  undef LOCKSTEP_QUINT_SAVED_Z80_H
#  ifndef Z80_H
#    define Z80_H
#  endif
#endif
#undef Z80

/* memories per emulator */
static uint8_t  MMEM[65536];
static uint8_t  SMEM[65536];
static uint8_t  CMEM[65536];
static uint8_t  PMEM[65536];
static uint8_t  RMEM[65536];    /* redcode */

/* superzazu memory callbacks */
static uint8_t sz_rd(void*u,uint16_t a){(void)u;return SMEM[a];}
static void    sz_wr(void*u,uint16_t a,uint8_t v){(void)u;SMEM[a]=v;}
static uint8_t sz_pin(sz80*z,uint8_t p){(void)z;(void)p;return 0;}
static void    sz_pout(sz80*z,uint8_t p,uint8_t v){(void)z;(void)p;(void)v;}
static uint8_t sz_reff(sz80*z){return (z->sf<<7)|(z->zf<<6)|(z->yf<<5)|(z->hf<<4)|(z->xf<<3)|(z->pf<<2)|(z->nf<<1)|z->cf;}

/* redcode callbacks (Z80Read signature: zuint8 (*)(void*, zuint16)). */
static unsigned char rc_rd  (void*, unsigned short a)             { return RMEM[a]; }
static void          rc_wr  (void*, unsigned short a, unsigned char v) { RMEM[a] = v; }
static unsigned char rc_in  (void*, unsigned short)               { return 0; }
static void          rc_out (void*, unsigned short, unsigned char){}

/* Run chips/z80 for one full instruction. */
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

    /* prep each memory: CP/M warm-boot vector at 0x0000, BDOS return at 0x0005 */
    memcpy(MMEM, img, 65536); MMEM[0]=0xC3;MMEM[1]=0;MMEM[2]=0;MMEM[5]=0xC9;
    memcpy(SMEM, img, 65536); SMEM[0]=0xC3;SMEM[1]=0;SMEM[2]=0;SMEM[5]=0xC9;
    memcpy(CMEM, img, 65536); CMEM[0]=0xC3;CMEM[1]=0;CMEM[2]=0;CMEM[5]=0xC9;
    memcpy(PMEM, img, 65536); PMEM[0]=0xC3;PMEM[1]=0;PMEM[2]=0;PMEM[5]=0xC9;
    memcpy(RMEM, img, 65536); RMEM[0]=0xC3;RMEM[1]=0;RMEM[2]=0;RMEM[5]=0xC9;

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
    C.af = 0xFFFF; C.bc = 0xFFFF; C.de = 0xFFFF; C.hl = 0xFFFF;
    C.ix = 0xFFFF; C.iy = 0xFFFF; C.wz = 0xFFFF;
    C.af2 = 0xFFFF; C.bc2 = 0xFFFF; C.de2 = 0xFFFF; C.hl2 = 0xFFFF;
    C.sp = 0xFFFE;
    cpins = cz80_prefetch_(&C, 0x100);

    /* suzukiplan/z80 */
    Z80 P(
        [](void*, unsigned short a) -> unsigned char { return PMEM[a]; },
        [](void*, unsigned short a, unsigned char v) { PMEM[a] = v; },
        [](void*, unsigned char) -> unsigned char { return 0; },
        [](void*, unsigned char, unsigned char) {},
        nullptr);
    P.reg.pair.A = 0xFF; P.reg.pair.F = 0xFF;
    P.reg.pair.B = 0xFF; P.reg.pair.C = 0xFF;
    P.reg.pair.D = 0xFF; P.reg.pair.E = 0xFF;
    P.reg.pair.H = 0xFF; P.reg.pair.L = 0xFF;
    P.reg.back.A = 0xFF; P.reg.back.F = 0xFF;
    P.reg.back.B = 0xFF; P.reg.back.C = 0xFF;
    P.reg.back.D = 0xFF; P.reg.back.E = 0xFF;
    P.reg.back.H = 0xFF; P.reg.back.L = 0xFF;
    P.reg.IX = 0xFFFF; P.reg.IY = 0xFFFF; P.reg.WZ = 0xFFFF;
    P.reg.PC = 0x0100; P.reg.SP = 0xFFFE;
    P.reg.I = 0; P.reg.R = 0; P.reg.IFF = 0; P.reg.interrupt = 0;

    /* redcode/Z80 */
    Z80_rc R = {};
    R.fetch_opcode = rc_rd;
    R.fetch        = rc_rd;
    R.read         = rc_rd;
    R.write        = rc_wr;
    R.in           = rc_in;
    R.out          = rc_out;
    z80_power(&R, 1);   /* power on */
    z80_instant_reset(&R);
    /* Match reset state of the others (0xFFFF in main regs). */
    R.af.uint16_value  = 0xFFFF;
    R.bc.uint16_value  = 0xFFFF;
    R.de.uint16_value  = 0xFFFF;
    R.hl.uint16_value  = 0xFFFF;
    R.af_.uint16_value = 0xFFFF;
    R.bc_.uint16_value = 0xFFFF;
    R.de_.uint16_value = 0xFFFF;
    R.hl_.uint16_value = 0xFFFF;
    R.ix_iy[0].uint16_value = 0xFFFF; /* IX */
    R.ix_iy[1].uint16_value = 0xFFFF; /* IY */
    R.memptr.uint16_value   = 0xFFFF;
    R.pc.uint16_value = 0x0100;
    R.sp.uint16_value = 0xFFFE;

    long long n = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (; n < cap; n++) {
        uint16_t mpc = M->cpu.rf[RFP_PC];
        uint8_t  op0 = MMEM[mpc], op1 = MMEM[(mpc+1)&0xFFFF],
                 op2 = MMEM[(mpc+2)&0xFFFF], op3 = MMEM[(mpc+3)&0xFFFF];
        if (mpc == 0) break;

        /* Snapshot our model's cycle counter before stepping so we can
         * give redcode exactly the same T-state budget. Our model's
         * `cycle` field is in HALF-T-states (phases) -- divide by 2
         * to get T-states. */
        uint64_t mc_before = M->cpu.cycle;
        z80_sys_step_instr(M);
        sz80_step(&S);
        cpins = cz_step_one(&C, cpins);
        P.execute(1);
        /* redcode: step by exactly the T-states our model just took.
         * z80_execute(N) is a per-call budget: cpu.cycle_limit = N,
         * cpu.cycles resets to 0 at start of call, loops until
         * cpu.cycles >= cycle_limit. So pass mc_delta_t directly --
         * NOT R.cycles + mc_delta_t (the absolute-target reading was
         * wrong; verified via /tmp/redcode_step_probe.c). */
        {
            uint64_t mc_delta_t = (M->cpu.cycle - mc_before) / 2;
            z80_execute(&R, mc_delta_t);
        }

        uint16_t maf = M->cpu.rf[RFP_AF], mbc = M->cpu.rf[RFP_BC], mde = M->cpu.rf[RFP_DE];
        uint16_t mhl = M->cpu.rf[RFP_HL], mix = M->cpu.rf[RFP_IX], miy = M->cpu.rf[RFP_IY];
        uint16_t msp = M->cpu.rf[RFP_SP], mpc2 = M->cpu.rf[RFP_PC];
        uint16_t saf = (S.a<<8) | sz_reff(&S), sbc=(S.b<<8)|S.c, sde=(S.d<<8)|S.e;
        uint16_t shl = (S.h<<8)|S.l, six = S.ix, siy = S.iy, ssp = S.sp, spc = S.pc;
        uint16_t cpc = (uint16_t)(C.pc - 1);    /* chips overlap M1 */
        uint16_t paf = (uint16_t)((P.reg.pair.A << 8) | P.reg.pair.F);
        uint16_t pbc = (uint16_t)((P.reg.pair.B << 8) | P.reg.pair.C);
        uint16_t pde = (uint16_t)((P.reg.pair.D << 8) | P.reg.pair.E);
        uint16_t phl = (uint16_t)((P.reg.pair.H << 8) | P.reg.pair.L);
        uint16_t pix = P.reg.IX, piy = P.reg.IY, psp = P.reg.SP, ppc = P.reg.PC;
        uint16_t raf = R.af.uint16_value, rbc = R.bc.uint16_value;
        uint16_t rde = R.de.uint16_value, rhl = R.hl.uint16_value;
        uint16_t rix = R.ix_iy[0].uint16_value, riy = R.ix_iy[1].uint16_value;
        uint16_t rsp = R.sp.uint16_value, rpc = R.pc.uint16_value;

        /* Mask the undocumented YF (bit 5) and XF (bit 3) of F before
         * comparing AF across the five oracles. Our model and
         * redcode/Z80 both implement David Banks' 2018 LDIR/LDDR/CPIR/
         * CPDR/INIR/INDR/OTIR/OTDR repeat-M-cycle Y/X fold-in
         * (YF=PC.13, XF=PC.11). superzazu, chips/z80.h and suzukiplan's
         * cores predate that work and still use the LDI / CPI / INI /
         * OUT single-shot Y/X formula during the repeat. The two
         * conventions disagree by exactly bits 5+3 in F at the boundary
         * right after a repeat aborts. Masking 0x28 lets the lockstep
         * keep catching real architectural drift (all the documented
         * bits + every other register) without firing on this known
         * Banks-vs-pre-Banks oracle gap. See docs/oracles.md §5. */
        const uint16_t AF_MASK = 0xFFD7u;   /* clear YF | XF in F */
        uint16_t maf_m = maf & AF_MASK;
        uint16_t saf_m = saf & AF_MASK;
        uint16_t caf_m = C.af & AF_MASK;
        uint16_t paf_m = paf & AF_MASK;
        uint16_t raf_m = raf & AF_MASK;
        bool diverge = (mpc2 != spc || mpc2 != cpc || mpc2 != ppc || mpc2 != rpc ||
                        maf_m != saf_m || maf_m != caf_m || maf_m != paf_m || maf_m != raf_m ||
                        mbc != sbc || mbc != C.bc || mbc != pbc || mbc != rbc ||
                        mde != sde || mde != C.de || mde != pde || mde != rde ||
                        mhl != shl || mhl != C.hl || mhl != phl || mhl != rhl ||
                        mix != six || mix != C.ix || mix != pix || mix != rix ||
                        miy != siy || miy != C.iy || miy != piy || miy != riy ||
                        msp != ssp || msp != C.sp || msp != psp || msp != rsp);
        if (diverge) {
            printf("DIVERGENCE at step %lld after @%04x: %02x %02x %02x %02x\n",
                n, mpc, op0,op1,op2,op3);
            printf("  MINE:    pc%04x af%04x bc%04x de%04x hl%04x ix%04x iy%04x sp%04x\n",
                mpc2,maf,mbc,mde,mhl,mix,miy,msp);
            printf("  SUPZ:    pc%04x af%04x bc%04x de%04x hl%04x ix%04x iy%04x sp%04x\n",
                spc,saf,sbc,sde,shl,six,siy,ssp);
            printf("  CHIPS:   pc%04x af%04x bc%04x de%04x hl%04x ix%04x iy%04x sp%04x\n",
                cpc,C.af,C.bc,C.de,C.hl,C.ix,C.iy,C.sp);
            printf("  SUZK:    pc%04x af%04x bc%04x de%04x hl%04x ix%04x iy%04x sp%04x\n",
                ppc,paf,pbc,pde,phl,pix,piy,psp);
            printf("  REDCODE: pc%04x af%04x bc%04x de%04x hl%04x ix%04x iy%04x sp%04x\n",
                rpc,raf,rbc,rde,rhl,rix,riy,rsp);
            return 1;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    printf("quint-lockstep clean: %lld instructions in %.2f s (%.2f Minstr/s, five emulators)\n",
        n, secs, n/secs/1e6);
    free(M);
    return 0;
}
