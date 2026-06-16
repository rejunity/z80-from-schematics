// ===========================================================================
// sim_main.cpp - Verilator testbench for z80_core. Wires the core to a 64K
// memory and emits the shared bus-cycle trace (one line per phase) to stdout,
// matching scripts/tracegen.c and tests/iverilog/tb_z80.v exactly.
//
//   sim_z80 <prog.hex> <num_phases>
//
// The per-phase loop mirrors cmodel/z80_sim.c::z80_sys_phase: clock edge first
// (latch + new outputs), then service writes and present read data.
// ===========================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Vz80_core.h"
#include "verilated.h"

static unsigned char mem[65536];

static bool read_active(Vz80_core *t) {
    return (!t->mreq_n && !t->rd_n) || (!t->iorq_n && !t->rd_n);
}
static bool write_active(Vz80_core *t) {
    return (!t->mreq_n && !t->wr_n) || (!t->iorq_n && !t->wr_n);
}

static void present_read(Vz80_core *t) {
    if (read_active(t)) t->data_in = mem[t->addr & 0xFFFF];
    else                t->data_in = 0;
}

static void dump(Vz80_core *t) {
    printf("%u %u %u %04x %02x %02x %u %u %u %u %u %u %u %u\n",
           (unsigned)t->dbg_t, (unsigned)t->dbg_phi, (unsigned)t->dbg_m,
           (unsigned)t->addr, (unsigned)t->data_out, (unsigned)t->data_in,
           (unsigned)t->mreq_n, (unsigned)t->iorq_n, (unsigned)t->rd_n,
           (unsigned)t->wr_n, (unsigned)t->m1_n, (unsigned)t->rfsh_n,
           (unsigned)t->halt_n, (unsigned)t->busack_n);
}

static void load_hex(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    unsigned addr = 0; char tok[64];
    while (fscanf(f, "%63s", tok) == 1) {
        if (tok[0] == '@') {
            addr = (unsigned)strtoul(tok + 1, NULL, 16) & 0xFFFF;
        } else if (tok[0] == '/' || tok[0] == '#' || tok[0] == ';') {
            /* Skip the rest of the comment line. Matches the loader in
             * scripts/tracegen.c and scripts/perfectz80_runner.c so the
             * same .hex file produces identical memory images on every
             * harness. */
            int ch; while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
        } else {
            mem[addr & 0xFFFF] = (unsigned char)strtoul(tok, NULL, 16);
            addr++;
        }
    }
    fclose(f);
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    if (argc < 3) { fprintf(stderr, "usage: %s <prog.hex> <phases> [nmi_phase]\n", argv[0]); return 2; }
    int phases = atoi(argv[2]);
    int nmi_phase = (argc > 3) ? atoi(argv[3]) : -1;
    memset(mem, 0, sizeof(mem));
    load_hex(argv[1]);

    Vz80_core *top = new Vz80_core;
    top->wait_n = 1; top->int_n = 1; top->nmi_n = 1; top->busreq_n = 1;
    top->data_in = 0;

    // assert async reset
    top->reset_n = 0; top->clk = 0; top->eval();
    top->clk = 1; top->eval();
    top->clk = 0; top->eval();
    top->reset_n = 1;

    // line 1 = reset state (T1.P)
    present_read(top); top->eval();
    dump(top);

    for (int i = 1; i < phases; i++) {
        // pulse NMI low for one phase at nmi_phase (matches iverilog/tb_z80.v)
        top->nmi_n = (i == nmi_phase) ? 0 : 1;
        top->clk = 1; top->eval();      // posedge: latch + next state
        top->clk = 0; top->eval();      // settle new outputs
        if (write_active(top)) mem[top->addr & 0xFFFF] = top->data_out;
        present_read(top); top->eval(); // present read data for this phase
        dump(top);
    }

    top->final();
    delete top;
    return 0;
}
