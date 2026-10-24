/* tape.c - see tape.h */
#include "tape.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T_PILOT   2168u        /* the ROM's timings                          */
#define T_SYNC1    667u
#define T_SYNC2    735u
#define T_ZERO     855u
#define T_ONE     1710u
#define T_MS      3500u        /* T-states a millisecond                     */

enum {
    ST_PILOT, ST_SYNC1, ST_SYNC2, ST_BITS, ST_PAUSE1, ST_PAUSE2, ST_DONE,
    ST_TONE, ST_SEQ, ST_DIRECT, ST_CSW, ST_GPILOT, ST_GDATA, ST_STOP, ST_LEVEL
};

static uint32_t w16(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8); }
static uint32_t w24(const uint8_t *p) { return w16(p) | (uint32_t)p[2] << 16; }
static uint32_t w32(const uint8_t *p) { return w24(p) | (uint32_t)p[3] << 24; }

/* ---- Z-RLE: a small inflate (RFC 1950 / 1951) ------------------------- */

typedef struct {
    const uint8_t *in;
    size_t   n, pos;
    uint32_t buf;
    int      cnt;
    uint8_t *out;
    size_t   on, ocap;
    bool     err;
} zs_t;

typedef struct { uint16_t count[16], sym[320]; } zh_t;

static int zbits(zs_t *s, int need)
{
    uint32_t v = s->buf;
    while (s->cnt < need) {
        if (s->pos >= s->n) {
            s->err = true;
            return 0;
        }
        v |= (uint32_t)s->in[s->pos++] << s->cnt;
        s->cnt += 8;
    }
    s->buf = v >> need;
    s->cnt -= need;
    return (int)(v & ((1u << need) - 1));
}

static void zput(zs_t *s, uint8_t c)
{
    if (s->on == s->ocap) {
        size_t cap = s->ocap ? s->ocap * 2 : 65536;
        uint8_t *o = realloc(s->out, cap);
        if (!o) {
            s->err = true;
            return;
        }
        s->out = o;
        s->ocap = cap;
    }
    s->out[s->on++] = c;
}

/* canonical codes from code lengths; < 0 if over-subscribed */
static int zbuild(zh_t *h, const uint8_t *len, int n)
{
    int off[16], left = 1;
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++)
        h->count[len[i]]++;
    for (int l = 1; l < 16; l++) {
        left = (left << 1) - h->count[l];
        if (left < 0)
            return -1;
    }
    off[1] = 0;
    for (int l = 1; l < 15; l++)
        off[l + 1] = off[l] + h->count[l];
    for (int i = 0; i < n; i++)
        if (len[i])
            h->sym[off[len[i]]++] = (uint16_t)i;
    return left;
}

static int zdecode(zs_t *s, const zh_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= zbits(s, 1);
        int c = h->count[l];
        if (code - first < c)
            return h->sym[index + (code - first)];
        index += c;
        first = (first + c) << 1;
        code <<= 1;
    }
    s->err = true;
    return -1;
}

static int zcodes(zs_t *s, const zh_t *lh, const zh_t *dh)
{
    static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                      3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
                                        385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
                                        12289, 16385, 24577 };
    static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                      9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;) {
        int sym = zdecode(s, lh);
        if (s->err)
            return -1;
        if (sym < 256) {
            zput(s, (uint8_t)sym);
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29)
                return -1;
            int len = lbase[sym] + zbits(s, lext[sym]);
            int d = zdecode(s, dh);
            if (d < 0 || d >= 30)
                return -1;
            size_t dist = dbase[d] + (size_t)zbits(s, dext[d]);
            if (s->err || dist > s->on)
                return -1;
            while (len-- && !s->err)
                zput(s, s->out[s->on - dist]);
        }
        if (s->err)
            return -1;
    }
}

/* a zlib stream inflated into a malloc'ed buffer; NULL if it is not one */
static uint8_t *zinflate(const uint8_t *in, size_t n, size_t *out_n)
{
    zs_t s = { .in = in, .n = n };
    if (n < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20))
        return NULL;
    s.pos = 2;
    int last;
    do {
        last = zbits(&s, 1);
        int type = zbits(&s, 2);
        if (type == 0) {                       /* stored */
            s.buf = 0;
            s.cnt = 0;
            if (s.pos + 4 > s.n)
                goto fail;
            unsigned len = w16(s.in + s.pos), nlen = w16(s.in + s.pos + 2);
            s.pos += 4;
            if ((len ^ 0xFFFF) != nlen || s.pos + len > s.n)
                goto fail;
            for (unsigned i = 0; i < len; i++)
                zput(&s, s.in[s.pos++]);
        } else if (type == 1) {                /* fixed codes */
            static zh_t lh, dh;
            static bool built;
            if (!built) {
                uint8_t len[288];
                for (int i = 0; i < 288; i++)
                    len[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                zbuild(&lh, len, 288);
                memset(len, 5, 30);
                zbuild(&dh, len, 30);
                built = true;
            }
            if (zcodes(&s, &lh, &dh))
                goto fail;
        } else if (type == 2) {                /* dynamic codes */
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            int nlen = zbits(&s, 5) + 257, ndist = zbits(&s, 5) + 1, ncode = zbits(&s, 4) + 4;
            uint8_t len[320] = { 0 };
            zh_t ch, lh, dh;
            if (nlen > 286 || ndist > 30)
                goto fail;
            for (int i = 0; i < ncode; i++)
                len[order[i]] = (uint8_t)zbits(&s, 3);
            if (zbuild(&ch, len, 19) != 0)
                goto fail;
            for (int i = 0; i < nlen + ndist;) {
                int sym = zdecode(&s, &ch), rep, v = 0;
                if (s.err)
                    goto fail;
                if (sym < 16) {
                    len[i++] = (uint8_t)sym;
                    continue;
                }
                if (sym == 16) {
                    if (!i)
                        goto fail;
                    v = len[i - 1];
                    rep = 3 + zbits(&s, 2);
                } else {
                    rep = sym == 17 ? 3 + zbits(&s, 3) : 11 + zbits(&s, 7);
                }
                if (i + rep > nlen + ndist)
                    goto fail;
                while (rep--)
                    len[i++] = (uint8_t)v;
            }
            memset(len + nlen + ndist, 0, sizeof len - (size_t)(nlen + ndist));
            if (!len[256] || zbuild(&lh, len, nlen) < 0 || zbuild(&dh, len + nlen, ndist) < 0)
                goto fail;
            if (zcodes(&s, &lh, &dh))
                goto fail;
        } else {
            goto fail;
        }
        if (s.err)
            goto fail;
    } while (!last);
    *out_n = s.on;
    return s.out;
fail:
    free(s.out);
    return NULL;
}

/* ---- the file --------------------------------------------------------- */

/* a TZX block's body length (after the ID); false if it does not fit */
static bool body_len(uint8_t id, const uint8_t *p, size_t avail, uint64_t *len)
{
#define NEED(k) do { if (avail < (k)) return false; } while (0)
    switch (id) {
    case 0x10: NEED(4);  *len = 4 + (uint64_t)w16(p + 2); break;
    case 0x11: NEED(18); *len = 18 + (uint64_t)w24(p + 15); break;
    case 0x12: *len = 4; break;
    case 0x13: NEED(1);  *len = 1 + 2 * (uint64_t)p[0]; break;
    case 0x14: NEED(10); *len = 10 + (uint64_t)w24(p + 7); break;
    case 0x15: NEED(8);  *len = 8 + (uint64_t)w24(p + 5); break;
    case 0x20: case 0x23: case 0x24: *len = 2; break;
    case 0x21: case 0x30: NEED(1); *len = 1 + (uint64_t)p[0]; break;
    case 0x22: case 0x25: case 0x27: *len = 0; break;
    case 0x26: NEED(2);  *len = 2 + 2 * (uint64_t)w16(p); break;
    case 0x28: case 0x32: NEED(2); *len = 2 + (uint64_t)w16(p); break;
    case 0x31: NEED(2);  *len = 2 + (uint64_t)p[1]; break;
    case 0x33: NEED(1);  *len = 1 + 3 * (uint64_t)p[0]; break;
    case 0x34: *len = 8; break;
    case 0x35: NEED(20); *len = 20 + (uint64_t)w32(p + 16); break;
    case 0x40: NEED(4);  *len = 4 + (uint64_t)w24(p + 1); break;
    case 0x5A: *len = 9; break;
    default:   NEED(4);  *len = 4 + (uint64_t)w32(p); break;   /* 0x16-0x19, 0x2A, 0x2B, 0x4B, ... */
    }
    return *len <= avail;
#undef NEED
}

static bool add_block(tape_t *t, int *cap, uint8_t id, size_t off, size_t body, uint32_t len)
{
    if (t->nb == *cap) {
        int c = *cap ? *cap * 2 : 64;
        tape_block_t *b = realloc(t->b, (size_t)c * sizeof *b);
        if (!b)
            return false;
        t->b = b;
        *cap = c;
    }
    t->b[t->nb++] = (tape_block_t){ .id = id, .body = t->data + body, .len = len, .off = off };
    return true;
}

/* 0x19's tables fit in its body; their places in the block */
static bool gen_fits(const tape_block_t *b)
{
    if (b->len < 18)
        return false;
    const uint8_t *p = b->body;
    uint64_t totp = w32(p + 6), totd = w32(p + 12), at = 18;
    unsigned npp = p[10], asp = p[11] ? p[11] : 256, npd = p[16], asd = p[17] ? p[17] : 256;
    unsigned nb = 0;
    while ((1u << nb) < asd)
        nb++;
    if (totp)
        at += (uint64_t)asp * (1 + 2 * npp) + totp * 3;
    if (totd)
        at += (uint64_t)asd * (1 + 2 * npd) + (nb * totd + 7) / 8;
    return at <= b->len;
}

static bool parse(tape_t *t)
{
    int cap = 0;
    if (t->size >= 10 && !memcmp(t->data, "ZXTape!\x1A", 8)) {
        t->tzx = true;
        for (size_t at = 10; at < t->size;) {
            uint8_t id = t->data[at];
            uint64_t len;
            if (!body_len(id, t->data + at + 1, t->size - at - 1, &len) || len > UINT32_MAX)
                return false;
            if (!add_block(t, &cap, id, at, at + 1, (uint32_t)len))
                return false;
            tape_block_t *b = &t->b[t->nb - 1];
            if (id == 0x18) {
                if (len < 14 || !w24(b->body + 6))
                    return false;
                if (b->body[9] == 2) {          /* Z-RLE: inflate it now */
                    b->csw = zinflate(b->body + 14, len - 14, &b->csw_n);
                    if (!b->csw)
                        return false;
                }
            }
            if (id == 0x19 && !gen_fits(b))
                return false;
            if ((id == 0x2B && len < 5) || (id == 0x28 && len >= 3 && t->data[at + 3] && len < 6))
                return false;
            at += 1 + len;
        }
    } else {
        /* a .tap: the blocks must tile the file */
        for (size_t at = 0; at < t->size;) {
            if (at + 2 > t->size)
                return false;
            uint32_t len = w16(t->data + at);
            if (at + 2 + len > t->size || !add_block(t, &cap, 0, at, at + 2, len))
                return false;
            at += 2 + len;
        }
    }
    t->blocks = (unsigned)t->nb;
    return true;
}

bool tape_load_mem(tape_t *t, const uint8_t *buf, size_t n)
{
    memset(t, 0, sizeof *t);
    if (!n || !(t->data = malloc(n))) {
        errno = n ? ENOMEM : EINVAL;
        return false;
    }
    memcpy(t->data, buf, n);
    t->size = n;
    t->call_block = -1;
    if (!parse(t)) {
        tape_free(t);
        errno = EINVAL;
        return false;
    }
    return true;
}

bool tape_load(tape_t *t, const char *path)
{
    memset(t, 0, sizeof *t);
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        free(buf);
        errno = n <= 0 ? EINVAL : EIO;
        return false;
    }
    fclose(fp);
    bool ok = tape_load_mem(t, buf, (size_t)n);
    free(buf);
    return ok;
}

void tape_free(tape_t *t)
{
    for (int i = 0; i < t->nb; i++)
        free(t->b[i].csw);
    free(t->b);
    free(t->data);
    memset(t, 0, sizeof *t);
}

int tape_percent(const tape_t *t)
{
    if (!t->data || !t->size)
        return 0;
    if (t->ended)
        return 100;
    size_t at = t->cur < t->nb ? t->b[t->cur].off : t->size;
    if (t->started && t->stage == ST_BITS && t->d >= t->data && t->d < t->data + t->size)
        at = (size_t)(t->d - t->data) + t->pos;
    return (int)(at * 100 / t->size);
}

static void put_text(char *buf, size_t n, size_t *at, const uint8_t *s, size_t len)
{
    for (size_t i = 0; i < len && *at + 1 < n; i++)
        buf[(*at)++] = s[i] >= 32 && s[i] < 127 ? (char)s[i] : ' ';
    buf[*at] = 0;
}

void tape_describe(const tape_t *t, char *buf, size_t n)
{
    size_t at = 0;
    if (!n)
        return;
    buf[0] = 0;
    for (int i = 0; i < t->nb; i++) {          /* archive info: title (publisher, year); loader */
        const tape_block_t *b = &t->b[i];
        if (b->id != 0x32 || b->len < 3)
            continue;
        const uint8_t *f[256] = { 0 }, *p = b->body + 3, *end = b->body + b->len;
        for (unsigned k = 0; k < b->body[2] && p + 2 <= end && p + 2 + p[1] <= end; k++, p += 2 + p[1])
            if (!f[p[0]])
                f[p[0]] = p;
        static const struct { uint8_t id; const char *pre; } part[] = {
            { 0x00, "" }, { 0x01, " (" }, { 0x03, ", " }, { 0x07, "; loader: " } };
        for (int k = 0; k < 4; k++) {
            const uint8_t *e = f[part[k].id];
            if (!e)
                continue;
            const char *pre = k == 2 && !f[0x01] ? " (" : part[k].pre;
            if (k == 3 && (f[0x01] || f[0x03]))
                pre = "); loader: ";
            put_text(buf, n, &at, (const uint8_t *)pre, strlen(pre));
            put_text(buf, n, &at, e + 2, e[1]);
        }
        if ((f[0x01] || f[0x03]) && !f[0x07])
            put_text(buf, n, &at, (const uint8_t *)")", 1);
        return;
    }
    for (int i = 0; i < t->nb; i++)            /* else the first text description */
        if (t->b[i].id == 0x30) {
            put_text(buf, n, &at, t->b[i].body + 1, t->b[i].body[0]);
            return;
        }
}

/* ---- the player ------------------------------------------------------- */

static int sample(const tape_t *t, uint32_t i)
{
    return t->d[i >> 3] >> (7 - (i & 7)) & 1;
}

/* set up block cur; false for a block that plays nothing (cur moved on) */
static bool start(tape_t *t)
{
    tape_block_t *b = &t->b[t->cur];
    const uint8_t *p = b->body;
    t->started = true;
    t->pos = 0;
    t->bit = 0;
    t->pause = 0;
    switch (b->id) {
    case 0x00:                                 /* .tap */
    case 0x10:
        t->pause = b->id ? w16(p) : 1000;
        t->d = b->id ? p + 4 : p;
        t->n = b->id ? w16(p + 2) : b->len;
        t->pilot = T_PILOT, t->sync1 = T_SYNC1, t->sync2 = T_SYNC2, t->zero = T_ZERO, t->one = T_ONE;
        t->count = !t->n ? 0 : t->d[0] < 0x80 ? 8063 : 3223;
        t->last = 8;
        t->stage = t->n ? ST_PILOT : ST_PAUSE1;
        return true;
    case 0x11:
        t->pilot = w16(p), t->sync1 = w16(p + 2), t->sync2 = w16(p + 4);
        t->zero = w16(p + 6), t->one = w16(p + 8), t->count = w16(p + 10);
        t->last = p[12] && p[12] <= 8 ? p[12] : 8;
        t->pause = w16(p + 13);
        t->n = w24(p + 15);
        t->d = p + 18;
        t->stage = ST_PILOT;
        return true;
    case 0x12:
        t->pilot = w16(p);
        t->count = w16(p + 2);
        t->stage = ST_TONE;
        return true;
    case 0x13:
        t->n = p[0];
        t->d = p + 1;
        t->stage = ST_SEQ;
        return true;
    case 0x14:
        t->zero = w16(p), t->one = w16(p + 2);
        t->last = p[4] && p[4] <= 8 ? p[4] : 8;
        t->pause = w16(p + 5);
        t->n = w24(p + 7);
        t->d = p + 10;
        t->count = 0;
        t->sync1 = t->sync2 = 0;
        t->stage = t->n ? ST_BITS : ST_PAUSE1;
        return true;
    case 0x15:
        t->zero = w16(p);                      /* T-states a sample */
        t->pause = w16(p + 2);
        t->last = p[4] && p[4] <= 8 ? p[4] : 8;
        t->n = w24(p + 5);
        t->d = p + 8;
        t->count = t->n ? (t->n - 1) * 8 + (uint32_t)t->last : 0;
        t->stage = ST_DIRECT;
        return true;
    case 0x18:
        t->pause = w16(p + 4);
        t->rate = w24(p + 6);
        t->count = w32(p + 10);
        t->d = b->csw ? b->csw : p + 14;
        t->n = b->csw ? (uint32_t)b->csw_n : b->len - 14;
        t->acc = t->csw_t = 0;
        t->stage = ST_CSW;
        return true;
    case 0x19: {
        t->pause = w16(p + 4);
        t->g_tot[0] = w32(p + 6), t->g_np[0] = p[10], t->g_as[0] = p[11] ? p[11] : 256;
        t->g_tot[1] = w32(p + 12), t->g_np[1] = p[16], t->g_as[1] = p[17] ? p[17] : 256;
        t->g_nb = 0;
        while ((1 << t->g_nb) < t->g_as[1])
            t->g_nb++;
        const uint8_t *q = p + 18;
        t->g_sym[0] = q;
        if (t->g_tot[0])
            q += t->g_as[0] * (1 + 2 * t->g_np[0]);
        t->g_prle = q;
        if (t->g_tot[0])
            q += t->g_tot[0] * 3;
        t->g_sym[1] = q;
        if (t->g_tot[1])
            q += t->g_as[1] * (1 + 2 * t->g_np[1]);
        t->g_ds = q;
        t->rep = 0;
        t->stage = ST_GPILOT;
        return true;
    }
    case 0x20:
        t->pause = w16(p);
        t->stage = t->pause ? ST_PAUSE1 : ST_STOP;
        return true;
    case 0x2A:                                 /* stop the tape if in 48K mode: this is one */
        t->stage = ST_STOP;
        return true;
    case 0x2B:
        t->stage = ST_LEVEL;
        return true;
    }
    /* the blocks that steer the tape, or play nothing */
    int next = t->cur + 1;
    switch (b->id) {
    case 0x23: {                               /* jump: 0 would loop forever */
        int16_t rel = (int16_t)w16(p);
        if (rel)
            next = t->cur + rel;
        break;
    }
    case 0x24:                                 /* loop start */
        if (t->loops < 8 && w16(p)) {
            t->loop[t->loops].start = t->cur + 1;
            t->loop[t->loops].left = (int)w16(p);
            t->loops++;
        }
        break;
    case 0x25:                                 /* loop end */
        if (t->loops) {
            if (--t->loop[t->loops - 1].left > 0)
                next = t->loop[t->loops - 1].start;
            else
                t->loops--;
        }
        break;
    case 0x26:                                 /* call a sequence */
        if (w16(p) && t->call_block < 0) {
            t->call_block = t->cur;
            t->call_i = 0;
            next = t->cur + (int16_t)w16(p + 2);
        }
        break;
    case 0x27:                                 /* return from it */
        if (t->call_block >= 0) {
            const uint8_t *c = t->b[t->call_block].body;
            if (++t->call_i < (int)w16(c)) {
                next = t->call_block + (int16_t)w16(c + 2 + 2 * t->call_i);
            } else {
                next = t->call_block + 1;
                t->call_block = -1;
            }
        }
        break;
    case 0x28:                                 /* select: the first entry */
        if (b->len >= 6 && p[2])
            next = t->cur + (int16_t)w16(p + 3);
        break;
    }
    if (next < 0 || next == t->cur)
        next = t->cur + 1;
    t->cur = next;
    t->started = false;
    return false;
}

/* the next pulse of block cur; false when it has played */
static bool play(tape_t *t, uint32_t *len, int *rule)
{
    *rule = TAPE_TOGGLE;
    for (;;) {
        switch (t->stage) {
        case ST_PILOT:
            if (t->count) {
                t->count--;
                *len = t->pilot;
                return true;
            }
            t->stage = ST_SYNC1;
            continue;
        case ST_SYNC1:
            t->stage = ST_SYNC2;
            if (t->sync1) {
                *len = t->sync1;
                return true;
            }
            continue;
        case ST_SYNC2:
            t->stage = ST_BITS;
            t->pos = 0;
            t->bit = 0;
            if (t->sync2) {
                *len = t->sync2;
                return true;
            }
            continue;
        case ST_BITS: {
            if (t->pos >= t->n) {
                t->stage = ST_PAUSE1;
                continue;
            }
            int bits = t->pos + 1 == t->n ? t->last : 8, k = t->bit >> 1;
            if (k >= bits) {
                t->pos++;
                t->bit = 0;
                continue;
            }
            *len = (t->d[t->pos] >> (7 - k) & 1) ? t->one : t->zero;
            t->bit++;
            return true;
        }
        case ST_PAUSE1:                        /* an edge ends the last pulse; 1 ms, then low */
            if (!t->pause) {
                t->stage = ST_DONE;
                continue;
            }
            t->stage = ST_PAUSE2;
            *len = T_MS;
            return true;
        case ST_PAUSE2:
            t->stage = ST_DONE;
            *len = (t->pause - 1) * T_MS;
            *rule = TAPE_LOW;
            return true;
        case ST_DONE:
            return false;
        case ST_TONE:
            if (t->count) {
                t->count--;
                *len = t->pilot;
                return true;
            }
            t->stage = ST_DONE;
            continue;
        case ST_SEQ:
            if (t->pos < t->n) {
                *len = w16(t->d + 2 * t->pos++);
                return true;
            }
            t->stage = ST_DONE;
            continue;
        case ST_DIRECT: {                      /* a run of equal samples: one pulse */
            if (!t->count || !t->zero) {
                t->stage = ST_PAUSE1;
                continue;
            }
            int v = sample(t, t->pos);
            uint32_t run = 1;
            while (run < t->count && sample(t, t->pos + run) == v)
                run++;
            t->pos += run;
            t->count -= run;
            *len = run * t->zero;
            *rule = v ? TAPE_HIGH : TAPE_LOW;
            return true;
        }
        case ST_CSW: {
            if (!t->count || t->pos >= t->n) {
                t->stage = ST_PAUSE1;
                continue;
            }
            uint32_t c = t->d[t->pos++];
            if (!c) {
                if (t->pos + 4 > t->n) {
                    t->stage = ST_PAUSE1;
                    continue;
                }
                c = w32(t->d + t->pos);
                t->pos += 4;
            }
            t->count--;
            t->acc += (uint64_t)c * 3500000u;
            uint64_t end = t->acc / t->rate;
            *len = (uint32_t)(end - t->csw_t);
            t->csw_t = end;
            return true;
        }
        case ST_GPILOT:
        case ST_GDATA: {
            int g = t->stage == ST_GDATA;
            if (!t->bit && !t->rep) {          /* the next symbol */
                if (t->pos >= t->g_tot[g]) {
                    if (!g) {
                        t->stage = ST_GDATA;
                        t->pos = 0;
                    } else {
                        t->stage = ST_PAUSE1;
                    }
                    continue;
                }
                if (!g) {
                    const uint8_t *e = t->g_prle + 3 * t->pos++;
                    t->sym = e[0];
                    t->rep = w16(e + 1);
                } else {
                    uint32_t s = 0, at = t->pos++ * (uint32_t)t->g_nb;
                    for (int i = 0; i < t->g_nb; i++, at++)
                        s = s << 1 | (uint32_t)(t->g_ds[at >> 3] >> (7 - (at & 7)) & 1);
                    t->sym = s;
                    t->rep = 1;
                }
                if (t->sym >= (uint32_t)t->g_as[g])
                    t->rep = 0;
                continue;
            }
            const uint8_t *def = t->g_sym[g] + t->sym * (uint32_t)(1 + 2 * t->g_np[g]);
            uint32_t l = t->bit < t->g_np[g] ? w16(def + 1 + 2 * t->bit) : 0;
            if (t->bit >= t->g_np[g] || (t->bit && !l)) {   /* the symbol has played */
                t->bit = 0;
                t->rep--;
                continue;
            }
            if (!t->bit)
                *rule = (int[]){ TAPE_TOGGLE, TAPE_KEEP, TAPE_LOW, TAPE_HIGH }[def[0] & 3];
            t->bit++;
            *len = l;
            return true;
        }
        case ST_STOP:
            t->stage = ST_DONE;
            *len = 0;
            *rule = TAPE_STOP;
            return true;
        case ST_LEVEL:
            t->stage = ST_DONE;
            *len = 0;
            *rule = t->b[t->cur].body[4] ? TAPE_HIGH : TAPE_LOW;
            return true;
        }
    }
}

bool tape_pulse(tape_t *t, uint32_t *len, int *rule)
{
    while (t->cur < t->nb) {
        t->blocks_done = (unsigned)t->cur;
        if (!t->started && !start(t))
            continue;
        if (play(t, len, rule))
            return true;
        t->cur++;
        t->started = false;
    }
    t->blocks_done = t->blocks;
    t->ended = true;
    return false;
}

bool tape_advance(tape_t *t, uint32_t n)
{
    if (!t->data || !t->playing)
        return t->level;
    t->now += n;
    while (t->now >= t->next) {
        uint32_t len;
        int rule;
        if (!tape_pulse(t, &len, &rule)) {     /* end of tape */
            t->playing = false;
            t->next = UINT64_MAX;
            break;
        }
        if (rule == TAPE_STOP) {               /* a stop block */
            t->playing = false;
            t->stopped = true;
            t->next = t->now;
            break;
        }
        t->level = rule == TAPE_TOGGLE ? !t->level : rule == TAPE_KEEP ? t->level : rule == TAPE_HIGH;
        t->next += len;
    }
    return t->level;
}
