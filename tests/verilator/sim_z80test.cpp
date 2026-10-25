// sim_z80test.cpp - Verilated z80_top driving Patrik Rak's z80test
// (.tap files at tests/z80test/). Mirrors scripts/z80test_runner.c but
// runs through the Verilog RTL instead of the C model. The same
// silicon-faithful conventions apply:
//   - load code block at addr from TAP header (always 0x8000 for Rak)
//   - PC = code-block load address, SP = 0xFFEE, AF/BC/DE/HL = 0xFFFF
//   - mem[0x0010] = 0xC9 (RET) so RST 10 returns cleanly; we trap on
//     M1 edge with PC == 0x0010 to emit the character in A
//   - mem[0x1601] = 0xC9 (CHAN-OPEN stub)
//   - Exit when PC == 0x0000 (Spectrum-style outer RET sentinel)
//   - IO callback returns (port & 1) ? 0xFF : 0xBF (ZX Spectrum 48K ULA
//     idle pattern -- matches redcode's cpu_in convention; required
//     for Rak's INI/IND test CRCs)
//
//   sim_z80test <variant.tap> [max_instr]
//
// Exits 0 when the suite prints "all tests passed", 1 when it reports
// failures, 2 when it ends without a verdict. No failure is tolerated.
//
// Verilator is ~20x slower than the C model so this is gated to the
// silicon-faithfulness CI job and run on demand; the C runner stays
// fast for local iteration.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include "Vz80_top.h"
#include "Vz80_top___024root.h"
#include "verilated.h"

static unsigned char mem[65536];

// Parse a Spectrum .tap container. Mirrors load_tap() in
// scripts/z80test_runner.c. Returns 0 on success, 1 on failure.
static int load_tap(const char *path, unsigned char *mem,
                    uint16_t *out_addr, size_t *out_len) {
    FILE *f = std::fopen(path, "rb");
    if (!f) { std::perror(path); return 1; }
    std::fseek(f, 0, SEEK_END);
    long total = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    unsigned char *tap = (unsigned char*)std::malloc((size_t)total);
    if (!tap) { std::fclose(f); return 1; }
    if (std::fread(tap, 1, (size_t)total, f) != (size_t)total) {
        std::fclose(f); std::free(tap); return 1;
    }
    std::fclose(f);
    long off = 0; int found = 0; uint16_t code_addr = 0;
    while (off < total - 2) {
        unsigned blen = (unsigned)tap[off] | ((unsigned)tap[off+1] << 8);
        if (off + 2 + (long)blen > total) break;
        unsigned char flag = tap[off + 2];
        unsigned char *body = &tap[off + 3];
        long body_len = (long)blen - 2;
        if (flag == 0x00 && body_len >= 14) {
            if (body[0] == 3) {     // CODE header
                code_addr = (uint16_t)(body[13] | (body[14] << 8));
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

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <variant.tap> [max_instr]\n", argv[0]);
        return 2;
    }
    long long max_instr   = (argc > 2) ? std::atoll(argv[2]) : 10000000000LL;

    uint16_t load_addr = 0; size_t code_len = 0;
    if (load_tap(argv[1], mem, &load_addr, &code_len) != 0) {
        std::fprintf(stderr, "tap load failed\n"); return 2;
    }

    // ROM stubs: RST 0x10 print path returns immediately; the harness
    // traps the print at the M1 edge where PC == 0x0010 (before the RET
    // executes, so A still holds the original character).
    mem[0x0010] = 0xC9;
    mem[0x1601] = 0xC9;

    // Exit sentinel at 0x0000-0x0001 (zero bytes installed so the
    // outer test's RET pops PC=0).
    mem[0x0000] = 0x00;
    mem[0x0001] = 0x00;

    Verilated::commandArgs(argc, argv);
    Vz80_top* t = new Vz80_top;
    auto* R = t->rootp;
    t->busreq_n = 1; t->int_n = 1; t->nmi_n = 1; t->wait_n = 1; t->data_in = 0;

    // Pulse reset.
    // a real falling edge on por_n (Verilator starts inputs at 0, which
    // would be no edge at all)
    t->por_n = 1; t->reset_n = 0; t->clk = 0; t->eval();
    t->por_n = 0; t->eval();
    t->por_n = 1; t->eval();
    t->clk = 1; t->eval();
    t->clk = 0; t->eval();
    t->reset_n = 1;
    // RESET is sampled on rising edges: the third one after release enters
    // T1 of the first M1 at PC, so the start state is poked after two
    for (int k = 0; k < 2; k++) { t->clk = 1; t->eval(); t->clk = 0; t->eval(); }

    // Poke the Patrik-runner initial state. Indices come from
    // rtl/z80_defs.vh: BC=0, DE=1, HL=2, AF=3, BC2=4, DE2=5, HL2=6,
    // AF2=7, IX=8, IY=9, SP=10, PC=11, WZ=12. Set the bank Rak's
    // outer harness expects (regs to 0xFFFF, SP to 0xFFEE).
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf[11]   = load_addr;       // PC
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf_n[11] = load_addr;
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf[10]   = 0xFFEE;          // SP
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf_n[10] = 0xFFEE;
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf[3]    = 0xFFFF;          // AF
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf_n[3]  = 0xFFFF;
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf[0]    = 0xFFFF;          // BC
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf_n[0]  = 0xFFFF;
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf[1]    = 0xFFFF;          // DE
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf_n[1]  = 0xFFFF;
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf[2]    = 0xFFFF;          // HL
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf_n[2]  = 0xFFFF;
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf[8]    = 0xFFFF;          // IX
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf_n[8]  = 0xFFFF;
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf[9]    = 0xFFFF;          // IY
    R->z80_top__DOT__u_core__DOT__u_regs__DOT__rf_n[9]  = 0xFFFF;

    long long phases = 0, instr = 0;
    bool prev_m1_n = true;
    uint16_t prev_pc_at_m1 = 0xFFFF;
    char *captured = (char*)std::malloc(1 << 20);
    if (!captured) { delete t; return 2; }
    size_t cap_used = 0, cap_sz = 1 << 20;

    auto t0 = std::chrono::steady_clock::now();

    while (instr < max_instr) {
        t->clk = !t->clk; t->eval();
        if (!t->mreq_n && !t->wr_n)      mem[t->addr & 0xFFFF] = t->data_out;
        if (!t->mreq_n && !t->rd_n)      t->data_in = mem[t->addr & 0xFFFF];
        else if (!t->iorq_n && !t->rd_n) {
            // ULA-idle port-parity: even ports = 0xBF, odd = 0xFF.
            // Matches scripts/z80test_runner.c (s->io_ula_idle = 1).
            t->data_in = (t->addr & 1) ? 0xFF : 0xBF;
        }
        else                              t->data_in = 0;
        phases++;
        if (prev_m1_n && !t->m1_n) {
            instr++;
            uint16_t pc = R->z80_top__DOT__u_core__DOT__rfl[11];
            if (pc == 0) break;     // outer RET to 0
            if (pc == 0x0010 && prev_pc_at_m1 != 0x0010) {
                // print A: logical AF is rfl[3] (rf[] holds physical pairs
                // behind the EX/EXX exchange flip-flops); A is the high byte.
                unsigned char ch = (R->z80_top__DOT__u_core__DOT__rfl[3] >> 8) & 0xFF;
                std::putchar((int)ch);
                if (cap_used < cap_sz - 1) captured[cap_used++] = (char)ch;
                std::fflush(stdout);
            }
            prev_pc_at_m1 = pc;
        }
        prev_m1_n = t->m1_n;
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    captured[cap_used] = 0;
    std::printf("\n=== sim_z80test (Verilator): %s ===\n", argv[1]);
    std::printf("phases: %lld  (~%lld instructions)  elapsed: %.2f s  (%.2f Minstr/s)\n",
                phases, instr, secs, (double)instr / secs / 1e6);

    // Verdict parsing matches scripts/z80test_runner.c: the suite's last
    // line is "Result: all tests passed." or "Result: N of M tests failed."
    int rc = 2, failed = 0, total = 0;
    const char *res = std::strstr(captured, "Result: ");
    if (res && std::strstr(res, "all tests passed")) {
        std::printf("verdict: PASS (all tests passed)\n");
        rc = 0;
    } else if (res && std::sscanf(res, "Result: %d of %d", &failed, &total) == 2) {
        std::printf("verdict: FAIL (%d of %d tests failed)\n", failed, total);
        rc = 1;
    } else {
        std::printf("verdict: FAIL (no result line -- max_instr too low?)\n");
    }

    std::free(captured);
    delete t;
    return rc;
}
