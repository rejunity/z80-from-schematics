/* perfectz80_runner.c - run a .hex program through the perfectz80 gate-level
   Z80 simulator (Brian Silverman et al's transistor-level netlist port of the
   Visual Z80 die scan; MIT) and dump a per-half-cycle pin trace in the same
   shape as scripts/tracegen.c / tb_z80.v so timing can be compared.

   Output columns (tab-separated for legibility, space-separated as in
   tracegen):
     phase addr data_o data_i mreq iorq rd wr m1 rfsh halt

   Usage: perfectz80_runner <prog.hex> <num_phases> [events_file]

   Notes: perfectz80 is gate-level slow (~10k phases/s here). Keep `num_phases`
   small. Reset itself consumes several cycles internally inside
   cpu_initAndResetChip; the first dumped phase is the moment reset releases,
   so cycle counts align with our tracegen's "reset state" line.

   Events file (optional): per-line `<phase> <pin> <value>` events that drive
   the gate-level netlist's input pins via cpu_writeNMI / cpu_writeINT /
   cpu_writeWAIT / cpu_writeRESET. This is the SAME format consumed by
   scripts/tracegen.c — a single sidecar exercises both harnesses identically.
   BUSREQ is currently not wired in perfectz80's public API; busreq events
   are accepted but printed as a warning and skipped here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "refs/perfectz80/perfectz80.h"

typedef struct { int phase; int pin; int value; } pinevent_t;
static pinevent_t s_events[1024];
static int        s_n_events = 0;

static int events_load(const char *path) {
    if (!path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    static const char *names[] = {"nmi","int","wait","busreq","reset"};
    while (fgets(line, sizeof line, f) && s_n_events < 1024) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == 0) continue;
        int phase, value;
        char pin[16];
        if (sscanf(line, "%d %15s %d", &phase, pin, &value) != 3) continue;
        int idx = -1;
        for (int i = 0; i < 5; i++) if (!strcmp(pin, names[i])) { idx = i; break; }
        if (idx < 0) { fprintf(stderr,"events: unknown pin '%s'\n", pin); continue; }
        s_events[s_n_events++] = (pinevent_t){phase, idx, value};
    }
    fclose(f);
    return s_n_events;
}

static void events_apply(void *st, int phase) {
    for (int i = 0; i < s_n_events; i++) {
        if (s_events[i].phase != phase) continue;
        bool high = s_events[i].value ? true : false;
        switch (s_events[i].pin) {
        case 0: cpu_writeNMI  (st, high); break;
        case 1: cpu_writeINT  (st, high); break;
        case 2: cpu_writeWAIT (st, high); break;
        case 3: fprintf(stderr,"events: busreq not wired in perfectz80 API; skipping phase %d\n",
                        s_events[i].phase); break;
        case 4: cpu_writeRESET(st, high); break;
        }
    }
}

static void load_hex(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(2); }
    unsigned addr = 0; char tok[64];
    while (fscanf(f, "%63s", tok) == 1) {
        if (tok[0] == '@') {
            addr = (unsigned)strtoul(tok + 1, NULL, 16) & 0xFFFF;
        } else if (tok[0] == '/' || tok[0] == '#' || tok[0] == ';') {
            /* Skip the rest of the comment line. Matches the C tracegen
             * loader in scripts/tracegen.c so the same .hex file produces
             * identical memory images on both sides. */
            int ch; while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
        } else {
            cpu_memory[addr & 0xFFFF] = (unsigned char)strtoul(tok, NULL, 16);
            addr++;
        }
    }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr,"usage: %s <prog.hex> <num_phases> [events_file]\n", argv[0]); return 2; }
    int phases = atoi(argv[2]);
    const char *events_path = (argc > 3) ? argv[3] : NULL;
    memset(cpu_memory, 0, sizeof cpu_memory);
    load_hex(argv[1]);
    int n_ev = events_load(events_path);
    if (events_path)
        fprintf(stderr, "perfectz80: loaded %d event(s) from %s\n", n_ev, events_path);

    void* st = cpu_initAndResetChip();
    /* After initAndReset, perfectz80 is past reset. We dump from here. */
    for (int i = 0; i < phases; i++) {
        /* apply pin-events for THIS phase (override default high). default
           any unspecified-event pin to high. We re-apply every phase so a
           single 1-phase pulse stays pulsed only for that phase unless
           the sidecar also specifies the release. */
        cpu_writeNMI(st, true); cpu_writeINT(st, true);
        cpu_writeWAIT(st, true);
        events_apply(st, i);

        /* present read data from our memory just like the iverilog/Verilator
           harnesses do (so all three see the same bus). perfectz80's internal
           memory accessor is already wired to cpu_memory, so for memory cycles
           we don't have to drive the data bus ourselves; for I/O reads we
           drive 0 to match tracegen. */
        if (!cpu_readMREQ(st) && cpu_readRD(st) == 0) {
            /* nothing to do — perfectz80 handles its own mem read */
        }
        if (!cpu_readIORQ(st) && cpu_readRD(st) == 0) {
            cpu_writeDataBus(st, 0);
        }
        unsigned addr   = cpu_readAddressBus(st);
        unsigned data_o = cpu_readDataBus(st);
        /* data_i is the value that will be latched next; for memory reads we
           use cpu_memory (matches our harness). */
        unsigned data_i = 0;
        if (!cpu_readMREQ(st) && !cpu_readRD(st)) data_i = cpu_memory[addr & 0xFFFF];
        printf("%d %04x %02x %02x %u %u %u %u %u %u %u\n",
            i, addr, data_o, data_i,
            cpu_readMREQ(st)?1:0, cpu_readIORQ(st)?1:0,
            cpu_readRD(st)?1:0,  cpu_readWR(st)?1:0,
            cpu_readM1(st)?1:0,  cpu_readRFSH(st)?1:0,
            cpu_readHALT(st)?1:0);
        cpu_step(st);
    }
    cpu_destroyChip(st);
    return 0;
}
