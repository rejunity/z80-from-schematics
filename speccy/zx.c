/* zx.c - see zx.h. cfast's implementation lives in this unit.
 *
 * Each CPU phase is followed by its bus (serve_bus) and then the ULA's
 * pixel clocks up to the one the CPU's next phase takes (to_next_phase):
 * one, or more while the ULA holds the CPU clock. With GCC or Clang the
 * phases run in cfast's run frame (z80f_run, the bus inlined as ZF_BUS);
 * elsewhere a loop of z80f_tick() calls does the same. */
#include "zx.h"
#include <string.h>

/* T-states since power-on (the tape's clock) */
static inline uint64_t now_t(const zx_t *z)
{
    return ((uint64_t)z->ula.frames * ULA_V * ULA_C + (uint64_t)z->ula.v * ULA_C + z->ula.c) / 2;
}

/* the byte an IN gets at the edge that ends the ULA's last period (out of
   the inlined path: rare) */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static uint8_t io_read(zx_t *z, uint16_t port)
{
    if (!(port & 1)) {
        /* the tape plays up to now; issue 3: EAR in is EAR out (port bit
           4), toggled by the tape's signal */
        uint64_t t = now_t(z);
        bool tape = tape_advance(&z->tape, (uint32_t)(t - z->tape_t));
        z->tape_t = t;
        z->fe_reads++;
        bool ear = (z->ula.port_fe >> 4 & 1) ^ (z->tape.playing && tape);
        return ula_read_fe(port, z->keys, ear);
    }
    return ula_floating(&z->ula, z->mem);
}

/* the memory the CPU's phase has just addressed: a write lands, a read's
   byte goes on D0-D7 for its next phase (I/O reads wait for the ULA) */
ULA_INLINE void serve_bus(zx_t *z, uint64_t *pp)
{
    uint64_t p = *pp;
    uint16_t a = Z80_GET_ADDR(p);
    if (p & Z80_MREQ) {
        if (p & Z80_RD)
            Z80_SET_DATA(p, z->mem[a]);
        else if ((p & Z80_WR) && a >= 0x4000)
            z->mem[a] = Z80_GET_DATA(p);
    } else if ((p & (Z80_IORQ | Z80_M1)) == (Z80_IORQ | Z80_M1)) {
        Z80_SET_DATA(p, 0xFF);                   /* the idle bus: the IM 2 vector */
    }
    *pp = p;
}

/* the ULA's pixel clocks up to the CPU's next phase, and that phase's
   inputs: an I/O read's byte (the keyboard and EAR, or the floating bus,
   as the edge that ends the last period sees it), INT and RESET; true when
   the frame ended on the way (line 248, before INT) */
ULA_INLINE bool to_next_phase(zx_t *z, uint64_t *pp)
{
    bool frame_end = ula_phase(&z->ula, z->mem, *pp);
    uint64_t p = *pp & ~(Z80_INT | Z80_RES);
    if ((p & (Z80_IORQ | Z80_RD | Z80_M1)) == (Z80_IORQ | Z80_RD))
        Z80_SET_DATA(p, io_read(z, Z80_GET_ADDR(p)));
    if (ula_int(&z->ula))
        p |= Z80_INT;
    if (z->reset_phases > 0) {
        p |= Z80_RES;
        z->reset_phases--;
    }
    *pp = p;
    return frame_end;
}

#if defined(__GNUC__)
static zx_t *zf_zx;                            /* the machine z80f_run serves */
#define ZF_BUS(p) do {                                  \
        serve_bus(zf_zx, &(p));                         \
        if (to_next_phase(zf_zx, &(p)))                 \
            zf_exit = 1;                                \
    } while (0)
#define ZF_BOUNDARY(c) 0
#endif
#define Z80_IMPL
#include "z80_cfast.h"

void zx_init(zx_t *z, const uint8_t rom[16384])
{
    memset(z, 0, sizeof *z);
    memcpy(z->mem, rom, 16384);
    z80_init(&z->cpu);
    z80f_init(&z->fast, &z->cpu);
    ula_init(&z->ula);
    to_next_phase(z, &z->in);                  /* the first pixel clock */
}

void zx_reset(zx_t *z)
{
    z->reset_phases = 8;
}

void zx_frame(zx_t *z)
{
    z->fe_reads = 0;
#if defined(__GNUC__)
    zf_zx = z;
    z->in = z80f_run(&z->fast, z->in);
#else
    for (;;) {
        uint64_t p = z80f_tick(&z->fast, z->in);
        serve_bus(z, &p);
        bool end = to_next_phase(z, &p);
        z->in = p;
        if (end)
            break;
    }
#endif
    z->pins = z->in;
    uint64_t t = now_t(z);
    tape_advance(&z->tape, (uint32_t)(t - z->tape_t));
    z->tape_t = t;
    z->fe_reads_last = z->fe_reads;
    z->frames++;
}

void zx_sync(zx_t *z)
{
    to_next_phase(z, &z->in);
}

void zx_phase(zx_t *z)
{
    uint64_t p = z80f_tick(&z->fast, z->in);
    z->pins = p;
    serve_bus(z, &p);
    to_next_phase(z, &p);
    z->in = p;
}

void zx_tape_motor(zx_t *z)
{
    if (!z->tape.data || tape_done(&z->tape)) {
        z->tape.playing = false;
        return;
    }
    uint16_t pc = z80_arch(&z->cpu)->rf[RFP_PC];
    bool reading = z->fe_reads_last >= 400 || (pc >= 0x0556 && pc < 0x0606);
    if (z->tape.stopped) {
        /* a stop block: PLAY again once a loader has waited half a second */
        z->tape_wait = reading ? z->tape_wait + 1 : 0;
        if (z->tape_wait >= 25) {
            z->tape.stopped = false;
            z->tape.playing = true;
            z->tape_idle = 0;
            z->tape_wait = 0;
        }
        return;
    }
    if (reading) {
        z->tape_idle = 0;
        z->tape.playing = true;
    } else if (z->tape.playing && ++z->tape_idle >= 150) {
        z->tape.playing = false;
    }
}

const uint8_t *zx_font(const zx_t *z)
{
    unsigned at = (uint16_t)((z->mem[23606] | z->mem[23607] << 8) + 256);
    return &z->mem[at <= 0x10000 - 768 ? at : 0x3D00];
}
