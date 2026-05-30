/* ============================================================================
 * z80.c - top-level model: init/reset and the per-phase engine.
 *
 * The engine advances one phase (half T-state) per call. To keep the external
 * pins consistent with (t_state, phi) for tracing, each call:
 *   1) advances from the phase driven last call (handling wait stalls and
 *      end-of-M-cycle dispatch into the micro-sequencer),
 *   2) samples inputs (read-data latch / wait) for the phase about to drive,
 *   3) drives the external pins for that phase.
 * After the call, the pins and (t_state, phi) describe the same phase.
 * ==========================================================================*/
#include <string.h>
#include "z80_internal.h"

/* ---- timing-point predicates ---- */

static bool is_wait_phase(const z80_t *c)
{
    if (c->phi != 1) return false;
    switch (c->bus_op) {
        case BUSOP_M1:
        case BUSOP_MRD:
        case BUSOP_MWR:  return c->t_state == 2;
        case BUSOP_IORD:
        case BUSOP_IOWR: return c->t_state == 3; /* the automatic Tw */
        default:         return false;
    }
}

static bool is_latch_phase(const z80_t *c)
{
    if (c->phi != 1) return false;
    switch (c->bus_op) {
        case BUSOP_M1:   return c->t_state == 2; /* opcode latched at T2.N */
        case BUSOP_INTA: return c->t_state == 2; /* interrupt vector byte  */
        case BUSOP_MRD:  return c->t_state == 3; /* data at T3.N           */
        case BUSOP_IORD: return c->t_state == 4; /* data at T3 (after Tw)  */
        default:         return false;
    }
}

static void do_latch(z80_t *c)
{
    switch (c->bus_op) {
    case BUSOP_M1:
        c->ir   = c->pins.data_in;
        c->tmp8 = c->pins.data_in;
        /* refresh counter: only low 7 bits increment */
        c->reg_r = (uint8_t)((c->reg_r & 0x80u) | ((c->reg_r + 1u) & 0x7Fu));
        if (c->suppress_decode) {
            c->suppress_decode = false;      /* ack M1: discard opcode, no PC++/decode */
        } else {
            (void)z80_pc_inc(c);             /* PC advances past opcode */
            c->ctl = z80_pla_decode(c->prefix, c->ir);
            c->decoded = true;
        }
        break;
    case BUSOP_INTA:                          /* interrupt-ack: latch bus byte, refresh */
        c->tmp8 = c->pins.data_in;
        c->reg_r = (uint8_t)((c->reg_r & 0x80u) | ((c->reg_r + 1u) & 0x7Fu));
        break;
    case BUSOP_MRD:
    case BUSOP_IORD:
        c->tmp8 = c->pins.data_in;
        break;
    default: break;
    }
}

/* set up a synthetic M1-style cycle for an interrupt/HALT sequence */
static void start_seq_m1(z80_t *c, z80_exec_t exec, uint8_t len, bool suppress)
{
    c->ctl.exec = exec; c->ctl.idx = 0; c->ctl.use_disp = false;
    c->decoded = true; c->instr_done = false;
    c->bus_op = BUSOP_M1; c->m_addr = c->rf[RFP_PC]; c->m_wdata = 0; c->m_len = len;
    c->t_state = 1; c->phi = 0; c->m_cycle = 1; c->ucode = 0;
    c->suppress_decode = suppress;
}
static void start_seq_inta(z80_t *c, uint8_t len)
{
    c->ctl.exec = EXEC_INT; c->ctl.idx = 0; c->ctl.use_disp = false;
    c->decoded = true; c->instr_done = false;
    c->bus_op = BUSOP_INTA; c->m_addr = c->rf[RFP_PC]; c->m_wdata = 0; c->m_len = len;
    c->t_state = 1; c->phi = 0; c->m_cycle = 1; c->ucode = 0;
}

/* decide what happens at an instruction boundary: bus grant, NMI, INT, HALT, or
   the next opcode fetch. (docs/timing.md interrupt/HALT/BUSREQ sections) */
static void begin_next(z80_t *c)
{
    if (!c->pins.busreq_n) { c->bus_granted = true; return; } /* DMA owns the bus */

    bool allow_int = !c->ei_delay;
    c->ei_delay = false;

    if (c->nmi_pending) {
        c->nmi_pending = false;
        if (c->halted) { c->rf[RFP_PC] = (uint16_t)(c->rf[RFP_PC] + 1); c->halted = false; }
        c->iff2 = c->iff1; c->iff1 = false;        /* IFF1->IFF2, disable */
        start_seq_m1(c, EXEC_NMI, 5, true);        /* 5T ack, opcode discarded */
        return;
    }
    if (allow_int && !c->pins.int_n && c->iff1) {
        if (c->halted) { c->rf[RFP_PC] = (uint16_t)(c->rf[RFP_PC] + 1); c->halted = false; }
        c->iff1 = c->iff2 = false;
        start_seq_inta(c, 7);                      /* INTA ack (IM0/1/2) */
        return;
    }
    if (c->halted) {
        start_seq_m1(c, EXEC_NOP, 4, true);        /* HALT: execute NOPs */
        return;
    }
    z80_start_m1(c);
}

static void advance(z80_t *c)
{
    if (c->phi == 0) {
        c->phi = 1;
        return;
    }
    c->phi = 0;
    if (c->stalled) return;                  /* hold this T-state as a Tw */
    c->t_state = (uint8_t)(c->t_state + 1);
    if (c->t_state > c->m_len) {
        z80_exec_step(c);                    /* set up next M-cycle / finish */
        if (c->instr_done) {
            c->instr_count++;
            c->prefix = PFX_NONE;
            begin_next(c);
        }
    }
}

/* ---- reset ---- */

static void reset_state(z80_t *c)
{
    c->rf[RFP_PC] = 0x0000;
    c->reg_i = 0; c->reg_r = 0; c->ir = 0;
    c->iff1 = c->iff2 = false;
    c->im = 0;
    c->halted = false;
    c->prefix = PFX_NONE;
    c->nmi_pending = false;
    c->prev_nmi_n = true;
    c->ei_delay = false;
    c->suppress_decode = false;
    c->bus_granted = false;

    c->t_state = 1; c->phi = 0; c->m_cycle = 1;
    c->bus_op = BUSOP_M1; c->m_len = 4; c->m_addr = 0x0000;
    c->decoded = false; c->instr_done = false; c->ucode = 0;
    c->phase_primed = false; c->stalled = false;

    c->pins.m1_n = c->pins.mreq_n = c->pins.iorq_n = 1;
    c->pins.rd_n = c->pins.wr_n = c->pins.rfsh_n = 1;
    c->pins.halt_n = 1; c->pins.busack_n = 1;
    c->pins.data_drive = false;
    c->pins.addr = 0x0000;
    c->pins.data_out = 0;
}

void z80_reset(z80_t *c)
{
    /* programmer-visible registers are undefined on real silicon; force a
       deterministic state for C<->RTL comparison (docs/known-differences.md). */
    for (int i = 0; i < RFP_COUNT; i++) c->rf[i] = 0xFFFF;
    reset_state(c);
}

/* Seed the program counter from outside (e.g. loading a CP/M .com at 0x0100).
   Also updates the pending M1 fetch address so the next opcode is fetched from
   pc rather than the stale reset address. Only valid at an instruction boundary
   (start of an M1 fetch), which is the case right after reset. */
void z80_set_pc(z80_t *c, uint16_t pc)
{
    c->rf[RFP_PC] = pc;
    if (c->bus_op == BUSOP_M1 && c->t_state == 1 && !c->decoded)
        c->m_addr = pc;
}

void z80_init(z80_t *c)
{
    memset(c, 0, sizeof(*c));
    c->pins.reset_n = 1;
    c->pins.wait_n  = 1;
    c->pins.int_n   = 1;
    c->pins.nmi_n   = 1;
    c->pins.busreq_n = 1;
    z80_reset(c);
}

/* ---- per-phase engine ---- */

void z80_phase_step(z80_t *c)
{
    c->cycle++;

    if (!c->pins.reset_n) {
        reset_state(c);
        return;
    }

    /* NMI is edge-triggered: latch a falling edge on nmi_n */
    if (c->prev_nmi_n && !c->pins.nmi_n) c->nmi_pending = true;
    c->prev_nmi_n = c->pins.nmi_n;

    /* bus grant (DMA): while BUSREQ held, release the bus and idle */
    if (c->bus_granted) {
        if (!c->pins.busreq_n) {
            c->pins.busack_n = 0;
            c->pins.m1_n = c->pins.mreq_n = c->pins.iorq_n = 1;
            c->pins.rd_n = c->pins.wr_n = c->pins.rfsh_n = 1;
            c->pins.data_drive = false;
            c->pins.halt_n = c->halted ? 0 : 1;
            return;
        }
        c->bus_granted = false;          /* BUSREQ released: resume */
        c->pins.busack_n = 1;
        z80_start_m1(c);
        c->phase_primed = false;
    }

    if (c->phase_primed)
        advance(c);
    c->phase_primed = true;

    c->stalled = is_wait_phase(c) && (c->pins.wait_n == 0);

    if (!c->stalled && is_latch_phase(c))
        do_latch(c);

    z80_drive_pins(c);

    /* HALT/refresh pin level: halt_n reflects halted state. */
    c->pins.halt_n = c->halted ? 0 : 1;
}
