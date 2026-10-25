/* ============================================================================
 * dump_tables.c - print the reference model's timing tables as JSON for the
 * cfast generator (cfast/gen_fast.py): per (bus_op, t_state, phi) the pins of
 * the timing ROM (z80_timing), the core's timing points (WAIT sample, data
 * latch, refresh end, the address held), each bus cycle's base length, and the
 * M-cycles with a register write inside them (wb_mid). Built as one unit
 * with cmodel/ so the core's file-static predicates are in reach.
 * ==========================================================================*/
#include <stdio.h>
#include "../cmodel/z80_alu.c"
#include "../cmodel/z80_core.c"
#include "../cmodel/z80_pla.c"
#include "../cmodel/z80_regfile.c"
#include "../cmodel/z80_timing.c"

int main(void)
{
    static const char *busop[8] = { "NONE", "M1", "MRD", "MWR", "IORD", "IOWR", "INTA", "INTERNAL" };
    z80_t c;
    z80_init(&c);                 /* builds the ROMs */
    printf("{\n  \"busops\": {");
    for (int op = 0; op < 8; op++)
        printf("%s\"%s\": [%d, %u]", op ? ", " : "", busop[op], op, z80_busop_base_len((uint8_t)op));
    printf("},\n  \"phases\": [\n");
    int first = 1;
    for (unsigned op = 0; op < 8; op++)
        for (unsigned t = 0; t < 16; t++)
            for (unsigned phi = 0; phi < 2; phi++) {
                unsigned ix = Z80_TIMING_ROM_INDEX(op, t, phi);
                z80_pins_t p;
                memset(&p, 0, sizeof p);
                memcpy((uint8_t *)&p + Z80_TIMING_ROM_FIRST, z80_timing_rom[ix].lvl, Z80_TIMING_ROM_LEN);
                z80_t s;
                memset(&s, 0, sizeof s);
                s.bus_op = (uint8_t)op; s.t_state = (uint8_t)t; s.phi = (uint8_t)phi;
                printf("%s    {\"op\": %u, \"t\": %u, \"phi\": %u, \"m1\": %d, \"mreq\": %d, "
                       "\"iorq\": %d, \"rd\": %d, \"wr\": %d, \"rfsh\": %d, \"doe\": %d, "
                       "\"refresh\": %d, \"wait_sample\": %d, \"latch\": %d, \"refresh_end\": %d, "
                       "\"held\": %d}",
                       first ? "" : ",\n", op, t, phi,
                       !p.m1_n, !p.mreq_n, !p.iorq_n, !p.rd_n, !p.wr_n, !p.rfsh_n, p.data_drive,
                       z80_timing_rom[ix].refresh,
                       (tpoint_rom[ix] & TP_WAIT_SAMPLE) != 0, (tpoint_rom[ix] & TP_LATCH) != 0,
                       is_refresh_end(&s), (tpoint_rom[ix] & TP_ADDR_HELD) != 0);
                first = 0;
            }
    /* the M-cycles wb_mid() names (cmodel/z80_core.c): their template and the
       edge -- the end of T-state t -- of the write wb_hooks() makes in them;
       cfast runs them in step variants (gen_fast.py) */
    static const struct { unsigned op, len, t; } mid[] = {
        { BUSOP_MWR, 3, 1 },      /* EX (SP),HL's first write: SP+1 */
        { BUSOP_MRD, 3, 2 },      /* DJNZ's offset read, OUTI's read: B-1 */
        { BUSOP_MWR, 5, 3 },      /* LDI's write: DE */
        { BUSOP_IORD, 4, 2 },     /* INI's read: B-1 */
    };
    printf("\n  ],\n  \"wb_mid\": [");
    for (unsigned i = 0; i < sizeof mid / sizeof mid[0]; i++)
        printf("%s{\"op\": \"%s\", \"len\": %u, \"t\": %u}", i ? ", " : "",
               busop[mid[i].op], mid[i].len, mid[i].t);
    printf("]\n}\n");
    return 0;
}
