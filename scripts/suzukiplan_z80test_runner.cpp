/* suzukiplan_z80test_runner.cpp — run Patrik Rak's z80test through
 * suzukiplan/z80.hpp (scripts/refs/suzukiplan_z80.hpp). MAME-derived
 * reference emulator widely used in retro projects.
 *
 *   suzukiplan_z80test_runner <variant.tap> [max_instr]
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>

#include "refs/suzukiplan_z80.hpp"

static unsigned char  pmem[65536];
static unsigned char  ports[65536];

static unsigned char rd(void*, unsigned short a) { return pmem[a]; }
static void          wr(void*, unsigned short a, unsigned char v) { pmem[a] = v; }
static unsigned char pin(void* arg, unsigned char lo) {
    Z80 *cpu = (Z80*)arg;
    unsigned short addr = ((unsigned short)cpu->reg.pair.B << 8) | lo;
    return ports[addr];
}
static void pout(void* arg, unsigned char lo, unsigned char v) {
    Z80 *cpu = (Z80*)arg;
    unsigned short addr = ((unsigned short)cpu->reg.pair.B << 8) | lo;
    ports[addr] = v;
}

static int load_tap(const char *path, unsigned char mem[65536],
                    unsigned short *out_addr, size_t *out_len)
{
    FILE *f = std::fopen(path, "rb");
    if (!f) { std::perror(path); return 1; }
    std::fseek(f, 0, SEEK_END); long total = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    unsigned char *tap = (unsigned char*)std::malloc((size_t)total);
    if (!tap) { std::fclose(f); return 1; }
    if (std::fread(tap, 1, (size_t)total, f) != (size_t)total) {
        std::fclose(f); std::free(tap); return 1;
    }
    std::fclose(f);
    long off = 0; int found = 0; unsigned short code_addr = 0;
    while (off < total - 2) {
        unsigned blen = (unsigned)tap[off] | ((unsigned)tap[off+1] << 8);
        if (off + 2 + (long)blen > total) break;
        unsigned char flag = tap[off + 2];
        unsigned char *body = &tap[off + 3];
        long body_len = (long)blen - 2;
        if (flag == 0x00 && body_len >= 14) {
            if (body[0] == 3) {
                code_addr = (unsigned short)(body[13] | (body[14] << 8));
                found = 1;
            }
        } else if (flag == 0xff && found) {
            size_t copy = (body_len < 0) ? 0 : (size_t)body_len;
            if ((unsigned)code_addr + copy > 0x10000) copy = 0x10000 - code_addr;
            std::memcpy(&mem[code_addr], body, copy);
            *out_addr = code_addr; *out_len = copy;
            std::free(tap); return 0;
        }
        off += 2 + (long)blen;
    }
    std::free(tap); return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <variant.tap> [max_instr]\n", argv[0]);
        return 2;
    }
    long long max_instr = (argc > 2) ? std::atoll(argv[2]) : 8000000000LL;

    unsigned short addr = 0; size_t n = 0;
    if (load_tap(argv[1], pmem, &addr, &n) != 0) {
        std::fprintf(stderr, "tap load failed\n"); return 2;
    }
    pmem[0x0010] = 0xC9;
    pmem[0x1601] = 0xC9;
    std::memset(ports, 0xFF, sizeof ports);
    for (int hi = 0; hi < 256; hi++)
        ports[(hi << 8) | 0xFE] = 0xBF;

    /* Construct with raw-pointer callback API. We pass `arg = &cpu_ptr`
     * indirection so the port_in/out callbacks can see the current B
     * register for high-byte port address. Trick: use the `arg=&cpu`
     * after construction. */
    Z80 cpu(rd, wr, pin, pout, nullptr);
    /* Re-set arg to the cpu instance itself (used by pin/pout above). */
    cpu.setupCallback(rd, wr, pin, pout, &cpu);

    /* Reset state to match our z80test_runner. */
    cpu.reg.pair.A = 0xFF; cpu.reg.pair.F = 0xFF;
    cpu.reg.pair.B = 0xFF; cpu.reg.pair.C = 0xFF;
    cpu.reg.pair.D = 0xFF; cpu.reg.pair.E = 0xFF;
    cpu.reg.pair.H = 0xFF; cpu.reg.pair.L = 0xFF;
    cpu.reg.back.A = 0xFF; cpu.reg.back.F = 0xFF;
    cpu.reg.back.B = 0xFF; cpu.reg.back.C = 0xFF;
    cpu.reg.back.D = 0xFF; cpu.reg.back.E = 0xFF;
    cpu.reg.back.H = 0xFF; cpu.reg.back.L = 0xFF;
    cpu.reg.IX = 0xFFFF; cpu.reg.IY = 0xFFFF; cpu.reg.WZ = 0xFFFF;
    cpu.reg.SP = 0xFFEE;
    pmem[0xFFEE] = 0x00; pmem[0xFFEF] = 0x00;
    cpu.reg.PC = addr;

    long long count = 0;
    char *captured = (char*)std::malloc(1 << 20);
    size_t cap_used = 0, cap_sz = 1 << 20;
    if (!captured) return 2;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        if (cpu.reg.PC == 0x0000) break;
        if (cpu.reg.PC == 0x0010) {
            unsigned char ch = cpu.reg.pair.A;
            std::putchar((int)ch);
            if (cap_used < cap_sz - 1) captured[cap_used++] = (char)ch;
            std::fflush(stdout);
        }
        cpu.execute(1); /* run one cycle's worth — typically one instruction */
        if (++count >= max_instr) break;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    captured[cap_used] = 0;

    std::printf("\n=== suzukiplan_z80test_runner: %s ===\n", argv[1]);
    std::printf("steps: %lld  elapsed: %.2f s  (%.2f Msteps/s)\n",
                count, secs, (double)count / secs / 1e6);
    const char *p = std::strstr(captured, "Result: ");
    if (p) std::printf("verdict from captured output: %s\n", p);
    else if (std::strstr(captured, "all tests passed")) std::printf("verdict: ALL TESTS PASSED\n");
    else std::printf("verdict: INCONCLUSIVE\n");

    std::free(captured);
    return 0;
}
