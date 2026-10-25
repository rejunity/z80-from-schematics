/* keys.c - see keys.h */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#include "keys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/input.h>
#include <linux/kd.h>
#endif
#endif

#define TAP_FRAMES 4   /* a legacy key press: the ROM scans once a frame */
#define TAP_GAP    1   /* frames with no key between two taps            */
#define TAP_AGAIN  6   /* ... of the same key: the ROM takes a key that
                          comes back within 5 frames as the same press  */

/* ------------------------------------------------------------- mapping */

/* the key a character is on, and whether it is a symbol (SYMBOL SHIFT) */
static uint64_t char_keys(uint32_t ch, bool *punct)
{
    static const char rows[8][6] = { "\001ZXCV", "ASDFG", "QWERT", "12345",
                                     "09876", "POIUY", "\002LKJH", "\003\004MNB" };
    /* each symbol on SYMBOL SHIFT, then the key it is on */
    static const char sym[] = "!1@2#3$4%5&6'7(8)9_0<R>T;O\"P^H-J+K=L:Z?C/V*B,N.M";
    *punct = false;
    if (ch == '\n' || ch == '\r')
        return ZXM(ZX_ENTER);
    if (ch == ' ')
        return ZXM(ZX_SPACE);
    if (ch == 0xA3) {                                  /* the pound sign */
        *punct = true;
        return ZXM(ZX_SS) | ZXM(ZX_X);
    }
    uint64_t shift = 0;
    if (ch >= 'a' && ch <= 'z') {
        ch -= 32;
    } else if (ch >= 'A' && ch <= 'Z') {
        shift = ZXM(ZX_CS);
    } else if (ch > ' ' && ch < 127 && !(ch >= '0' && ch <= '9')) {
        for (const char *p = sym; *p; p += 2)
            if ((uint32_t)(unsigned char)*p == ch) {
                *punct = true;
                shift = ZXM(ZX_SS);
                ch = (uint32_t)(unsigned char)p[1];
                break;
            }
        if (!shift)
            return 0;
    }
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 5; b++)
            if ((uint32_t)(unsigned char)rows[r][b] == ch)
                return shift | ZXM(ZXK(r, b));
    return 0;
}

/* host keys that are not characters */
enum { HK_BACKSPACE = 0x110000, HK_LEFT, HK_RIGHT, HK_UP, HK_DOWN, HK_ESC, HK_TAB,
       HK_ENTER, HK_F5, HK_F10, HK_F12, HK_SHIFT, HK_CTRL, HK_ALT, HK_IGNORE };

static uint64_t special_keys(uint32_t hk)
{
    switch (hk) {
    case HK_BACKSPACE: return ZXM(ZX_CS) | ZXM(ZX_0);    /* DELETE */
    case HK_LEFT:      return ZXM(ZX_CS) | ZXM(ZX_5);
    case HK_DOWN:      return ZXM(ZX_CS) | ZXM(ZX_6);
    case HK_UP:        return ZXM(ZX_CS) | ZXM(ZX_7);
    case HK_RIGHT:     return ZXM(ZX_CS) | ZXM(ZX_8);
    case HK_ESC:       return ZXM(ZX_CS) | ZXM(ZX_SPACE); /* BREAK */
    case HK_TAB:       return ZXM(ZX_CS) | ZXM(ZX_SS);    /* EXTENDED MODE */
    case HK_ENTER:     return ZXM(ZX_ENTER);
    }
    return 0;
}

/* a command key, acted on when pressed */
static bool command(keys_t *k, uint32_t hk)
{
    switch (hk) {
    case HK_F10: k->quit = true; return true;
    case HK_F5:  k->reset = true; return true;
    case HK_F12: k->turbo_toggle = true; return true;
    }
    return false;
}

/* ---------------------------------------------------------------- taps */

void keys_tap(keys_t *k, uint64_t zx, int frames)
{
    int next = (k->qt + 1) % 256;
    if (next == k->qh || !zx)
        return;
    k->q[k->qt].zx = zx;
    k->q[k->qt].frames = frames;
    k->qt = next;
}

void keys_type(keys_t *k, const char *s)
{
    for (; *s; s++) {
        bool punct;
        keys_tap(k, char_keys((unsigned char)*s, &punct), TAP_FRAMES);
    }
}

void keys_frame(keys_t *k)
{
    if (k->tap_frames > 0) {
        if (--k->tap_frames == 0) {
            k->last_tap = k->tap;
            k->tap = 0;
            k->tap_gap = TAP_GAP;
            k->released = 0;
        }
        return;
    }
    k->released++;
    if (k->tap_gap > 0) {
        k->tap_gap--;
        return;
    }
    if (k->qh != k->qt && (k->q[k->qh].zx != k->last_tap || k->released >= TAP_AGAIN)) {
        k->tap = k->q[k->qh].zx;
        k->tap_frames = k->q[k->qh].frames;
        k->qh = (k->qh + 1) % 256;
    }
}

/* legacy: a key arrived; the same key again (autorepeat) keeps it held */
static void legacy_key(keys_t *k, uint64_t zx)
{
    if (!zx)
        return;
    if (k->qh != k->qt && k->q[(k->qt + 255) % 256].zx == zx)
        return;                                  /* already queued */
    if (k->qh == k->qt && k->tap == zx && k->tap_frames > 0) {
        k->tap_frames = TAP_FRAMES;              /* still held: extend */
        return;
    }
    keys_tap(k, zx, TAP_FRAMES);
}

/* ----------------------------------------------------- the keys held */

static void recompute(keys_t *k)
{
    uint64_t m = 0;
    bool punct = false;
    for (int i = 0; i < k->nhost; i++) {
        m |= k->host[i].zx;
        punct |= k->host[i].punct;
    }
    if (k->shift && !punct)            /* a symbol key brings its own shift */
        m |= ZXM(ZX_CS);
    if (k->sym)
        m |= ZXM(ZX_SS);
    k->held = m;
}

static void host_key(keys_t *k, uint32_t code, uint64_t zx, bool punct, bool down)
{
    int i;
    for (i = 0; i < k->nhost && k->host[i].code != code; i++)
        ;
    if (down && i == k->nhost && k->nhost < 16 && zx) {
        k->host[k->nhost].code = code;
        k->host[k->nhost].zx = zx;
        k->host[k->nhost].punct = punct;
        k->nhost++;
    } else if (!down && i < k->nhost) {
        k->host[i] = k->host[--k->nhost];
    }
    recompute(k);
}

static void release_all(keys_t *k)
{
    k->nhost = 0;
    k->shift = k->sym = k->ctrl = k->alt = false;
    recompute(k);
}

/* A key in a native mode, by its Linux input keycode (the macOS keycodes
   are translated to these): letters and digits where they sit, the
   punctuation keys as the symbols they type on a US layout. */
static void native_key(keys_t *k, unsigned code, bool down)
{
    static const char plain[64] = "\0\0" "1234567890-=\0\0" "qwertyuiop\0\0\0\0" "asdfghjkl;'`\0\\" "zxcvbnm,./";
    static const char shifted[64] = "\0\0" "!@#$%^&*()_+\0\0" "QWERTYUIOP\0\0\0\0" "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?";
    uint32_t hk = 0;
    switch (code) {
    case 1:   hk = HK_ESC; break;
    case 14:  hk = HK_BACKSPACE; break;
    case 15:  hk = HK_TAB; break;
    case 28: case 96: hk = HK_ENTER; break;
    case 42: case 54: k->shift = down; recompute(k); return;
    case 29: case 97: k->ctrl = down; k->sym = k->ctrl || k->alt; recompute(k); return;
    case 56: case 100: k->alt = down; k->sym = k->ctrl || k->alt; recompute(k); return;
    case 57:  host_key(k, code, ZXM(ZX_SPACE), false, down); return;
    case 63:  hk = HK_F5; break;
    case 68:  hk = HK_F10; break;
    case 88:  hk = HK_F12; break;
    case 103: hk = HK_UP; break;
    case 105: hk = HK_LEFT; break;
    case 106: hk = HK_RIGHT; break;
    case 108: hk = HK_DOWN; break;
    }
    if (down && code == 46 && k->ctrl) {                  /* Ctrl-C */
        k->quit = true;
        return;
    }
    if (hk) {
        if (down && command(k, hk))
            return;
        host_key(k, code, special_keys(hk), false, down);
        return;
    }
    if (code >= 64 || !plain[code])
        return;
    char c = plain[code];
    bool punct = false;
    uint64_t zx;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        zx = char_keys((unsigned char)c, &punct);         /* CAPS SHIFT from shift */
    else
        zx = char_keys((unsigned char)(k->shift ? shifted[code] : c), &punct);
    host_key(k, code, zx, punct, down);
}

const char *keys_mode_name(int mode)
{
    switch (mode) {
    case KEYS_KITTY:   return "kitty";
    case KEYS_EVDEV:   return "evdev";
    case KEYS_CONSOLE: return "console";
    case KEYS_MACOS:   return "macos";
    case KEYS_WINDOWS: return "windows";
    case KEYS_LEGACY:  return "legacy";
    case KEYS_AUTO:    return "auto";
    }
    return "none";
}

bool keys_reads_replies(const keys_t *k)
{
    return k->mode == KEYS_LEGACY || k->mode == KEYS_KITTY || k->mode == KEYS_EVDEV ||
           k->mode == KEYS_MACOS;
}

int keys_mode_by_name(const char *name)
{
    for (int m = KEYS_AUTO; m <= KEYS_WINDOWS; m++)
        if (!strcmp(name, keys_mode_name(m)))
            return m;
    return -1;
}

#ifdef _WIN32
/* ------------------------------------------------------------ Windows */
static HANDLE hin;
static DWORD  old_in_mode;

int keys_open(keys_t *k, int want)
{
    memset(k, 0, sizeof *k);
    k->focused = true;
    hin = GetStdHandle(STD_INPUT_HANDLE);
    if (want == KEYS_NONE || hin == INVALID_HANDLE_VALUE || !GetConsoleMode(hin, &old_in_mode))
        return k->mode = KEYS_NONE;
    SetConsoleMode(hin, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
    return k->mode = KEYS_WINDOWS;
}

void keys_close(keys_t *k)
{
    if (k->mode == KEYS_WINDOWS)
        SetConsoleMode(hin, old_in_mode);
}

/* a virtual-key code -> its Linux keycode, for native_key() */
static unsigned vk_linux(WORD vk)
{
    static const unsigned char letters[26] = { 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
                                               49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44 };
    if (vk >= 'A' && vk <= 'Z')
        return letters[vk - 'A'];
    if (vk >= '1' && vk <= '9')
        return 2u + (vk - '1');
    switch (vk) {
    case '0': return 11;
    case VK_OEM_MINUS: return 12;
    case VK_OEM_PLUS: return 13;
    case VK_BACK: return 14;
    case VK_TAB: return 15;
    case VK_RETURN: return 28;
    case VK_OEM_1: return 39;      /* ;: */
    case VK_OEM_7: return 40;      /* '" */
    case VK_OEM_COMMA: return 51;
    case VK_OEM_PERIOD: return 52;
    case VK_OEM_2: return 53;      /* /? */
    case VK_SPACE: return 57;
    case VK_ESCAPE: return 1;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return 42;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return 29;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return 56;
    case VK_F5: return 63;
    case VK_F10: return 68;
    case VK_F12: return 88;
    case VK_UP: return 103;
    case VK_LEFT: return 105;
    case VK_RIGHT: return 106;
    case VK_DOWN: return 108;
    }
    return 0;
}

void keys_poll(keys_t *k)
{
    DWORD n = 0;
    while (k->mode == KEYS_WINDOWS && GetNumberOfConsoleInputEvents(hin, &n) && n) {
        INPUT_RECORD rec[64];
        DWORD got = 0;
        if (!ReadConsoleInputW(hin, rec, 64, &got))
            return;
        for (DWORD i = 0; i < got; i++) {
            if (rec[i].EventType == WINDOW_BUFFER_SIZE_EVENT)
                k->resized = true;
            else if (rec[i].EventType == FOCUS_EVENT && !rec[i].Event.FocusEvent.bSetFocus)
                release_all(k);
            else if (rec[i].EventType == KEY_EVENT) {
                unsigned code = vk_linux(rec[i].Event.KeyEvent.wVirtualKeyCode);
                if (code)
                    native_key(k, code, rec[i].Event.KeyEvent.bKeyDown != 0);
            }
        }
    }
}

#else
/* -------------------------------------------------------------- POSIX */
static struct termios old_tio;
static bool tio_saved;
#ifdef __linux__
static int  old_kbmode = -1;
static int  evfd[16];
static int  nevfd;
#endif
/* macOS: CGEventSourceKeyState, found at run time */
typedef bool (*cg_keystate_fn)(int32_t state, uint16_t key);
static cg_keystate_fn cg_keystate;
static bool cg_down[128];

static int read_some(int fd, unsigned char *buf, int cap, int timeout_ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    if (poll(&p, 1, timeout_ms) <= 0)
        return 0;
    ssize_t n = read(fd, buf, (size_t)cap);
    return n > 0 ? (int)n : 0;
}

static void restore_console(void)
{
#ifdef __linux__
    if (old_kbmode >= 0) {
        ioctl(STDIN_FILENO, KDSKBMODE, old_kbmode);
        old_kbmode = -1;
    }
#endif
    if (tio_saved)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_tio);
}

/* a crash must not leave the console keyboard in raw mode */
static void fatal_signal(int sig)
{
    restore_console();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* the terminal answers CSI ? u (its kitty protocol flags) before CSI c */
static bool kitty_supported(void)
{
    fputs("\x1b[?u\x1b[c", stdout);
    fflush(stdout);
    char reply[256];
    int len = 0;
    for (int tries = 0; tries < 20 && len < (int)sizeof reply - 1; tries++) {
        len += read_some(STDIN_FILENO, (unsigned char *)reply + len, (int)sizeof reply - 1 - len, 50);
        reply[len] = 0;
        const char *da = strstr(reply, "\x1b[?");
        if (da && strchr(da, 'c'))
            break;
    }
    reply[len] = 0;
    for (char *p = reply; (p = strstr(p, "\x1b[?")) != NULL; p += 3) {
        char *q = p + 3;
        while (*q >= '0' && *q <= '9')
            q++;
        if (*q == 'u')
            return true;
    }
    return false;
}

#ifdef __linux__
static bool open_evdev(keys_t *k)
{
    char path[32];
    for (int i = 0; i < 32 && nevfd < 16; i++) {
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        unsigned long bits[(KEY_MAX + 1) / (8 * sizeof(unsigned long)) + 1] = { 0 };
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits);
#define HAS(b) (bits[(b) / (8 * sizeof(unsigned long))] >> ((b) % (8 * sizeof(unsigned long))) & 1)
        if (HAS(KEY_A) && HAS(KEY_Z) && HAS(KEY_SPACE) && HAS(KEY_ENTER))
            evfd[nevfd++] = fd;
        else
            close(fd);
#undef HAS
    }
    if (!nevfd)
        k->note = "no readable keyboard in /dev/input (the input group)";
    return nevfd > 0;
}
#endif

static bool open_macos(keys_t *k)
{
    static const char *const libs[] = {
        "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
        "/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices" };
    for (unsigned i = 0; i < 2 && !cg_keystate; i++) {
        void *h = dlopen(libs[i], RTLD_LAZY);
        if (!h)
            continue;
        cg_keystate = (cg_keystate_fn)dlsym(h, "CGEventSourceKeyState");
        bool (*preflight)(void) = (bool (*)(void))dlsym(h, "CGPreflightListenEventAccess");
        bool (*request)(void) = (bool (*)(void))dlsym(h, "CGRequestListenEventAccess");
        if (cg_keystate && preflight && !preflight()) {
            /* the system asks the user to allow the terminal app; this run
               goes on without */
            if (request)
                request();
            k->note = "allow the terminal app under Privacy & Security > Input Monitoring";
            cg_keystate = NULL;
            return false;
        }
    }
    return cg_keystate != NULL;
}

int keys_open(keys_t *k, int want)
{
    memset(k, 0, sizeof *k);
    k->focused = true;
    if (want == KEYS_NONE || !isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &old_tio) != 0)
        return k->mode = KEYS_NONE;
    tio_saved = true;
    struct termios t = old_tio;
    t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | ISIG | IEXTEN);
    t.c_iflag &= ~(tcflag_t)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    signal(SIGSEGV, fatal_signal);
    signal(SIGABRT, fatal_signal);
    signal(SIGBUS, fatal_signal);
#ifdef __linux__
    int kbmode;
    if ((want == KEYS_AUTO || want == KEYS_CONSOLE) && ioctl(STDIN_FILENO, KDGKBMODE, &kbmode) == 0) {
        if (ioctl(STDIN_FILENO, KDSKBMODE, K_MEDIUMRAW) == 0) {
            old_kbmode = kbmode;
            return k->mode = KEYS_CONSOLE;
        }
        k->note = "the console keyboard cannot be put in raw mode";
    }
#endif
    if (want == KEYS_CONSOLE)
        return k->mode = KEYS_LEGACY;
    bool kitty = (want == KEYS_AUTO || want == KEYS_KITTY) && kitty_supported();
    fputs("\x1b[?1004h", stdout);                 /* focus reports */
    if (kitty) {
        /* 1 disambiguate, 2 event types, 4 alternate keys, 8 all keys */
        fputs("\x1b[>15u", stdout);
        fflush(stdout);
        return k->mode = KEYS_KITTY;
    }
    fflush(stdout);
#ifdef __linux__
    if ((want == KEYS_AUTO || want == KEYS_EVDEV) && open_evdev(k))
        return k->mode = KEYS_EVDEV;
#endif
    if ((want == KEYS_AUTO || want == KEYS_MACOS) && open_macos(k))
        return k->mode = KEYS_MACOS;
    return k->mode = KEYS_LEGACY;
}

void keys_close(keys_t *k)
{
    if (k->mode == KEYS_KITTY)
        fputs("\x1b[<u", stdout);
    if (k->mode != KEYS_CONSOLE && k->mode != KEYS_NONE)
        fputs("\x1b[?1004l", stdout);
    fflush(stdout);
#ifdef __linux__
    for (int i = 0; i < nevfd; i++)
        close(evfd[i]);
    nevfd = 0;
#endif
    restore_console();
}

/* kitty: a key's code -> a host key; text keys are their Unicode codes */
static uint32_t kitty_code(uint32_t code)
{
    switch (code) {
    case 127: case 8: return HK_BACKSPACE;
    case 27:  return HK_ESC;
    case 9:   return HK_TAB;
    case 13:  return HK_ENTER;
    case 57441: case 57447: return HK_SHIFT;           /* left / right shift */
    case 57442: case 57448: return HK_CTRL;
    case 57443: case 57449: return HK_ALT;
    case 57414: return HK_ENTER;                        /* keypad enter */
    }
    return code >= 57344 && code <= 63743 ? HK_IGNORE : 0;
}

/* a CSI sequence: number groups split by ';', sub-fields by ':' */
static void kitty_event(keys_t *k, const char *params, char final)
{
    unsigned v[4][3] = { { 0 } };
    int g = 0, s = 0;
    for (const char *p = params; *p && g < 4; p++) {
        if (*p >= '0' && *p <= '9')
            v[g][s] = v[g][s] * 10 + (unsigned)(*p - '0');
        else if (*p == ':' && s < 2)
            s++;
        else if (*p == ';') {
            g++;
            s = 0;
        }
    }
    unsigned mods = v[1][0] ? v[1][0] - 1 : 0, ev = v[1][1] ? v[1][1] : 1;
    bool down = ev != 3;
    uint32_t hk = 0, code = 0;
    switch (final) {
    case 'u': code = v[0][0]; hk = kitty_code(code); break;
    case 'A': hk = HK_UP; break;
    case 'B': hk = HK_DOWN; break;
    case 'C': hk = HK_RIGHT; break;
    case 'D': hk = HK_LEFT; break;
    case '~':
        switch (v[0][0]) {
        case 15: hk = HK_F5; break;
        case 21: hk = HK_F10; break;
        case 24: hk = HK_F12; break;
        default: return;
        }
        break;
    default: return;
    }
    if (hk == HK_IGNORE)
        return;
    if (hk == HK_SHIFT) { k->shift = down; recompute(k); return; }
    if (hk == HK_CTRL || hk == HK_ALT) { k->sym = down; recompute(k); return; }
    if (down && code == 'c' && (mods & 4)) { k->quit = true; return; }
    if (hk) {
        if (ev == 1 && command(k, hk))
            return;
        host_key(k, hk, special_keys(hk), false, down);
        return;
    }
    /* letters and digits are their keys (the host's shift is CAPS SHIFT);
       other keys type their symbol, the shifted one when shifted */
    bool punct = false;
    uint64_t zx;
    if ((code >= 'a' && code <= 'z') || (code >= '0' && code <= '9'))
        zx = char_keys(code, &punct);
    else
        zx = char_keys((mods & 1) && v[0][1] ? v[0][1] : code, &punct);
    host_key(k, code, zx, punct, down);
}

static void legacy_seq(keys_t *k, const char *params, char final)
{
    uint32_t hk = 0;
    switch (final) {
    case 'A': hk = HK_UP; break;
    case 'B': hk = HK_DOWN; break;
    case 'C': hk = HK_RIGHT; break;
    case 'D': hk = HK_LEFT; break;
    case '~':
        switch (atoi(params)) {
        case 15: hk = HK_F5; break;
        case 21: hk = HK_F10; break;
        case 24: hk = HK_F12; break;
        }
        break;
    }
    if (hk && !command(k, hk))
        legacy_key(k, special_keys(hk));
}

static void legacy_char(keys_t *k, uint32_t ch)
{
    bool punct;
    if (ch == 3)
        k->quit = true;
    else if (ch == 127 || ch == 8)
        legacy_key(k, special_keys(HK_BACKSPACE));
    else if (ch == '\t')
        legacy_key(k, special_keys(HK_TAB));
    else
        legacy_key(k, char_keys(ch, &punct));
}

/* the console in K_MEDIUMRAW: a keycode a byte, bit 7 set on release
   (codes above 127 take three bytes: 0, then 7 bits each, high first) */
static void console_bytes(keys_t *k, const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++) {
        bool up = b[i] & 0x80;
        unsigned code = b[i] & 0x7F;
        if (!code && i + 2 < n) {
            code = (unsigned)(b[i + 1] & 0x7F) << 7 | (b[i + 2] & 0x7F);
            i += 2;
        }
        native_key(k, code, !up);
    }
}

/* what the terminal sends: keys (legacy, kitty) and focus reports */
static void terminal_bytes(keys_t *k, unsigned char *buf, int *plen)
{
    int len = *plen, i = 0;
    bool native = k->mode == KEYS_EVDEV || k->mode == KEYS_MACOS;
    while (i < len) {
        unsigned char c = buf[i];
        if (c == 0x1b) {
            if (i + 1 >= len) {                  /* a lone ESC */
                if (k->mode == KEYS_LEGACY)
                    legacy_key(k, special_keys(HK_ESC));
                i++;
                continue;
            }
            if (buf[i + 1] == '[' || buf[i + 1] == 'O') {
                int j = i + 2;
                while (j < len && !(buf[j] >= 0x40 && buf[j] <= 0x7e))
                    j++;
                if (j >= len)
                    break;                        /* incomplete: wait */
                char params[64];
                int n = j - (i + 2) < 63 ? j - (i + 2) : 63;
                memcpy(params, buf + i + 2, (size_t)n);
                params[n] = 0;
                char final = (char)buf[j], intro = (char)buf[i + 1];
                i = j + 1;
                if (intro == 'O') {               /* SS3: arrows, F1-F4 */
                    if (k->mode == KEYS_LEGACY && final >= 'A' && final <= 'D')
                        legacy_seq(k, "", final);
                } else if (final == 'n' && !strcmp(params, "0")) {
                    k->acks++;                    /* status OK: a frame got there */
                } else if (!params[0] && (final == 'I' || final == 'O')) {
                    k->focused = final == 'I';   /* focus in / out */
                    if (!k->focused)
                        release_all(k);
                } else if (params[0] == '?') {
                    /* a reply: nothing to do */
                } else if (k->mode == KEYS_KITTY) {
                    kitty_event(k, params, final);
                } else if (k->mode == KEYS_LEGACY) {
                    legacy_seq(k, params, final);
                }
                continue;
            }
            i += 2;                               /* ESC x (Alt-x): ignored */
            continue;
        }
        uint32_t ch = c;                          /* UTF-8 */
        int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (i + n > len)
            break;
        if (n > 1) {
            ch = c & (0x7Fu >> n);
            for (int b = 1; b < n; b++)
                ch = ch << 6 | (buf[i + b] & 0x3Fu);
        }
        i += n;
        if (k->mode == KEYS_LEGACY)
            legacy_char(k, ch);
        else if (ch == 3 && !native)
            k->quit = true;                       /* before the kitty flags */
    }
    memmove(buf, buf + i, (size_t)(len - i));
    *plen = len - i;
}

/* the macOS keycodes of the keys native_key() knows, with their Linux codes */
static const unsigned char mac_keys[][2] = {
    { 0x00, 30 }, { 0x01, 31 }, { 0x02, 32 }, { 0x03, 33 }, { 0x04, 35 }, { 0x05, 34 },
    { 0x06, 44 }, { 0x07, 45 }, { 0x08, 46 }, { 0x09, 47 }, { 0x0B, 48 }, { 0x0C, 16 },
    { 0x0D, 17 }, { 0x0E, 18 }, { 0x0F, 19 }, { 0x10, 21 }, { 0x11, 20 }, { 0x12, 2 },
    { 0x13, 3 }, { 0x14, 4 }, { 0x15, 5 }, { 0x16, 7 }, { 0x17, 6 }, { 0x18, 13 },
    { 0x19, 10 }, { 0x1A, 8 }, { 0x1B, 12 }, { 0x1C, 9 }, { 0x1D, 11 }, { 0x1F, 24 },
    { 0x20, 22 }, { 0x22, 23 }, { 0x23, 25 }, { 0x24, 28 }, { 0x25, 38 }, { 0x26, 36 },
    { 0x27, 40 }, { 0x28, 37 }, { 0x29, 39 }, { 0x2B, 51 }, { 0x2C, 53 }, { 0x2D, 49 },
    { 0x2E, 50 }, { 0x2F, 52 }, { 0x30, 15 }, { 0x31, 57 }, { 0x33, 14 }, { 0x35, 1 },
    { 0x38, 42 }, { 0x3A, 56 }, { 0x3B, 29 }, { 0x3C, 54 }, { 0x3D, 100 }, { 0x3E, 97 },
    { 0x4C, 96 }, { 0x60, 63 }, { 0x6D, 68 }, { 0x6F, 88 }, { 0x7B, 105 }, { 0x7C, 106 },
    { 0x7D, 108 }, { 0x7E, 103 } };

void keys_poll(keys_t *k)
{
    static unsigned char buf[4096];
    static int len;
    if (k->mode == KEYS_NONE)
        return;
    int n = read_some(STDIN_FILENO, buf + len, (int)sizeof buf - len, 0);
    if (k->mode == KEYS_CONSOLE) {
        console_bytes(k, buf, n);
        return;
    }
    len += n;
    terminal_bytes(k, buf, &len);
#ifdef __linux__
    if (k->mode == KEYS_EVDEV) {
        struct input_event ev[64];
        for (int f = 0; f < nevfd; f++) {
            ssize_t got;
            while ((got = read(evfd[f], ev, sizeof ev)) > 0)
                for (size_t i = 0; i < (size_t)got / sizeof ev[0]; i++)
                    if (ev[i].type == EV_KEY && ev[i].value != 2 && k->focused)
                        native_key(k, ev[i].code, ev[i].value == 1);
        }
    }
#endif
    if (k->mode == KEYS_MACOS && cg_keystate) {
        for (unsigned i = 0; i < sizeof mac_keys / sizeof mac_keys[0]; i++) {
            bool d = k->focused && cg_keystate(1 /* HID system state */, mac_keys[i][0]);
            if (d != cg_down[mac_keys[i][0]]) {
                cg_down[mac_keys[i][0]] = d;
                native_key(k, mac_keys[i][1], d);
            }
        }
    }
}
#endif
