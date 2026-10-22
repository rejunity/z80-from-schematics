/* ula.c - see ula.h */
#include "ula.h"
#include <string.h>

void ula_init(ula_t *u)
{
    memset(u, 0, sizeof *u);
    u->border = 7;
    u->draw = true;
}

/* the display file address of paper line y (0-191), byte column x (0-31) */
static inline uint16_t pixel_addr(unsigned y, unsigned x)
{
    return (uint16_t)(0x4000 | (y & 0xC0) << 5 | (y & 7) << 8 | (y & 0x38) << 2 | x);
}

static inline uint8_t paper_pixel(const ula_t *u, const uint8_t *mem, unsigned line, unsigned t, unsigned half)
{
    unsigned y = line - 64, x = t >> 2, bit = 7 - ((t & 3) * 2 + half);
    uint8_t attr = mem[0x5800 + (y >> 3) * 32 + x];
    bool on = mem[pixel_addr(y, x)] >> bit & 1;
    if ((attr & 0x80) && (u->frames & 16))
        on = !on;
    return (uint8_t)((attr >> 3 & 8) | (on ? attr & 7 : attr >> 3 & 7));
}

bool ula_tick(ula_t *u, const uint8_t *mem, uint64_t pins)
{
    (void)pins;
    if (u->draw) {
        unsigned t = u->tick >> 1, half = u->tick & 1;
        unsigned line = t / ULA_LINE_T, lt = t % ULA_LINE_T;
        int row = -1, x = 0;
        uint8_t c = u->border;
        if (lt < 128) {                        /* paper (or top / bottom border) */
            row = (int)line - ULA_FIRST_LINE;
            x = ULA_PAPER_X + (int)(lt * 2 + half);
            if (line >= 64 && line < 256)
                c = paper_pixel(u, mem, line, lt, half);
        } else if (lt < 152) {                 /* right border */
            row = (int)line - ULA_FIRST_LINE;
            x = ULA_PAPER_X + 256 + (int)((lt - 128) * 2 + half);
        } else if (lt >= 200) {                /* the next line's left border */
            row = (int)line + 1 - ULA_FIRST_LINE;
            x = (int)((lt - 200) * 2 + half);
        }
        if (row >= 0 && row < ULA_FB_H)
            u->fb[row][x] = c;
    }
    if (++u->tick == ULA_FRAME_TICKS) {
        u->tick = 0;
        u->frames++;
    }
    return true;                               /* TODO: contention */
}

uint8_t ula_read_fe(const ula_t *u, uint16_t port, uint64_t keys, bool ear_in)
{
    uint8_t v = 0x1F;
    for (int r = 0; r < 8; r++)
        if (!(port >> (8 + r) & 1))
            v &= (uint8_t)~(keys >> (5 * r)) & 0x1F;
    (void)u;
    return (uint8_t)(v | 0xA0 | (ear_in ? 0x40 : 0));
}

void ula_write_fe(ula_t *u, uint8_t v)
{
    u->border = v & 7;
    u->mic = v >> 3 & 1;
    u->ear = v >> 4 & 1;
}

uint8_t ula_floating(const ula_t *u, const uint8_t *mem)
{
    (void)u;
    (void)mem;
    return 0xFF;                               /* TODO: the fetches */
}
