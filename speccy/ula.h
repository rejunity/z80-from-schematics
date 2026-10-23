/* ============================================================================
 * ula.h - the 48K Spectrum's ULA (6C001), stepped once a 7 MHz pixel clock.
 *
 * Written from the die: emu-russia/ula's gate-level reconstruction of a
 * 6C001E-7 (CC0), checked against the published timings (FUSE's frame
 * checksums, the WoS FAQ tables); see speccy/README.md.
 *
 * Counters: C 0..447 pixel clocks a line, V 0..311 lines; the paper (fetch
 * and contention) area is C < 256, V < 192. The ULA makes the CPU clock,
 * CLK = C0 | HOLD: it rises as C turns odd (the CPU enters phi 0, a new
 * T-state) and falls as C turns even (phi 1), unless HOLD keeps it high:
 *
 *   HOLD = CLK & (C2 | C3) & paper & !IOREQT2 &
 *          ((A14 & !A15 & !MREQT2) | (IORQ & !A0))
 *
 * decided with the new C on entering an even C, where MREQT2 and IOREQT2
 * follow MREQ and IORQ & !A0 while the clock is low (transparent latches)
 * and hold while it is high. So the ULA only ever stretches the high half
 * of a T1 (of a T2 for its own port, of every T-state that keeps a lower
 * 16K address on the bus with no MREQ): a T1 rising into C[3:0] = 3, 5,
 * 7, 9, 11, 13, 15, 1 of a paper line waits 6, 5, 4, 3, 2, 1, 0, 0 T-states,
 * FUSE's pattern from T-state 14335.
 *
 * Video: in each 16-pixel group the ULA reads bitmap, attribute, bitmap,
 * attribute at C[3:0] = 9, 11, 13, 15; the shift register loads at
 * C[2:0] = 4, the attribute / border latch at 5; column c shows at C =
 * 13 + 8c. INT: the die pulls it on line 248 for C 0..63 (32 T-states);
 * a Z80 sampling INT at the last T-state's rising edge sees FUSE's "early"
 * timing with it on for C 2..65 (the default; --late: 0..63).
 * ==========================================================================*/
#ifndef SPECCY_ULA_H
#define SPECCY_ULA_H
#include <stdbool.h>
#include <stdint.h>

/* the per-phase code inlined into each of cfast's run-frame steps, where
   the strobe tests fold away */
#if defined(__GNUC__)
#define ULA_INLINE static inline __attribute__((always_inline))
#else
#define ULA_INLINE static inline
#endif

#define ULA_C          448
#define ULA_V          312
#define ULA_FRAME_T    (ULA_C * ULA_V / 2)            /* 69888 T-states */
/* the picture kept: 48 pixels of border left and right, 48 lines above,
   56 below (the paper's top-left pixel at ULA_PAPER_X, ULA_PAPER_Y) */
#define ULA_FB_W       352
#define ULA_FB_H       296
#define ULA_PAPER_X    48
#define ULA_PAPER_Y    48

/* the CPU's pins the ULA reads (the cfast pin word, active high) */
#define ULA_MREQ  (1ULL << 25)
#define ULA_IORQ  (1ULL << 26)
#define ULA_RD    (1ULL << 27)
#define ULA_WR    (1ULL << 28)

typedef struct {
    uint16_t c, v;              /* counters; between CPU phases the clock is
                                   C0 (no hold lasts past a ula_phase call)  */
    bool     mreq_t2, ioreq_t2; /* MREQ, IORQ & !A0 seen while clk was low   */
    uint8_t  port_fe;           /* the port latch: border 0-2, MIC 3, EAR 4  */
    unsigned int_from;          /* V * 448 + C where INT starts: line 248,
                                   C 2 (early) or C 0 (late, ula_set_late)   */
    uint32_t frames;            /* frames (FLASH: bit 4 of a 5-bit divider)  */
    /* the video pipeline, run while draw is set */
    bool     draw;
    uint8_t  dl, al, sr, ao;    /* bitmap and attribute latches, shift
                                   register, attribute / border output latch */
    bool     viden;
    /* the picture: bits 0-3 the colour shown (bright << 3 | G R B), 4-6
       the attribute's other colour (G R B: paper where the pixel shows
       ink, ink where it shows paper; 0 in the border), 7 the bitmap's
       bit (before FLASH) -- for braille, whose dots are the ink */
    uint8_t  fb[ULA_FB_H][ULA_FB_W];
} ula_t;

void ula_init(ula_t *u);
/* Late timing: INT on C 0..63 of line 248, the die's own decode (early,
   the default: C 2..65, as a Z80 sampling INT at the rising edge of the
   last T-state sees FUSE's timing). */
void ula_set_late(ula_t *u, bool late);
/* The video pipeline for the period just entered (draw set). */
void ula_video(ula_t *u, const uint8_t *mem);
/* Out of the inlined path: n pixel clocks drawn, and a new line (rare);
   both return whether line 248 began. */
bool ula_advance_drawn(ula_t *u, const uint8_t *mem, unsigned n);
bool ula_new_line(ula_t *u);

/* The ULA's pixel clocks from a CPU phase to the CPU's next one. The CPU
   has just taken a half-phase (its pins as they are now): phi 0 if C is
   odd (the clock high), else phi 1. Returns true if line 248 began on the way
   (the frame's end: INT comes next).

   After a low half the T2 latches copy MREQ and IOREQ, and the clock rises
   at the next (odd) C. After a high half the clock falls at the next C,
   unless HOLD keeps it high; nothing HOLD reads changes while it holds
   (the CPU waits, the latches are shut), except the window C2 | C3, so the
   clock falls at the next C that is a multiple of 16. */
ULA_INLINE bool ula_phase(ula_t *u, const uint8_t *mem, uint64_t pins)
{
    uint16_t a = (uint16_t)pins;
    bool ioreq = (pins & ULA_IORQ) && !(a & 1);   /* the board gates /IOREQ with A0 */
    unsigned n = 1;
    if (!(u->c & 1)) {
        u->mreq_t2 = (pins & ULA_MREQ) != 0;
        u->ioreq_t2 = ioreq;
    } else {
        unsigned c1 = u->c + 1u;
        if ((c1 & 0x0C) && c1 < 256 && u->v < 192 && !u->ioreq_t2 &&
            (((a & 0xC000) == 0x4000 && !u->mreq_t2) || ioreq))
            n = 17 - (c1 & 15);
    }
    if (ioreq && (pins & ULA_WR) && u->ioreq_t2)
        u->port_fe = (uint8_t)(pins >> 16) & 0x1F;  /* from T2-low of the OUT */
    if (u->draw)
        return ula_advance_drawn(u, mem, n);
    if ((u->c += (uint16_t)n) >= ULA_C) {
        u->c = (uint16_t)(u->c - ULA_C);
        return ula_new_line(u);
    }
    return false;
}

/* INT as the CPU sees it in this period. */
ULA_INLINE bool ula_int(const ula_t *u)
{
    return (unsigned)(u->v * ULA_C + u->c) - u->int_from < 64u;
}

/* A read of port 0xFE (A0 low): the keys of the half-rows A8-A15 select
   (keys: bit row * 5 + key, 1 = down) in bits 0-4, EAR in bit 6; the ULA
   drives neither bit 5 nor bit 7, which read 1. */
uint8_t ula_read_fe(uint16_t port, uint64_t keys, bool ear);
/* What a read nobody answers sees at the edge that ends the last period:
   the byte the ULA fetched for it, or the idle 0xFF. */
uint8_t ula_floating(const ula_t *u, const uint8_t *mem);
#endif
