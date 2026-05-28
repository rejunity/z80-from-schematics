/* ============================================================================
 * z80_bus.c - bus-cycle setup helpers and M-cycle length table.
 * No internal tri-states: the data bus is an explicit driver + enable in
 * z80_drive_pins(); this file just sets up which kind of bus cycle runs next.
 * ==========================================================================*/
#include "z80_internal.h"

uint8_t z80_busop_base_len(uint8_t busop)
{
    switch (busop) {
        case BUSOP_M1:   return 4;
        case BUSOP_MRD:  return 3;
        case BUSOP_MWR:  return 3;
        case BUSOP_IORD: return 4; /* T1 T2 Tw T3 */
        case BUSOP_IOWR: return 4;
        case BUSOP_INTA: return 5; /* M1-like + 2 wait states */
        default:         return 0; /* INTERNAL: caller supplies length via extra */
    }
}

void z80_start_mcycle(z80_t *cpu, uint8_t busop, uint16_t addr,
                      uint8_t wdata, uint8_t extra_t)
{
    cpu->bus_op  = busop;
    cpu->m_addr  = addr;
    cpu->m_wdata = wdata;
    cpu->m_len   = (uint8_t)(z80_busop_base_len(busop) + extra_t);
    cpu->t_state = 1;
    cpu->phi     = 0;
    cpu->m_cycle = (uint8_t)(cpu->m_cycle + 1);
}

/* Begin an opcode-fetch (M1) cycle for the current PC. Used both for a new
   instruction and to continue after a prefix byte. */
void z80_start_m1(z80_t *cpu)
{
    cpu->bus_op   = BUSOP_M1;
    cpu->m_addr   = cpu->rf[RFP_PC];
    cpu->m_wdata  = 0;
    cpu->m_len    = 4;
    cpu->t_state  = 1;
    cpu->phi      = 0;
    cpu->m_cycle  = 1;
    cpu->ucode    = 0;
    cpu->decoded  = false;
    cpu->instr_done = false;
}
