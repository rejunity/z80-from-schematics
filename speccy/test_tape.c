/* ============================================================================
 * test_tape.c - speccy's tape player (make speccy_test).
 *
 *   speccy_tape_test DIR ROM
 *
 * First the pulses each kind of block makes, from tapes built here: their
 * lengths, what they do to the level, flow control and stops. Then every
 * tape tzxgen.py wrote into DIR -- a .tap and the same blocks as standard,
 * turbo, pure, generalized, direct and CSW (RLE and Z-RLE) TZX blocks, and
 * one with groups, a selection, a jump, a loop, a call and two stops --
 * loaded through the 48K ROM as LOAD "" types it: each must leave the same
 * 700 bytes at 32768 and no error report, and flow.tzx must have stopped
 * the tape twice and started it again.
 * ==========================================================================*/
#include "keys.h"
#include "zx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

#define CHECK(c, ...) do {                                              \
        checks++;                                                       \
        if (!(c)) {                                                     \
            fails++;                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                 \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        }                                                               \
    } while (0)

/* ---- pulses ----------------------------------------------------------- */

static bool tzx(tape_t *t, const uint8_t *blocks, size_t n)
{
    uint8_t buf[4096] = "ZXTape!\x1A\x01\x14";
    memcpy(buf + 10, blocks, n);
    return tape_load_mem(t, buf, 10 + n);
}

typedef struct { uint32_t len; int rule; } pulse_t;

static int all_pulses(tape_t *t, pulse_t *p, int max)
{
    int n = 0;
    uint32_t len;
    int rule;
    while (n < max && tape_pulse(t, &len, &rule))
        p[n++] = (pulse_t){ len, rule };
    return n;
}

static bool same(const pulse_t *p, int n, const pulse_t *want, int nwant)
{
    if (n != nwant)
        return false;
    for (int i = 0; i < n; i++)
        if (p[i].len != want[i].len || p[i].rule != want[i].rule)
            return false;
    return true;
}

#define T TAPE_TOGGLE
#define LO TAPE_LOW
#define HI TAPE_HIGH

static void test_pulses(void)
{
    static pulse_t p[20000];
    tape_t t;
    int n;

    {   /* 0x12 pure tone, 0x13 pulse sequence */
        static const uint8_t b[] = { 0x12, 0xE8, 0x03, 3, 0, 0x13, 3, 100, 0, 200, 0, 0x2C, 0x01 };
        static const pulse_t want[] = { {1000, T}, {1000, T}, {1000, T}, {100, T}, {200, T}, {300, T} };
        CHECK(tzx(&t, b, sizeof b), "0x12/0x13 load");
        n = all_pulses(&t, p, 100);
        CHECK(same(p, n, want, 6), "0x12/0x13: %d pulses", n);
        CHECK(tape_done(&t), "0x12/0x13: done");
        tape_free(&t);
    }
    {   /* 0x14 pure data: 12 bits of A5 F0, 4 used in the last byte, no pause */
        static const uint8_t b[] = { 0x14, 0xF4, 0x01, 0xE8, 0x03, 4, 0, 0, 2, 0, 0, 0xA5, 0xF0 };
        int bits[12] = { 1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1 };
        pulse_t want[24];
        for (int i = 0; i < 12; i++)
            want[2 * i] = want[2 * i + 1] = (pulse_t){ bits[i] ? 1000u : 500u, T };
        CHECK(tzx(&t, b, sizeof b), "0x14 load");
        n = all_pulses(&t, p, 100);
        CHECK(same(p, n, want, 24), "0x14: %d pulses", n);
        tape_free(&t);
    }
    {   /* 0x10 standard data, a 5 ms pause: data flag >= 0x80, so 3223 pilot pulses */
        static const uint8_t b[] = { 0x10, 5, 0, 1, 0, 0xFF };
        CHECK(tzx(&t, b, sizeof b), "0x10 load");
        n = all_pulses(&t, p, 20000);
        CHECK(n == 3223 + 2 + 16 + 2, "0x10: %d pulses", n);
        CHECK(p[0].len == 2168 && p[3222].len == 2168 && p[3223].len == 667 && p[3224].len == 735
              && p[3225].len == 1710 && p[3240].len == 1710, "0x10: pilot, sync, bits");
        CHECK(p[n - 2].len == 3500 && p[n - 2].rule == T && p[n - 1].len == 4 * 3500 && p[n - 1].rule == LO,
              "0x10: the pause: an edge, 1 ms, then low");
        tape_free(&t);
    }
    {   /* a .tap: a header's flag (0x00) gets 8063 pilot pulses and a second's pause */
        static const uint8_t b[] = { 2, 0, 0x00, 0x00 };
        CHECK(tape_load_mem(&t, b, sizeof b), ".tap load");
        n = all_pulses(&t, p, 20000);
        CHECK(n == 8063 + 2 + 32 + 2 && p[8064].len == 735 && p[n - 1].len == 999 * 3500, ".tap: %d pulses", n);
        tape_free(&t);
        static const uint8_t bad[] = { 5, 0, 1, 2 };
        CHECK(!tape_load_mem(&t, bad, sizeof bad), "a .tap whose blocks overrun the file");
    }
    {   /* 0x15 direct recording: F0 0F at 10 T a sample */
        static const uint8_t b[] = { 0x15, 10, 0, 0, 0, 8, 2, 0, 0, 0xF0, 0x0F };
        static const pulse_t want[] = { {40, HI}, {80, LO}, {40, HI} };
        CHECK(tzx(&t, b, sizeof b), "0x15 load");
        n = all_pulses(&t, p, 100);
        CHECK(same(p, n, want, 3), "0x15: %d pulses", n);
        tape_free(&t);
    }
    {   /* 0x18 CSW, RLE at 3.5 MHz: 5, 10000 (a 0 and a DWORD), 7 */
        static const uint8_t b[] = { 0x18, 17, 0, 0, 0, 0, 0, 0xE0, 0x67, 0x35, 1, 3, 0, 0, 0,
                                     5, 0, 0x10, 0x27, 0, 0, 7 };
        static const pulse_t want[] = { {5, T}, {10000, T}, {7, T} };
        CHECK(tzx(&t, b, sizeof b), "0x18 RLE load");
        n = all_pulses(&t, p, 100);
        CHECK(same(p, n, want, 3), "0x18 RLE: %d pulses", n);
        tape_free(&t);
    }
    {   /* 0x18 CSW, Z-RLE: a stored deflate block, then fixed codes */
        static const uint8_t b[] = { 0x18, 24, 0, 0, 0, 0, 0, 0xE0, 0x67, 0x35, 2, 3, 0, 0, 0,
                                     0x78, 0x01, 0x01, 3, 0, 0xFC, 0xFF, 5, 6, 7, 0, 0, 0, 0 };
        static const pulse_t want[] = { {5, T}, {6, T}, {7, T} };
        CHECK(tzx(&t, b, sizeof b), "0x18 Z-RLE (stored) load");
        n = all_pulses(&t, p, 100);
        CHECK(same(p, n, want, 3), "0x18 Z-RLE stored: %d pulses", n);
        tape_free(&t);
        static const uint8_t f[] = { 0x18, 21, 0, 0, 0, 0, 0, 0xE0, 0x67, 0x35, 2, 20, 0, 0, 0,
                                     0x78, 0xDA, 0xE3, 0xE4, 0xC4, 0x04, 0x00, 0x07, 0x76, 0x00, 0xB5 };
        CHECK(tzx(&t, f, sizeof f), "0x18 Z-RLE (fixed codes) load");
        n = all_pulses(&t, p, 100);
        bool ok = n == 20;
        for (int i = 0; i < n; i++)
            ok = ok && p[i].len == 9;
        CHECK(ok, "0x18 Z-RLE fixed codes: %d pulses", n);
        tape_free(&t);
    }
    {   /* 0x18 Z-RLE, dynamic codes: 300 pulses of 1..23 T from a generator */
        static const uint8_t z[] = {
            0x78, 0xDA, 0x0D, 0xCE, 0x89, 0x11, 0x04, 0x21, 0x08, 0x00, 0xB0, 0xF1, 0x45, 0x11, 0x44, 0x60,
            0xC5, 0xFE, 0x3B, 0xBD, 0x6B, 0x20, 0x93, 0x32, 0x95, 0xF3, 0x59, 0xFB, 0x1E, 0x2E, 0x6F, 0x16,
            0xFB, 0x52, 0xEC, 0xB5, 0xCA, 0xE8, 0x4A, 0xBC, 0x5E, 0x9D, 0xFC, 0x28, 0xF1, 0x81, 0xC2, 0x33,
            0xD5, 0xA0, 0x63, 0x68, 0x2D, 0x15, 0x6C, 0xF1, 0xE5, 0xB3, 0x59, 0x60, 0x58, 0x59, 0xC0, 0x6C,
            0x30, 0x1B, 0x79, 0xC3, 0x91, 0xAF, 0xE3, 0x99, 0xF7, 0xF5, 0x73, 0x49, 0xF3, 0xDC, 0x6F, 0xD8,
            0x2C, 0xCD, 0xBC, 0xF9, 0x40, 0xB5, 0xE8, 0xB4, 0x5A, 0xFF, 0x83, 0xBE, 0x86, 0xD5, 0xFC, 0x79,
            0xFE, 0x24, 0x2F, 0x50, 0x2C, 0xC3, 0xBF, 0x37, 0x3A, 0xEA, 0xB1, 0x2D, 0x7D, 0x87, 0xB8, 0xEB,
            0x92, 0x30, 0x51, 0x0C, 0x1A, 0x54, 0x7D, 0x44, 0x85, 0xE5, 0x27, 0x72, 0x95, 0x09, 0x21, 0x5B,
            0x1A, 0x01, 0x0C, 0x56, 0xFD, 0x66, 0x95, 0x62, 0x37, 0x25, 0xC9, 0x20, 0xA7, 0xB3, 0x4A, 0x87,
            0xE9, 0x5F, 0xFB, 0xD7, 0x81, 0x1F, 0x90, 0x3C, 0x7D, 0x69, 0xEB, 0x81, 0x71, 0x81, 0x8B, 0xB7,
            0xDE, 0xF1, 0x46, 0x24, 0x7A, 0x9A, 0xF3, 0xF9, 0x3F, 0x03, 0x23, 0xE9, 0x46, 0x43, 0x49, 0x4E,
            0x71, 0x44, 0x13, 0x21, 0xEE, 0xB1, 0xDA, 0x57, 0x59, 0x94, 0x5B, 0xBB, 0x72, 0x75, 0x25, 0x23,
            0xCA, 0xF0, 0x03, 0x46, 0x07, 0x0E, 0x44 };
        uint8_t b[512] = { 0x18, 0, 0, 0, 0, 0, 0, 0xE0, 0x67, 0x35, 2, 0x2C, 0x01, 0, 0 };
        b[1] = (uint8_t)(10 + sizeof z);
        memcpy(b + 15, z, sizeof z);
        CHECK(tzx(&t, b, 15 + sizeof z), "0x18 Z-RLE (dynamic codes) load");
        n = all_pulses(&t, p, 1000);
        bool ok = n == 300;
        uint32_t x = 1;
        for (int i = 0; ok && i < n; i++) {
            x = (x * 1103515245u + 12345u) & 0x7FFFFFFF;
            ok = p[i].len == 1 + (x >> 16) % 23;
        }
        CHECK(ok, "0x18 Z-RLE dynamic codes: %d pulses", n);
        tape_free(&t);
    }
    {   /* 0x19 generalized data: a pilot symbol forced high (one pulse, then
           0 ends it) twice; data symbols forced low 50 60 and an edge 70,
           the stream 1 0 1 */
        static const uint8_t b[] = { 0x19, 33, 0, 0, 0,
                                     0, 0,                    /* pause */
                                     1, 0, 0, 0, 2, 1,        /* TOTP 1, NPP 2, ASP 1 */
                                     3, 0, 0, 0, 2, 2,        /* TOTD 3, NPD 2, ASD 2 */
                                     3, 100, 0, 0, 0,         /* pilot symbol 0 */
                                     0, 2, 0,                 /* PRLE: symbol 0, twice */
                                     2, 50, 0, 60, 0,         /* data symbol 0 */
                                     0, 70, 0, 0, 0,          /* data symbol 1 */
                                     0xA0 };
        static const pulse_t want[] = { {100, HI}, {100, HI}, {70, T}, {50, LO}, {60, T}, {70, T} };
        CHECK(tzx(&t, b, sizeof b), "0x19 load");
        n = all_pulses(&t, p, 100);
        CHECK(same(p, n, want, 6), "0x19: %d pulses", n);
        tape_free(&t);
    }
    {   /* flow: a selection, a jump, a loop, a call, groups and info blocks;
           tones of 1..6 T say which blocks played */
        static const uint8_t b[] = {
            0x28, 9, 0, 2, 2, 0, 1, 'a', 1, 0, 1, 'b',     /* 0 select: the first entry (2) */
            0x12, 9, 0, 1, 0,                              /* 1 not played */
            0x21, 1, 'g',                                  /* 2 group */
            0x12, 1, 0, 1, 0,                              /* 3 tone 1 */
            0x22,                                          /* 4 group end */
            0x23, 2, 0,                                    /* 5 jump over 6 */
            0x12, 9, 0, 1, 0,                              /* 6 not played */
            0x24, 3, 0,                                    /* 7 loop 3 times */
            0x12, 2, 0, 1, 0,                              /* 8 tone 2 */
            0x25,                                          /* 9 loop end */
            0x26, 2, 0, 4, 0, 5, 0,                        /* 10 call 14, then 15 */
            0x12, 3, 0, 1, 0,                              /* 11 tone 3, after the calls */
            0x23, 5, 0,                                    /* 12 jump to 17 */
            0x30, 1, 'x',                                  /* 13 */
            0x12, 4, 0, 1, 0,                              /* 14 tone 4, called; on into 15 */
            0x12, 5, 0, 1, 0,                              /* 15 tone 5 (both calls play it) */
            0x27,                                          /* 16 return */
            0x2B, 1, 0, 0, 0, 1,                           /* 17 level high */
            0x12, 6, 0, 1, 0,                              /* 18 tone 6 */
            0x20, 0, 0,                                    /* 19 stop the tape */
            0x2A, 0, 0, 0, 0,                              /* 20 stop if 48K */
            0x5A, 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 1, 20, /* 21 glue */
            0x77, 2, 0, 0, 0, 1, 2,                        /* 22 an unknown block */
            0x12, 7, 0, 1, 0 };                            /* 23 tone 7 */
        static const pulse_t want[] = { {1, T}, {2, T}, {2, T}, {2, T}, {4, T}, {5, T}, {5, T},
                                        {3, T}, {0, HI}, {6, T}, {0, TAPE_STOP}, {0, TAPE_STOP}, {7, T} };
        CHECK(tzx(&t, b, sizeof b), "flow load");
        n = all_pulses(&t, p, 100);
        CHECK(same(p, n, want, 13), "flow: %d pulses", n);
        for (int i = 0; i < n && i < 13; i++)
            if (p[i].len != want[i].len || p[i].rule != want[i].rule)
                printf("  pulse %d: %u/%d, want %u/%d\n", i, p[i].len, p[i].rule, want[i].len, want[i].rule);
        tape_free(&t);
    }
    {   /* a stop block stops tape_advance; it plays on from there when started */
        static const uint8_t b[] = { 0x12, 100, 0, 2, 0, 0x20, 0, 0, 0x12, 100, 0, 2, 0 };
        CHECK(tzx(&t, b, sizeof b), "stop load");
        t.playing = true;
        for (int i = 0; i < 1000 && t.playing; i++)
            tape_advance(&t, 10);
        CHECK(!t.playing && t.stopped && !tape_done(&t), "stop: stopped, not done");
        t.stopped = false;
        t.playing = true;
        for (int i = 0; i < 1000 && t.playing; i++)
            tape_advance(&t, 10);
        CHECK(tape_done(&t), "stop: done after starting again");
        tape_free(&t);
    }
    {   /* malformed: a block past the end, a bad Z-RLE stream, a 0x19 whose tables overrun */
        static const uint8_t b1[] = { 0x10, 0, 0, 10, 0, 1, 2 };
        static const uint8_t b2[] = { 0x18, 14, 0, 0, 0, 0, 0, 0xE0, 0x67, 0x35, 2, 1, 0, 0, 0, 0x78, 0x01, 0x07, 0 };
        static const uint8_t b3[] = { 0x19, 14, 0, 0, 0, 0, 0, 1, 0, 0, 0, 2, 1, 0, 0, 0, 0, 2, 2 };
        CHECK(!tzx(&t, b1, sizeof b1), "a block past the end of the file");
        CHECK(!tzx(&t, b2, sizeof b2), "a Z-RLE stream that does not inflate");
        CHECK(!tzx(&t, b3, sizeof b3), "a 0x19 whose tables do not fit");
    }
    {   /* the archive info */
        static const uint8_t b[] = { 0x32, 18, 0, 3, 0, 4, 'G', 'a', 'm', 'e', 1, 3, 'P', 'u', 'b',
                                     3, 4, '1', '9', '8', '7' };
        char s[64];
        CHECK(tzx(&t, b, sizeof b), "0x32 load");
        tape_describe(&t, s, sizeof s);
        CHECK(!strcmp(s, "Game (Pub, 1987)"), "describe: \"%s\"", s);
        tape_free(&t);
    }
    printf("pulses of every kind of block: %s\n", fails ? "FAIL" : "OK");
}

/* ---- through the ROM -------------------------------------------------- */

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc(len > 0 ? (size_t)len : 1);
    *n = b && len > 0 && fread(b, 1, (size_t)len, fp) == (size_t)len ? (size_t)len : 0;
    fclose(fp);
    return b;
}

static void test_load(const char *dir, const char *name, const uint8_t rom[16384],
                      const uint8_t *code, size_t ncode, int stops)
{
    static zx_t z;
    keys_t k;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    zx_init(&z, rom);
    if (!tape_load(&z.tape, path)) {
        CHECK(0, "%s: cannot load", path);
        return;
    }
    memset(&k, 0, sizeof k);
    int stopped = 0, frames = 0, end = 12000;
    bool was = false;
    for (; frames < end; frames++) {
        if (z.frames == 150)
            keys_type(&k, "j\"\"\n");          /* LOAD "" */
        keys_frame(&k);
        z.keys = keys_down(&k);
        z.ula.draw = false;
        zx_frame(&z);
        zx_tape_motor(&z);
        if (z.tape.stopped && !was)
            stopped++;
        was = z.tape.stopped;
        if (tape_done(&z.tape) && end == 12000)
            end = frames + 150;                /* 3 s more: BASIC reports */
    }
    bool same_code = !memcmp(&z.mem[32768], code, ncode);
    CHECK(same_code, "%s: the bytes at 32768", name);
    CHECK(z.mem[23610] == 0xFF, "%s: report %d (ERR_NR %02X), not 0 OK", name, z.mem[23610] + 1, z.mem[23610]);
    CHECK(stopped == stops, "%s: the tape stopped %d times, not %d", name, stopped, stops);
    printf("%-11s loads through the ROM in %5.1f s of the machine's time (%d frames)%s: %s\n", name,
           (frames - 150) / 50.08, frames - 150, stops ? ", two stops" : "",
           same_code && z.mem[23610] == 0xFF && stopped == stops ? "OK" : "FAIL");
    tape_free(&z.tape);
}

int main(int argc, char **argv)
{
    test_pulses();
    if (argc < 3) {
        fprintf(stderr, "usage: speccy_tape_test DIR ROM\n");
        return 2;
    }
    size_t n, ncode;
    uint8_t *rom = slurp(argv[2], &n);
    if (!rom || n != 16384) {
        fprintf(stderr, "speccy_tape_test: %s: not a 16K ROM\n", argv[2]);
        return 2;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/code.bin", argv[1]);
    uint8_t *code = slurp(path, &ncode);
    if (!code || !ncode) {
        fprintf(stderr, "speccy_tape_test: %s: missing (tzxgen.py writes it)\n", path);
        return 2;
    }
    static const char *names[] = { "test.tap", "std.tzx", "turbo.tzx", "pure.tzx", "gen.tzx",
                                   "direct.tzx", "csw.tzx", "cswz.tzx", "flow.tzx" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        test_load(argv[1], names[i], rom, code, ncode, !strcmp(names[i], "flow.tzx") ? 2 : 0);
    printf("%d checks, %d failed\n", checks, fails);
    free(rom);
    free(code);
    return fails ? 1 : 0;
}
