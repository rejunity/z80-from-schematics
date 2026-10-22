/* ============================================================================
 * zx.h - the 48K Spectrum: cfast's Z80, the ULA, 16K ROM and 48K RAM, the
 * keyboard and the tape, stepped a pixel clock (a CPU phase) at a time.
 * ==========================================================================*/
#ifndef SPECCY_ZX_H
#define SPECCY_ZX_H
#include "z80_cfast.h"
#include "tape.h"
#include "ula.h"

typedef struct {
    z80_t    cpu;
    z80f_t   fast;
    uint64_t pins;          /* the CPU's pins after its last frame       */
    uint64_t in;            /* the pin word its next phase gets          */
    uint8_t  mem[65536];    /* 0x0000-0x3FFF ROM, the rest RAM           */
    ula_t    ula;
    tape_t   tape;
    uint64_t keys;          /* the 40 keys down (keys.h numbering)       */
    uint64_t tape_t;        /* T-state up to which the tape has played   */
    uint32_t fe_reads;      /* port 0xFE reads this frame                */
    uint32_t fe_reads_last; /* ... in the last frame                     */
    uint32_t tape_idle;     /* frames since the program last read the tape */
    int      reset_phases;  /* RESET held for this many more phases      */
    bool     iowr;          /* an I/O write seen in this cycle           */
    uint64_t frames;
} zx_t;

void zx_init(zx_t *z, const uint8_t rom[16384]);
/* Hold RESET for a few T-states, as the button would. */
void zx_reset(zx_t *z);
/* Run one frame (69888 T-states of the ULA's time). */
void zx_frame(zx_t *z);
/* The tape motor: on while a program reads the tape -- port 0xFE read
   thousands of times a frame, or the PC in the ROM's tape routines -- and
   off 3 s after (the ROM sits a second in a delay loop when it first hears
   a tone). Call once a frame. */
void zx_tape_motor(zx_t *z);
/* The font the ROM prints with (CHARS + 256), 768 bytes. */
const uint8_t *zx_font(const zx_t *z);
#endif
