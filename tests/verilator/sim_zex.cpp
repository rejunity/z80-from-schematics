// sim_zex.cpp - Verilated z80_core driving a CP/M .com (apples-to-apples
// against scripts/zexrunner.c on the C model). No trace output. Reports
// instructions executed, wall time, and Minstr/s for speed comparison.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include "Vz80_core.h"
#include "Vz80_core___024root.h"
#include "verilated.h"

static unsigned char mem[65536];

static void bdos(Vz80_core* t, Vz80_core___024root* R) {
    uint8_t c = R->z80_core__DOT__rf[0] & 0xFF;          // RFP_BC = 0; low byte = C
    if (c == 2) {
        putchar(R->z80_core__DOT__rf[1] & 0xFF);         // RFP_DE = 1; low byte = E
    } else if (c == 9) {
        uint16_t de = R->z80_core__DOT__rf[1];
        for (int i = 0; i < 65536; i++) {
            uint8_t ch = mem[de++];
            if (ch == '$') break;
            putchar(ch);
        }
    }
    fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <prog.com> [max_instr]\n", argv[0]); return 2; }
    long long max_instr = (argc > 2) ? atoll(argv[2]) : 4000000000LL;
    FILE* f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    fread(&mem[0x100], 1, 0x10000 - 0x100, f); fclose(f);
    mem[0] = 0xC3; mem[1] = 0; mem[2] = 0; mem[5] = 0xC9;

    Verilated::commandArgs(argc, argv);
    Vz80_core* t = new Vz80_core;
    auto* R = t->rootp;
    t->busreq_n = 1; t->int_n = 1; t->nmi_n = 1; t->wait_n = 1; t->data_in = 0;

    // reset
    t->reset_n = 0; t->clk = 0; t->eval();
    t->clk = 1; t->eval();
    t->clk = 0; t->eval();
    t->reset_n = 1;

    // poke PC=0x100, SP=0xFFFE (RFP_PC=11, RFP_SP=10)
    R->z80_core__DOT__rf[11]   = 0x0100;
    R->z80_core__DOT__rf_n[11] = 0x0100;
    R->z80_core__DOT__rf[10]   = 0xFFFE;
    R->z80_core__DOT__rf_n[10] = 0xFFFE;

    long long phases = 0, instr = 0;
    bool prev_m1_n = true;
    uint16_t prev_pc_at_m1 = 0xFFFF;  // PC observed at the last M1 edge

    auto t0 = std::chrono::steady_clock::now();

    while (instr < max_instr) {
        // one phase
        t->clk = !t->clk; t->eval();
        if (!t->mreq_n && !t->wr_n)      mem[t->addr & 0xFFFF] = t->data_out;
        if (!t->mreq_n && !t->rd_n)      t->data_in = mem[t->addr & 0xFFFF];
        else if (!t->iorq_n && !t->rd_n) t->data_in = 0;
        else                              t->data_in = 0;
        phases++;
        // detect new instruction: m1_n falling edge (1->0) marks the start of
        // the next M1 fetch. Sample PC *after* eval so we see the PC the
        // model latched into the M1 address bus for this fetch.
        if (prev_m1_n && !t->m1_n) {
            instr++;
            uint16_t pc = R->z80_core__DOT__rf[11];
            if (pc == 0) break;                       // exit trap (RET to 0x0000)
            // Service BDOS once per execution of the RET at 0x0005, not once
            // per phase that happens to sample PC==5 (which would print the
            // string 3-6 times — one per T-state of the RET fetch+decode).
            if (pc == 5 && prev_pc_at_m1 != 5) bdos(t, R);
            prev_pc_at_m1 = pc;
        }
        prev_m1_n = t->m1_n;
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    printf("\n=== sim_zex (Verilator): %s ===\n", argv[1]);
    printf("phases executed: %lld  (~%lld instructions)\n", phases, instr);
    printf("elapsed: %.2f s  (%.2f Mphases/s; ~%.2f Minstr/s)\n",
           secs, phases / secs / 1e6, instr / secs / 1e6);
    delete t;
    return 0;
}
