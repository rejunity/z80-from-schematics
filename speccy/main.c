/* ============================================================================
 * main.c - speccy: a 48K ZX Spectrum in a terminal.
 *
 *   speccy [options] [game.tap]
 *
 * With a .tap it types LOAD "" once the ROM has started, plays the tape
 * while the program reads it (a tape loader reads port 0xFE thousands of
 * times a frame), and runs flat out until the loading is done.
 * ==========================================================================*/
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#include "keys.h"
#include "term.h"
#include "zx.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define FRAME_SECONDS (69888.0 / 3500000.0)   /* 50.08 frames a second */
#define AUTOLOAD_FRAME 150                    /* the ROM is ready by then (3 s) */

static double now_seconds(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void sleep_seconds(double s)
{
    if (s <= 0)
        return;
#ifdef _WIN32
    Sleep((DWORD)(s * 1000));
#else
    struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
    nanosleep(&ts, NULL);
#endif
}

static bool read_file(const char *path, uint8_t *buf, size_t size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;
    size_t n = fread(buf, 1, size, fp);
    int extra = fgetc(fp);
    fclose(fp);
    return n == size && extra == EOF;
}

/* the ROM: --rom, $SPECCY_ROM, 48.rom next to the program, or where the
   Debian package and FUSE install it */
static bool find_rom(const char *opt, const char *argv0, uint8_t rom[16384], char *found, size_t cap)
{
    const char *paths[8];
    int n = 0;
    char beside[1024] = "";
    if (opt) {
        paths[n++] = opt;
    } else {
        if (getenv("SPECCY_ROM"))
            paths[n++] = getenv("SPECCY_ROM");
        const char *slash = strrchr(argv0, '/');
#ifdef _WIN32
        const char *bs = strrchr(argv0, '\\');
        if (bs && (!slash || bs > slash))
            slash = bs;
#endif
        if (slash && (size_t)(slash - argv0) + 8 < sizeof beside) {
            memcpy(beside, argv0, (size_t)(slash - argv0 + 1));
            strcpy(beside + (slash - argv0 + 1), "48.rom");
            paths[n++] = beside;
        } else {
            paths[n++] = "48.rom";
        }
        paths[n++] = "/usr/share/spectrum-roms/48.rom";
        paths[n++] = "/usr/share/fuse/48.rom";
        paths[n++] = "/usr/local/share/fuse/48.rom";
        paths[n++] = "/opt/homebrew/share/fuse/48.rom";
    }
    for (int i = 0; i < n; i++)
        if (read_file(paths[i], rom, 16384)) {
            snprintf(found, cap, "%s", paths[i]);
            return true;
        }
    return false;
}

/* the screen's text: each 8x8 cell that is a glyph of the font (or its
   inverse) as that character, other non-blank cells as '#' */
static void dump_text(const zx_t *z, FILE *fp)
{
    const uint8_t *font = zx_font(z);
    for (int row = 0; row < 24; row++) {
        char line[33];
        for (int col = 0; col < 32; col++) {
            uint8_t cell[8];
            bool blank = true, full = true;
            for (int r = 0; r < 8; r++) {
                unsigned y = (unsigned)(row * 8 + r);
                cell[r] = z->mem[0x4000 | (y & 0xC0) << 5 | (y & 7) << 8 | (y & 0x38) << 2 | (unsigned)col];
                blank &= cell[r] == 0;
                full &= cell[r] == 0xFF;
            }
            char ch = blank || full ? ' ' : '#';
            for (int g = 1; g < 96 && ch == '#'; g++) {
                bool same = true, inv = true;
                for (int r = 0; r < 8; r++) {
                    same &= cell[r] == font[8 * g + r];
                    inv &= cell[r] == (uint8_t)~font[8 * g + r];
                }
                if (same || inv)
                    ch = (char)(32 + g);
            }
            line[col] = ch;
        }
        int end = 32;
        while (end > 0 && line[end - 1] == ' ')
            end--;
        line[end] = 0;
        fprintf(fp, "%s\n", line);
    }
}

static volatile sig_atomic_t got_signal, got_winch;
static void on_signal(int sig) { got_signal = sig; }
#ifdef SIGWINCH
static void on_winch(int sig) { (void)sig; got_winch = 1; }
#endif

static void usage(void)
{
    puts("usage: speccy [options] [game.tap]\n"
         "  --rom FILE        the 48K ROM (default: $SPECCY_ROM, 48.rom beside the program,\n"
         "                    /usr/share/spectrum-roms, FUSE's share/fuse)\n"
         "  --mode M          256 (half blocks), 128, 120 or 80 columns (default: the widest\n"
         "                    that fits; 256 only where octants are missing)\n"
         "  --colors N        24, 256 or 16 (default: 24 if COLORTERM says so, else 256)\n"
         "  --braille         braille dots instead of octants (the default in Terminal.app\n"
         "                    and VS Code, whose fonts have no octants)\n"
         "  --octants         octants even there (a font that has Unicode 16's octants)\n"
         "  --quadrants[=R]   2x2 quadrants in modes 128 and 120: R 96, every pixel but twice\n"
         "                    as tall as wide; R 48, half the rows (a quadrant two pixels\n"
         "                    tall, blended; --crisp: the ink); default 96 if the window has them\n"
         "  --crisp           mode 80 and half-height quadrants: no blended shades\n"
         "  --fullwidth       mode 80: double-width characters\n"
         "  --keys K          auto, kitty, evdev, console, macos, windows or legacy\n"
         "  --no-autoload     don't type LOAD \"\" for a .tap\n"
         "  --turbo           run flat out (F12 toggles)\n"
         "  --late            late ULA timing (INT a T-state earlier against the screen)\n"
         "  --frames N        headless: run N frames, then print the screen's text\n"
         "  --save-scr FILE   with --frames: also write the screen as a .scr\n"
         "keys: F10 or Ctrl-C quit, F5 reset, F12 full speed; Esc is BREAK, Tab\n"
         "EXTENDED MODE, shift CAPS SHIFT, ctrl or alt SYMBOL SHIFT.");
}

int main(int argc, char **argv)
{
    const char *rom_opt = NULL, *tap = NULL, *save_scr = NULL;
    term_opts_t topt = { 0 };
    int keys_want = KEYS_AUTO;
    long frames_headless = -1;
    bool autoload = true, turbo = false, late = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--rom") && v) { rom_opt = v; i++; }
        else if (!strcmp(a, "--mode") && v) { topt.mode = atoi(v); i++; }
        else if (!strcmp(a, "--colors") && v) { topt.colors = atoi(v); i++; }
        else if (!strcmp(a, "--braille")) topt.glyphs = GLYPHS_BRAILLE;
        else if (!strcmp(a, "--octants")) topt.glyphs = GLYPHS_OCTANTS;
        else if (!strncmp(a, "--quadrants", 11) && (!a[11] || a[11] == '=')) {
            topt.glyphs = GLYPHS_QUADRANTS;
            topt.quad_rows = a[11] ? atoi(a + 12) : 0;
            if (topt.quad_rows && topt.quad_rows != 96 && topt.quad_rows != 48) {
                fprintf(stderr, "speccy: --quadrants=96 or --quadrants=48\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--crisp")) topt.crisp = true;
        else if (!strcmp(a, "--fullwidth")) topt.fullwidth = true;
        else if (!strcmp(a, "--keys") && v) {
            keys_want = keys_mode_by_name(v);
            i++;
            if (keys_want < 0) { fprintf(stderr, "speccy: unknown --keys %s\n", v); return 2; }
        }
        else if (!strcmp(a, "--no-autoload")) autoload = false;
        else if (!strcmp(a, "--turbo")) turbo = true;
        else if (!strcmp(a, "--late")) late = true;
        else if (!strcmp(a, "--frames") && v) { frames_headless = atol(v); i++; }
        else if (!strcmp(a, "--save-scr") && v) { save_scr = v; i++; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] != '-' && !tap) tap = a;
        else { usage(); return 2; }
    }
    if (topt.mode && topt.mode != 256 && topt.mode != 128 && topt.mode != 120 && topt.mode != 80) {
        fprintf(stderr, "speccy: --mode is 256, 128, 120 or 80\n");
        return 2;
    }
    if (topt.glyphs == GLYPHS_QUADRANTS && (topt.mode == 80 || topt.mode == 256)) {
        fprintf(stderr, "speccy: --quadrants draws modes 128 and 120\n");
        return 2;
    }

    static uint8_t rom[16384];
    char rom_path[1024];
    if (!find_rom(rom_opt, argv[0], rom, rom_path, sizeof rom_path)) {
        fprintf(stderr, "speccy: no 48K ROM (16384 bytes) found; give one with --rom, or run\n"
                        "        make speccy_rom   (downloads FUSE's copy: Amstrad allows it to be\n"
                        "        distributed, not sold)\n");
        return 1;
    }
    static zx_t z;
    zx_init(&z, rom);
    ula_set_late(&z.ula, late);
    if (tap && !tape_load(&z.tape, tap)) {
        fprintf(stderr, "speccy: %s: %s\n", tap, errno == EINVAL ? "not a .tap file" : strerror(errno));
        return 1;
    }
    keys_t k;

    if (frames_headless >= 0) {                /* tests: no terminal */
        memset(&k, 0, sizeof k);
        for (long f = 0; f < frames_headless; f++) {
            if (tap && autoload && z.frames == AUTOLOAD_FRAME)
                keys_type(&k, "j\"\"\n");      /* LOAD "" */
            keys_frame(&k);
            z.keys = keys_down(&k);
            z.ula.draw = false;
            zx_frame(&z);
            zx_tape_motor(&z);
        }
        dump_text(&z, stdout);
        if (tap)
            fprintf(stderr, "speccy: tape: block %u of %u, %d%%, %s; %u reads of port 0xFE last frame\n",
                    z.tape.blocks_done, z.tape.blocks, tape_percent(&z.tape),
                    z.tape.playing ? "playing" : tape_done(&z.tape) ? "done" : "stopped", z.fe_reads_last);
        if (save_scr) {
            FILE *fp = fopen(save_scr, "wb");
            if (fp) {
                fwrite(&z.mem[0x4000], 1, 6912, fp);
                fclose(fp);
            }
        }
        return 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGWINCH
    signal(SIGWINCH, on_winch);
#endif
    int kmode = keys_open(&k, keys_want);
    term_open(&topt);
    double next = now_seconds(), last_draw = 0, ff_t0 = 0;
    uint64_t ff_frames0 = 0;
    char title[256];
    while (!k.quit && !got_signal) {
        keys_poll(&k);
        if (k.resized || got_winch) {
            k.resized = false;
            got_winch = 0;
            term_resize();
        }
        if (k.reset) {
            k.reset = false;
            zx_reset(&z);
        }
        if (k.turbo_toggle) {
            k.turbo_toggle = false;
            turbo = !turbo;
        }
        bool booting = tap && autoload && z.frames <= AUTOLOAD_FRAME + 40;
        bool loading = z.tape.playing;
        bool ff = turbo || booting || loading;
        if (tap && autoload && z.frames == AUTOLOAD_FRAME)
            keys_type(&k, "j\"\"\n");          /* LOAD "" */
        keys_frame(&k);
        z.keys = k.focused ? keys_down(&k) : 0;
        double t = now_seconds();
        bool draw = !ff || t - last_draw >= 0.1;
        z.ula.draw = draw;
        zx_frame(&z);
        zx_tape_motor(&z);
        if (draw) {
            term_frame(&z.ula.fb[0][0], ULA_FB_W, ULA_FB_H, ULA_PAPER_X, ULA_PAPER_Y, zx_font(&z));
            last_draw = t;
            double speed = 0;
            if (ff && t > ff_t0 + 0.5) {
                speed = (double)(z.frames - ff_frames0) * FRAME_SECONDS / (t - ff_t0);
                ff_t0 = t;
                ff_frames0 = z.frames;
            }
            static double shown_speed;
            if (speed > 0)
                shown_speed = speed;
            int n = snprintf(title, sizeof title, "speccy | mode %d %s | keys: %s", term_mode(),
                             term_glyphs(), keys_mode_name(kmode));
            if (tap)
                n += snprintf(title + n, sizeof title - (size_t)n, " | tape %d%% %s", tape_percent(&z.tape),
                              z.tape.playing ? "playing" : tape_done(&z.tape) ? "done" : "stopped");
            if (ff && shown_speed > 0)
                n += snprintf(title + n, sizeof title - (size_t)n, " | fast x%.0f", shown_speed);
            snprintf(title + n, sizeof title - (size_t)n, " | F10 quit");
            term_title(title);
        }
        if (ff) {
            next = now_seconds();
        } else {
            if (ff_t0) {
                ff_t0 = 0;
            }
            next += FRAME_SECONDS;
            double wait = next - now_seconds();
            if (wait < -0.2)
                next = now_seconds();          /* fell behind: don't catch up */
            sleep_seconds(wait);
        }
        if (ff && !ff_t0) {
            ff_t0 = t;
            ff_frames0 = z.frames;
        }
    }
    term_close();
    keys_close(&k);
    if (k.note)
        fprintf(stderr, "speccy: keys: %s\n", k.note);
    return 0;
}
