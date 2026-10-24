/* event_sweep_redcode.c - one event-sweep case (scripts/event_sweep.py)
   through redcode/Z80, the reference emulator in scripts/refs/redcode_z80
   (Zilog NMOS; make redcode_sweep), for scripts/redcode_sweep.py to compare
   with perfectz80's recorded trace of the same case.

   Usage: event_sweep_redcode [-e events] [-r run_T] [-m marker] [-i]
                              [-a int_at] [-v vec] [-o ops] [-x drop]
                              [-M die_samples] [-I] prog.hex

   -I: every I/O read gives 00 (perfectz80_runner's bus, for the trace
   programs and pin scenarios); otherwise an IN reads what an OUT wrote.

   The options and the interrupting device are event_sweep_c.c's (-H, -B,
   -W and -D are accepted and ignored: WAIT, BUSREQ and the data window are
   timing redcode has no notion of). redcode runs a whole instruction, or
   interrupt response, per z80_run(cpu, 1) call -- a DD / FD prefix is one
   call of its own, the instruction it starts the next -- so this harness
   is the system an instruction-level emulator lives in: it keeps time in
   T-states (from the marker fetch's T1, as the pulses are given: sixteenths
   of a T-state) and presents the pins as the die samples them:

   - INT: low or not at the rising edge that starts an instruction's last
     T-state (z80_int before redcode decides at the boundary).
   - NMI: the die's NMI flip-flop is set by the pin's falling edge; an edge
     at or before that same rising edge is taken there (z80_nmi).
   - RESET: sampled at every rising edge. Seen in the T2 of an M1 cycle and
     gone at the next edge, it is the special reset (z80_special_reset).
     Otherwise the die finishes the T-state it saw it in and idles; redcode
     cannot stop inside an instruction, so the one cut there either runs to
     its end (default) or is undone (-x drop), then z80_instant_reset, and
     the next fetch, at 0000, is on the third rising edge after RESET goes.
   - -M: the die's instructions (scripts/redcode_sweep.py takes them from
     the recorded trace where WAIT or BUSREQ stretch the die's time): per
     line the T-state one starts in, the one whose rising edge samples INT /
     NMI at its end, and the one the next starts in. The instruction redcode
     runs from the same T-state ends where the die's does.

   Prints one line per bus transaction from the marker fetch on: the T-state
   of its M-cycle's T1 (exact for M1-type cycles, the instruction's start
   for the others), and F (opcode fetch, an NMI's discarded fetch, a HALT
   NOP), A (INTA), Q (the refresh after either: I and R), R / W (memory),
   I / O (I/O), with the address and data;
   'X k r t' where a RESET is seen at T-state k in the instruction that
   started in T-state t, and the fetch at 0000 comes in T-state r; 'U t' at
   each instruction boundary (redcode's end of a call), and 'S t f wz q':
   F, WZ and Q there, for the M1 that starts in T-state t. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "Z80.h"

enum { P_NMI, P_INT, P_WAIT, P_BUSREQ, P_RESET };
typedef struct { int pin, lo, hi; } pulse_t;
static pulse_t pulses[8];
static int n_pulses;
static long long nmi_fall[16];   /* the NMI wire's falling edges, and whether given to redcode */
static int n_nmi, nmi_done[16];

static zuint8 mem[0x10000], io[0x10000];
static Z80 cpu;
static int marker = 0x0200, inta_release = 0, int_at = -1, vec = 0xff, n_ops = 0, drop = 0, in_zero = 0;
static zuint8 ops[8];
static long die_s[8192], die_e[8192], die_n[8192];
static int n_die = 0;

static long long T;           /* T-states since the start, at the current call's start */
static long long t_mark = -1; /* the marker fetch's T1 */
static long long m1_off;      /* T-states of M1-type cycles so far in this call */
static long long m1_at[16];   /* their starts (absolute), for the special reset */
static int n_m1;
static int dev_pos = 0;
static long long int_at_from = -1, released = -1;   /* sixteenths from the marker */

/* the transactions of the current call, printed once the call stands */
typedef struct { long long t; char k; unsigned a, d; } tr_t;
static tr_t buf[256];
static int n_buf;

static long long rel(long long abs_t) { return abs_t - t_mark; }

static void tr(char k, unsigned a, unsigned d, long long t) {
    if (t_mark < 0 || n_buf >= 256) return;
    buf[n_buf].t = rel(t); buf[n_buf].k = k; buf[n_buf].a = a & 0xffff; buf[n_buf].d = d & 0xff;
    n_buf++;
}

static void refresh(long long t) {       /* the refresh in an M1's T3 / T4: I and R */
    /* redcode has stepped R for this M1 already; the die steps it at the
       refresh's end, so the refresh shows the value before */
    unsigned r = ((cpu.r - 1) & 0x7f) | (cpu.r7 & 0x80);
    tr('Q', ((unsigned)cpu.i << 8) | r, 0, t + 2);
}
static long long m1(int len) {            /* an M1-type cycle starts: its T1 */
    long long t = T + m1_off;
    if (n_m1 < 16) m1_at[n_m1++] = t;
    m1_off += len;
    return t;
}

static zuint8 fetch_opcode(void *u, zuint16 a) {
    (void)u;
    long long t = m1(4);
    if (t_mark < 0 && a == marker) t_mark = t;
    tr('F', a, mem[a], t);
    refresh(t);
    dev_pos = 0;                                    /* an opcode from memory */
    if (int_at >= 0 && a == int_at && t_mark >= 0) int_at_from = 16 * rel(t) + 8;
    return mem[a];
}
static zuint8 fetch(void *u, zuint16 a) { (void)u; tr('R', a, mem[a], T); return mem[a]; }
static zuint8 rd(void *u, zuint16 a) { (void)u; tr('R', a, mem[a], T); return mem[a]; }
static void wr(void *u, zuint16 a, zuint8 v) { (void)u; mem[a] = v; tr('W', a, v, T); }
static zuint8 in_(void *u, zuint16 a) {        /* -I: every read 00, as perfectz80_runner */
    (void)u;
    zuint8 b = in_zero ? 0 : io[a];
    tr('I', a, b, T);
    return b;
}
static void out_(void *u, zuint16 a, zuint8 v) { (void)u; io[a] = v; tr('O', a, v, T); }
static zuint8 nop(void *u, zuint16 a) {         /* a HALT NOP, the special reset's NOP */
    (void)u;
    long long t = m1(4);
    tr('F', a, mem[a], t);
    refresh(t);
    return mem[a];
}
static int in_nmi;              /* this call is an NMI response */
static zuint8 nmia(void *u, zuint16 a) {        /* the NMI response's fetch, 5 T */
    (void)u;
    in_nmi = 1;
    long long t = m1(5);
    tr('F', a, mem[a], t);
    refresh(t);
    dev_pos = 0;
    return mem[a];
}
static zuint8 inta(void *u, zuint16 a) {        /* an INTA cycle: 4 T + 2 wait states */
    (void)u;
    long long t = m1(6);
    zuint8 b = dev_pos == 0 ? (zuint8)vec : dev_pos <= n_ops ? ops[dev_pos - 1] : 0xff;
    dev_pos++;
    tr('A', a, b, t);
    refresh(t + 2);                             /* after the two wait states */
    /* -i, -a: the device drives INT high at the start of each half-cycle
       with M1 and IORQ low (T3.N to the 2nd wait state's end): a pulse that
       falls before the last of those halves is lost too */
    if ((inta_release || int_at >= 0) && t_mark >= 0) released = 16 * rel(t) + 40;
    return b;
}
static zuint8 int_fetch(void *u, zuint16 a) {   /* IM0: an operand from the device */
    (void)u;
    zuint8 b = mem[a];
    if (dev_pos > 0 && dev_pos <= n_ops) b = ops[dev_pos++ - 1];
    tr('R', a, b, T);
    return b;
}

static void load_hex(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(2); }
    unsigned addr = 0; char tok[64];
    while (fscanf(f, "%63s", tok) == 1) {
        if (tok[0] == '@') addr = (unsigned)strtoul(tok + 1, NULL, 16) & 0xffff;
        else if (tok[0] == '/' || tok[0] == '#' || tok[0] == ';') {   /* a comment: the rest of the line */
            int ch; while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
        } else mem[addr++ & 0xffff] = (zuint8)strtoul(tok, NULL, 16);
    }
    fclose(f);
}

/* A pin is one wire: the pulses' edges in time order, the last one before
   a moment sets its level there (two pulses that overlap: the pin goes high
   when the first ends, as the die's harness drives it). The level a rising
   edge at sixteenth s (from the marker) sees: a transition exactly on the
   edge counts after it, as in the die's runs. */
typedef struct { long long t; int low; } edge_t;
static int edges(int pin, edge_t *e) {         /* the pin's transitions, in order */
    int n = 0;
    for (int i = 0; i < n_pulses; i++)
        if (pulses[i].pin == pin) {
            e[n].t = pulses[i].lo; e[n++].low = 1;
            if (pulses[i].hi >= 0) { e[n].t = pulses[i].hi; e[n++].low = 0; }
        }
    if (pin == P_INT) {                         /* the system's own: -a and -i */
        if (int_at_from >= 0) { e[n].t = int_at_from; e[n++].low = 1; }
        if (released >= 0) {
            e[n].t = released; e[n++].low = 0;
            e[n].t = released + 15; e[n++].low = 0;   /* a fall before its last half is lost */
        }
    }
    for (int i = 1; i < n; i++)                 /* by time, a fall before a rise at one moment */
        for (int j = i; j > 0 && (e[j].t < e[j - 1].t || (e[j].t == e[j - 1].t && e[j].low > e[j - 1].low)); j--) {
            edge_t x = e[j]; e[j] = e[j - 1]; e[j - 1] = x;
        }
    return n;
}
static int pin_low(int pin, long long s) {
    edge_t e[24];
    int n = edges(pin, e), low = 0;
    for (int i = 0; i < n && e[i].t < s; i++) low = e[i].low;
    return low;
}
static int int_low(long long s) { return pin_low(P_INT, s); }

int main(int argc, char **argv) {
    const char *events = NULL, *samples = NULL;
    int run = 100, opt;
    while ((opt = getopt(argc, argv, "e:r:m:ia:H:T:v:o:BWD:x:M:I")) != -1) {
        switch (opt) {
        case 'e': events = optarg; break;
        case 'r': run = atoi(optarg); break;
        case 'm': marker = (int)strtol(optarg, NULL, 16); break;
        case 'i': inta_release = 1; break;
        case 'a': int_at = (int)strtol(optarg, NULL, 16); break;
        case 'v': vec = (int)strtol(optarg, NULL, 16) & 0xff; break;
        case 'o':
            for (n_ops = 0; n_ops < 8 && optarg[2 * n_ops] && optarg[2 * n_ops + 1]; n_ops++) {
                char b[3] = {optarg[2 * n_ops], optarg[2 * n_ops + 1], 0};
                ops[n_ops] = (zuint8)strtol(b, NULL, 16);
            }
            break;
        case 'x': drop = !strcmp(optarg, "drop"); break;
        case 'M': samples = optarg; break;
        case 'I': in_zero = 1; break;
        case 'H': case 'T': case 'B': case 'W': case 'D': break;
        default: fprintf(stderr, "bad option\n"); return 2;
        }
    }
    if (optind >= argc) { fprintf(stderr, "usage: %s [options] prog.hex\n", argv[0]); return 2; }
    memset(io, 0xff, sizeof io);
    load_hex(argv[optind]);
    if (events) {
        FILE *f = fopen(events, "r"); if (!f) { perror(events); return 2; }
        while (n_pulses < 8 && fscanf(f, "%d %d %d", &pulses[n_pulses].pin,
                                      &pulses[n_pulses].lo, &pulses[n_pulses].hi) == 3) n_pulses++;
        fclose(f);
    }
    {                                           /* the NMI wire's falling edges */
        edge_t e[24];
        int n = edges(P_NMI, e), low = 0;
        for (int i = 0; i < n; i++) {
            if (e[i].low && !low && n_nmi < 16) nmi_fall[n_nmi++] = e[i].t;
            low = e[i].low;
        }
    }
    if (samples) {
        FILE *f = fopen(samples, "r"); if (!f) { perror(samples); return 2; }
        while (n_die < 8192 && fscanf(f, "%ld %ld %ld", &die_s[n_die], &die_e[n_die], &die_n[n_die]) == 3) n_die++;
        fclose(f);
    }

    cpu.fetch_opcode = fetch_opcode; cpu.fetch = fetch; cpu.read = rd; cpu.write = wr;
    cpu.in = in_; cpu.out = out_; cpu.nop = nop; cpu.nmia = nmia; cpu.inta = inta;
    cpu.int_fetch = int_fetch;
    cpu.options = Z80_MODEL_ZILOG_NMOS;
    z80_power(&cpu, 1);
    /* the die's power-on registers (perfectz80's netlist boots with every
       register pair 5555; PC, I, R, the IFFs and IM are cleared) */
    cpu.af.uint16_value = cpu.bc.uint16_value = cpu.de.uint16_value = cpu.hl.uint16_value = 0x5555;
    cpu.af_.uint16_value = cpu.bc_.uint16_value = cpu.de_.uint16_value = cpu.hl_.uint16_value = 0x5555;
    cpu.ix_iy[0].uint16_value = cpu.ix_iy[1].uint16_value = 0x5555;
    cpu.sp.uint16_value = cpu.memptr.uint16_value = 0x5555;

    static zuint8 mem_save[0x10000], io_save[0x10000];
    Z80 cpu_save;
    long long instr_t0 = -1;   /* where the instruction redcode is in started (for -M) */
    for (int calls = 0; calls < 100000; calls++) {
        int was_marked = t_mark >= 0;
        if (was_marked) { memcpy(mem_save, mem, sizeof mem); memcpy(io_save, io, sizeof io); cpu_save = cpu; }
        m1_off = 0; n_m1 = 0; n_buf = 0; in_nmi = 0;
        long long t0 = T;
        zusize c = z80_run(&cpu, 1);
        T += (long long)c;
        if (t_mark < 0) continue;
        /* after a DD / FD prefix (or one an IM0 device gave) the instruction goes on in
           the next call: no boundary yet. A HALT and each of its NOPs end one. */
        int midway = cpu.resume == Z80_RESUME_XY || cpu.resume == Z80_RESUME_IM0_XY;
        long long r0 = rel(t0), r1 = rel(T);  /* this call's T-states: r0 .. r1-1 */
        if (instr_t0 < 0) instr_t0 = r0;
        int die_i = -1;
        if (n_die && !midway)
            for (int i = 0; i < n_die; i++) if (die_s[i] == instr_t0) { die_i = i; break; }
        if (die_i >= 0) {
            /* -M: the die's time. Shift this call's end (and so the next
               start) to where the die starts the next instruction. */
            long long shift = die_n[die_i] - r1;
            T += shift; r1 += shift;
        }

        /* RESET: the first rising edge in this call that sees it */
        long long k_entry = -1, k_special = -1;
        if (was_marked || r0 >= 0)
            for (long long k = (r0 < 0 ? 0 : r0); k < r1 && k_entry < 0; k++) {
                if (!pin_low(P_RESET, 16 * k)) continue;
                int t2 = 0;
                for (int i = 0; i < n_m1; i++) if (rel(m1_at[i]) + 1 == k) t2 = 1;
                if (t2 && !pin_low(P_RESET, 16 * (k + 1))) { k_special = k; continue; }
                k_entry = t2 ? k + 1 : k;
            }
        if (k_entry >= 0 && k_entry < r1 - 1 && drop) {
            memcpy(mem, mem_save, sizeof mem); memcpy(io, io_save, sizeof io); cpu = cpu_save;
            n_buf = 0;
        }
        for (int i = 0; i < n_buf; i++) printf("%lld %c %04x %02x\n", buf[i].t, buf[i].k, buf[i].a, buf[i].d);
        if (k_entry >= 0) {
            long long k = k_entry;
            while (k < 16 * 4096 && pin_low(P_RESET, 16 * k)) k++;
            long long restart = k + 2;        /* the 3rd rising edge after the release */
            printf("X %lld %lld %lld\n", k_entry, restart, r0);
            z80_instant_reset(&cpu);
            if (k >= 16 * 4096) break;        /* never released */
            T = t_mark + restart;
            n_buf = 0;
            instr_t0 = -1;
            for (int i = 0; i < n_nmi; i++)   /* the die keeps no NMI edge from before the */
                if (nmi_fall[i] < 16 * (k + 1))     /* 2nd rising edge it sees RESET high on */
                    nmi_done[i] = 1;
            if (r1 > run + 40) break;
            continue;
        }
        if (k_special >= 0) z80_special_reset(&cpu);
        if (midway) continue;
        printf("U %lld\n", r1);
        /* the state the instruction leaves, as the next M1 meets it */
        printf("S %lld %02x %04x %02x\n", r1, cpu.af.uint8_values.at_0, cpu.memptr.uint16_value, cpu.q);
        long long e = die_i >= 0 ? die_e[die_i] : r1 - 1;
        instr_t0 = -1;
        long long s = 16 * e;                 /* the rising edge that starts the last T-state */
        z80_int(&cpu, int_low(s));
        /* an edge before that rising edge is taken at this boundary; one
           inside an NMI response, the die loses: it goes to redcode right
           after the response, where redcode rejects a second NMI */
        for (int i = 0; i < n_nmi; i++)
            if (!nmi_done[i] && (nmi_fall[i] < s || (in_nmi && nmi_fall[i] < 16 * r1))) {
                nmi_done[i] = 1;
                z80_nmi(&cpu);
            }
        if (r1 > run + 40) break;
    }
    if (t_mark < 0) printf("marker not reached\n");
    return 0;
}
