/* floooh/chips z80.h: z80_tick, one T-state per call, the pins in a word */
#include "bench_common.h"
#define CHIPS_IMPL
#include "chips_z80.h"

int main(int argc, char **argv)
{
    long long N = args(argc, argv), n = 0;
    z80_t C;
    uint64_t pins = z80_init(&C);
    C.af = C.bc = C.de = C.hl = C.ix = C.iy = C.wz = 0xFFFF;
    C.af2 = C.bc2 = C.de2 = C.hl2 = 0xFFFF;
    C.sp = 0xFFFE;
    pins = z80_prefetch(&C, 0x100);
    double t = 0;
    while (n < N) {
        pins = z80_tick(&C, pins);
        t += 1;
        if (pins & Z80_MREQ) {
            const uint16_t a = Z80_GET_ADDR(pins);
            if (pins & Z80_RD) { Z80_SET_DATA(pins, MEM[a]); }
            else if (pins & Z80_WR) { MEM[a] = Z80_GET_DATA(pins); }
        } else if ((pins & Z80_IORQ) && (pins & Z80_RD)) { Z80_SET_DATA(pins, 0); }
        if (z80_opdone(&C)) {                       /* an instruction's end */
            n++;
            const uint16_t pc = (uint16_t)(C.pc - 1);   /* the next opcode's fetch is in flight */
            if (pc == 0) break;
            if (pc == 5) bdos((uint8_t)C.bc, C.de, (uint8_t)C.de);
        }
    }
    report("floooh/chips", n, N, t, (unsigned)(C.pc - 1));
    return 0;
}
