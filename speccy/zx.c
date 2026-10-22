/* zx.c - see zx.h. cfast's implementation lives in this unit.
 *
 * Each CPU phase is followed by its bus (serve_bus) and then the ULA's
 * pixel clocks up to the one the CPU's next phase takes (to_next_phase):
 * one, or more while the ULA holds the CPU clock. With GCC or Clang the
 * phases run in cfast's run frame (z80f_run, the bus inlined as ZF_BUS);
 * elsewhere a loop of z80f_tick() calls does the same. */
#include "zx.h"
#include <string.h>

static inline uint64_t now_t(const zx_t *z)
{
    return (uint64_t)z->ula.frames * ULA_FRAME_T + z->ula.tick / 2;
}

static uint8_t io_read(zx_t *z, uint16_t port)
{
    if (!(port & 1)) {
        /* the tape plays up to now: the EAR level the CPU reads */
        uint64_t t = now_t(z);
        bool ear = tape_advance(&z->tape, (uint32_t)(t - z->tape_t));
        z->tape_t = t;
        z->fe_reads++;
        if (!z->tape.playing)                  /* issue 3: EAR in follows EAR out */
            ear = z->ula.ear;
        return ula_read_fe(&z->ula, port, z->keys, ear);
    }
    return ula_floating(&z->ula, z->mem);
}

/* the bus for the phase the CPU has just produced: memory, I/O and the
   interrupt acknowledge; D0-D7 for its next phase */
ULA_INLINE void serve_bus(zx_t *z, uint64_t *pp)
{
    uint64_t p = *pp;
    uint16_t a = Z80_GET_ADDR(p);
    uint8_t d = 0xFF;
    bool iowr = false;
    if (p & Z80_MREQ) {
        if (p & Z80_RD)
            d = z->mem[a];
        else if ((p & Z80_WR) && a >= 0x4000)
            z->mem[a] = Z80_GET_DATA(p);
    } else if (p & Z80_IORQ) {
        if (p & Z80_M1)
            d = 0xFF;                          /* interrupt acknowledge */
        else if (p & Z80_RD)
            d = io_read(z, a);
        else if (p & Z80_WR) {
            iowr = true;
            if (!z->iowr && !(a & 1))
                ula_write_fe(&z->ula, Z80_GET_DATA(p));
        }
    }
    z->iowr = iowr;
    Z80_SET_DATA(p, d);
    *pp = p;
}

/* the ULA's pixel clocks up to the CPU's next phase, and that phase's INT
   and RESET; true when a frame ended on the way */
ULA_INLINE bool to_next_phase(zx_t *z, uint64_t *pp)
{
    bool frame_end = false;
    while (!ula_tick(&z->ula, z->mem, *pp))
        frame_end |= z->ula.tick == 0;
    frame_end |= z->ula.tick == 0;
    uint64_t p = *pp & ~(Z80_INT | Z80_RES);
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

void zx_tape_motor(zx_t *z)
{
    if (!z->tape.data || tape_done(&z->tape)) {
        z->tape.playing = false;
        return;
    }
    uint16_t pc = z80_arch(&z->cpu)->rf[RFP_PC];
    bool reading = z->fe_reads_last >= 400 || (pc >= 0x0556 && pc < 0x0606);
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
