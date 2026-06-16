// sim_basic.cpp - Verilated z80_core driving NASCOM BASIC 4.7 / Tiny BASIC
// ROMs over an emulated 68B50 ACIA at ports 0x80 (status) / 0x81 (data).
// Pre-drains stdin into a queue at startup (the canned-script CI path);
// no termios / signal / Ctrl-* handling — purely batch.
//
// Status reg: RDRF (bit 0) follows queue-not-empty, TDRE (bit 1) always 1.
// /INT is asserted while stdin queue is non-empty (NASCOM's interrupt-
// driven RX path).
//
//   sim_basic [--autostart] [--exit-on <substr>] <rom.hex> [max_instr]
//
// --exit-on <substr>: when <substr> appears in the BASIC stdout, run a
// brief grace window (~50K more instructions) so any trailing "Ok\n" or
// newline finishes flushing, then exit cleanly. Used by the canned-script
// CI tests so the harness doesn't waste 5+ minutes spinning past the
// test's own "DONE-XYZ" sentinel before the max_instr cap fires.
//
// Exit code: 0 always — the test driver greps stdout for expected substrings.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <queue>
#include <string>
#include "Vz80_core.h"
#include "Vz80_core___024root.h"
#include "verilated.h"

static uint8_t mem[65536];
static std::queue<uint8_t> q_in;

// Intel HEX loader (matches basicrunner.c's parser well enough for our ROMs).
static int load_hex(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { perror(path); return 1; }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (line[0] != ':') continue;
        unsigned len, addr, type;
        if (sscanf(line + 1, "%2x%4x%2x", &len, &addr, &type) != 3) continue;
        if (type == 1) break;          // EOF record
        if (type != 0) continue;        // skip non-data records
        for (unsigned i = 0; i < len; i++) {
            unsigned b;
            if (sscanf(line + 9 + i * 2, "%2x", &b) == 1)
                mem[(addr + i) & 0xFFFF] = (uint8_t)b;
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    bool        autostart = false;
    const char* rom_path  = nullptr;
    const char* exit_on   = nullptr;
    long long   max_instr = 30000000LL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--autostart")) autostart = true;
        else if (!strcmp(argv[i], "--exit-on") && i + 1 < argc) exit_on = argv[++i];
        else if (!rom_path)                  rom_path = argv[i];
        else                                 max_instr = atoll(argv[i]);
    }
    if (!rom_path) {
        fprintf(stderr,"usage: %s [--autostart] [--exit-on <substr>] <rom.hex> [max_instr]\n",argv[0]);
        return 2;
    }

    if (load_hex(rom_path)) return 2;
    fprintf(stderr, "[sim_basic] loaded ROM from %s\n", rom_path);

    // --autostart: NASCOM BASIC 4.7 first asks "Memory top?" and reads
    // a line. Pre-queue "0\r" so the loader skips straight to the BASIC
    // prompt. Tiny BASIC ignores leading bytes here; harmless when set.
    if (autostart) { q_in.push('0'); q_in.push('\r'); }

    // Drain stdin into the queue (caller pipes the entire canned input
    // before the simulation starts).
    int c;
    while ((c = getchar()) != EOF) q_in.push((uint8_t)c);
    fprintf(stderr, "[sim_basic] stdin queue: %zu bytes%s\n",
            q_in.size(), autostart ? " (autostart prepended)" : "");

    Verilated::commandArgs(argc, argv);
    Vz80_core* t = new Vz80_core;
    auto* R = t->rootp;
    t->busreq_n = 1; t->int_n = 1; t->nmi_n = 1; t->wait_n = 1; t->data_in = 0;

    // reset (3 phases of !reset_n is enough for our model to latch the
    // synchronous reset block exactly like sim_zex does it).
    t->reset_n = 0; t->clk = 0; t->eval();
    t->clk = 1; t->eval();
    t->clk = 0; t->eval();
    t->reset_n = 1;

    // PC=0 (the ROM's reset vector); SP=0xFFFE (typical BASIC ROM init).
    R->z80_core__DOT__rf[11]   = 0x0000;
    R->z80_core__DOT__rf_n[11] = 0x0000;
    R->z80_core__DOT__rf[10]   = 0xFFFE;
    R->z80_core__DOT__rf_n[10] = 0xFFFE;

    long long phases = 0, instr = 0;
    bool prev_m1_n  = true;
    bool prev_iord_active = false;     // edge-detect for IORD start
    bool prev_iowr_active = false;     // edge-detect for IOWR start
    uint8_t latched_io_data = 0xFF;   // sticky for the current IORD cycle
    // --exit-on bookkeeping: rolling tail of the last few hundred bytes
    // emitted to stdout. When `exit_on` is set and shows up in the tail,
    // arm a small instruction-grace countdown so the trailing "Ok\n" can
    // flush, then break out.
    std::string tail_buf;
    bool      sentinel_seen   = false;
    long long grace_remaining = 0;
    static const size_t TAIL_KEEP = 512;
    static const long long EXIT_GRACE_INSTR = 50000;

    auto t0 = std::chrono::steady_clock::now();

    while (instr < max_instr) {
        // /INT mirrors RDRF: drop while stdin queue non-empty so NASCOM's
        // ACIA-IRQ-driven RX path fires.
        t->int_n = q_in.empty() ? 1 : 0;

        // one phase
        t->clk = !t->clk; t->eval();

        // Memory: write on (mreq && wr), read on (mreq && rd).
        if (!t->mreq_n && !t->wr_n)       mem[t->addr & 0xFFFF] = t->data_out;
        else if (!t->mreq_n && !t->rd_n)  t->data_in = mem[t->addr & 0xFFFF];

        // I/O: 6850-style ACIA at 0x80/0x81 (NASCOM) or 0x00/0x01 (Tiny).
        // Edge-detect each cycle by its OWN "active" condition. Z80 drops
        // iorq_n at T2.P and wr_n one phase later at T2.N — so the IOWR
        // cycle isn't fully "active" until T2.N. IORD drops iorq_n and
        // rd_n together at T2.P, so its first-active phase is T2.P.
        bool iord_active = !t->iorq_n && !t->rd_n;
        bool iowr_active = !t->iorq_n && !t->wr_n;

        if (iord_active) {
            if (!prev_iord_active) {            // start of this IORD cycle
                uint8_t port = (uint8_t)(t->addr & 0xFF);
                bool has = !q_in.empty();
                if (port == 0x80) {                                  // NASCOM status
                    latched_io_data = (uint8_t)(0x02 | (has ? 0x01 : 0));
                } else if (port == 0x00) {                            // Tiny status: ANA A; JZ
                    latched_io_data = has ? 0xFF : 0x00;
                } else if (port == 0x81 || port == 0x01) {            // data (NASCOM or Tiny)
                    if (has) { latched_io_data = q_in.front(); q_in.pop(); }
                    else     latched_io_data = 0;
                } else {
                    latched_io_data = 0xFF;
                }
            }
            t->data_in = latched_io_data;
        }
        if (iowr_active && !prev_iowr_active) { // start of this IOWR cycle
            uint8_t port = (uint8_t)(t->addr & 0xFF);
            if (port == 0x81 || port == 0x01) {                       // data write
                unsigned char ch = (unsigned char)t->data_out;
                putchar((int)ch); fflush(stdout);
                if (exit_on && !sentinel_seen) {
                    tail_buf.push_back((char)ch);
                    if (tail_buf.size() > TAIL_KEEP)
                        tail_buf.erase(0, tail_buf.size() - TAIL_KEEP);
                    if (tail_buf.find(exit_on) != std::string::npos) {
                        sentinel_seen = true;
                        grace_remaining = EXIT_GRACE_INSTR;
                        fprintf(stderr,"\n[sim_basic] sentinel '%s' seen; exiting in %lld instructions\n",
                                exit_on, grace_remaining);
                    }
                }
            }
            // 0x80 / 0x00 (control register) ignored
        }
        prev_iord_active = iord_active;
        prev_iowr_active = iowr_active;

        phases++;
        if (prev_m1_n && !t->m1_n) {
            instr++;
            if (sentinel_seen && --grace_remaining <= 0) break;
        }
        prev_m1_n = t->m1_n;
    }

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    fprintf(stderr, "\n[sim_basic] %lld phases (~%lld instructions) in %.2fs (%.2f Mphases/s)\n",
            phases, instr, secs, phases / secs / 1e6);

    delete t;
    return 0;
}
