/* zx.c - see zx.h. cfast's implementation lives in this unit. */
#define Z80_IMPL
#include "zx.h"
#include <string.h>

void zx_init(zx_t *z, const uint8_t rom[16384])
{
    memset(z, 0, sizeof *z);
    memcpy(z->mem, rom, 16384);
    z80_init(&z->cpu);
    z80f_init(&z->fast, &z->cpu);
    ula_init(&z->ula);
}

void zx_reset(zx_t *z)
{
    z->reset_phases = 8;
}

static uint8_t io_read(zx_t *z, uint16_t port)
{
    if (!(port & 1)) {
        /* the tape plays up to now: the EAR level the CPU reads */
        bool ear = tape_advance(&z->tape, (uint32_t)(z->t - z->tape_t));
        z->tape_t = z->t;
        z->fe_reads++;
        if (!z->tape.playing)                  /* issue 3: EAR in follows EAR out */
            ear = z->ula.ear;
        return ula_read_fe(&z->ula, port, z->keys, ear);
    }
    return ula_floating(&z->ula, z->mem);
}

/* one CPU phase, and the bus it leaves */
static inline void cpu_phase(zx_t *z)
{
    uint64_t in = z->in & ~(Z80_INT | Z80_RES);
    if (ula_int(&z->ula))
        in |= Z80_INT;
    if (z->reset_phases > 0) {
        in |= Z80_RES;
        z->reset_phases--;
    }
    uint64_t p = z80f_tick(&z->fast, in);
    z->pins = p;
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
    z->in = p;
}

void zx_frame(zx_t *z)
{
    z->fe_reads = 0;
    for (uint32_t i = 0; i < ULA_FRAME_TICKS; i++) {
        if (ula_tick(&z->ula, z->mem, z->pins))
            cpu_phase(z);
        z->t += i & 1;
    }
    tape_advance(&z->tape, (uint32_t)(z->t - z->tape_t));
    z->tape_t = z->t;
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
