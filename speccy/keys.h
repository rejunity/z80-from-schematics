/* ============================================================================
 * keys.h - the host keyboard as the Spectrum's 40-key matrix.
 *
 * A Spectrum program reads keys as held, so input needs key releases. The
 * ways to get them, tried by keys_open() in this order (or one forced):
 *   console  Linux virtual console: the keyboard in raw mode (KDSKBMODE
 *            K_MEDIUMRAW), keycodes with press and release;
 *   kitty    a terminal speaking the kitty keyboard protocol (kitty, foot,
 *            Ghostty, WezTerm, Alacritty, iTerm2 ...): press, repeat and
 *            release, asked for with CSI > 15 u after CSI ? u finds it;
 *   evdev    Linux, any terminal: the kernel's keyboard devices
 *            (/dev/input/event*, readable by the input group);
 *   macos    CoreGraphics' CGEventSourceKeyState, polled, loaded at run
 *            time (the terminal app needs Input Monitoring permission);
 *   windows  the console's input records (ReadConsoleInput);
 *   legacy   any other terminal: characters only, each a tap held for a
 *            few frames, extended while it autorepeats, queued.
 * evdev and macos see the keyboard whatever window has focus, so they
 * follow the terminal's focus reports (CSI ? 1004 h) where it sends them.
 *
 * Letters, digits, space and enter are the Spectrum's keys where they sit;
 * shift is CAPS SHIFT, ctrl or alt SYMBOL SHIFT; the host's punctuation
 * keys type their symbols (SYMBOL SHIFT and a key); backspace is DELETE,
 * the arrows CAPS SHIFT 5-8, Esc BREAK, Tab EXTENDED MODE. F5 resets, F12
 * toggles full speed, F10 or Ctrl-C quits.
 *
 * Keys are numbered row * 5 + bit, as the ULA reads them: row r is the
 * half-row address line A(8+r) selects, bit 0 its outermost key.
 * ==========================================================================*/
#ifndef SPECCY_KEYS_H
#define SPECCY_KEYS_H
#include <stdbool.h>
#include <stdint.h>

#define ZXK(row, bit) ((row) * 5 + (bit))
enum {
    ZX_CS = ZXK(0, 0), ZX_Z, ZX_X, ZX_C, ZX_V,
    ZX_A = ZXK(1, 0), ZX_S, ZX_D, ZX_F, ZX_G,
    ZX_Q = ZXK(2, 0), ZX_W, ZX_E, ZX_R, ZX_T,
    ZX_1 = ZXK(3, 0), ZX_2, ZX_3, ZX_4, ZX_5,
    ZX_0 = ZXK(4, 0), ZX_9, ZX_8, ZX_7, ZX_6,
    ZX_P = ZXK(5, 0), ZX_O, ZX_I, ZX_U, ZX_Y,
    ZX_ENTER = ZXK(6, 0), ZX_L, ZX_K, ZX_J, ZX_H,
    ZX_SPACE = ZXK(7, 0), ZX_SS, ZX_M, ZX_N, ZX_B
};
#define ZXM(k) (1ULL << (k))

enum { KEYS_AUTO, KEYS_NONE, KEYS_LEGACY, KEYS_KITTY, KEYS_EVDEV, KEYS_CONSOLE,
       KEYS_MACOS, KEYS_WINDOWS };

typedef struct {
    int      mode;                 /* KEYS_* */
    bool     focused;              /* the terminal has focus (or says nothing) */
    uint64_t held;                 /* the keys the host holds, mapped      */
    struct { uint32_t code; uint64_t zx; bool punct; } host[16];
    int      nhost;
    bool     shift, sym;           /* host modifiers: CAPS / SYMBOL SHIFT  */
    bool     ctrl, alt;            /* (SYMBOL SHIFT is either)             */
    /* taps: legacy input and typing for the machine (autoload) */
    struct { uint64_t zx; int frames; } q[256];
    int      qh, qt;
    uint64_t tap;                  /* the tap held now                      */
    int      tap_frames;           /* frames left of it                     */
    int      tap_gap;              /* frames of no key before the next tap  */
    uint64_t last_tap;             /* the tap before                        */
    int      released;             /* frames since it ended                 */
    /* commands */
    bool     quit, reset, turbo_toggle, resized;
    const char *note;              /* why a native mode was not used       */
} keys_t;

/* Raw mode and an input mode: KEYS_AUTO picks the first that works. */
int  keys_open(keys_t *k, int want);
void keys_close(keys_t *k);
/* Read what has arrived; once a frame. */
void keys_poll(keys_t *k);
/* Advance the taps by one emulated frame. */
void keys_frame(keys_t *k);
/* The 40 keys down now (bit ZXK(row, bit)). */
static inline uint64_t keys_down(const keys_t *k) { return k->held | k->tap; }
/* Queue a tap of these keys for this many frames. */
void keys_tap(keys_t *k, uint64_t zx, int frames);
/* Queue a string as taps: letters, digits, space, '\n' (ENTER) and the
   symbols on SYMBOL SHIFT. */
void keys_type(keys_t *k, const char *s);
static inline bool keys_busy(const keys_t *k) { return k->qh != k->qt || k->tap_frames > 0 || k->tap_gap > 0; }
const char *keys_mode_name(int mode);
int keys_mode_by_name(const char *name);
#endif
