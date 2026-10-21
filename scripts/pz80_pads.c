/* pz80_pads.c - which of perfectz80's output pads the die drives.

   Every output pad of the netlist has a set of pull-ups to vcc gated by one
   internal node and a set of pull-downs to vss gated by another: the pad
   is driven while either gate is high and floats while both are low (the
   address bus during a bus grant, the data bus except in writes). The
   pads' gate nodes are found from the transistor list at start-up. */
#include <stdio.h>
#include <stdlib.h>
/* perfectz80.c defines the netlist's arrays; this file takes its own copy
   under other names, for the node numbers and the transistor list */
#define netlist_z80_node_is_pullup pz80_pads_node_is_pullup
#define netlist_z80_transdefs pz80_pads_transdefs
#include "refs/perfectz80/types.h"
#include "refs/perfectz80/netlist_z80.h"
#include "refs/perfectz80/netlist_sim.h"
#include "pz80_pads.h"

static const nodenum_t pads[32] = {
    ab0, ab1, ab2, ab3, ab4, ab5, ab6, ab7, ab8, ab9, ab10, ab11, ab12, ab13, ab14, ab15,
    db0, db1, db2, db3, db4, db5, db6, db7,
    _mreq, _iorq, _rd, _wr, _m1, _rfsh, _halt, _busak,
};
static nodenum_t up[32], down[32];   /* the gate nodes of each pad's pull-up / pull-down */

void pz80_pads_init(void) {
    int nt = (int)(sizeof pz80_pads_transdefs / sizeof *pz80_pads_transdefs);
    for (int p = 0; p < 32; p++) {
        up[p] = down[p] = 0;
        for (int i = 0; i < nt; i++) {
            const netlist_transdefs *t = &pz80_pads_transdefs[i];
            int other = t->c1 == pads[p] ? t->c2 : t->c2 == pads[p] ? t->c1 : -1;
            nodenum_t *g = other == vcc ? &up[p] : other == vss ? &down[p] : NULL;
            if (!g) continue;
            if (*g && *g != t->gate) {
                fprintf(stderr, "pz80_pads: pad node %d has two %s gates\n", pads[p], other == vcc ? "pull-up" : "pull-down");
                exit(2);
            }
            *g = (nodenum_t)t->gate;
        }
        if (!up[p] || !down[p]) {
            fprintf(stderr, "pz80_pads: pad node %d has no push-pull driver\n", pads[p]);
            exit(2);
        }
    }
}

static unsigned driven(void *st, int first, int n) {
    unsigned m = 0;
    for (int i = 0; i < n; i++)
        if (isNodeHigh(st, up[first + i]) || isNodeHigh(st, down[first + i])) m |= 1u << i;
    return m;
}

unsigned pz80_addr_driven(void *st) { return driven(st, 0, 16); }
unsigned pz80_data_driven(void *st) { return driven(st, 16, 8); }
unsigned pz80_ctl_driven(void *st)  { return driven(st, 24, 8); }
