/* ============================================================================
 * tape.h - a .tap file played as the ROM loader hears it: the EAR level
 * over time, in T-states of the 3.5 MHz CPU clock.
 *
 * A .tap holds standard ROM-loader blocks (a 2-byte length, then the flag,
 * data and checksum bytes). Each plays as the ROM's SA-BYTES writes it:
 * a pilot tone of 2168-T half-waves (8063 of them before a header, flag
 * < 0x80, 3223 before data), sync half-waves of 667 and 735 T, then two
 * half-waves a bit, MSB first: 855 T each for a 0, 1710 T for a 1. A
 * block ends with one more edge and a second of silence.
 * ==========================================================================*/
#ifndef SPECCY_TAPE_H
#define SPECCY_TAPE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;          /* the whole file                              */
    size_t   size;
    size_t   block;         /* offset of the current block's length word   */
    size_t   pos;           /* the byte being sent                         */
    size_t   end;           /* one past the block's last byte              */
    int      stage;         /* TAPE_PILOT ... (tape.c)                      */
    int      count;         /* pulses or bits left in the stage            */
    int      bit;           /* 7..0 in the byte, second half-wave flag     */
    bool     level;         /* the EAR level                               */
    bool     playing;       /* the motor runs                              */
    uint64_t next;          /* T-state of the next edge                    */
    uint64_t now;           /* T-states played so far                      */
    unsigned blocks, blocks_done;
} tape_t;

/* Load a .tap; false (with errno) if it cannot be read or is malformed. */
bool tape_load(tape_t *t, const char *path);
void tape_free(tape_t *t);
/* Advance the tape by n T-states (while playing) and return the EAR level. */
bool tape_advance(tape_t *t, uint32_t n);
/* All blocks played. */
static inline bool tape_done(const tape_t *t) { return t->data && t->block >= t->size; }
/* 0..100: how far through the file. */
int tape_percent(const tape_t *t);
#endif
