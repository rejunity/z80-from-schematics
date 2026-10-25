/* ============================================================================
 * main.c - speccy: a 48K ZX Spectrum in a terminal.
 *
 *   speccy [options] [game.tap | game.tzx]
 *
 * With a tape it types LOAD "" once the ROM has started, plays the tape
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
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
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
   inverse) as that character, other non-blank cells as '#'; 24 lines */
static void screen_text(const zx_t *z, char text[24 * 33 + 1])
{
    const uint8_t *font = zx_font(z);
    char *out = text;
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
        memcpy(out, line, (size_t)end);
        out += end;
        *out++ = '\n';
    }
    *out = 0;
}

static void dump_text(const zx_t *z, FILE *fp)
{
    char text[24 * 33 + 1];
    screen_text(z, text);
    fputs(text, fp);
}

/* the --type text: \n is ENTER */
static void type_text(keys_t *k, const char *s)
{
    char text[256];
    size_t n = 0;
    for (; *s && n + 1 < sizeof text; s++)
        text[n++] = s[0] == '\\' && s[1] == 'n' ? (s++, '\n') : *s;
    text[n] = 0;
    keys_type(k, text);
}

static volatile sig_atomic_t got_signal, got_winch;
static void on_signal(int sig) { got_signal = sig; }
#ifdef SIGWINCH
static void on_winch(int sig) { (void)sig; got_winch = 1; }
#endif

static void usage(void)
{
    puts("usage: speccy [options] [game.tap | game.tzx]\n"
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
         "  --bars            half-width bars in modes 128 and 120 (Block Elements, any font):\n"
         "                    a cell four bars, each two pixels wide (blended; --crisp: the\n"
         "                    ink) and one tall: square like octants, half the columns\n"
         "  --crisp           mode 80, half-height quadrants and bars: no blended shades\n"
         "  --fullwidth       mode 80: double-width characters\n"
         "  --keys K          auto, kitty, evdev, console, macos, windows or legacy\n"
         "  --no-autoload     don't type LOAD \"\" for a tape\n"
         "  --turbo           run flat out (F12 toggles)\n"
         "  --late            late ULA timing (INT a T-state earlier against the screen)\n"
         "  --frames N        headless: run N frames, then print the screen's text\n"
         "  --save-scr FILE   with --frames: also write the screen as a .scr\n"
         "  --type F:TEXT     type TEXT from frame F (\\n is ENTER); repeatable\n"
         "  --shot F:NAME     with --frames: after frame F write NAME.scr and NAME.txt (the\n"
         "                    screen's text); repeatable\n"
         "  --type-on S:TEXT  with --frames: type TEXT whenever the screen shows S (and print\n"
         "                    the screen, the frame first); repeatable\n"
         "  --exit-on S       with --frames: stop when the screen shows S\n"
         "  --stats           the title shows the frames drawn and emulated a second, bytes a\n"
         "                    frame, KB/s and frames skipped; a summary on exit\n"
         "  --baud N          the output's budget, N/10 bytes a second: a serial line (or one to\n"
         "                    try); without it the terminal's replies and the device's queue\n"
         "                    say when the link is behind\n"
         "  --min-fps F       draw at least F frames a second however slow the link, the\n"
         "                    emulation waiting for it if need be (default 0: it never waits)\n"
         "  --no-skip         draw every frame, the emulation waiting for the link\n"
         "  --quit-at N       quit after frame N, drawing it whatever the link\n"
         "  --fast-until N    run flat out to frame N (as while a tape loads)\n"
         "  --measure FILE    with --frames: draw every frame into a terminal of its own and\n"
         "                    write \"frame bytes loading\" lines (the bytes it sends)\n"
         "  --measure-every D with --measure: draw every D-th frame only\n"
         "  --size CxR        with --measure: that terminal's size (default: the mode's own)\n"
         "  --measure-from F  with --measure: start at frame F (the first frame drawn is all of\n"
         "                    it: leave it out)\n"
         "  --measure-set L   with --measure-from (POSIX): at frame F fork one measurer for\n"
         "                    each of L, comma-separated GLYPHS:D -- GLYPHS octants, braille,\n"
         "                    half (mode 256), quad96, quad48, bars or text (mode 80), D the\n"
         "                    --measure-every -- each writing FILE.GLYPHS.D\n"
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
    static struct { long frame; const char *arg; } types[1024], shots[1024];
    int ntypes = 0, nshots = 0;
    struct { char on[64]; const char *text; } ons[16];
    int nons = 0;
    const char *exit_on = NULL;
    bool stats = false, no_skip = false;
    long baud = 0, quit_at = -1;
    double min_fps = 0;
    const char *measure = NULL, *measure_set = NULL;
    int measure_every = 1, size_c = 0, size_r = 0;
    long measure_from = 0, fast_until = -1;
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
        else if (!strcmp(a, "--bars")) topt.glyphs = GLYPHS_BARS;
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
        else if ((!strcmp(a, "--type") || !strcmp(a, "--shot")) && v && strchr(v, ':')) {
            bool type = a[2] == 't';
            if ((type ? ntypes : nshots) == 1024) { fprintf(stderr, "speccy: at most 1024 %s\n", a); return 2; }
            if (type) { types[ntypes].frame = atol(v); types[ntypes++].arg = strchr(v, ':') + 1; }
            else { shots[nshots].frame = atol(v); shots[nshots++].arg = strchr(v, ':') + 1; }
            i++;
        }
        else if (!strcmp(a, "--type-on") && v && strrchr(v, ':') && nons < 16) {
            size_t n = (size_t)(strrchr(v, ':') - v);
            snprintf(ons[nons].on, sizeof ons[nons].on, "%.*s", (int)n, v);
            ons[nons++].text = strrchr(v, ':') + 1;
            i++;
        }
        else if (!strcmp(a, "--exit-on") && v) { exit_on = v; i++; }
        else if (!strcmp(a, "--stats")) stats = true;
        else if (!strcmp(a, "--no-skip")) no_skip = true;
        else if (!strcmp(a, "--baud") && v) { baud = atol(v); i++; }
        else if (!strcmp(a, "--min-fps") && v) { min_fps = atof(v); i++; }
        else if (!strcmp(a, "--quit-at") && v) { quit_at = atol(v); i++; }
        else if (!strcmp(a, "--fast-until") && v) { fast_until = atol(v); i++; }
        else if (!strcmp(a, "--measure") && v) { measure = v; i++; }
        else if (!strcmp(a, "--measure-every") && v) { measure_every = atoi(v); i++; }
        else if (!strcmp(a, "--measure-from") && v) { measure_from = atol(v); i++; }
        else if (!strcmp(a, "--measure-set") && v) { measure_set = v; i++; }
        else if (!strcmp(a, "--size") && v && strchr(v, 'x')) {
            size_c = atoi(v);
            size_r = atoi(strchr(v, 'x') + 1);
            i++;
        }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] != '-' && !tap) tap = a;
        else { usage(); return 2; }
    }
    if (min_fps < 0 || min_fps > 50 || measure_every < 1 || baud < 0) {
        fprintf(stderr, "speccy: --min-fps is 0 to 50, --measure-every 1 or more, --baud 0 or more\n");
        return 2;
    }
    if (topt.mode && topt.mode != 256 && topt.mode != 128 && topt.mode != 120 && topt.mode != 80) {
        fprintf(stderr, "speccy: --mode is 256, 128, 120 or 80\n");
        return 2;
    }
    if ((topt.glyphs == GLYPHS_QUADRANTS || topt.glyphs == GLYPHS_BARS) && (topt.mode == 80 || topt.mode == 256)) {
        fprintf(stderr, "speccy: --%s draws modes 128 and 120\n", topt.glyphs == GLYPHS_BARS ? "bars" : "quadrants");
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
        fprintf(stderr, "speccy: %s: %s\n", tap, errno == EINVAL ? "not a .tap or .tzx file" : strerror(errno));
        return 1;
    }
    char tape_info[256];
    tape_describe(&z.tape, tape_info, sizeof tape_info);
    keys_t k;

    if (frames_headless >= 0) {                /* tests: no terminal */
        memset(&k, 0, sizeof k);
        if (tape_info[0])
            fprintf(stderr, "speccy: tape: %s\n", tape_info);
        FILE *mf = NULL, *mnull = NULL;
        char mpath[1024];
        snprintf(mpath, sizeof mpath, "%s", measure ? measure : "");
        bool measuring = measure && !measure_set;
        for (long f = 0; measure_set && f < measure_from; f++) {   /* to frame F, once */
            if (tap && autoload && z.frames == AUTOLOAD_FRAME)
                keys_type(&k, "j\"\"\n");
            for (int i = 0; i < ntypes; i++)
                if (types[i].frame == f)
                    type_text(&k, types[i].arg);
            keys_frame(&k);
            z.keys = keys_down(&k);
            z.ula.draw = false;
            zx_frame(&z);
            zx_tape_motor(&z);
        }
#ifndef _WIN32
        if (measure_set) {                     /* a measurer for each, from here */
            fflush(stdout);
            fflush(stderr);
            char list[4096];
            snprintf(list, sizeof list, "%s", measure_set);
            for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
                char g[32];
                int every = 1;
                if (sscanf(tok, "%31[a-z0-9]:%d", g, &every) < 1 || every < 1) {
                    fprintf(stderr, "speccy: --measure-set: %s?\n", tok);
                    return 2;
                }
                pid_t pid = fork();
                if (pid < 0) {
                    perror("fork");
                    return 1;
                }
                if (pid) {                     /* one at a time */
                    waitpid(pid, NULL, 0);
                    continue;
                }
                topt.mode = !strcmp(g, "half") ? 256 : !strcmp(g, "text") ? 80 : 128;
                topt.glyphs = !strcmp(g, "braille") ? GLYPHS_BRAILLE : !strcmp(g, "quad96") || !strcmp(g, "quad48")
                              ? GLYPHS_QUADRANTS : !strcmp(g, "bars") ? GLYPHS_BARS : GLYPHS_OCTANTS;
                topt.quad_rows = !strcmp(g, "quad48") ? 48 : !strcmp(g, "quad96") ? 96 : 0;
                measure_every = every;
                snprintf(mpath, sizeof mpath, "%s.%s.%d", measure, g, every);
                measuring = true;
                break;
            }
            if (!measuring)                    /* the parent: they are done */
                return 0;
        }
#endif
        if (measuring) {                       /* what each frame would send */
            if (!size_c) {
                int m = topt.mode ? topt.mode : 128;
                size_c = m == 80 ? 64 : m;
                size_r = m == 256 ? 96 : m == 80 ? 24 :
                         topt.glyphs == GLYPHS_QUADRANTS && topt.quad_rows != 48 ? 96 : 48;
            }
            topt.cols = size_c;
            topt.rows = size_r;
#ifdef _WIN32
            mnull = fopen("NUL", "w");
#else
            mnull = fopen("/dev/null", "w");
#endif
            mf = fopen(mpath, "w");
            if (!mf || !mnull) {
                perror(mpath);
                return 1;
            }
            topt.out = mnull;
            if (!topt.colors)
                topt.colors = 24;
            term_open(&topt);
        }
        for (long f = measure_set ? measure_from : 0; f < frames_headless; f++) {
            if (tap && autoload && z.frames == AUTOLOAD_FRAME)
                keys_type(&k, "j\"\"\n");      /* LOAD "" */
            for (int i = 0; i < ntypes; i++)
                if (types[i].frame == f)
                    type_text(&k, types[i].arg);
            keys_frame(&k);
            z.keys = keys_down(&k);
            bool meas = mf && f >= measure_from && (f - measure_from) % measure_every == 0;
            z.ula.draw = meas;
            zx_frame(&z);
            zx_tape_motor(&z);
            if (meas) {
                uint64_t b0 = term_bytes();
                term_frame(&z.ula.fb[0][0], ULA_FB_W, ULA_FB_H, ULA_PAPER_X, ULA_PAPER_Y, zx_font(&z));
                fprintf(mf, "%llu %llu %d\n", (unsigned long long)z.frames,
                        (unsigned long long)(term_bytes() - b0), z.tape.playing ? 1 : 0);
            }
            if ((nons || exit_on) && z.frames % 10 == 0 && !keys_busy(&k)) {
                char text[24 * 33 + 1];
                screen_text(&z, text);
                if (exit_on && strstr(text, exit_on)) {
                    fflush(stdout);
                    fprintf(stderr, "speccy: \"%s\" at frame %llu\n", exit_on, (unsigned long long)z.frames);
                    break;
                }
                for (int i = 0; i < nons; i++)
                    if (strstr(text, ons[i].on)) {
                        printf("== frame %llu: %s\n%s", (unsigned long long)z.frames, ons[i].on, text);
                        type_text(&k, ons[i].text);
                        break;
                    }
            }
            for (int i = 0; i < nshots; i++)
                if (shots[i].frame == f + 1) {
                    char path[1024];
                    snprintf(path, sizeof path, "%s.scr", shots[i].arg);
                    FILE *fp = fopen(path, "wb");
                    if (fp) {
                        fwrite(&z.mem[0x4000], 1, 6912, fp);
                        fclose(fp);
                    }
                    snprintf(path, sizeof path, "%s.txt", shots[i].arg);
                    if ((fp = fopen(path, "w"))) {
                        dump_text(&z, fp);
                        fclose(fp);
                    }
                }
        }
        if (mf) {
            term_close();
            fclose(mf);
            fclose(mnull);
        }
        dump_text(&z, stdout);
        fflush(stdout);
        if (tap)
            fprintf(stderr, "speccy: tape: block %u of %u, %d%%, %s; %u reads of port 0xFE last frame\n",
                    z.tape.blocks_done, z.tape.blocks, tape_percent(&z.tape),
                    z.tape.playing ? "playing" : tape_done(&z.tape) ? "done" :
                    z.tape.stopped ? "stopped by the tape" : "stopped", z.fe_reads_last);
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
#ifndef _WIN32
    /* the terminal tells when a frame has got there: CSI 5 n after each one,
       answered CSI 0 n in order with the keys -- through a pty, ssh or a
       serial line, whatever buffers lie between */
    topt.ack = !no_skip && keys_reads_replies(&k) && isatty(STDOUT_FILENO);
#endif
    term_open(&topt);
    double next = now_seconds(), last_draw = 0, ff_t0 = 0;
    uint64_t ff_frames0 = 0;
    char title[256];
    /* The link (README, "A slow link"): a frame is drawn when div frames
       have passed since the last one drawn -- 50, 25, 16.7, 12.5 ... a
       second -- and the link is not behind; div goes up when it is, and
       back down once it has kept up for a while. */
    int div = 1, max_div = min_fps > 0 ? (int)(1.0 / (FRAME_SECONDS * min_fps) + 0.5) : 50;
    if (max_div < 1)
        max_div = 1;
    double cap_est = 0;                            /* bytes a second the link took when full */
    uint64_t delivered0 = 0;
    /* since the step last changed: its bytes, and the link's time on them */
    double step_t0 = next, step_busy = 0;
    uint64_t step_bytes = 0;
    uint64_t last_drawn = 0;
    bool any_drawn = false;
    double rate = (double)baud / 10.0, credit = 0, cap = rate * 0.2, t_prev = next;
    if (cap < 2048)
        cap = 2048;
    unsigned asked = 0, acked = 0;                 /* CSI 5 n out, CSI 0 n back */
    double ask_t[256], rtt_min = 0, rtt_min_t = 0, ack_t0 = 0, last_ping = 0;
    bool ask_ping[256];                            /* a lone CSI 5 n: the link's own latency */
    bool acks_seen = false;
    double win_t0 = next, calm_since = next, last_step = -100, step_wait = 1.0;
    unsigned win_frames = 0, win_drawn = 0, win_skipped = 0, win_congested = 0;
    uint64_t win_bytes = 0, last_bytes = 0;
    char stats_text[200] = "";
    uint64_t play_frames = 0, play_drawn = 0, play_bytes = 0, skipped_total = 0;
    double play_s = 0, t_start = next;
    double play_wall = 0;
    while (!k.quit && !got_signal) {
        double t_iter = now_seconds();
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
        bool ff = turbo || booting || loading || (long long)z.frames < fast_until;
        if (tap && autoload && z.frames == AUTOLOAD_FRAME)
            keys_type(&k, "j\"\"\n");          /* LOAD "" */
        for (int i = 0; i < ntypes; i++)
            if (types[i].frame == (long)z.frames)
                type_text(&k, types[i].arg);
        keys_frame(&k);
        z.keys = k.focused || keys_busy(&k) ? keys_down(&k) : 0;
        double t = now_seconds();

        /* how far behind the link is */
        if (rate > 0) {
            credit += rate * (t - t_prev);
            if (credit > cap)
                credit = cap;
        }
        t_prev = t;
        while (acked != k.acks && acked != asked) {
            double rtt = t - ask_t[acked % 256];
            if (!acks_seen || rtt < rtt_min || t - rtt_min_t > 5) {
                rtt_min = rtt;
                rtt_min_t = t;
            }
            acks_seen = true;
            if (!ask_ping[acked % 256])
                step_busy += rtt - rtt_min;       /* a frame: its time on the way */
            acked++;
        }
        if (topt.ack && !acks_seen && asked >= 25 && t - ack_t0 > 2) {
            topt.ack = false;                    /* the terminal does not answer */
            term_ack(false);
        }
        /* when nothing is on its way, now and then a question alone: its
           answer is the link's latency without a frame in it */
        if (topt.ack && asked == acked && term_queued() == 0 && t - last_ping >= 1.0) {
            term_ping();
            if (!asked)
                ack_t0 = t;
            ask_t[asked % 256] = t;
            ask_ping[asked % 256] = true;
            asked++;
            last_ping = t;
        }
        double lag = asked != acked ? t - ask_t[acked % 256] : 0;
        long backlog = term_backlog();
        bool congested = !no_skip &&
            ((rate > 0 && credit < 0) ||
             /* a frame still on its way a whole drawing period after the
                quickest answer: the link is not keeping up at this step */
             (topt.ack && acks_seen && (asked - acked >= 200 || lag > rtt_min + div * FRAME_SECONDS)) ||
             term_queued() > 0 || backlog > 2048);

        /* the frame about to run is z.frames + 1 */
        bool due = !any_drawn || (ff ? t - last_draw >= 0.1 : z.frames + 1 - last_drawn >= (uint64_t)div);
        bool floor_hit = min_fps > 0 && any_drawn && t - last_draw >= 1.0 / min_fps;
        bool last = quit_at >= 0 && z.frames + 1 >= (uint64_t)quit_at;
        bool draw = last || (due && (!congested || floor_hit));
        if (due && !draw) {
            win_skipped++;
            skipped_total++;
        }
        if (draw && congested)
            term_drain();                        /* the floor: wait for the link */
        if (due && congested)
            win_congested++;
        z.ula.draw = draw;
        zx_frame(&z);
        zx_tape_motor(&z);
        win_frames++;
        if (!ff)
            play_frames++;
        if (draw) {
            uint64_t b0 = term_bytes();
            term_frame(&z.ula.fb[0][0], ULA_FB_W, ULA_FB_H, ULA_PAPER_X, ULA_PAPER_Y, zx_font(&z));
            if (topt.ack) {
                if (!asked)
                    ack_t0 = t;
                ask_t[asked % 256] = t;
                ask_ping[asked % 256] = false;
                asked++;
            }
            last_draw = t;
            last_drawn = z.frames;
            any_drawn = true;
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
                              z.tape.playing ? "playing" : tape_done(&z.tape) ? "done" :
                              z.tape.stopped ? "stopped by the tape" : "stopped");
            if (ff && shown_speed > 0)
                n += snprintf(title + n, sizeof title - (size_t)n, " | fast x%.0f", shown_speed);
            if (stats)
                n += snprintf(title + n, sizeof title - (size_t)n, "%s", stats_text);
            snprintf(title + n, sizeof title - (size_t)n, " | F10 quit");
            term_title(title);
            last_bytes = term_bytes() - b0;
            win_bytes += last_bytes;
            step_bytes += last_bytes;
            win_drawn++;
            if (!ff) {
                play_drawn++;
                play_bytes += last_bytes;
            }
            if (rate > 0)
                credit -= (double)last_bytes;
            if (no_skip)
                term_drain();                    /* every frame: wait for the link */
        }
        if (t - win_t0 >= 0.5) {                 /* every half second */
            double w = t - win_t0;
            /* what the link carried: the frames the terminal answered for,
               or what left the queue for the device */
            uint64_t delivered = term_bytes() - term_queued();
            double carried = (double)(delivered - delivered0) / w;
            delivered0 = delivered;
            if (rate > 0)
                cap_est = rate;
            else if (win_congested)               /* the queue was full: that is what it takes */
                cap_est = cap_est > 0 ? 0.5 * cap_est + 0.5 * carried : carried;
            /* how busy this step has kept the link: with --baud, its bytes;
               with answers, the frames' time on the way (beyond the quickest
               answer); else its bytes against what the device took when the
               queue was full */
            double span = t - step_t0;
            double load = rate > 0 ? (double)step_bytes / span / rate
                        : topt.ack && acks_seen ? step_busy / span
                        : cap_est > 0 ? (double)step_bytes / span / cap_est : 0;
            /* the lowest step that would keep it within 85 % */
            int fit = (int)ceil(load * div / 0.85), was = div;
            if (fit < 1)
                fit = 1;
            if (win_congested) {
                if (div < max_div)
                    div++;
                if (t - last_step < 2)             /* a step down that did not hold */
                    step_wait = step_wait * 2 > 8 ? 8 : step_wait * 2;
                calm_since = t;
            } else if (fit < div && span >= 1.0 && t - calm_since >= step_wait) {
                div = fit;
                last_step = t;
            } else if (t - last_step > 10) {
                step_wait = 1.0;
            }
            if (div != was) {
                step_t0 = t;
                step_busy = 0;
                step_bytes = 0;
            }
            snprintf(stats_text, sizeof stats_text,
                     " | draw %.1f fps 1/%d | emu %.1f fps | %llu B/frame, avg %.0f | %.1f KB/s | skipped %llu%s",
                     win_drawn / w, div, win_frames / w, (unsigned long long)last_bytes,
                     win_drawn ? (double)win_bytes / win_drawn : 0.0, (double)win_bytes / w / 1000.0,
                     (unsigned long long)skipped_total,
                     topt.ack && acks_seen ? "" : rate > 0 ? " | budget" : "");
            win_t0 = t;
            win_frames = win_drawn = win_skipped = win_congested = 0;
            win_bytes = 0;
        }
        if (last)
            break;
        if (ff) {
            term_pump(0);
            next = now_seconds();
        } else {
            if (ff_t0) {
                ff_t0 = 0;
            }
            play_s += FRAME_SECONDS;
            next += FRAME_SECONDS;
            double wait = next - now_seconds();
            if (wait < -0.2)
                next = now_seconds();          /* fell behind: don't catch up */
            term_pump(wait);                   /* the link takes the queue meanwhile */
        }
        if (ff && !ff_t0) {
            ff_t0 = t;
            ff_frames0 = z.frames;
        }
        if (!ff)
            play_wall += now_seconds() - t_iter;
    }
    double wall = now_seconds() - t_start;
    /* the terminal answers every CSI 5 n before the shell gets it back:
       a late answer would land in the shell's input */
    term_drain();
    for (double give_up = now_seconds() + 3; topt.ack && k.acks != asked && now_seconds() < give_up;) {
        keys_poll(&k);
        term_pump(0.01);
    }
    term_close();
    keys_close(&k);
    if (k.note)
        fprintf(stderr, "speccy: keys: %s\n", k.note);
    if (stats || quit_at >= 0)
        fprintf(stderr, "speccy: stats: frame %llu after %.2f s; in play %llu frames (%.2f s of Spectrum time), "
                        "%.2f s of wall time, %llu drawn (%.1f a second), %llu bytes (%.0f a frame drawn, %.1f KB/s), "
                        "%llu skipped, draw every %d at the end%s\n",
                (unsigned long long)z.frames, wall, (unsigned long long)play_frames, play_s, play_wall,
                (unsigned long long)play_drawn, play_s > 0 ? play_drawn / play_s : 0.0,
                (unsigned long long)play_bytes, play_drawn ? (double)play_bytes / play_drawn : 0.0,
                play_s > 0 ? play_bytes / play_s / 1000.0 : 0.0, (unsigned long long)skipped_total, div,
                topt.ack && acks_seen ? ", the terminal answering" : "");
    return 0;
}
