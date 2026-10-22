/* ula.c - see ula.h */
#include "ula.h"
#include <string.h>

void ula_init(ula_t *u)
{
    memset(u, 0, sizeof *u);
    u->port_fe = 7;
    u->draw = true;
    ula_set_late(u, false);
}

void ula_set_late(ula_t *u, bool late)
{
    u->int_from = 248 * ULA_C + (late ? 0 : 2);
}

/* the offsets from 0x4000 of paper line y's bitmap and attribute bytes */
static inline uint16_t bitmap_off(unsigned y, unsigned col)
{
    return (uint16_t)((y & 0xC0) << 5 | (y & 7) << 8 | (y & 0x38) << 2 | col);
}

static inline uint16_t attr_off(unsigned y, unsigned col)
{
    return (uint16_t)(0x1800 | (y >> 3) << 5 | col);
}

void ula_video(ula_t *u, const uint8_t *mem)
{
    unsigned c = u->c, v = u->v;
    bool paper = c < 256 && v < 192;
    if (paper && (c & 9) == 9) {               /* C[3:0] = 9, 11, 13, 15 */
        unsigned col = 2 * (c >> 4) + (c >> 2 & 1);
        if (c & 2)
            u->al = mem[0x4000 + attr_off(v, col)];
        else
            u->dl = mem[0x4000 + bitmap_off(v, col)];
    }
    if (c & 8)                                  /* transparent while C3 */
        u->viden = paper;
    if ((c & 7) == 5)                           /* the attribute / border latch */
        u->ao = u->viden ? u->al : (uint8_t)((u->port_fe & 7) << 3);
    bool ink = (u->sr >> 7 & 1) ^ ((u->ao >> 7) & (u->frames >> 4) & 1);
    uint8_t colour = (uint8_t)((u->ao >> 3 & 8) | (ink ? u->ao & 7 : u->ao >> 3 & 7));
    if ((c & 7) == 4 && u->viden)               /* shows from the next pixel */
        u->sr = u->dl;
    else
        u->sr = (uint8_t)(u->sr << 1);
    /* where the pixel lands: column 0 of the paper is C = 13; the left
       border of a row starts on the line before (C 413..447) */
    unsigned p = v * ULA_C + c + (ULA_PAPER_X - 13);
    unsigned x = p % ULA_C, y = (p / ULA_C + ULA_PAPER_Y) % ULA_V;
    if (x < ULA_FB_W && y < ULA_FB_H)
        u->fb[y][x] = colour;
}

bool ula_new_line(ula_t *u)
{
    if (++u->v == ULA_V)
        u->v = 0;
    if (u->v == 0)
        u->frames++;
    return u->v == 248;
}

bool ula_advance_drawn(ula_t *u, const uint8_t *mem, unsigned n)
{
    bool line248 = false;
    while (n--) {
        if (++u->c == ULA_C) {
            u->c = 0;
            line248 |= ula_new_line(u);
        }
        ula_video(u, mem);
    }
    return line248;
}

uint8_t ula_read_fe(uint16_t port, uint64_t keys, bool ear)
{
    uint8_t v = 0x1F;
    for (int r = 0; r < 8; r++)
        if (!(port >> (8 + r) & 1))
            v &= (uint8_t)~(keys >> (5 * r)) & 0x1F;
    return (uint8_t)(v | 0xA0 | (ear ? 0x40 : 0));
}

uint8_t ula_floating(const ula_t *u, const uint8_t *mem)
{
    /* the period that ended: the ULA's bus holds what it fetched at
       C[3:0] = 9 (bitmap 2g), 11 (attribute 2g), 13 (bitmap 2g+1), 15
       (attribute 2g+1) until C[3:0] = 1, and 0xFF otherwise */
    unsigned c = u->c ? u->c - 1u : ULA_C - 1u, v = u->v;
    if (c == ULA_C - 1u)
        return 0xFF;                            /* never a paper period */
    if (v >= 192)
        return 0xFF;
    unsigned k = c & 15, g = c >> 4;
    if (k == 0) {                               /* still group g-1's last attribute */
        if (g == 0)
            return 0xFF;
        g--;
        k = 15;
    }
    if (k < 9 || g >= 16)
        return 0xFF;
    unsigned col = 2 * g + (k >= 13);
    return (k == 9 || k == 10 || k == 13 || k == 14) ? mem[0x4000 + bitmap_off(v, col)]
                                                       : mem[0x4000 + attr_off(v, col)];
}
