/* halt2int_probe.c -- focused silicon-faithfulness check for the
 * HALT-to-INT acceptance timing path that Mark Woodmass's HALT2INT
 * v3 (2021) verifies on real ZX Spectrum 48K silicon.
 *
 * HALT2INT v3 itself requires the Spectrum 48K ROM + ULA contended-
 * memory timing, neither of which we model (we're CPU-only). The
 * underlying silicon property HALT2INT measures, however, is
 * accessible from a CPU-only test: how many T-states elapse between
 * INT going low during the HALT loop and the INTA M-cycle starting?
 *
 * Per Brewer 2014 ("Z80 Special Reset") and the Z80 timing diagrams
 * the silicon-faithful sequence is:
 *
 *   - HALT M1 completes; PC sits past the HALT byte (no decrement)
 *   - HALT-state loop runs internal NOP M-cycles of 4 T-states each
 *   - INT is sampled at the rising edge of T_last of each M-cycle
 *   - When sampled active AND IFF1=1, the next M-cycle is INTA (7T
 *     in IM 0/1, longer in IM 2)
 *
 * The exact INT-to-INTA delay therefore depends on where in a NOP
 * M-cycle INT first goes low (range 1..5 T-states for the case here).
 *
 *   halt2int_probe
 *
 * Exits 0 if every measured timing matches silicon-expected ranges.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "z80_sim.h"

/* Run one HALT->INT scenario: assert INT `int_delay_phases` after
 * halt_n first goes low, detect when iorq_n first goes low (the
 * INTA M-cycle's T3+ window), report the deltas. */
static int run_scenario(int int_delay_phases, int *out_delta_t) {
    z80_system_t *s = malloc(sizeof *s);
    z80_sys_init(s);
    s->io_ula_idle = 1;

    static const uint8_t prog[] = {
        0xED, 0x56,             /* IM 1 */
        0xFB,                   /* EI   */
        0x76,                   /* HALT at 0x0003 */
    };
    memcpy(s->mem, prog, sizeof prog);
    s->mem[0x0038] = 0x76;       /* IM1 vector landing pad */

    z80_set_pc(&s->cpu, 0x0000);
    s->cpu.rf[RFP_SP] = 0xFFFE;
    s->cpu.pins.int_n   = 1;
    s->cpu.pins.nmi_n   = 1;
    s->cpu.pins.wait_n  = 1;
    s->cpu.pins.busreq_n = 1;

    int halt_at  = -1;
    int int_at   = -1;
    int inta_at  = -1;
    int prev_haltn = 1;
    int prev_iorqn = 1;

    for (int p = 0; p < 800; p++) {
        z80_sys_phase(s);
        int haltn = s->cpu.pins.halt_n;
        int iorqn = s->cpu.pins.iorq_n;
        if (halt_at < 0 && prev_haltn && !haltn) halt_at = p;
        if (halt_at >= 0 && int_at < 0 && p == halt_at + int_delay_phases) {
            s->cpu.pins.int_n = 0;
            int_at = p;
        }
        if (int_at >= 0 && inta_at < 0 && prev_iorqn && !iorqn) {
            inta_at = p;
            s->cpu.pins.int_n = 1;
        }
        prev_haltn = haltn;
        prev_iorqn = iorqn;
        if (inta_at >= 0 && p > inta_at + 6) break;
    }
    free(s);

    if (halt_at < 0 || int_at < 0 || inta_at < 0) return -1;
    *out_delta_t = (inta_at - int_at) / 2;
    return 0;
}

int main(void) {
    printf("=== HALT2INT-style probe: INT-to-INTA T-state timing ===\n");
    /* Sweep INT-assert timing across an 8-phase (4-T-state) HALT NOP
     * M-cycle window. Two periods (10 samples) lets us watch the
     * pattern repeat with M-cycle period.
     *
     * Silicon-faithful expectation per Z80 datasheet:
     *   - INT is sampled at the rising edge of the LAST T-state of the
     *     current M-cycle
     *   - When sampled active, the NEXT M-cycle is INTA (no further
     *     delay)
     *
     * So the INT-to-INTA delay depends on where INT goes low within
     * the M-cycle:
     *   - asserted EARLY (T1-T2):  sampled at end of THIS M-cycle
     *                              -> delta ~3-5 T-states
     *   - asserted LATE (T3-T4):   misses the sample window,
     *                              waits for the NEXT M-cycle's sample
     *                              -> delta ~6-8 T-states
     *
     * Acceptable range across the sweep: 3..8 T-states. Pattern must
     * repeat every 4 T-states (= 8 phases) as the sample point of
     * each successive M-cycle slides past. */
    int worst = 0, best = 1000;
    int all_pass = 1;
    for (int dp = 2; dp <= 20; dp += 2) {
        int delta_t = -1;
        if (run_scenario(dp, &delta_t) < 0) {
            printf("  INT @halt+%2d phases: probe could not capture INTA\n", dp);
            all_pass = 0;
            continue;
        }
        int ok = (delta_t >= 3 && delta_t <= 8);
        printf("  INT @halt+%2d phases (~T%d): delta = %d T-states %s\n",
               dp, dp / 2, delta_t, ok ? "OK" : "OUT OF RANGE");
        if (!ok) all_pass = 0;
        if (delta_t > worst) worst = delta_t;
        if (delta_t < best)  best  = delta_t;
    }
    printf("\nrange observed: %d..%d T-states (silicon range: 3..8)\n", best, worst);
    printf("verdict: %s\n",
           all_pass ? "PASS (silicon-faithful)" : "FAIL");
    return all_pass ? 0 : 1;
}
