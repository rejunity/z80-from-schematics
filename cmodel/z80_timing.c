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
#include <assert.h>
#include <string.h>
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
        /* Refresh strobe is asserted during T3..T4 of every M1 cycle,
         * including extended-length M1s (NMI ack = 5T, INTA = 7T). Per
         * perfectz80's gate-level trace, silicon deasserts RFSH at T4
         * end EVEN IF the M1 continues into T5+. Bound to T3..T4
         * exactly. (Earlier `t_state >= 3` left RFSH asserted into
         * T5+, causing 2-phase late deassert on NMI/INT ack M-cycles
         * vs perfectz80 — `prog10_halt_nmi` phase 52/53 and
         * `prog17_reset` phase 50/51.) */
        bool refresh = (t_state == 3 || t_state == 4);
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
        /* The phase engine holds WR low when WAIT repeats T2 as Tw.P. */
        bool active = !(t_state == 1 && phi == 0) &&
                      !(t_state == 3 && phi == 1) && t_state <= 3;
        *mreq_n = active ? 0 : 1;
        /* Data from T1.N through T3.N: it stays driven past WR's rise at
           T3.N and floats at the next rising edge (Zilog PS0178 Figure 6,
           TdWRr(D) / TdCr(Dz); perfectz80's data bus floats at T1.P). */
        *data_drive = !(t_state == 1 && phi == 0) && t_state <= 3;
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
        /* WR falls with IORQ at T2.P, unlike a memory write's T2.N
           (PS0178 Figure 7, TdCr(WRf); perfectz80) */
        *wr_n = active ? 0 : 1;
        break;
    }
    case BUSOP_INTA: {
        /* interrupt acknowledge: M1+IORQ */
        *m1_n = (t_state <= 4) ? 0 : 1;
        *iorq_n = ((t_state == 3 && phi) || t_state == 4) ? 0 : 1;
        if (t_state == 5 || t_state == 6) *addr = (uint16_t)((reg_i << 8) | reg_r);
        *rfsh_n = (t_state == 5 || t_state == 6) ? 0 : 1;
        *mreq_n = ((t_state == 5 && phi) || (t_state == 6 && !phi)) ? 0 : 1;
        break;
    }
    default: /* BUSOP_INTERNAL / NONE: no bus activity, hold address */
        break;
    }
}

/* ============================================================================
 * The timing ROM: z80_timing() compiled into a table.
 *
 * The strobes and the data enable are a function of (bus_op, t_state, phi)
 * alone, and the address only selects between m_addr and the refresh I:R;
 * data_out is m_wdata in every state. z80_timing_rom_init() evaluates the
 * function above over that space once, so the phase engine drives the pins
 * with one lookup and the function stays the readable source of truth. From
 * T7 on no bus cycle asserts anything, so t_state saturates at 15.
 *
 * An entry holds the pins as the bytes of z80_pins_t from data_drive through
 * rfsh_n (adjacent bools), ready to copy over the core's pins. z80_reset()
 * calls the builder; a second build would write the same bytes.
 * ==========================================================================*/
/* the image must fit an entry's lvl[] (C99 static assert) */
typedef char z80_timing_rom_fits[
    (Z80_TIMING_ROM_LEN <= sizeof(((z80_timing_rom_t *)0)->lvl)) ? 1 : -1];

z80_timing_rom_t z80_timing_rom[Z80_TIMING_ROM_SIZE];
static bool rom_built;

static void timing_pins(unsigned op, unsigned t, unsigned phi, uint8_t m_len,
                        uint16_t m_addr, uint8_t m_wdata, uint8_t i, uint8_t r,
                        z80_pins_t *p)
{
    memset(p, 0, sizeof *p);
    z80_timing((uint8_t)op, (uint8_t)t, (uint8_t)phi, m_len, m_addr, m_wdata, i, r,
               &p->addr, &p->data_out, &p->data_drive,
               &p->m1_n, &p->mreq_n, &p->iorq_n, &p->rd_n, &p->wr_n, &p->rfsh_n);
}

void z80_timing_rom_init(void)
{
    if (rom_built) return;
    for (unsigned op = 0; op < 8; op++)
        for (unsigned t = 0; t < 16; t++)
            for (unsigned phi = 0; phi < 2; phi++) {
                z80_pins_t p, q;
                /* m_addr 0000 against I:R FFFF tells the refresh select */
                timing_pins(op, t, phi, 0, 0x0000, 0x00, 0xFF, 0xFF, &p);
                z80_timing_rom_t *e = &z80_timing_rom[Z80_TIMING_ROM_INDEX(op, t, phi)];
                memset(e, 0, sizeof *e);
                memcpy(e->lvl, (const uint8_t *)&p + Z80_TIMING_ROM_FIRST, Z80_TIMING_ROM_LEN);
                e->refresh = (p.addr == 0xFFFF);
                /* the ROM's premises: no m_len dependence, the address is
                   m_addr or I:R, data_out is m_wdata */
                timing_pins(op, t, phi, (uint8_t)t, 0x1234, 0x5A, 0xAB, 0xCD, &q);
                assert(memcmp((const uint8_t *)&q + Z80_TIMING_ROM_FIRST, e->lvl,
                              Z80_TIMING_ROM_LEN) == 0);
                assert(q.addr == (e->refresh ? 0xABCD : 0x1234) && q.data_out == 0x5A);
            }
    rom_built = true;
}
