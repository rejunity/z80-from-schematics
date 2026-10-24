/* superzazu (superzazu/z80): z80_step, one instruction per call */
#include "bench_common.h"
#include "superzazu_z80.h"
#include "superzazu_z80.c"

static uint8_t rd(void *u, uint16_t a) { (void)u; return MEM[a]; }
static void wr(void *u, uint16_t a, uint8_t v) { (void)u; MEM[a] = v; }
static uint8_t pin(z80 *z, uint8_t p) { (void)z; (void)p; return 0; }
static void pout(z80 *z, uint8_t p, uint8_t v) { (void)z; (void)p; (void)v; }

int main(int argc, char **argv)
{
    long long N = args(argc, argv), n = 0;
    z80 S;
    z80_init(&S);
    S.read_byte = rd; S.write_byte = wr; S.port_in = pin; S.port_out = pout;
    S.a = 0xff;
    S.sf = S.zf = S.yf = S.hf = S.xf = S.pf = S.nf = S.cf = 1;
    S.b = S.c = S.d = S.e = S.h = S.l = 0xff;
    S.ix = S.iy = 0xffff;
    S.pc = 0x100; S.sp = 0xFFFE;
    for (; n < N; n++) {
        if (S.pc == 0) break;
        if (S.pc == 5) bdos(S.c, (uint16_t)(S.d << 8 | S.e), S.e);
        z80_step(&S);
    }
    report("superzazu", n, N, (double)S.cyc, S.pc);
    return 0;
}
