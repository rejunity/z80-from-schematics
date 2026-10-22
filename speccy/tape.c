/* tape.c - see tape.h */
#include "tape.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TAPE_START, TAPE_PILOT, TAPE_SYNC1, TAPE_SYNC2, TAPE_DATA, TAPE_TAIL, TAPE_PAUSE };

#define T_PILOT   2168u
#define T_SYNC1    667u
#define T_SYNC2    735u
#define T_ZERO     855u
#define T_ONE     1710u
#define T_TAIL     945u        /* the last edge after a block's final bit     */
#define T_PAUSE 3500000u       /* the second of silence after a block         */

bool tape_load(tape_t *t, const char *path)
{
    memset(t, 0, sizeof *t);
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0 || !(t->data = malloc((size_t)n)) || fread(t->data, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        free(t->data);
        t->data = NULL;
        errno = n <= 0 ? EINVAL : EIO;
        return false;
    }
    fclose(fp);
    t->size = (size_t)n;
    /* the blocks must tile the file */
    for (size_t at = 0; at < t->size; t->blocks++) {
        if (at + 2 > t->size) {
            free(t->data);
            t->data = NULL;
            errno = EINVAL;
            return false;
        }
        at += 2 + (size_t)(t->data[at] | t->data[at + 1] << 8);
        if (at > t->size) {
            free(t->data);
            t->data = NULL;
            errno = EINVAL;
            return false;
        }
    }
    t->stage = TAPE_START;
    return true;
}

void tape_free(tape_t *t)
{
    free(t->data);
    memset(t, 0, sizeof *t);
}

int tape_percent(const tape_t *t)
{
    if (!t->data || !t->size)
        return 0;
    size_t at = t->stage == TAPE_DATA ? t->pos : t->block;
    return (int)(at * 100 / t->size);
}

/* the stage after an edge; sets t->next */
static void edge(tape_t *t)
{
    for (;;) {
        switch (t->stage) {
        case TAPE_START: {
            if (t->block >= t->size) {        /* end of tape */
                t->playing = false;
                t->next = UINT64_MAX;
                return;
            }
            size_t len = (size_t)(t->data[t->block] | t->data[t->block + 1] << 8);
            t->pos = t->block + 2;
            t->end = t->pos + len;
            if (len == 0) {                    /* an empty block: skip it */
                t->block = t->end;
                t->blocks_done++;
                continue;
            }
            t->stage = TAPE_PILOT;
            t->count = t->data[t->pos] < 0x80 ? 8063 : 3223;
            /* the first half-wave, from the end of the last pause */
            t->next = (t->next ? t->next : t->now) + T_PILOT;
            return;
        }
        case TAPE_PILOT:
            t->level = !t->level;
            if (--t->count > 0) {
                t->next += T_PILOT;
                return;
            }
            t->stage = TAPE_SYNC1;
            t->next += T_SYNC1;
            return;
        case TAPE_SYNC1:
            t->level = !t->level;
            t->stage = TAPE_SYNC2;
            t->next += T_SYNC2;
            return;
        case TAPE_SYNC2:
            t->level = !t->level;
            t->stage = TAPE_DATA;
            t->bit = 7 << 1;                   /* bit 7, first half-wave */
            t->next += (t->data[t->pos] & 0x80) ? T_ONE : T_ZERO;
            return;
        case TAPE_DATA: {
            t->level = !t->level;
            if (!(t->bit & 1)) {               /* the bit's second half-wave */
                t->bit |= 1;
            } else if (t->bit >> 1) {          /* the next bit */
                t->bit = ((t->bit >> 1) - 1) << 1;
            } else if (++t->pos < t->end) {    /* the next byte */
                t->bit = 7 << 1;
            } else {
                t->stage = TAPE_TAIL;
                t->next += T_TAIL;
                return;
            }
            t->next += (t->data[t->pos] >> (t->bit >> 1) & 1) ? T_ONE : T_ZERO;
            return;
        }
        case TAPE_TAIL:
            t->level = !t->level;
            t->stage = TAPE_PAUSE;
            t->next += T_PAUSE;
            return;
        case TAPE_PAUSE:
            t->block = t->end;
            t->blocks_done++;
            t->stage = TAPE_START;
            t->level = false;
            continue;
        }
    }
}

bool tape_advance(tape_t *t, uint32_t n)
{
    if (!t->data || !t->playing)
        return t->level;
    if (t->stage == TAPE_START && t->next == 0 && t->block < t->size)
        edge(t);                               /* the motor's first start */
    t->now += n;
    while (t->now >= t->next)
        edge(t);
    return t->level;
}
