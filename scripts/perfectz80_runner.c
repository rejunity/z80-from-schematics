/* perfectz80_runner.c - run a .hex program through the perfectz80 gate-level
   Z80 simulator (Brian Silverman et al's transistor-level netlist port of the
   Visual Z80 die scan; MIT) and dump a per-half-cycle pin trace in the same
   shape as scripts/tracegen.c / tb_z80.v so timing can be compared.

   Output columns (tab-separated for legibility, space-separated as in
   tracegen):
     phase addr data_o data_i mreq iorq rd wr m1 rfsh halt

   Usage: perfectz80_runner <prog.hex> <num_phases> [nmi_phase]

   Notes: perfectz80 is gate-level slow (~10k phases/s here). Keep `num_phases`
   small. Reset itself consumes several cycles internally inside
   cpu_initAndResetChip; the first dumped phase is the moment reset releases,
   so cycle counts align with our tracegen's "reset state" line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "refs/perfectz80/perfectz80.h"

static void load_hex(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(2); }
    unsigned addr = 0; char tok[64];
    while (fscanf(f, "%63s", tok) == 1) {
        if (tok[0] == '@') addr = (unsigned)strtoul(tok + 1, NULL, 16) & 0xFFFF;
        else { cpu_memory[addr & 0xFFFF] = (unsigned char)strtoul(tok, NULL, 16); addr++; }
    }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr,"usage: %s <prog.hex> <num_phases> [nmi_phase]\n", argv[0]); return 2; }
    int phases = atoi(argv[2]);
    int nmi_phase = (argc>3) ? atoi(argv[3]) : -1;
    memset(cpu_memory, 0, sizeof cpu_memory);
    load_hex(argv[1]);

    void* st = cpu_initAndResetChip();
    /* After initAndReset, perfectz80 is past reset. We dump from here. */
    for (int i = 0; i < phases; i++) {
        /* present read data from our memory just like the iverilog/Verilator
           harnesses do (so all three see the same bus). perfectz80's internal
           memory accessor is already wired to cpu_memory, so for memory cycles
           we don't have to drive the data bus ourselves; for I/O reads we
           drive 0 to match tracegen. */
        if (i == nmi_phase) cpu_writeNMI(st, false);
        else                cpu_writeNMI(st, true);
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
