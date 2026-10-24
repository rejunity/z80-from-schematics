/* ============================================================================
 * tape.h - a .tap or .tzx file played as the ROM loader hears it: the EAR
 * level over time, in T-states of the 3.5 MHz CPU clock.
 *
 * A .tap holds standard ROM-loader blocks (a 2-byte length, then the flag,
 * data and checksum bytes). Each plays as the ROM's SA-BYTES writes it:
 * a pilot tone of 2168-T pulses (8063 of them before a header, flag
 * < 0x80, 3223 before data), sync pulses of 667 and 735 T, then two
 * pulses a bit, MSB first: 855 T each for a 0, 1710 T for a 1; then a
 * second of silence.
 *
 * A .tzx (v1.20) describes the signal itself, block by block: standard
 * and turbo data (0x10, 0x11), tones and pulse sequences (0x12, 0x13),
 * pure data (0x14), direct recordings (0x15), CSW recordings, RLE or
 * Z-RLE (0x18), generalized data (0x19), pauses and stops (0x20, 0x2A),
 * the signal level (0x2B), groups, jumps, loops, calls and selections
 * (0x21-0x28; a selection takes its first entry), and information that
 * plays nothing (0x30-0x35, 0x5A). Blocks it does not know are skipped by
 * their length (the format's extension rule).
 *
 * The player hands out pulses: at a pulse's start the level toggles (or,
 * where the block says so, stays, or goes low or high), and holds for the
 * pulse's length. A pause after a block starts with the edge that ends
 * the last pulse, holds that level 1 ms, then goes low for the rest of it
 * (TZX 1.20, as FUSE plays it). A stop block (a 0 ms pause, or 0x2A on this 48K
 * machine) stops the tape: tape.stopped, until the machine starts it again
 * (zx_tape_motor).
 * ==========================================================================*/
#ifndef SPECCY_TAPE_H
#define SPECCY_TAPE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* what a pulse does to the level at its start */
enum { TAPE_TOGGLE, TAPE_KEEP, TAPE_LOW, TAPE_HIGH, TAPE_STOP };

typedef struct {
    uint8_t        id;      /* the TZX block ID; 0 for a .tap block        */
    const uint8_t *body;    /* the block after its ID byte                 */
    uint32_t       len;     /* the body's length                           */
    size_t         off;     /* where the block starts in the file          */
    uint8_t       *csw;     /* 0x18: the pulses, Z-RLE inflated (owned)    */
    size_t         csw_n;
} tape_block_t;

typedef struct {
    uint8_t      *data;     /* the whole file                              */
    size_t        size;
    tape_block_t *b;        /* its blocks                                  */
    int           nb;
    bool          tzx;
    /* the player: block cur, and where in it (tape.c) */
    int           cur;
    bool          started;  /* block cur's stage is set up                 */
    int           stage;
    uint32_t      count;    /* pulses, samples or symbols left             */
    uint32_t      pos;      /* the byte, pulse, PRLE entry or symbol       */
    int           bit;      /* the bit and half (data), the pulse (0x19)   */
    uint32_t      sym, rep; /* 0x19: the symbol playing, its repeats left  */
    uint64_t      acc;      /* 0x18: samples played times 3.5 MHz          */
    uint64_t      csw_t;    /* 0x18: T-states handed out                   */
    /* block cur's parameters */
    uint32_t      pilot, sync1, sync2, zero, one, pause, rate;
    const uint8_t *d;       /* its data, samples, pulses or RLE bytes      */
    uint32_t      n;        /* their length in bytes                       */
    int           last;     /* bits used in the last byte                  */
    const uint8_t *g_sym[2], *g_prle, *g_ds;   /* 0x19: tables, stream     */
    uint32_t      g_tot[2]; /* 0x19: TOTP, TOTD                            */
    int           g_np[2], g_as[2], g_nb;      /* NPP/NPD, ASP/ASD, bits   */
    struct { int start, left; } loop[8];
    int           loops;
    int           call_block, call_i;  /* 0x26: the call playing (-1: none) */
    /* the signal */
    bool          level;    /* the EAR level                               */
    bool          playing;  /* the motor runs                              */
    bool          stopped;  /* a stop block stopped it                     */
    bool          ended;    /* every block played                          */
    uint64_t      next;     /* T-state of the next pulse's start           */
    uint64_t      now;      /* T-states played so far                      */
    unsigned      blocks, blocks_done;
} tape_t;

/* Load a .tap or .tzx (by its signature); false (with errno: EINVAL for a
   malformed file) if it cannot be read. tape_load_mem copies buf. */
bool tape_load(tape_t *t, const char *path);
bool tape_load_mem(tape_t *t, const uint8_t *buf, size_t n);
void tape_free(tape_t *t);
/* Advance the tape by n T-states (while playing) and return the EAR level. */
bool tape_advance(tape_t *t, uint32_t n);
/* All blocks played. */
static inline bool tape_done(const tape_t *t) { return t->data && t->ended; }
/* 0..100: how far through the file. */
int tape_percent(const tape_t *t);
/* The tape's title, publisher, year and loader as its archive info block
   (0x32) gives them, or its first text description (0x30); "" if none. */
void tape_describe(const tape_t *t, char *buf, size_t n);
/* For tests: the next pulse (its length, and TAPE_TOGGLE ... TAPE_STOP);
   false at the end of the tape. */
bool tape_pulse(tape_t *t, uint32_t *len, int *rule);
#endif
