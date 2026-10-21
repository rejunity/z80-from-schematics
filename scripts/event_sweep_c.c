/* event_sweep_c.c - one RESET / INT / NMI sweep case through the C model
   (cmodel/). Same options and output as scripts/event_sweep_pz80.c and
   tests/iverilog/tb_event_sweep.v, so scripts/event_sweep.py --side c can
   diff the C model against perfectz80 half-cycle by half-cycle.

   Usage: event_sweep_c [-e events] [-r run_T] [-m marker] [-i]
                        [-a int_at] [-H hold_addr -T hold_T] [-v vec]
                        [-o ops] prog.hex

   The model advances whole half-cycles: every pin transition inside a
   half-cycle is applied, in time order, before the step into the next
   one, so the edge that ends the half-cycle sees the level it ended at --
   what the RTL's flip-flops sample. A pulse that starts and ends inside
   one half-cycle (it spans no clock edge) is never seen, as in the RTL,
   except on NMI: here the harness is rtl/z80_top.v's NMI pulse catcher
   (the die's NMI flip-flop, set by the pin's falling edge itself), which
   holds NMI low for the model until the next rising edge has seen it.
   The interrupting device (-v, -o) is the one in event_sweep_pz80.c; I/O
   reads FF. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "z80_sim.h"

typedef struct { int pin, lo, hi; } pulse_t;
static pulse_t pulses[8];
static int n_pulses;

/* the NMI pin and z80_top's pulse catcher: hold is set by the pin's falling
   edge (unless the clear is active), clr samples hold on each rising edge
   and clears it right after */
static bool nmi_pin = true, nmi_hold, nmi_clr;

static void load_hex(z80_system_t *s, const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(2); }
    unsigned addr = 0; char tok[64];
    while (fscanf(f, "%63s", tok) == 1) {
        if (tok[0] == '@') addr = (unsigned)strtoul(tok + 1, NULL, 16) & 0xffff;
        else if (tok[0] == '/' || tok[0] == '#' || tok[0] == ';') {
            int ch; while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
        } else s->mem[addr++ & 0xffff] = (uint8_t)strtoul(tok, NULL, 16);
    }
    fclose(f);
}

static void drive(z80_pins_t *p, int pin, bool high) {
    switch (pin) {
    case 0:
        if (nmi_pin && !high && !nmi_clr) nmi_hold = true;
        nmi_pin = high;
        p->nmi_n = nmi_pin && !nmi_hold;
        break;
    case 1: p->int_n = high; break;
    case 2: p->wait_n = high; break;
    case 3: p->busreq_n = high; break;
    case 4: p->reset_n = high; break;
    }
}

/* the transitions in half-cycle `rel` (sixteenths 8*rel .. 8*rel+7), in
   time order; lo before hi at the same sixteenth */
static void apply_events(z80_pins_t *p, int rel) {
    for (int s = 8 * rel; s < 8 * rel + 8; s++)
        for (int i = 0; i < n_pulses; i++) {
            if (pulses[i].lo == s) drive(p, pulses[i].pin, false);
            if (pulses[i].hi == s) drive(p, pulses[i].pin, true);
        }
}

int main(int argc, char **argv) {
    const char *events = NULL;
    int run = 100, marker = 0x0200, inta_release = 0, int_at = -1, hold = -1, hold_t = 0, vec = 0xff, opt;
    uint8_t ops[8]; int n_ops = 0;
    while ((opt = getopt(argc, argv, "e:r:m:ia:H:T:v:o:")) != -1) {
        switch (opt) {
        case 'e': events = optarg; break;
        case 'r': run = atoi(optarg); break;
        case 'm': marker = (int)strtol(optarg, NULL, 16); break;
        case 'i': inta_release = 1; break;
        case 'a': int_at = (int)strtol(optarg, NULL, 16); break;
        case 'H': hold = (int)strtol(optarg, NULL, 16); break;
        case 'T': hold_t = atoi(optarg); break;
        case 'v': vec = (int)strtol(optarg, NULL, 16) & 0xff; break;
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
    static z80_system_t s;
    z80_sys_init(&s);
    memset(s.io, 0xff, sizeof s.io);
    load_hex(&s, argv[optind]);
    if (events) {
        FILE *f = fopen(events, "r"); if (!f) { perror(events); return 2; }
        while (n_pulses < 8 && fscanf(f, "%d %d %d", &pulses[n_pulses].pin,
                                      &pulses[n_pulses].lo, &pulses[n_pulses].hi) == 3) n_pulses++;
        fclose(f);
    }

    z80_pins_t *p = &s.cpu.pins;
    int found = 0, mark = 0, hold_until = -1, in_read = 0;
    bool wait_low = false;
    int dev_pos = 0, was_read = 0, was_inta = 0;  /* the device, as in event_sweep_pz80.c */
    uint8_t dev_byte = (uint8_t)vec;
    z80_sys_phase(&s);                    /* into the first half: T1 of the fetch at 0000 */
    for (int h = 0; h < 2 * 4000; h++) {
        /* the pins of half-cycle h; the bus as the system presents it */
        if (!p->iorq_n && !p->m1_n) {
            if (!was_inta) {
                dev_byte = dev_pos == 0 ? (uint8_t)vec : dev_pos <= n_ops ? ops[dev_pos - 1] : 0xff;
                dev_pos++;
            }
            was_inta = 1;
            p->data_in = dev_byte;
        } else was_inta = 0;
        if (!p->mreq_n && !p->rd_n) {
            if (!p->m1_n) dev_pos = 0;                    /* an opcode from memory */
            else if (!was_read && dev_pos > 0 && dev_pos <= n_ops) {
                dev_byte = ops[dev_pos - 1]; dev_pos++; was_read = 2;
            }
            if (!was_read) was_read = 1;
            if (was_read == 2) p->data_in = dev_byte;
        } else was_read = 0;
        int clk = s.cpu.phi == 0;         /* PHI_P: the half after a rising edge */
        if (!found && !p->m1_n && p->addr == (unsigned)marker) { found = 1; mark = clk ? h : h - 1; }
        if (found) {
            int rel = h - mark;
            /* reactive pins, from the pins of this half-cycle */
            if (inta_release && !p->m1_n && !p->iorq_n) p->int_n = true;
            if (int_at >= 0 && !p->m1_n && !p->mreq_n && p->addr == (unsigned)int_at) p->int_n = false;
            if (int_at >= 0 && !p->m1_n && !p->iorq_n) p->int_n = true;
            if (hold >= 0 && p->m1_n && !p->mreq_n && !p->rd_n && p->addr == (unsigned)hold) {
                if (!in_read) { wait_low = true; in_read = 1; hold_until = h + 2 * hold_t; p->wait_n = false; }
            } else in_read = 0;
            if (wait_low && (h >= hold_until || !p->reset_n)) { wait_low = false; p->wait_n = true; }
            apply_events(p, rel);
            if (!p->reset_n && !p->wait_n) { wait_low = false; p->wait_n = true; }
            printf("%d %d %04x %02x %d %d %d %d %d %d %d %d\n", rel, clk, p->addr, p->data_out,
                   p->mreq_n, p->iorq_n, p->rd_n, p->wr_n, p->m1_n, p->rfsh_n, p->halt_n, p->busack_n);
            if (rel >= 2 * run - 1) break;
        }
        z80_sys_phase(&s);
        if (s.cpu.phi == 0) {             /* that was a rising edge: the catcher's clear */
            nmi_clr = nmi_hold;
            if (nmi_clr) nmi_hold = false;
            p->nmi_n = nmi_pin && !nmi_hold;
        }
    }
    if (!found) printf("marker not reached\n");
    return 0;
}
