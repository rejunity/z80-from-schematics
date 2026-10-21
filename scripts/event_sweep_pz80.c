/* event_sweep_pz80.c - one RESET / INT / NMI sweep case through the perfectz80
   gate-level netlist. Same options and output as tests/iverilog/tb_event_sweep.v
   (see that file), so scripts/event_sweep.py can diff the two half-cycle by
   half-cycle.

   Usage: event_sweep_pz80 [-e events] [-r run_T] [-m marker] [-i]
                           [-a int_at] [-H hold_addr -T hold_T] [-v vec]
                           [-o ops] [-B] [-W] prog.hex

   -B walks BUSREQ through the instruction: low from the marker on, and
   each grant released for one T-state (high when BUSACK is seen low, low
   again once BUSACK is high), so every M-cycle end the die grants at shows.
   -W walks WAIT: low for the three half-cycles from each fall of MREQ (not
   a refresh) or IORQ, so each bus cycle gets the wait states its sampling
   edges see.

   Each line also has the pads the die drives (scripts/pz80_pads.c): the
   address pads, the data pads, and MREQ IORQ RD WR M1 RFSH HALT BUSAK
   (bit 0 first), in hex.

   The interrupting device: -v is the byte it puts on the bus in INTA (hex,
   FF if not given), -o more bytes (hex, first byte first, e.g. -o 3412)
   it hands out after it, one per INTA cycle or memory read, in place of
   memory: the rest of an IM0 instruction (the die fetches the opcode
   after a prefix with another INTA and reads the operands at the
   unchanged PC). An opcode fetch from memory ends the device's
   instruction.

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
#include "pz80_pads.h"

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
    int run = 100, marker = 0x0200, inta_release = 0, int_at = -1, hold = -1, hold_t = 0, vec = 0xff, opt;
    int busreq_walk = 0, wait_walk = 0;
    uint8_t ops[8]; int n_ops = 0;
    while ((opt = getopt(argc, argv, "e:r:m:ia:H:T:v:o:BW")) != -1) {
        switch (opt) {
        case 'e': events = optarg; break;
        case 'r': run = atoi(optarg); break;
        case 'm': marker = (int)strtol(optarg, NULL, 16); break;
        case 'i': inta_release = 1; break;
        case 'a': int_at = (int)strtol(optarg, NULL, 16); break;
        case 'H': hold = (int)strtol(optarg, NULL, 16); break;
        case 'T': hold_t = atoi(optarg); break;
        case 'v': vec = (int)strtol(optarg, NULL, 16) & 0xff; break;
        case 'B': busreq_walk = 1; break;
        case 'W': wait_walk = 1; break;
        case 'o':
            for (n_ops = 0; n_ops < 8 && optarg[2 * n_ops] && optarg[2 * n_ops + 1]; n_ops++) {
                char b[3] = {optarg[2 * n_ops], optarg[2 * n_ops + 1], 0};
                ops[n_ops] = (uint8_t)strtol(b, NULL, 16);
            }
            break;
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
    pz80_pads_init();
    cpu_setIntVec((uint8_t)vec);   /* set again at each INTA, see the device */
    int found = 0, mark = 0, hold_until = -1, in_read = 0;
    bool wait_low = false;
    int walk_up = 0, walk_wait_until = -1, prev_strobe = 0;   /* -B, -W */
    /* the device: its next byte at each INTA (perfectz80 puts int_vector on
       the bus), and at each memory read after one until it has none left
       (cpu_memory holds it for that read, restored when the read ends) */
    int dev_pos = 0, was_read = 0, was_inta = 0, dev_addr = -1;
    uint8_t dev_saved = 0;
    for (int h = 0; h < 2 * 4000; h++) {
        bool clk = cpu_readCLK(st);
        bool m1 = cpu_readM1(st), mreq = cpu_readMREQ(st), iorq = cpu_readIORQ(st), rd = cpu_readRD(st);
        bool rfsh = cpu_readRFSH(st), busak = cpu_readBUSAK(st);
        unsigned a = cpu_readAddressBus(st);
        if (!m1 && !iorq) {
            if (!was_inta) {
                cpu_setIntVec(dev_pos == 0 ? (uint8_t)vec : dev_pos <= n_ops ? ops[dev_pos - 1] : 0xff);
                dev_pos++;
            }
            was_inta = 1;
        } else was_inta = 0;
        if (!mreq && !rd) {
            if (!m1) dev_pos = 0;                         /* an opcode from memory */
            else if (!was_read && dev_pos > 0 && dev_pos <= n_ops) {
                dev_addr = (int)a; dev_saved = cpu_memory[a];
                cpu_memory[a] = ops[dev_pos - 1]; dev_pos++;
            }
            was_read = 1;
        } else {
            if (dev_addr >= 0) { cpu_memory[dev_addr] = dev_saved; dev_addr = -1; }
            was_read = 0;
        }
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
            if (busreq_walk) {
                if (rel == 0) cpu_writeBUSRQ(st, false);
                else if (!busak && !walk_up) { cpu_writeBUSRQ(st, true); walk_up = 1; }
                else if (walk_up && busak) { cpu_writeBUSRQ(st, false); walk_up = 0; }
            }
            if (wait_walk) {
                int strobe = (!mreq && rfsh) || !iorq;
                if (strobe && !prev_strobe) { cpu_writeWAIT(st, false); walk_wait_until = rel + 3; }
                else if (walk_wait_until >= 0 && rel >= walk_wait_until) { cpu_writeWAIT(st, true); walk_wait_until = -1; }
                prev_strobe = strobe;
            }
            apply_events(st, rel);
            if (wait_low && !cpu_readRESET(st)) { wait_low = false; cpu_writeWAIT(st, true); }
            printf("%d %d %04x %02x %d %d %d %d %d %d %d %d %04x %02x %02x\n", rel, cpu_readCLK(st) ? 1 : 0,
                   cpu_readAddressBus(st), cpu_readDataBus(st),
                   cpu_readMREQ(st), cpu_readIORQ(st), cpu_readRD(st), cpu_readWR(st),
                   cpu_readM1(st), cpu_readRFSH(st), cpu_readHALT(st), cpu_readBUSAK(st),
                   pz80_addr_driven(st), pz80_data_driven(st), pz80_ctl_driven(st));
            if (rel >= 2 * run - 1) break;
        }
        cpu_step(st);
    }
    if (!found) printf("marker not reached\n");
    cpu_destroyChip(st);
    return 0;
}
