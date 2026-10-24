/* redcode_basicrunner.c - basicrunner.c's BASIC ROM computer around
   redcode/Z80 (scripts/refs/redcode_z80, Zilog NMOS): a 68B50 ACIA at ports
   80 (status) / 81 (data) for NASCOM BASIC 4.7c, ports 00 / 01 for 1K Tiny
   BASIC, wired to stdin / stdout. /INT is low while a stdin byte waits (the
   ACIA's RDRF), as NASCOM BASIC's receive path is interrupt-driven (IM1).

   Usage: redcode_basicrunner [--autostart] [--max-instr N] [--exit-on S] rom.hex

   Same options as basicrunner, so tests/basic/run_basic_tests.sh redcode
   runs the same canned scripts. --max-instr counts half T-states, as
   basicrunner's phases do, so both get the same budget. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include "Z80.h"

static zuint8 mem[0x10000];
static const char *prefeed = NULL;
static int prefeed_i = 0, stdin_eof = 0;
static unsigned char ring[4096];
static int ring_h = 0, ring_t = 0;
static const char *exit_on = NULL;
static char tail[513];
static size_t tail_n = 0;
static int sentinel = 0;

static void drain(void) {
    while (!stdin_eof && (ring_t + 1) % 4096 != ring_h) {
        fd_set fds; FD_ZERO(&fds); FD_SET(0, &fds);
        struct timeval tv = {0, 0};
        if (select(1, &fds, NULL, NULL, &tv) <= 0) break;
        unsigned char ch;
        ssize_t r = read(0, &ch, 1);
        if (r <= 0) { stdin_eof = 1; break; }
        ring[ring_t] = ch; ring_t = (ring_t + 1) % 4096;
    }
}
static int available(void) {
    if (prefeed && prefeed[prefeed_i]) return 1;
    drain();
    return ring_h != ring_t;
}
static int consume(void) {
    int b;
    if (prefeed && prefeed[prefeed_i]) b = prefeed[prefeed_i++] & 0xff;
    else { drain(); if (ring_h == ring_t) return 0; b = ring[ring_h]; ring_h = (ring_h + 1) % 4096; }
    if (b == 0x7f) b = 0x08;
    return b;
}

static zuint8 rd(void *u, zuint16 a) { (void)u; return mem[a]; }
static void wr(void *u, zuint16 a, zuint8 v) { (void)u; mem[a] = v; }
static zuint8 in_(void *u, zuint16 a) {
    (void)u;
    switch (a & 0xff) {
    case 0x80: return (zuint8)(0x02 | (available() ? 0x01 : 0));
    case 0x00: return available() ? 0xff : 0x00;
    case 0x81: case 0x01: return (zuint8)consume();
    default: return 0xff;
    }
}
static void out_(void *u, zuint16 a, zuint8 v) {
    (void)u;
    if ((a & 0xff) != 0x81 && (a & 0xff) != 0x01) return;
    putchar(v); fflush(stdout);
    if (exit_on && !sentinel) {
        if (tail_n == 512) { memmove(tail, tail + 1, 511); tail_n = 511; }
        tail[tail_n++] = (char)v; tail[tail_n] = 0;
        if (strstr(tail, exit_on)) sentinel = 1;
    }
}

static void load_hex(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(2); }
    char line[600];
    while (fgets(line, sizeof line, f)) {
        unsigned len, addr, type;
        if (line[0] != ':' || sscanf(line + 1, "%2x%4x%2x", &len, &addr, &type) != 3) continue;
        if (type == 1) break;
        if (type != 0) continue;
        for (unsigned i = 0; i < len; i++) {
            unsigned b;
            if (sscanf(line + 9 + 2 * i, "%2x", &b) == 1) mem[(addr + i) & 0xffff] = (zuint8)b;
        }
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *rom = NULL;
    long long max_halves = 5000000000LL;
    int autostart = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--autostart")) autostart = 1;
        else if (!strcmp(argv[i], "--max-instr") && i + 1 < argc) max_halves = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--exit-on") && i + 1 < argc) exit_on = argv[++i];
        else if (!strcmp(argv[i], "--prefeed") && i + 1 < argc) prefeed = argv[++i];
        else if (!rom) rom = argv[i];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }
    if (!rom) { fprintf(stderr, "usage: %s [--autostart] [--max-instr N] [--exit-on S] rom.hex\n", argv[0]); return 2; }
    if (autostart && !prefeed) prefeed = "\r";
    load_hex(rom);
    static Z80 cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.fetch_opcode = rd; cpu.fetch = rd; cpu.read = rd; cpu.write = wr;
    cpu.in = in_; cpu.out = out_; cpu.nop = rd;
    cpu.options = Z80_MODEL_ZILOG_NMOS;
    z80_power(&cpu, 1);
    cpu.sp.uint16_value = 0xffff;                        /* as basicrunner */
    unsigned long long t = 0, after = 0;
    while ((long long)(2 * t) < max_halves) {
        z80_int(&cpu, available());                      /* /INT = RDRF */
        t += z80_run(&cpu, 1);
        if (sentinel && (after += 1) >= 50000) {
            fprintf(stderr, "\n[redcode_basicrunner] sentinel '%s' seen; exiting\n", exit_on);
            return 0;
        }
    }
    fprintf(stderr, "\n[redcode_basicrunner] limit reached at PC=%04X\n", cpu.pc.uint16_value);
    return 0;
}
