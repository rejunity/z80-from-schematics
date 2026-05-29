/* ============================================================================
 * tracegen.c - C-model trace generator. Loads a hex program, runs N phases,
 * emits the shared bus-cycle trace (docs/timing.md) to stdout. Compared
 * against the RTL testbench trace by scripts/compare_traces.py.
 *
 *   tracegen <prog.hex> <num_phases>
 *
 * Program file: whitespace-separated 2-digit hex bytes; optional @HEX sets the
 * load address (same convention as Verilog $readmemh).
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "z80_sim.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <prog.hex> <num_phases> [nmi_phase]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    int phases = atoi(argv[2]);
    int nmi_phase = (argc > 3) ? atoi(argv[3]) : -1;  /* pulse nmi_n low at this phase */

    z80_system_t s;
    z80_sys_init(&s);

    unsigned addr = 0;
    char tok[64];
    while (fscanf(f, "%63s", tok) == 1) {
        if (tok[0] == '@') {
            addr = (unsigned)strtoul(tok + 1, NULL, 16) & 0xFFFF;
        } else if (tok[0] == '/' || tok[0] == '#' || tok[0] == ';') {
            int ch; while ((ch = fgetc(f)) != '\n' && ch != EOF) {} /* comment line */
        } else {
            unsigned v = (unsigned)strtoul(tok, NULL, 16);
            s.mem[addr & 0xFFFF] = (uint8_t)v;
            addr++;
        }
    }
    fclose(f);

    z80_trace_header(stdout);
    for (int i = 0; i < phases; i++) {
        s.cpu.pins.nmi_n = (i == nmi_phase) ? 0 : 1;   /* NMI edge at nmi_phase */
        z80_sys_phase(&s);
        z80_trace_rec_t r;
        z80_trace_capture(&s.cpu, &r);
        z80_trace_emit(stdout, &r);
    }
    return 0;
}
