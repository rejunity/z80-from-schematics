/* redcode (redcode/Z80): z80_execute, one instruction per call */
#include "bench_common.h"
#include "Z80.h"

static zuint8 rd(void *u, zuint16 a) { (void)u; return MEM[a]; }
static void wr(void *u, zuint16 a, zuint8 v) { (void)u; MEM[a] = v; }
static zuint8 in_(void *u, zuint16 a) { (void)u; (void)a; return 0; }
static void out_(void *u, zuint16 a, zuint8 v) { (void)u; (void)a; (void)v; }

int main(int argc, char **argv)
{
    long long N = args(argc, argv), n = 0;
    static Z80 R;
    memset(&R, 0, sizeof R);
    R.fetch_opcode = rd; R.fetch = rd; R.read = rd; R.write = wr; R.in = in_; R.out = out_;
    z80_power(&R, 1);
    z80_instant_reset(&R);
    R.af.uint16_value = R.bc.uint16_value = R.de.uint16_value = R.hl.uint16_value = 0xFFFF;
    R.ix_iy[0].uint16_value = R.ix_iy[1].uint16_value = R.memptr.uint16_value = 0xFFFF;
    R.pc.uint16_value = 0x100; R.sp.uint16_value = 0xFFFE;
    double t = 0;
    for (; n < N; n++) {
        if (R.pc.uint16_value == 0) break;
        if (R.pc.uint16_value == 5) bdos(R.bc.uint8_values.at_0, R.de.uint16_value, R.de.uint8_values.at_0);
        t += (double)z80_execute(&R, 1);
    }
    report("redcode", n, N, t, R.pc.uint16_value);
    return 0;
}
