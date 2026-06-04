/* ============================================================================
 * z80_timing.c - the Z80 timing module: drive external pins as a pure
 * combinational function of the current timing state.
 *
 * Mirrors rtl/z80_timing.v one-to-one. Inputs and outputs match the Verilog
 * port list exactly; the function has no access to z80_t — the caller in
 * z80.c samples cpu->* fields into positional arguments and writes the
 * outputs back into cpu->pins.
 *
 * Convention: t.P = (phi==0), t.N = (phi==1). See docs/timing.md for the
 * phase-by-phase contract this implements.
 * ==========================================================================*/
#include "z80.h"

void z80_timing(uint8_t   bus_op,     /* z80_busop_t                          */
                uint8_t   t_state,    /* 1-based T-state within the M-cycle   */
                uint8_t   phi,        /* 0 = PHI_P, 1 = PHI_N                  */
                uint8_t   m_len,      /* total T-states this M-cycle           */
                uint16_t  m_addr,     /* address being driven this M-cycle    */
                uint8_t   m_wdata,    /* data byte for writes                 */
                uint8_t   reg_i,      /* I register (refresh high byte)       */
                uint8_t   reg_r,      /* R register (refresh low byte)        */
                uint16_t *addr,
                uint8_t  *data_out,
                bool     *data_drive,
                bool     *m1_n,
                bool     *mreq_n,
                bool     *iorq_n,
                bool     *rd_n,
                bool     *wr_n,
                bool     *rfsh_n)
{
    /* defaults: all strobes inactive (high), not driving data */
    *m1_n = *mreq_n = *iorq_n = *rd_n = *wr_n = *rfsh_n = 1;
    *data_drive = false;
    *data_out = m_wdata;     /* always presented; gated by data_drive */
    *addr = m_addr;
    /* halt_n / busack_n are owned by other subsystems; not driven here */

    switch (bus_op) {
    case BUSOP_M1: {
        bool refresh = (t_state >= 3);
        *addr   = refresh ? (uint16_t)(((uint16_t)reg_i << 8) | reg_r)
                          : m_addr;
        *m1_n   = (t_state <= 2) ? 0 : 1;
        *rfsh_n = refresh ? 0 : 1;
        /* opcode fetch MREQ/RD: T1.N .. end of T2 */
        bool fetch_lo = ((t_state == 1 && phi == 1) || (t_state == 2));
        /* refresh MREQ: T3.N .. T4.P */
        bool refr_lo  = ((t_state == 3 && phi == 1) || (t_state == 4 && phi == 0));
        *mreq_n = (fetch_lo || refr_lo) ? 0 : 1;
        *rd_n   = fetch_lo ? 0 : 1;
        break;
    }
    case BUSOP_MRD: {
        /* MREQ/RD active T1.N .. T3.P (deassert at T3.N — falling-edge
           transition matches gate-level / perfectz80 convention). Extra
           T-states beyond T3 (e.g. CB (HL) reads use m_len=4) are internal
           compute padding after the bus cycle completes; MREQ stays high. */
        (void)m_len;
        bool active = !(t_state == 1 && phi == 0) &&
                      !(t_state == 3 && phi == 1) && t_state <= 3;
        *mreq_n = active ? 0 : 1;
        *rd_n   = active ? 0 : 1;
        break;
    }
    case BUSOP_MWR: {
        bool active = !(t_state == 1 && phi == 0) &&
                      !(t_state == 3 && phi == 1) && t_state <= 3;
        *mreq_n = active ? 0 : 1;
        *data_drive = active;
        *data_out = m_wdata;
        *wr_n = ((t_state == 2 && phi == 1) || (t_state == 3 && phi == 0)) ? 0 : 1;
        break;
    }
    case BUSOP_IORD: {
        bool active = (t_state >= 2) && !(t_state == 4 && phi == 1) && t_state <= 4;
        *iorq_n = active ? 0 : 1;
        *rd_n   = active ? 0 : 1;
        break;
    }
    case BUSOP_IOWR: {
        bool active = (t_state >= 2) && !(t_state == 4 && phi == 1) && t_state <= 4;
        *iorq_n = active ? 0 : 1;
        *data_drive = !(t_state == 1 && phi == 0) && t_state <= 4;
        *data_out = m_wdata;
        *wr_n = ((t_state == 2 && phi == 1) || (t_state == 3) || (t_state == 4 && phi == 0)) ? 0 : 1;
        break;
    }
    case BUSOP_INTA: {
        /* interrupt acknowledge: M1+IORQ */
        *m1_n   = (t_state <= 2) ? 0 : 1;
        *iorq_n = (t_state >= 3) ? 0 : 1;
        break;
    }
    default: /* BUSOP_INTERNAL / NONE: no bus activity, hold address */
        break;
    }
}
