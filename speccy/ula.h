/* ============================================================================
 * ula.h - the 48K Spectrum's ULA, stepped once a 7 MHz pixel clock.
 *
 * The ULA divides the 14 MHz crystal into the 7 MHz pixel clock and the
 * 3.5 MHz CPU clock, so a pixel clock is one phase of the CPU's clock (the
 * model's half T-state). Each ula_tick() draws one pixel (border or paper)
 * and says whether the CPU's clock moves on (the ULA holds it high to keep
 * the CPU off the lower 16K while it fetches the screen: contention).
 *
 * A frame is 312 lines of 224 T-states (69888), counted from INT, which the
 * ULA holds for the first 32 T-states. Line 64 is the first paper line; a
 * line's T-states 0-127 draw its paper, 128-151 its right border, 200-223
 * the next line's left border.
 * ==========================================================================*/
#ifndef SPECCY_ULA_H
#define SPECCY_ULA_H
#include <stdbool.h>
#include <stdint.h>

#define ULA_LINE_T     224
#define ULA_LINES      312
#define ULA_FRAME_T    (ULA_LINE_T * ULA_LINES)      /* 69888 */
#define ULA_FRAME_TICKS (2 * ULA_FRAME_T)
/* the picture kept: 48 pixels of border left and right, 48 above, 56 below */
#define ULA_FB_W       352
#define ULA_FB_H       296
#define ULA_PAPER_X    48
#define ULA_PAPER_Y    48
#define ULA_FIRST_LINE (64 - ULA_PAPER_Y)            /* the frame line of fb row 0 */

typedef struct {
    uint32_t tick;              /* pixel clocks since INT, 0 .. ULA_FRAME_TICKS-1 */
    uint32_t frames;            /* frames since power-on (FLASH: every 16) */
    uint8_t  border;            /* port 0xFE bits 0-2 */
    bool     mic, ear;          /* port 0xFE bits 3 and 4 */
    bool     draw;              /* draw pixels (off to save time when no one looks) */
    uint8_t  fb[ULA_FB_H][ULA_FB_W];   /* colour indices: bright << 3 | G R B */
} ula_t;

void ula_init(ula_t *u);
/* One pixel clock. mem is the 64K the CPU sees, pins the CPU's last pins.
   Returns whether the CPU clock moves on in this phase. */
bool ula_tick(ula_t *u, const uint8_t *mem, uint64_t pins);
/* INT is held now. */
static inline bool ula_int(const ula_t *u) { return u->tick < 64; }
/* A read of port 0xFE (A0 low): the keys of the half-rows A8-A15 select
   (keys: bit row * 5 + key, 1 = down), EAR in bit 6. */
uint8_t ula_read_fe(const ula_t *u, uint16_t port, uint64_t keys, bool ear_in);
void    ula_write_fe(ula_t *u, uint8_t v);
/* What a read of a port nothing answers sees: the byte the ULA fetches
   now, or 0xFF. */
uint8_t ula_floating(const ula_t *u, const uint8_t *mem);
#endif
