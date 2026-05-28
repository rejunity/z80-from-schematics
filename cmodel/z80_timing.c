/* ============================================================================
 * z80_timing.c - drive external pins as a function of the timing state.
 *
 * Outputs are a pure function of (bus_op, t_state, phi, m_addr, m_wdata, I, R),
 * exactly like the registered/combinational outputs of the RTL. See
 * docs/timing.md for the phase-by-phase contract this implements.
 *
 * Convention: t.P = (phi==0), t.N = (phi==1).
 * ==========================================================================*/
#include "z80_internal.h"

void z80_drive_pins(z80_t *cpu)
{
    z80_pins_t *p = &cpu->pins;
    uint8_t t   = cpu->t_state;
    uint8_t phi = cpu->phi;

    /* defaults: all strobes inactive (high), not driving data */
    p->m1_n = p->mreq_n = p->iorq_n = p->rd_n = p->wr_n = p->rfsh_n = 1;
    p->data_drive = false;
    p->data_out = cpu->m_wdata;     /* always presented; gated by data_drive */
    p->addr = cpu->m_addr;
    /* halt_n / busack_n are owned by other subsystems; leave as-is here */

    switch (cpu->bus_op) {
    case BUSOP_M1: {
        bool refresh = (t >= 3);
        p->addr   = refresh ? (uint16_t)(((uint16_t)cpu->reg_i << 8) | cpu->reg_r)
                            : cpu->m_addr;
        p->m1_n   = (t <= 2) ? 0 : 1;
        p->rfsh_n = refresh ? 0 : 1;
        /* opcode fetch MREQ/RD: T1.N .. end of T2 */
        bool fetch_lo = ((t == 1 && phi == 1) || (t == 2));
        /* refresh MREQ: T3.N .. T4.P */
        bool refr_lo  = ((t == 3 && phi == 1) || (t == 4 && phi == 0));
        p->mreq_n = (fetch_lo || refr_lo) ? 0 : 1;
        p->rd_n   = fetch_lo ? 0 : 1;
        break;
    }
    case BUSOP_MRD: {
        bool active = !(t == 1 && phi == 0);   /* T1.N onward */
        p->mreq_n = active ? 0 : 1;
        p->rd_n   = active ? 0 : 1;
        break;
    }
    case BUSOP_MWR: {
        bool active = !(t == 1 && phi == 0);
        p->mreq_n = active ? 0 : 1;
        p->data_drive = active;
        p->data_out = cpu->m_wdata;
        /* WR low from T2.N .. end of T3 */
        p->wr_n = ((t == 2 && phi == 1) || (t == 3)) ? 0 : 1;
        break;
    }
    case BUSOP_IORD: {
        bool active = (t >= 2);                 /* IORQ/RD from T2.P */
        p->iorq_n = active ? 0 : 1;
        p->rd_n   = active ? 0 : 1;
        break;
    }
    case BUSOP_IOWR: {
        bool active = (t >= 2);
        p->iorq_n = active ? 0 : 1;
        p->data_drive = !(t == 1 && phi == 0);
        p->data_out = cpu->m_wdata;
        p->wr_n = ((t == 2 && phi == 1) || (t >= 3)) ? 0 : 1;
        break;
    }
    case BUSOP_INTA: {
        /* interrupt acknowledge: M1+IORQ, handled fully in task 9 */
        p->m1_n = (t <= 2) ? 0 : 1;
        p->iorq_n = (t >= 3) ? 0 : 1;
        break;
    }
    default: /* BUSOP_INTERNAL / NONE: no bus activity, hold address */
        break;
    }
}
