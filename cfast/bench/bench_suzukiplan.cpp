/* suzukiplan (suzukiplan/z80): execute(1), one instruction per call */
#include "bench_common.h"
#include "suzukiplan_z80.hpp"

int main(int argc, char **argv)
{
    long long N = args(argc, argv), n = 0;
    Z80 P([](void *, unsigned short a) -> unsigned char { return MEM[a]; },
          [](void *, unsigned short a, unsigned char v) { MEM[a] = v; },
          [](void *, unsigned char) -> unsigned char { return 0; },
          [](void *, unsigned char, unsigned char) {}, nullptr);
    P.reg.pair.A = P.reg.pair.F = P.reg.pair.B = P.reg.pair.C = 0xFF;
    P.reg.pair.D = P.reg.pair.E = P.reg.pair.H = P.reg.pair.L = 0xFF;
    P.reg.IX = P.reg.IY = P.reg.WZ = 0xFFFF;
    P.reg.PC = 0x100; P.reg.SP = 0xFFFE;
    double t = 0;
    for (; n < N; n++) {
        if (P.reg.PC == 0) break;
        if (P.reg.PC == 5) bdos(P.reg.pair.C, (uint16_t)(P.reg.pair.D << 8 | P.reg.pair.E), P.reg.pair.E);
        t += P.execute(1);
    }
    report("suzukiplan", n, N, t, P.reg.PC);
    return 0;
}
