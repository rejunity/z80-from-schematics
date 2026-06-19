/* ============================================================================
 * z80_sim.c - system model: external memory + I/O responding to the core bus.
 * ==========================================================================*/
#include <string.h>
#include "z80_sim.h"

void z80_sys_init(z80_system_t *s)
{
    memset(s, 0, sizeof(*s));
    z80_init(&s->cpu);
}

void z80_sys_load(z80_system_t *s, uint16_t addr, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++)
        s->mem[(addr + i) & 0xFFFF] = data[i];
}

void z80_sys_phase(z80_system_t *s)
{
    z80_t *c = &s->cpu;

    /* advance the core one phase. The latch inside uses data_in as presented
       after the previous phase (the address is stable across a read M-cycle). */
    z80_phase_step(c);

    /* capture writes from the just-driven outputs */
    if (!c->pins.mreq_n && !c->pins.wr_n)
        s->mem[c->pins.addr] = c->pins.data_out;
    else if (!c->pins.iorq_n && !c->pins.wr_n)
        s->io[c->pins.addr] = c->pins.data_out;

    /* present read data combinationally for the just-driven bus state, exactly
       like the RTL testbench's memory (so C and Verilog traces match). */
    if (!c->pins.mreq_n && !c->pins.rd_n)
        c->pins.data_in = s->mem[c->pins.addr];
    else if (!c->pins.iorq_n && !c->pins.rd_n)
        c->pins.data_in = s->io_ula_idle
            ? ((c->pins.addr & 1) ? 0xFFu : 0xBFu)
            : s->io[c->pins.addr];
    else
        c->pins.data_in = 0;
}

int z80_sys_step_instr(z80_system_t *s)
{
    uint64_t start = s->cpu.instr_count;
    int n = 0;
    while (s->cpu.instr_count == start && n < 100000) {
        z80_sys_phase(s);
        n++;
    }
    return n;
}

int z80_sys_run_instrs(z80_system_t *s, int count)
{
    int total = 0;
    for (int i = 0; i < count; i++)
        total += z80_sys_step_instr(s);
    return total;
}
