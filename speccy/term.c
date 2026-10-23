/* term.c - see term.h */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#include "term.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------- colours */

static uint32_t rgb_of(int c)
{
    unsigned v = (c & 8) ? 0xFF : 0xD7;
    return ((c & 2) ? v << 16 : 0) | ((c & 4) ? v << 8 : 0) | ((c & 1) ? v : 0);
}

static double lin(unsigned v)
{
    double x = v / 255.0;
    return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}

/* an RGB colour in linear light */
static void lin3(uint32_t rgb, double *l)
{
    for (int s = 0; s < 3; s++)
        l[s] = lin(rgb >> (16 - 8 * s) & 255);
}

static unsigned srgb(double l)
{
    double v = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1 / 2.4) - 0.055;
    return (unsigned)(v * 255 + 0.5);
}

/* The xterm cube's levels 0, 95, 135, 175, 215, 255 hold the Spectrum's
   normal (0xD7) and bright (0xFF) exactly; blends take the nearest entry. */
static int index256(uint32_t rgb)
{
    static const int cube[6] = { 0, 95, 135, 175, 215, 255 };
    int r = (int)(rgb >> 16), g = (int)(rgb >> 8 & 255), b = (int)(rgb & 255);
    int best = 16, bestd = 1 << 30;
    for (int i = 0; i < 216; i++) {
        int dr = cube[i / 36] - r, dg = cube[i / 6 % 6] - g, db = cube[i % 6] - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < bestd) { bestd = d; best = 16 + i; }
    }
    for (int i = 0; i < 24; i++) {
        int v = 8 + 10 * i, d = (v - r) * (v - r) + (v - g) * (v - g) + (v - b) * (v - b);
        if (d < bestd) { bestd = d; best = 232 + i; }
    }
    return best;
}

/* The ANSI colours are the Spectrum's eight with the bits the other way
   round (B G R) and their bright set; bright black stays black. */
static int ansi16(uint32_t rgb)
{
    int best = 0, bestd = 1 << 30;
    for (int c = 0; c < 16; c++) {
        uint32_t p = rgb_of(c);
        int dr = (int)(p >> 16) - (int)(rgb >> 16), dg = (int)(p >> 8 & 255) - (int)(rgb >> 8 & 255),
            db = (int)(p & 255) - (int)(rgb & 255), d = dr * dr + dg * dg + db * db;
        if (d < bestd) { bestd = d; best = c; }
    }
    int a = (best >> 1 & 1) | (best >> 1 & 2) | (best << 2 & 4);
    return a + ((best & 8) && (best & 7) ? 60 : 0);
}

/* ------------------------------------------------------------- glyphs */

/* A cell's 2x4 blocks as a pattern, bit 2r+c the block in row r, column c.
   Unicode 16's octants (U+1CD00..U+1CDE5) are 230 of the 256 patterns, in
   order; these 26 it had before (checked against UnicodeData 16.0). */
static const struct { uint8_t p; uint32_t cp; } older[26] = {
    { 0x00, 0x20 }, { 0xFF, 0x2588 }, { 0x0F, 0x2580 }, { 0xF0, 0x2584 }, { 0x55, 0x258C },
    { 0xAA, 0x2590 }, { 0x05, 0x2598 }, { 0x0A, 0x259D }, { 0x50, 0x2596 }, { 0xA0, 0x2597 },
    { 0xF5, 0x2599 }, { 0xA5, 0x259A }, { 0x5F, 0x259B }, { 0xAF, 0x259C }, { 0x5A, 0x259E },
    { 0xFA, 0x259F }, { 0xC0, 0x2582 }, { 0xFC, 0x2586 }, { 0x03, 0x1FB82 }, { 0x3F, 0x1FB85 },
    { 0x14, 0x1FBE6 }, { 0x28, 0x1FBE7 }, { 0x01, 0x1CEA8 }, { 0x02, 0x1CEAB }, { 0x40, 0x1CEA3 },
    { 0x80, 0x1CEA0 } };
static uint32_t octant[256], braille[256];
/* quadrants: bit 0 upper left, 1 upper right, 2 lower left, 3 lower right */
static const uint32_t quadrant[16] = {
    0x20, 0x2598, 0x259D, 0x2580, 0x2596, 0x258C, 0x259E, 0x259B,
    0x2597, 0x259A, 0x2590, 0x259C, 0x2584, 0x2599, 0x259F, 0x2588 };
/* half-width bars: bit r the bar in row r, from the top. Block Elements
   have the patterns whose colour changes at most once down the cell: the
   upper and lower halves, the lower quarter and three quarters (bar[p],
   p's bars in fg), and with fg and bg swapped the other four (bar[~p]) */
static const uint32_t bar[16] = { [0x3] = 0x2580, [0xC] = 0x2584, [0x8] = 0x2582, [0xE] = 0x2586 };

static void glyph_tables(void)
{
    uint32_t cp = 0x1CD00;
    for (int p = 0; p < 256; p++) {
        octant[p] = 0;
        for (int i = 0; i < 26; i++)
            if (older[i].p == p)
                octant[p] = older[i].cp;
        if (!octant[p])
            octant[p] = cp++;
        /* braille dots 1 2 3 7 down the left, 4 5 6 8 down the right */
        static const uint8_t dot[8] = { 0x01, 0x08, 0x02, 0x10, 0x04, 0x20, 0x40, 0x80 };
        uint32_t b = 0;
        for (int i = 0; i < 8; i++)
            if (p >> i & 1)
                b |= dot[i];
        braille[p] = 0x2800 + b;
    }
}

/* ------------------------------------------------------------- state */

#define NO_FG 0xFFFFFFFFu
typedef struct { uint32_t ch, fg, bg; } cell_t;   /* ch 0: a wide glyph's right half */

static term_opts_t opt;
static int cols = 80, rows = 24, mode;
static bool glyphs_auto;                         /* glyphs picked by TERM_PROGRAM */
static int qrows;                                /* quadrants: 96 or 48 rows */
static int grid_w, grid_h, ox, oy;               /* the picture's cells, top-left */
static cell_t *cur, *prev;
static bool redraw = true;
static char *out;
static size_t out_len, out_cap;

static void put(const char *s, size_t n)
{
    if (out_len + n > out_cap) {
        out_cap = (out_len + n) * 2;
        out = realloc(out, out_cap);
    }
    memcpy(out + out_len, s, n);
    out_len += n;
}

static void puts_(const char *s) { put(s, strlen(s)); }

static void put_utf8(uint32_t c)
{
    char b[4];
    size_t n;
    if (c < 0x80) { b[0] = (char)c; n = 1; }
    else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 63)); n = 2; }
    else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 63)); b[2] = (char)(0x80 | (c & 63)); n = 3; }
    else { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 63)); b[2] = (char)(0x80 | (c >> 6 & 63)); b[3] = (char)(0x80 | (c & 63)); n = 4; }
    put(b, n);
}

static void flush_out(void)
{
    fwrite(out, 1, out_len, stdout);
    fflush(stdout);
    out_len = 0;
}

static void query_size(void)
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        cols = info.srWindow.Right - info.srWindow.Left + 1;
        rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
#endif
    if (cols > 1000) cols = 1000;
    if (rows > 500) rows = 500;
}

bool term_open(const term_opts_t *o)
{
    opt = *o;
    if (!opt.colors) {
        const char *ct = getenv("COLORTERM");
        opt.colors = ct && (!strcmp(ct, "truecolor") || !strcmp(ct, "24bit")) ? 24 : 256;
    }
    glyphs_auto = opt.glyphs == GLYPHS_AUTO;
    if (glyphs_auto) {
        /* the fonts of Terminal.app and VS Code's terminal have no octants
           (Unicode 16), and neither terminal draws them itself */
        const char *tp = getenv("TERM_PROGRAM");
        opt.glyphs = tp && (!strcmp(tp, "Apple_Terminal") || !strcmp(tp, "vscode")) ? GLYPHS_BRAILLE
                                                                                    : GLYPHS_OCTANTS;
    }
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m;
    if (GetConsoleMode(h, &m))
        SetConsoleMode(h, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
    SetConsoleOutputCP(CP_UTF8);
    if (!opt.colors) opt.colors = 24;
#endif
    glyph_tables();
    fputs("\x1b[?1049h\x1b[?25l\x1b[2J", stdout);
    fflush(stdout);
    term_resize();
    return true;
}

void term_close(void)
{
    fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    free(cur);
    free(prev);
    free(out);
    cur = prev = NULL;
    out = NULL;
    out_cap = out_len = 0;
}

void term_resize(void)
{
    query_size();
    mode = opt.mode;
    qrows = opt.quad_rows ? opt.quad_rows : rows >= 96 ? 96 : 48;
    if (!mode) {
        if (opt.glyphs == GLYPHS_QUADRANTS || opt.glyphs == GLYPHS_BARS)
            mode = cols >= 128 ? 128 : 120;
        else if (glyphs_auto && opt.glyphs == GLYPHS_BRAILLE && cols >= 256 && rows >= 96)
            mode = 256;           /* no octants here, but room for half blocks */
        else
            mode = cols >= 128 && rows >= 48 ? 128 : cols >= 120 && rows >= 48 ? 120 : 80;
    }
    free(cur);
    free(prev);
    cur = calloc((size_t)cols * (size_t)rows, sizeof *cur);
    prev = calloc((size_t)cols * (size_t)rows, sizeof *prev);
    redraw = true;
}

int term_mode(void) { return mode; }
static bool quads(void) { return opt.glyphs == GLYPHS_QUADRANTS && (mode == 128 || mode == 120); }
static bool bars(void) { return opt.glyphs == GLYPHS_BARS && (mode == 128 || mode == 120); }
const char *term_glyphs(void)
{
    return mode == 256 ? "half blocks" : quads() ? (qrows == 48 ? "half-height quadrants" : "quadrants")
         : bars() ? "half-width bars" : opt.glyphs == GLYPHS_BRAILLE ? "braille" : "octants";
}
void term_size(int *c, int *r) { *c = cols; *r = rows; }

void term_title(const char *s)
{
    printf("\x1b]2;%s\x07", s);
    fflush(stdout);
}

/* ---------------------------------------------------------- the cells */

static const uint8_t *g_fb;
static int g_w, g_h;

/* a pixel as it is stored (ula.h), and its colour */
static inline int pixraw(int x, int y)
{
    if (x < 0 || y < 0 || x >= g_w || y >= g_h)
        return 0;
    return g_fb[(size_t)y * (size_t)g_w + (size_t)x];
}

static inline int pix(int x, int y) { return pixraw(x, y) & 15; }

static int dist2(uint32_t a, uint32_t b)
{
    int d = 0;
    for (int s = 0; s < 24; s += 8) {
        int x = (int)(a >> s & 255) - (int)(b >> s & 255);
        d += x * x;
    }
    return d;
}

/* octants (n 8) or quadrants (n 4): p's blocks in fg, the rest in bg */
static cell_t block_cell(unsigned p, uint32_t fg, uint32_t bg, int n)
{
    cell_t c;
    unsigned all = (1u << n) - 1;
    if (fg == bg)
        p = 0;
    if (p == 0) { c.ch = ' '; c.fg = NO_FG; c.bg = bg; return c; }
    if (p == all) { c.ch = ' '; c.fg = NO_FG; c.bg = fg; return c; }
    c.ch = n == 4 ? quadrant[p] : octant[p];
    c.fg = fg;
    c.bg = bg;
    return c;
}

/* n colour indices (2x4, 2x2 or 1x4, bit order) -> the cell's colours, a
   the commonest and b (-1: only a), and the pattern of the blocks in b:
   two colours exact, more than two the two most common, the rest to the
   nearer of them */
static unsigned index_split(const int *v, int n, int *pa, int *pb)
{
    int count[16] = { 0 }, a = -1, b = -1;
    for (int i = 0; i < n; i++)
        count[v[i]]++;
    for (int i = 0; i < n; i++) {
        int c = v[i];
        if (a < 0 || count[c] > count[a])     /* the first of the most common */
            a = c;
    }
    for (int i = 0; i < n; i++) {
        int c = v[i];
        if (c != a && (b < 0 || count[c] > count[b]))
            b = c;
    }
    *pa = a;
    *pb = b;
    if (b < 0)
        return 0;
    uint32_t ra = rgb_of(a), rb = rgb_of(b);
    unsigned p = 0;
    for (int i = 0; i < n; i++) {
        int c = v[i];
        bool to_b = c == b;
        if (c != a && c != b)
            to_b = dist2(rgb_of(c), rb) < dist2(rgb_of(c), ra);
        if (to_b)
            p |= 1u << i;
    }
    return p;
}

/* n colour indices (2x4 or 2x2) -> a cell */
static cell_t index_cell(const int *v, int n)
{
    int a, b;
    unsigned p = index_split(v, n, &a, &b);
    if (b < 0)
        return block_cell(0, rgb_of(a), rgb_of(a), n);
    return block_cell(p, rgb_of(b), rgb_of(a), n);
}

static bool bar_drawn(unsigned p) { return p == 0 || p == 15 || bar[p] || bar[~p & 15]; }

/* half-width bars: p's bars in fg, the rest in bg. A pattern Block
   Elements lack (a bar alone in the middle, bars that alternate) becomes
   the one they have with the least squared error in linear light (l: the
   bars' shades, lo and hi: bg's and fg's), a tie keeping the bars of the
   colour that has fewer (a stroke one pixel tall stays, two tall). */
static cell_t bar_cell(unsigned p, uint32_t fg, uint32_t bg, double l[][3], const double *lo, const double *hi)
{
    if (!bar_drawn(p)) {
        int ones = (int)((p & 1) + (p >> 1 & 1) + (p >> 2 & 1) + (p >> 3 & 1));
        double best = HUGE_VAL;
        bool best_kept = false;
        unsigned bq = 0;
        for (unsigned q = 0; q < 16; q++) {
            if (!bar_drawn(q))
                continue;
            double e = 0;
            for (int r = 0; r < 4; r++) {
                const double *s = q >> r & 1 ? hi : lo;
                for (int k = 0; k < 3; k++)
                    e += (l[r][k] - s[k]) * (l[r][k] - s[k]);
            }
            bool kept = ones < 2 ? (q & p) == p : ones > 2 && !(q & ~p & 15);
            if (e < best || (e == best && kept && !best_kept)) {
                best = e;
                best_kept = kept;
                bq = q;
            }
        }
        p = bq;
    }
    cell_t c;
    if (fg == bg)
        p = 0;
    if (p == 0 || p == 15) {
        c.ch = ' ';
        c.fg = NO_FG;
        c.bg = p ? fg : bg;
        return c;
    }
    bool swap = !bar[p];
    c.ch = swap ? bar[~p & 15] : bar[p];
    c.fg = swap ? bg : fg;
    c.bg = swap ? fg : bg;
    return c;
}

/* mode 256: the upper pixel in the upper half block, the lower one below */
static cell_t half_block_cell(int top, int bottom)
{
    cell_t c;
    c.bg = rgb_of(bottom);
    if (rgb_of(top) == c.bg) { c.ch = ' '; c.fg = NO_FG; return c; }
    c.ch = 0x2580;
    c.fg = rgb_of(top);
    return c;
}

/* Blocks of bw x bh pixels, gx across and gy down, block (r, c) covering
   xs[bw c ..], ys[bh r ..]: mode 80's 4x8 pixels as 2x4 blocks of 2x2
   (octants), half-height quadrants' 2x4 pixels as 2x2 blocks of 1x2,
   half-width bars' as 1x4 blocks of 2x1. A block is its pixels' mean in
   linear light, and the cell the two shades that fit its blocks best;
   crisp (or with 16 colours) a block is its commonest colour, a tie going
   to the first, or with ink_ties to an ink pixel's (a quadrant's two
   pixels: so a stroke one pixel tall stays; a bar's, one pixel wide). */
static cell_t blend_cell(const int *xs, const int *ys, int gx, int gy, int bw, int bh, bool ink_ties)
{
    int n = gx * gy, v[8];
    double l[8][3];
    bool crisp = opt.crisp || opt.colors == 16;
    for (int r = 0; r < gy; r++)
        for (int c = 0; c < gx; c++) {
            int q[4], ink[4], m = 0, k = gx * r + c;
            for (int j = 0; j < bh; j++)
                for (int i = 0; i < bw; i++) {
                    int raw = pixraw(xs[bw * c + i], ys[bh * r + j]);
                    ink[m] = raw >> 7 & 1;
                    q[m++] = raw & 15;
                }
            if (crisp) {
                int best = q[0], bn = 0, bink = ink[0];
                for (int a = 0; a < m; a++) {
                    int same = 0;
                    for (int t = 0; t < m; t++)
                        same += q[t] == q[a];
                    if (same > bn || (same == bn && ink_ties && ink[a] && !bink)) {
                        bn = same;
                        best = q[a];
                        bink = ink[a];
                    }
                }
                v[k] = best;
            } else {
                for (int s = 0; s < 3; s++) {
                    double sum = 0;
                    for (int a = 0; a < m; a++)
                        sum += lin(rgb_of(q[a]) >> (16 - 8 * s) & 255);
                    l[k][s] = sum / m;
                }
            }
        }
    if (crisp && gx == 2)
        return index_cell(v, n);
    if (crisp) {
        int a, b;
        unsigned p = index_split(v, n, &a, &b);
        double la[3], lb[3];
        if (b < 0)
            b = a;
        for (int k = 0; k < n; k++)
            lin3(rgb_of(v[k]), l[k]);
        lin3(rgb_of(a), la);
        lin3(rgb_of(b), lb);
        return bar_cell(p, rgb_of(b), rgb_of(a), l, la, lb);
    }
    /* blended: the two shades that fit the blocks best (2-means in linear
       light, from the two blocks furthest apart) */
    int ia = 0, ib = 0;
    double far = -1;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            double d = 0;
            for (int s = 0; s < 3; s++)
                d += (l[i][s] - l[j][s]) * (l[i][s] - l[j][s]);
            if (d > far) { far = d; ia = i; ib = j; }
        }
    double ca[3], cb[3];
    for (int s = 0; s < 3; s++) { ca[s] = l[ia][s]; cb[s] = l[ib][s]; }
    unsigned p = 0;
    for (int it = 0; it < 3; it++) {
        double sa[3] = { 0 }, sb[3] = { 0 };
        int na = 0, nb = 0;
        p = 0;
        for (int i = 0; i < n; i++) {
            double da = 0, db = 0;
            for (int s = 0; s < 3; s++) {
                da += (l[i][s] - ca[s]) * (l[i][s] - ca[s]);
                db += (l[i][s] - cb[s]) * (l[i][s] - cb[s]);
            }
            if (db < da) {
                p |= 1u << i;
                nb++;
                for (int s = 0; s < 3; s++) sb[s] += l[i][s];
            } else {
                na++;
                for (int s = 0; s < 3; s++) sa[s] += l[i][s];
            }
        }
        for (int s = 0; s < 3; s++) {
            if (na) ca[s] = sa[s] / na;
            if (nb) cb[s] = sb[s] / nb;
        }
    }
    uint32_t fa = srgb(ca[0]) << 16 | srgb(ca[1]) << 8 | srgb(ca[2]);
    uint32_t fb2 = srgb(cb[0]) << 16 | srgb(cb[1]) << 8 | srgb(cb[2]);
    if (gx == 1)
        return bar_cell(p, fb2, fa, l, ca, cb);
    return block_cell(p, fb2, fa, n);
}

/* Braille shows only its dots, so which colour is the dots is decided per
   8x8 character (the paper's grid, carried into the border): the ink
   pixels (bit 7, ula.h), as the Spectrum draws a letter or a sprite, even
   where ink covers most of a cell; but the paper pixels where the ink
   covers most of the character and has the colour of the paper around it
   (the commonest paper colour in the 5x5 characters of the paper centred
   on it) -- a road drawn in black ink with its white dashes left as
   paper, beside road that is black paper. Ink around a character (big
   lettering, a large sprite) never makes it flip. */
#define CH_N 64
static int ch_x0, ch_y0;                 /* the top-left of character (0, 0) */
static bool ch_flip[CH_N][CH_N];

static int black0(int c) { return c & 7 ? c : 0; }      /* bright black is black */

static void braille_chars(int w, int h, int px, int py)
{
    static uint16_t hist[CH_N][CH_N][16];      /* its paper pixels' colours */
    static int8_t inkc[CH_N][CH_N];
    static uint8_t inkn[CH_N][CH_N];
    ch_x0 = px - (px + 7) / 8 * 8;
    ch_y0 = py - (py + 7) / 8 * 8;
    int nx = (w - ch_x0 + 7) / 8, ny = (h - ch_y0 + 7) / 8;
    if (nx > CH_N) nx = CH_N;
    if (ny > CH_N) ny = CH_N;
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            int ink[16] = { 0 }, n = 0;
            memset(hist[j][i], 0, sizeof hist[j][i]);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int v = pixraw(ch_x0 + 8 * i + x, ch_y0 + 8 * j + y), c = black0(v & 15);
                    if (v & 0x80) { ink[c]++; n++; }
                    else hist[j][i][c]++;
                }
            int best = 0;
            for (int c = 1; c < 16; c++)
                if (ink[c] > ink[best]) best = c;
            inkn[j][i] = (uint8_t)n;
            inkc[j][i] = (int8_t)(n ? best : -1);
        }
    /* around a character of the paper, the paper only: the border is not
       the scene's background (the border has no ink, so no flips) */
    int pi0 = (px - ch_x0) / 8, pj0 = (py - ch_y0) / 8;
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            int around[16] = { 0 }, best = 0;
            ch_flip[j][i] = false;
            if (i < pi0 || j < pj0 || i >= pi0 + 32 || j >= pj0 + 24)
                continue;
            for (int b = j - 2; b <= j + 2; b++)
                for (int a = i - 2; a <= i + 2; a++)
                    if (a >= pi0 && b >= pj0 && a < pi0 + 32 && b < pj0 + 24 && a < nx && b < ny)
                        for (int c = 0; c < 16; c++)
                            around[c] += hist[b][a][c];
            for (int c = 1; c < 16; c++)
                if (around[c] > around[best]) best = c;
            ch_flip[j][i] = inkn[j][i] > 32 && around[best] && inkc[j][i] == best;
        }
}

/* Dot (r, c) covers pixels xs[sx c ..], ys[sy r ..]: one pixel, or 2x2 in
   mode 80 (ink if half of them are). A cell with more than one ink or
   paper colour takes the commonest of each. */
static cell_t braille_cell(const int *xs, const int *ys, int sx, int sy)
{
    int inkn[16] = { 0 }, papern[16] = { 0 };
    unsigned p = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 2; c++) {
            int n = 0;
            for (int j = 0; j < sy; j++)
                for (int i = 0; i < sx; i++) {
                    int v = pixraw(xs[sx * c + i], ys[sy * r + j]), shown = v & 15;
                    int other = (v & 8) | (v >> 4 & 7);
                    bool bit = v >> 7 & 1;
                    n += bit;
                    inkn[bit ? shown : other]++;
                    papern[bit ? other : shown]++;
                }
            if (2 * n >= sx * sy)
                p |= 1u << (2 * r + c);
        }
    int ink = 0, paper = 0;
    for (int k = 1; k < 16; k++) {
        if (inkn[k] > inkn[ink]) ink = k;
        if (papern[k] > papern[paper]) paper = k;
    }
    int i = (xs[0] - ch_x0) / 8, j = (ys[0] - ch_y0) / 8;
    if (i >= 0 && j >= 0 && i < CH_N && j < CH_N && ch_flip[j][i]) {
        int t = ink; ink = paper; paper = t;
        p ^= 0xFF;
    }
    cell_t cell;
    cell.bg = rgb_of(paper);
    if (p == 0 || black0(ink) == black0(paper)) { cell.ch = ' '; cell.fg = NO_FG; return cell; }
    cell.ch = braille[p];
    cell.fg = rgb_of(ink);
    return cell;
}

/* mode 80: an 8x8 character cell that is a glyph of the font, or its
   inverse: two cells, the character and a blank (or one wide glyph) */
static bool text_cells(int x0, int y0, const uint8_t *font, cell_t *c0, cell_t *c1)
{
    int a = pix(x0, y0), b = -1;
    uint64_t bits = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int v = pix(x0 + x, y0 + y);
            if (v != a) {
                if (b >= 0 && v != b)
                    return false;               /* three colours */
                b = v;
                bits |= 1ULL << (63 - (y * 8 + x));
            }
        }
    if (b < 0) {                                /* one colour: blank */
        c0->ch = c1->ch = ' ';
        c0->fg = c1->fg = NO_FG;
        c0->bg = c1->bg = rgb_of(a);
        return true;
    }
    for (int inv = 0; inv < 2; inv++) {
        uint64_t want = inv ? ~bits : bits;
        for (int g = 1; g < 96; g++) {
            uint64_t glyph = 0;
            for (int r = 0; r < 8; r++)
                glyph = glyph << 8 | font[8 * g + r];
            if (glyph && glyph == want) {
                uint32_t ink = rgb_of(inv ? a : b), paper = rgb_of(inv ? b : a);
                int code = 32 + g;
                uint32_t ch = code == 0x60 ? 0xA3 : code == 0x5E ? 0x2191 : code == 0x7F ? 0xA9 : (uint32_t)code;
                if (opt.fullwidth && ch > 0x20 && ch < 0x7F) {
                    c0->ch = ch + 0xFEE0;
                    c0->fg = ink;
                    c0->bg = paper;
                    c1->ch = 0;                  /* the wide glyph's right half */
                    c1->fg = ink;
                    c1->bg = paper;
                } else {
                    c0->ch = ch;
                    c0->fg = ink;
                    c0->bg = paper;
                    c1->ch = ' ';
                    c1->fg = NO_FG;
                    c1->bg = paper;
                }
                return true;
            }
        }
    }
    return false;
}

/* the picture's pixel columns (x) for each cell column, two (four in mode
   80, one in mode 256) a cell; mode 120 draws the paper's character columns
   3, 7, ... without their pixel columns 0 and 7 */
static int colx[1024][4], rowy[512][8];

void term_frame(const uint8_t *fb, int w, int h, int px, int py, const uint8_t *font)
{
    g_fb = fb;
    g_w = w;
    g_h = h;
    bool q = quads(), hw = bars();
    int pw = mode == 80 ? 4 : mode == 256 ? 1 : 2;                     /* pixels a cell */
    int ph = mode == 80 ? 8 : mode == 256 || (q && qrows == 96) ? 2 : 4;
    int paper_w = mode == 256 ? 256 : mode == 128 ? 128 : mode == 120 ? 120 : 64, paper_h = 192 / ph;
    int bx = (cols - paper_w) / 2, by = (rows - paper_h) / 2;
    if (bx > px / pw) bx = px / pw;
    if (bx > (w - px - 256) / pw) bx = (w - px - 256) / pw;
    if (by > py / ph) by = py / ph;
    if (by > (h - py - 192) / ph) by = (h - py - 192) / ph;
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;
    grid_w = paper_w + 2 * bx;
    grid_h = paper_h + 2 * by;
    ox = (cols - grid_w) / 2;
    oy = (rows - grid_h) / 2;
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    /* columns: border, paper, border */
    int n = 0, xs[1024 * 4];
    for (int x = px - bx * pw; x < px; x++) xs[n++] = x;
    for (int x = 0; x < 256; x++)
        if (!(mode == 120 && (x >> 3) % 4 == 3 && ((x & 7) == 0 || (x & 7) == 7)))
            xs[n++] = px + x;
    for (int x = px + 256; x < px + 256 + bx * pw; x++) xs[n++] = x;
    for (int c = 0; c < grid_w && c < 1024; c++)
        for (int i = 0; i < pw; i++)
            colx[c][i] = xs[c * pw + i];
    for (int r = 0; r < grid_h && r < 512; r++)
        for (int i = 0; i < ph; i++)
            rowy[r][i] = py - by * ph + r * ph + i;

    bool dots = opt.glyphs == GLYPHS_BRAILLE && mode != 256;
    if (dots)
        braille_chars(w, h, px, py);
    /* the cells */
    for (int r = 0; r < grid_h; r++) {
        for (int c = 0; c < grid_w; c++) {
            if (r + oy >= rows || c + ox >= cols)
                continue;
            cell_t *cell = &cur[(size_t)(r + oy) * (size_t)cols + (size_t)(c + ox)];
            if (mode == 256) {
                *cell = half_block_cell(pix(colx[c][0], rowy[r][0]), pix(colx[c][0], rowy[r][1]));
            } else if (mode == 80) {
                /* in the paper: text first */
                int pc = c - bx, pr = r - by;
                if (font && pc >= 0 && pc < 64 && pr >= 0 && pr < 24 && !(pc & 1)) {
                    cell_t t0, t1;
                    if (text_cells(px + pc / 2 * 8, py + pr * 8, font, &t0, &t1)) {
                        *cell = t0;
                        if (c + 1 + ox < cols)
                            cell[1] = t1;
                        c++;
                        continue;
                    }
                }
                *cell = dots ? braille_cell(colx[c], rowy[r], 2, 2) : blend_cell(colx[c], rowy[r], 2, 4, 2, 2, false);
            } else if (dots) {
                *cell = braille_cell(colx[c], rowy[r], 1, 1);
            } else if (hw) {
                *cell = blend_cell(colx[c], rowy[r], 1, 4, 2, 1, true);
            } else if (q && qrows == 48) {
                *cell = blend_cell(colx[c], rowy[r], 2, 2, 1, 2, true);
            } else {
                int v[8];
                for (int rr = 0; rr < ph; rr++)
                    for (int cc = 0; cc < 2; cc++)
                        v[2 * rr + cc] = pix(colx[c][cc], rowy[r][rr]);
                *cell = index_cell(v, 2 * ph);
            }
        }
    }

    /* what changed */
    puts_("\x1b[?2026h");
    if (redraw)
        puts_("\x1b[0m\x1b[2J");
    uint32_t fg = NO_FG - 1, bg = NO_FG - 1;
    int at_r = -1, at_c = -1;
    char seq[64];
    for (int r = 0; r < grid_h && r + oy < rows; r++) {
        for (int c = 0; c < grid_w && c + ox < cols; c++) {
            size_t i = (size_t)(r + oy) * (size_t)cols + (size_t)(c + ox);
            cell_t *a = &cur[i], *p = &prev[i];
            if (!redraw && a->ch == p->ch && a->fg == p->fg && a->bg == p->bg)
                continue;
            if (a->ch == 0)                      /* a wide glyph's right half */
                continue;
            if (at_r != r + oy || at_c != c + ox) {
                snprintf(seq, sizeof seq, "\x1b[%d;%dH", r + oy + 1, c + ox + 1);
                puts_(seq);
            }
            char sgr[64];
            size_t sl = 0;
            sgr[0] = 0;
            for (int pass = 0; pass < 2; pass++) {
                uint32_t want = pass ? a->fg : a->bg, *have = pass ? &fg : &bg;
                if (want == NO_FG || want == *have)
                    continue;
                *have = want;
                int base = pass ? 38 : 48;
                if (opt.colors == 24)
                    sl += (size_t)snprintf(sgr + sl, sizeof sgr - sl, "%s%d;2;%u;%u;%u", sl ? ";" : "",
                                           base, want >> 16, want >> 8 & 255, want & 255);
                else if (opt.colors == 256)
                    sl += (size_t)snprintf(sgr + sl, sizeof sgr - sl, "%s%d;5;%d", sl ? ";" : "", base, index256(want));
                else
                    sl += (size_t)snprintf(sgr + sl, sizeof sgr - sl, "%s%d", sl ? ";" : "",
                                           (pass ? 30 : 40) + ansi16(want));
            }
            if (sl) {
                puts_("\x1b[");
                put(sgr, sl);
                puts_("m");
            }
            put_utf8(a->ch);
            at_r = r + oy;
            at_c = c + ox + (a->ch >= 0xFF01 && a->ch <= 0xFF5E ? 2 : 1);
        }
    }
    puts_("\x1b[0m\x1b[?2026l");
    flush_out();
    memcpy(prev, cur, (size_t)cols * (size_t)rows * sizeof *cur);
    redraw = false;
}
