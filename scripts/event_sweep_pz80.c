/* event_sweep_pz80.c - one RESET / INT / NMI sweep case through the perfectz80
   gate-level netlist. Same options and output as tests/iverilog/tb_event_sweep.v
   (see that file), so scripts/event_sweep.py can diff the two half-cycle by
   half-cycle.

   Usage: event_sweep_pz80 [-e events] [-r run_T] [-m marker] [-i]
                           [-a int_at] [-H hold_addr -T hold_T] prog.hex

   Events are placed in sixteenths of a T-state from the rising edge that
   starts the marker fetch's T1. perfectz80 only advances whole half-cycles,
   so every transition is applied at the start of the half-cycle that
   contains it, in time order. A pulse that starts and ends inside one
   half-cycle (it spans no clock edge) is applied low then high within that
   half-cycle: the netlist settles after each write, so internal latches
   that are transparent during that half see it. */
#define _POSIX_C_SOURCE 200809L   /* getopt/optarg under -std=c99 on glibc */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "refs/perfectz80/perfectz80.h"

extern uint8_t cpu_memory[0x10000];

typedef struct { int pin, lo, hi; } pulse_t;
static pulse_t pulses[8];
static int n_pulses;

static void load_hex(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(2); }
    unsigned addr = 0; char tok[64];
    while (fscanf(f, "%63s", tok) == 1) {
        if (tok[0] == '@') addr = (unsigned)strtoul(tok + 1, NULL, 16) & 0xffff;
        else if (tok[0] == '/' || tok[0] == '#' || tok[0] == ';') {
            int ch; while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
        } else cpu_memory[addr++ & 0xffff] = (uint8_t)strtoul(tok, NULL, 16);
    }
    fclose(f);
}

static void drive(void *st, int pin, bool high) {
    switch (pin) {
    case 0: cpu_writeNMI(st, high); break;
    case 1: cpu_writeINT(st, high); break;
    case 2: cpu_writeWAIT(st, high); break;
    case 3: cpu_writeBUSRQ(st, high); break;
    case 4: cpu_writeRESET(st, high); break;
    }
}

/* Apply the transitions that fall in half-cycle `rel` (sixteenths 8*rel ..
   8*rel+7), in time order; lo before hi at the same sixteenth. */
static void apply_events(void *st, int rel) {
    for (int s = 8 * rel; s < 8 * rel + 8; s++)
        for (int i = 0; i < n_pulses; i++) {
            if (pulses[i].lo == s) drive(st, pulses[i].pin, false);
            if (pulses[i].hi == s) drive(st, pulses[i].pin, true);
        }
}

int main(int argc, char **argv) {
    const char *events = NULL;
    int run = 100, marker = 0x0200, inta_release = 0, int_at = -1, hold = -1, hold_t = 0, opt;
    while ((opt = getopt(argc, argv, "e:r:m:ia:H:T:")) != -1) {
        switch (opt) {
        case 'e': events = optarg; break;
        case 'r': run = atoi(optarg); break;
        case 'm': marker = (int)strtol(optarg, NULL, 16); break;
        case 'i': inta_release = 1; break;
        case 'a': int_at = (int)strtol(optarg, NULL, 16); break;
        case 'H': hold = (int)strtol(optarg, NULL, 16); break;
        case 'T': hold_t = atoi(optarg); break;
        default: fprintf(stderr, "bad option\n"); return 2;
        }
    }
    if (optind >= argc) { fprintf(stderr, "usage: %s [options] prog.hex\n", argv[0]); return 2; }
    memset(cpu_memory, 0, sizeof cpu_memory);
    load_hex(argv[optind]);
    if (events) {
        FILE *f = fopen(events, "r"); if (!f) { perror(events); return 2; }
        while (n_pulses < 8 && fscanf(f, "%d %d %d", &pulses[n_pulses].pin,
                                      &pulses[n_pulses].lo, &pulses[n_pulses].hi) == 3) n_pulses++;
        fclose(f);
    }

    void *st = cpu_initAndResetChip();
    cpu_setIntVec(0xff);
    int found = 0, mark = 0, hold_until = -1, in_read = 0;
    bool wait_low = false;
    for (int h = 0; h < 2 * 4000; h++) {
        bool clk = cpu_readCLK(st);
        bool m1 = cpu_readM1(st), mreq = cpu_readMREQ(st), iorq = cpu_readIORQ(st), rd = cpu_readRD(st);
        unsigned a = cpu_readAddressBus(st);
        if (!found && !m1 && a == (unsigned)marker) { found = 1; mark = clk ? h : h - 1; }
        if (found) {
            int rel = h - mark;
            /* Reactive pins, from the pins just after this half-cycle's edge. */
            if (inta_release && !m1 && !iorq) drive(st, 1, true);
            if (int_at >= 0 && !m1 && !mreq && a == (unsigned)int_at) drive(st, 1, false);
            if (int_at >= 0 && !m1 && !iorq) drive(st, 1, true);
            if (hold >= 0 && m1 && !mreq && !rd && a == (unsigned)hold) {
                if (!in_read) { wait_low = true; in_read = 1; hold_until = h + 2 * hold_t; cpu_writeWAIT(st, false); }
            } else in_read = 0;
            if (wait_low && (h >= hold_until || !cpu_readRESET(st))) { wait_low = false; cpu_writeWAIT(st, true); }
            apply_events(st, rel);
            if (wait_low && !cpu_readRESET(st)) { wait_low = false; cpu_writeWAIT(st, true); }
            printf("%d %d %04x %02x %d %d %d %d %d %d %d %d\n", rel, cpu_readCLK(st) ? 1 : 0,
                   cpu_readAddressBus(st), cpu_readDataBus(st),
                   cpu_readMREQ(st), cpu_readIORQ(st), cpu_readRD(st), cpu_readWR(st),
                   cpu_readM1(st), cpu_readRFSH(st), cpu_readHALT(st), cpu_readBUSAK(st));
            if (rel >= 2 * run - 1) break;
        }
        cpu_step(st);
    }
    if (!found) printf("marker not reached\n");
    cpu_destroyChip(st);
    return 0;
}
