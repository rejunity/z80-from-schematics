/* ============================================================================
 * z80_sim.h - simple system model (Z80 + 64K memory + 64K I/O space) used by
 * tests, trace generation, and the ZEX harness. Mirrors how the RTL testbench
 * wires external memory to the core's bus.
 * ==========================================================================*/
#ifndef Z80_SIM_H
#define Z80_SIM_H

#include <stddef.h>
#include "z80.h"

typedef struct {
    z80_t   cpu;
    uint8_t mem[65536];
    uint8_t io[65536];
    /* When non-zero, IORD ignores `io[]` and returns the ZX Spectrum ULA
     * idle pattern (`(addr & 1) ? 0xFF : 0xBF`). Required to match Patrik
     * Rak's z80test INI/IND CRC values, which were captured against real
     * Spectrum silicon: OUTs land on no-device ports so subsequent INs
     * still see the static ULA pattern. With io[] write-through, OUTs
     * corrupt the read-side and INI/IND CRC drifts. Default 0. Set by
     * scripts/z80test_runner.c. */
    int     io_ula_idle;
} z80_system_t;

void z80_sys_init(z80_system_t *s);
void z80_sys_phase(z80_system_t *s);              /* advance one phase */
void z80_sys_load(z80_system_t *s, uint16_t addr, const uint8_t *data, size_t n);
int  z80_sys_step_instr(z80_system_t *s);          /* run one full instruction */
int  z80_sys_run_instrs(z80_system_t *s, int n);   /* run n instructions */

#endif /* Z80_SIM_H */
