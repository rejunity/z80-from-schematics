/* ============================================================================
 * z80_trace.c - shared bus-cycle trace format (docs/timing.md / brief).
 * Emits one record per phase. Active-low pins are printed at their raw level
 * (0 = asserted). The same column layout is produced by the RTL testbench so
 * the C and Verilog traces diff line-for-line (see scripts/compare_traces.py).
 * ==========================================================================*/
#include <stdio.h>
#include "z80.h"

void z80_trace_capture(const z80_t *cpu, z80_trace_rec_t *r)
{
    r->cycle = cpu->cycle;
    r->phase = cpu->phi;
    r->t_state = cpu->t_state;
    r->m_cycle = cpu->m_cycle;
    r->pc = cpu->rf[RFP_PC];
    r->ir = cpu->ir;
    r->prefix_state = (uint8_t)cpu->prefix;
    r->addr = cpu->pins.addr;
    r->data_out = cpu->pins.data_out;
    r->data_in = cpu->pins.data_in;
    r->mreq = (uint8_t)cpu->pins.mreq_n;
    r->iorq = (uint8_t)cpu->pins.iorq_n;
    r->rd = (uint8_t)cpu->pins.rd_n;
    r->wr = (uint8_t)cpu->pins.wr_n;
    r->m1 = (uint8_t)cpu->pins.m1_n;
    r->rfsh = (uint8_t)cpu->pins.rfsh_n;
    r->halt = (uint8_t)cpu->pins.halt_n;
    r->busack = (uint8_t)cpu->pins.busack_n;
    r->wait = (uint8_t)cpu->pins.wait_n;
    r->intr = (uint8_t)cpu->pins.int_n;
    r->nmi = (uint8_t)cpu->pins.nmi_n;
}

void z80_trace_header(void *fp)
{
    fprintf((FILE *)fp,
        "# t phi m  addr data_o data_i mreq iorq rd wr m1 rfsh halt busack\n");
}

void z80_trace_emit(void *fp, const z80_trace_rec_t *r)
{
    /* lowercase hex to match Verilog $display, so traces diff byte-for-byte */
    fprintf((FILE *)fp,
        "%u %u %u %04x %02x %02x %u %u %u %u %u %u %u %u\n",
        r->t_state, r->phase, r->m_cycle,
        r->addr, r->data_out, r->data_in,
        r->mreq, r->iorq, r->rd, r->wr, r->m1, r->rfsh, r->halt, r->busack);
}
