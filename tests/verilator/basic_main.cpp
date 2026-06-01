// basic_main.cpp - Verilator counterpart of scripts/basicrunner.c. Runs a
// Z80 BASIC ROM through the synthesisable Verilog RTL (z80_core) instead of
// the C model, with the same emulated 68B50 ACIA (NASCOM ports 0x80/0x81,
// Tiny BASIC ports 0/1) wired to host stdin/stdout. ~21x slower than the C
// runner but bit-for-bit the same architectural behaviour.
//
// Usage:
//   sim_basic [--bin] <rom.{hex,bin}> [--start ADDR] [--prefeed STR] [--autostart] [--max-phases N]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <sys/select.h>
#include "Vz80_core.h"
#include "verilated.h"

#define ACIA_STATUS 0x80
#define ACIA_DATA   0x81
#define STATUS_RDRF 0x01
#define STATUS_TDRE 0x02

static unsigned char mem[65536];

// ---- termios (raw stdin) -----------------------------------------------
static struct termios saved_termios;
static int            termios_saved = 0;
static void restore_termios(void) { if (termios_saved) tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios); }
static void on_signal(int sig) { restore_termios(); _exit(128 + sig); }
static void enable_raw_stdin(void) {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
        termios_saved = 1; atexit(restore_termios);
        struct termios t = saved_termios;
        t.c_iflag &= (tcflag_t)~(BRKINT|ICRNL|INLCR|IGNCR|INPCK|ISTRIP|IXON|IXOFF|PARMRK);
        t.c_lflag &= (tcflag_t)~(ICANON|ECHO|ECHONL|IEXTEN|ISIG);
        t.c_cflag |= CS8;
        t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
}

// ---- stdin ring buffer (mirrors basicrunner.c) -------------------------
#define RING_CAP 256
static unsigned char ring[RING_CAP];
static int ring_head = 0, ring_tail = 0;
static int stdin_eof = 0;
static const char* prefeed_buf = nullptr;
static int prefeed_idx = 0;

static int ring_empty() { return ring_head == ring_tail; }
static int ring_full()  { return ((ring_tail + 1) % RING_CAP) == ring_head; }
static void ring_push(unsigned char b) { if (!ring_full()) { ring[ring_tail] = b; ring_tail = (ring_tail+1)%RING_CAP; } }
static int  ring_pop()  { int b = ring[ring_head]; ring_head = (ring_head+1)%RING_CAP; return b; }
static int  prefeed_has() { return prefeed_buf && prefeed_buf[prefeed_idx] != 0; }
static int  prefeed_pop() { return prefeed_buf[prefeed_idx++] & 0xFF; }

static int translate_in(int b) {
    if (b == 0x1C) {
        fprintf(stderr, "\r\n[sim_basic] Ctrl-\\ pressed - exiting\r\n");
        restore_termios(); _exit(0);
    }
    if (b == 0x00) b = 0x03;
    if (b == 0x7F) b = 0x08;
    return b;
}
static void drain_stdin() {
    if (stdin_eof) return;
    for (int i = 0; i < 32; i++) {
        if (ring_full()) break;
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 0};
        if (select(STDIN_FILENO+1, &fds, NULL, NULL, &tv) <= 0) break;
        unsigned char ch;
        ssize_t r = read(STDIN_FILENO, &ch, 1);
        if (r == 0) { stdin_eof = 1; break; }
        if (r < 0) break;
        if (ch == 0x1C) translate_in(ch);
        if (ch == 0x00) ch = 0x03;
        ring_push(ch);
    }
}
static int stdin_byte_available() { if (prefeed_has()) return 1; drain_stdin(); return !ring_empty(); }
static int stdin_consume_byte()   {
    int b;
    if (prefeed_has()) b = prefeed_pop();
    else { drain_stdin(); if (ring_empty()) return 0; b = ring_pop(); }
    return translate_in(b);
}

// ---- ROM loaders -------------------------------------------------------
static size_t load_intel_hex(const char* path) {
    FILE* f = fopen(path, "r"); if (!f) { perror(path); exit(2); }
    char line[600]; size_t loaded = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] != ':') continue;
        unsigned len, addr, type;
        if (sscanf(line+1, "%2x%4x%2x", &len, &addr, &type) != 3) continue;
        if (type == 1) break;
        if (type != 0) continue;
        for (unsigned i = 0; i < len; i++) {
            unsigned b;
            if (sscanf(line + 9 + 2*i, "%2x", &b) != 1) { fprintf(stderr, "bad hex\n"); exit(2); }
            mem[(addr + i) & 0xFFFF] = (uint8_t)b;
            loaded++;
        }
    }
    fclose(f); return loaded;
}
static size_t load_raw_bin(const char* path, uint16_t at) {
    FILE* f = fopen(path, "rb"); if (!f) { perror(path); exit(2); }
    size_t n = fread(mem + at, 1, 0x10000 - at, f);
    fclose(f); return n;
}

// ---- main --------------------------------------------------------------
int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    const char* rom = nullptr; int is_bin = 0; uint16_t load_at = 0; int autostart = 0;
    long long max_phases = 50000000000LL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bin")) is_bin = 1;
        else if (!strcmp(argv[i], "--start") && i+1<argc) load_at = (uint16_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--max-phases") && i+1<argc) max_phases = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--prefeed") && i+1<argc) prefeed_buf = argv[++i];
        else if (!strcmp(argv[i], "--autostart")) autostart = 1;
        else if (!rom) rom = argv[i];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }
    if (!rom) { fprintf(stderr, "usage: %s [--bin] <rom> [--start ADDR] [--max-phases N] [--prefeed STR] [--autostart]\n", argv[0]); return 2; }
    if (autostart && !prefeed_buf) prefeed_buf = "\r";

    memset(mem, 0, sizeof mem);
    size_t loaded = is_bin ? load_raw_bin(rom, load_at) : load_intel_hex(rom);
    fprintf(stderr, "[sim_basic] loaded %zu bytes from %s%s (RTL via Verilator)\n",
            loaded, rom, is_bin ? " (binary)" : " (intel hex)");
    if (isatty(STDIN_FILENO))
        fprintf(stderr, "[sim_basic] Ctrl-\\ exits the runner; Ctrl-C or Ctrl-Space => BREAK in BASIC\n");
    enable_raw_stdin();

    Vz80_core* t = new Vz80_core;
    t->wait_n = 1; t->int_n = 1; t->nmi_n = 1; t->busreq_n = 1; t->data_in = 0;

    // reset (active-low)
    t->reset_n = 0; t->clk = 0; t->eval();
    t->clk = 1; t->eval();
    t->clk = 0; t->eval();
    t->reset_n = 1;

    int prev_mem_wr = 1, prev_io_wr = 1, prev_io_rd = 1;
    int latched_data_byte = -1;
    long long phases = 0;
    while (phases++ < max_phases) {
        // Update /INT before the clock edge so the RTL latches it correctly.
        t->int_n = stdin_byte_available() ? 0 : 1;
        t->clk = 1; t->eval();                  // posedge: latch + next state
        t->clk = 0; t->eval();                  // settle new outputs

        int mreq = !t->mreq_n, iorq = !t->iorq_n;
        int rd   = !t->rd_n,    wr   = !t->wr_n;
        int mem_wr_active = mreq && wr;
        int io_wr_active  = iorq && wr;
        int io_rd_active  = iorq && rd;

        if (mem_wr_active && !prev_mem_wr) mem[t->addr & 0xFFFF] = t->data_out;
        if (io_wr_active  && !prev_io_wr) {
            uint8_t port = t->addr & 0xFF;
            if (port == ACIA_DATA || port == 0x01) { putchar((int)t->data_out); fflush(stdout); }
        }
        if (io_rd_active && !prev_io_rd) {
            uint8_t port = t->addr & 0xFF;
            int has = stdin_byte_available();
            if (port == ACIA_STATUS)         latched_data_byte = STATUS_TDRE | (has ? STATUS_RDRF : 0);
            else if (port == 0x00)           latched_data_byte = has ? 0xFF : 0x00;
            else if (port == ACIA_DATA || port == 0x01) latched_data_byte = stdin_consume_byte();
            else                             latched_data_byte = 0xFF;
        }
        if (!io_rd_active) latched_data_byte = -1;

        // drive data_in for whatever read is currently in progress
        if (mreq && rd)                              t->data_in = mem[t->addr & 0xFFFF];
        else if (io_rd_active && latched_data_byte >= 0) t->data_in = (uint8_t)latched_data_byte;
        else                                          t->data_in = 0;

        prev_mem_wr = mem_wr_active;
        prev_io_wr  = io_wr_active;
        prev_io_rd  = io_rd_active;
    }
    fprintf(stderr, "[sim_basic] max_phases reached\n");
    delete t;
    return 0;
}
