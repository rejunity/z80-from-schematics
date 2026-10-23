/* ============================================================================
 * term.h - the Spectrum's picture in a terminal, as text.
 *
 * A terminal cell is one glyph in two colours, and the Spectrum gives each
 * 8x8 character two colours (ink and paper), so a cell that stays inside
 * one character loses nothing. The modes, named after the columns they
 * need for the 256x192 paper:
 *   128  octants (Unicode 16's 2x4 block characters, U+1CD00..): a block a
 *        pixel, 128x48 cells; cells about twice as tall as wide make the
 *        blocks square. Exact.
 *   120  the same, with the fourth character column of every four drawn 6
 *        pixels wide (without its pixel columns 0 and 7, blank in the ROM
 *        font's letters and digits but for T and Y), 120x48.
 *   80   two cells a character, 64x24: a character whose 8x8 bitmap is a
 *        glyph of the font (or its inverse) prints as that character, the
 *        rest as octants at half resolution, blended or crisp.
 * The 2x4 blocks print as octants, or as braille dots (U+2800..) where the
 * terminal's font has no octants: speccy picks braille itself under
 * Terminal.app and VS Code (TERM_PROGRAM). The dots of a cell stand on
 * its 8x8 character's commonest colour, so a letter keeps one background.
 * The border shows as far as the terminal has room. A cell where the
 * picture has more than two colours (attributes or the border changed
 * within it) keeps the two most common. Each frame sends only the cells
 * that changed, inside a synchronized update (mode 2026).
 * ==========================================================================*/
#ifndef SPECCY_TERM_H
#define SPECCY_TERM_H
#include <stdbool.h>
#include <stdint.h>

enum { GLYPHS_AUTO, GLYPHS_OCTANTS, GLYPHS_BRAILLE };
typedef struct {
    int  mode;        /* 128, 120, 80, or 0: the widest that fits             */
    int  colors;      /* 24 (bit), 256 or 16; 0: 24 if COLORTERM says so      */
    int  glyphs;      /* GLYPHS_*; AUTO: braille in Terminal.app and VS Code  */
    bool crisp;       /* mode 80: no blended shades                          */
    bool fullwidth;   /* mode 80: double-width characters                    */
} term_opts_t;

bool term_open(const term_opts_t *o);
void term_close(void);
/* The terminal's size changed (or not): lay out again, redraw it all. */
void term_resize(void);
/* Draw the picture: fb holds w x h colour indices (bright << 3 | G R B),
   the paper's top-left pixel at (px, py); font is the 96 glyphs (768
   bytes, codes 32-127) mode 80 matches text against, or NULL. */
void term_frame(const uint8_t *fb, int w, int h, int px, int py, const uint8_t *font);
/* The window title. */
void term_title(const char *s);
/* The mode in use, whether it draws braille, and the terminal's size. */
int  term_mode(void);
bool term_braille(void);
void term_size(int *cols, int *rows);
#endif
