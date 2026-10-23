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
    if (opt.glyphs == GLYPHS_AUTO) {
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
    if (!mode)
        mode = cols >= 128 && rows >= 48 ? 128 : cols >= 120 && rows >= 48 ? 120 : 80;
    free(cur);
    free(prev);
    cur = calloc((size_t)cols * (size_t)rows, sizeof *cur);
    prev = calloc((size_t)cols * (size_t)rows, sizeof *prev);
    redraw = true;
}

int term_mode(void) { return mode; }
bool term_braille(void) { return opt.glyphs == GLYPHS_BRAILLE; }
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

/* octants: p's blocks in fg, the rest in bg */
static cell_t block_cell(unsigned p, uint32_t fg, uint32_t bg)
{
    cell_t c;
    if (fg == bg)
        p = 0;
    if (p == 0) { c.ch = ' '; c.fg = NO_FG; c.bg = bg; return c; }
    if (p == 0xFF) { c.ch = ' '; c.fg = NO_FG; c.bg = fg; return c; }
    c.ch = octant[p];
    c.fg = fg;
    c.bg = bg;
    return c;
}

/* eight colour indices (2x4, bit order) -> a cell: two colours exact, more
   than two the two most common, the rest to the nearer of them */
static cell_t index_cell(const int v[8])
{
    int count[16] = { 0 }, a = -1, b = -1;
    for (int i = 0; i < 8; i++)
        count[v[i]]++;
    for (int i = 0; i < 8; i++) {
        int c = v[i];
        if (a < 0 || count[c] > count[a])     /* the first of the most common */
            a = c;
    }
    for (int i = 0; i < 8; i++) {
        int c = v[i];
        if (c != a && (b < 0 || count[c] > count[b]))
            b = c;
    }
    if (b < 0)
        return block_cell(0, rgb_of(a), rgb_of(a));
    uint32_t ra = rgb_of(a), rb = rgb_of(b);
    unsigned p = 0;
    for (int i = 0; i < 8; i++) {
        int c = v[i];
        bool to_b = c == b;
        if (c != a && c != b)
            to_b = dist2(rgb_of(c), rb) < dist2(rgb_of(c), ra);
        if (to_b)
            p |= 1u << i;
    }
    return block_cell(p, rb, ra);
}

/* mode 80: 4x8 pixels as 2x4 blocks of 2x2 */
static cell_t half_cell(int x0, int y0)
{
    int v[8];
    double l[8][3];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 2; c++) {
            int q[4] = { pix(x0 + 2 * c, y0 + 2 * r), pix(x0 + 2 * c + 1, y0 + 2 * r),
                         pix(x0 + 2 * c, y0 + 2 * r + 1), pix(x0 + 2 * c + 1, y0 + 2 * r + 1) };
            int i = 2 * r + c;
            if (opt.crisp || opt.colors == 16) {
                /* the block's most common colour (the first on a tie) */
                int best = q[0], bn = 0;
                for (int a = 0; a < 4; a++) {
                    int n = 0;
                    for (int b = 0; b < 4; b++)
                        n += q[b] == q[a];
                    if (n > bn) { bn = n; best = q[a]; }
                }
                v[i] = best;
            } else {
                for (int s = 0; s < 3; s++) {
                    double sum = 0;
                    for (int a = 0; a < 4; a++)
                        sum += lin(rgb_of(q[a]) >> (16 - 8 * s) & 255);
                    l[i][s] = sum / 4;
                }
            }
        }
    if (opt.crisp || opt.colors == 16)
        return index_cell(v);
    /* blended: the two shades that fit the eight blocks best (2-means in
       linear light, from the two blocks furthest apart) */
    int ia = 0, ib = 0;
    double far = -1;
    for (int i = 0; i < 8; i++)
        for (int j = i + 1; j < 8; j++) {
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
        for (int i = 0; i < 8; i++) {
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
    return block_cell(p, fb2, fa);
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
   80) a cell; mode 120 draws the paper's character columns 3, 7, ... without
   their pixel columns 0 and 7 */
static int colx[1024][4], rowy[512][8];

void term_frame(const uint8_t *fb, int w, int h, int px, int py, const uint8_t *font)
{
    g_fb = fb;
    g_w = w;
    g_h = h;
    int pw = mode == 80 ? 4 : 2, ph = mode == 80 ? 8 : 4;          /* pixels a cell */
    int paper_w = mode == 128 ? 128 : mode == 120 ? 120 : 64, paper_h = mode == 80 ? 24 : 48;
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

    bool dots = opt.glyphs == GLYPHS_BRAILLE;
    if (dots)
        braille_chars(w, h, px, py);
    /* the cells */
    for (int r = 0; r < grid_h; r++) {
        for (int c = 0; c < grid_w; c++) {
            if (r + oy >= rows || c + ox >= cols)
                continue;
            cell_t *cell = &cur[(size_t)(r + oy) * (size_t)cols + (size_t)(c + ox)];
            if (mode == 80) {
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
                *cell = dots ? braille_cell(colx[c], rowy[r], 2, 2) : half_cell(colx[c][0], rowy[r][0]);
            } else if (dots) {
                *cell = braille_cell(colx[c], rowy[r], 1, 1);
            } else {
                int v[8];
                for (int rr = 0; rr < 4; rr++)
                    for (int cc = 0; cc < 2; cc++)
                        v[2 * rr + cc] = pix(colx[c][cc], rowy[r][rr]);
                *cell = index_cell(v);
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
