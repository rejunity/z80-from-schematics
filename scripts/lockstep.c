/* lockstep.c - run a zex .com on the superzazu reference and our C model in
   lockstep (identical 0xFFFF reset), comparing registers after every
   instruction; stop and print the culprit at the first divergence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* superzazu reference, symbols renamed to avoid clashing with our model */
#define z80 sz80
#define z80_init sz80_init
#define z80_step sz80_step
#define z80_gen_nmi sz80_gen_nmi
#define z80_gen_int sz80_gen_int
#define z80_debug_output sz80_dbg
#include "/tmp/z80ref/z80.h"
#include "/tmp/z80ref/z80.c"
#undef z80
#undef z80_init
#undef z80_step
#undef z80_gen_nmi
#undef z80_gen_int
#undef z80_debug_output

#include "z80_sim.h"   /* our model */

static uint8_t RMEM[65536];
static uint8_t rrd(void* u, uint16_t a){ (void)u; return RMEM[a]; }
static void    rwr(void* u, uint16_t a, uint8_t v){ (void)u; RMEM[a]=v; }
static uint8_t rin(sz80* z, uint8_t p){ (void)z;(void)p; return 0; }
static void    rout(sz80* z, uint8_t p, uint8_t v){ (void)z;(void)p;(void)v; }

static uint8_t reff(sz80* z){ return (z->sf<<7)|(z->zf<<6)|(z->yf<<5)|(z->hf<<4)|(z->xf<<3)|(z->pf<<2)|(z->nf<<1)|z->cf; }

int main(int argc, char** argv){
    if (argc<2){ fprintf(stderr,"usage: lockstep prog.com [max]\n"); return 2; }
    FILE* f=fopen(argv[1],"rb"); if(!f){perror("open");return 2;}
    static uint8_t img[65536]; size_t sz=fread(&img[0x100],1,0x10000-0x100,f); fclose(f);

    /* reference */
    memset(RMEM,0,sizeof RMEM); memcpy(RMEM,img,sizeof RMEM);
    RMEM[0]=0xC3;RMEM[1]=0;RMEM[2]=0;RMEM[5]=0xC9;
    sz80 z; sz80_init(&z); z.read_byte=rrd; z.write_byte=rwr; z.port_in=rin; z.port_out=rout;
    z.a=0xff; z.sf=z.zf=z.yf=z.hf=z.xf=z.pf=z.nf=z.cf=1;
    z.b=z.c=z.d=z.e=z.h=z.l=0xff; z.ix=z.iy=0xffff;
    z.a_=z.b_=z.c_=z.d_=z.e_=z.h_=z.l_=z.f_=0xff;
    z.pc=0x100; z.sp=0xFFFE;

    /* our model (default reset already 0xFFFF), reload same image */
    z80_system_t* s=malloc(sizeof *s); z80_sys_init(s);
    memcpy(s->mem,img,sizeof s->mem);
    s->mem[0]=0xC3;s->mem[1]=0;s->mem[2]=0;s->mem[5]=0xC9;
    z80_set_pc(&s->cpu,0x100); s->cpu.rf[RFP_SP]=0xFFFE;

    long long cap=(argc>2)?atoll(argv[2]):20000000LL, n=0;
    for(; n<cap; n++){
        uint16_t pc=z.pc;
        if(pc==0) { printf("both reached exit at step %lld\n", n); break; }
        uint8_t op0=RMEM[pc],op1=RMEM[pc+1],op2=RMEM[pc+2],op3=RMEM[pc+3];
        sz80_step(&z);
        z80_sys_step_instr(s);
        /* memory check: full 64K every step (memcmp on 64K is fast) */
        if (memcmp(RMEM, s->mem, 65536) != 0) {
            int da=-1; for(int i=0;i<65536;i++) if(RMEM[i]!=s->mem[i]){da=i;break;}
            printf("MEMORY DIVERGENCE at step %lld, after @%04x: %02x %02x %02x %02x\n",
                n, pc, op0,op1,op2,op3);
            printf("  first diff @%04x  ref=%02x  mine=%02x\n", da, RMEM[da], s->mem[da]);
            printf("  REF : pc%04x af%04x bc%04x de%04x hl%04x ix%04x iy%04x sp%04x\n",
                z.pc,(z.a<<8)|reff(&z),(z.b<<8)|z.c,(z.d<<8)|z.e,(z.h<<8)|z.l,z.ix,z.iy,z.sp);
            printf("  MINE: pc%04x af%04x bc%04x de%04x hl%04x ix%04x iy%04x sp%04x\n",
                s->cpu.rf[RFP_PC],s->cpu.rf[RFP_AF],s->cpu.rf[RFP_BC],s->cpu.rf[RFP_DE],
                s->cpu.rf[RFP_HL],s->cpu.rf[RFP_IX],s->cpu.rf[RFP_IY],s->cpu.rf[RFP_SP]);
            return 1;
        }
        uint8_t mf=s->cpu.rf[RFP_AF]&0xFF, rf=reff(&z);
        unsigned raf=(z.a<<8)|rf;
        if (z.pc != s->cpu.rf[RFP_PC] || raf != s->cpu.rf[RFP_AF] ||
            ((z.b<<8)|z.c)!=s->cpu.rf[RFP_BC] || ((z.d<<8)|z.e)!=s->cpu.rf[RFP_DE] ||
            ((z.h<<8)|z.l)!=s->cpu.rf[RFP_HL] || z.ix!=s->cpu.rf[RFP_IX] ||
            z.iy!=s->cpu.rf[RFP_IY] || z.sp!=s->cpu.rf[RFP_SP]) {
            printf("DIVERGENCE at step %lld, after instruction @%04x: %02x %02x %02x %02x\n",
                n, pc, op0,op1,op2,op3);
            printf("  REF : pc%04x af%04x bc%04x de%04x hl%04x ix%04x iy%04x sp%04x\n",
                z.pc,raf,(z.b<<8)|z.c,(z.d<<8)|z.e,(z.h<<8)|z.l,z.ix,z.iy,z.sp);
            printf("  MINE: pc%04x af%04x bc%04x de%04x hl%04x ix%04x iy%04x sp%04x\n",
                s->cpu.rf[RFP_PC],s->cpu.rf[RFP_AF],s->cpu.rf[RFP_BC],s->cpu.rf[RFP_DE],
                s->cpu.rf[RFP_HL],s->cpu.rf[RFP_IX],s->cpu.rf[RFP_IY],s->cpu.rf[RFP_SP]);
            return 1;
        }
    }
    printf("no register divergence in %lld steps\n", n);
    (void)sz;
    return 0;
}
