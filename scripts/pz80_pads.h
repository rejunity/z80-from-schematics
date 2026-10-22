/* pz80_pads.h - which of perfectz80's output pads the die drives
   (scripts/pz80_pads.c). */
#ifndef PZ80_PADS_H
#define PZ80_PADS_H
void pz80_pads_init(void);
/* bit i set: the pad is driven (its pull-up or its pull-down is on) */
unsigned pz80_addr_driven(void *state);   /* A0..A15 */
unsigned pz80_data_driven(void *state);   /* D0..D7 */
unsigned pz80_ctl_driven(void *state);    /* MREQ IORQ RD WR M1 RFSH HALT BUSAK, bit 0 first */
#endif
