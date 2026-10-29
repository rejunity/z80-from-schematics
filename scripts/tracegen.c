/* ============================================================================
 * tracegen.c - C-model trace generator. Loads a hex program, runs N phases,
 * emits the shared bus-cycle trace (docs/timing.md) to stdout. Compared
 * against the RTL testbench trace by scripts/compare_traces.py, and against
 * the perfectz80 gate-level netlist by scripts/compare_signal_timing.py.
 *
 *   tracegen <prog.hex> <num_phases> [events_file|-] [machine_file]
 *
 * Program file: whitespace-separated 2-digit hex bytes; optional @HEX sets
 * the load address (same convention as Verilog $readmemh).
 *
 * Events file (optional, shared with perfectz80_runner / sim_main.cpp via
 * the SAME parser): per-line `<phase> <pin> <value>` events. Pin tokens:
 *   nmi  int  wait  busreq  reset
 * Value is 0 (active-low asserted) or 1 (deasserted). Lines starting with
 * '#' are comments; blank lines ignored. See docs/test-expansion-plan.md
 * for the full convention.
 *
 * Machine file (optional, `<prog>.machine`): the machine around the CPU,
 * the same model as scripts/perfectz80_runner.c (see MACHINE there) and
 * tests/iverilog/tb_z80*.v.
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "z80_sim.h"

/* ---- Shared pin-event sidecar parser. Compatible with the same .events
 * format consumed by scripts/perfectz80_runner.c so a single sidecar file
 * drives both harnesses identically. (Duplicated rather than placed in a
 * new header per the project's no-new-.h-files rule.) ---- */
typedef struct { int phase; int pin; int value; } pinevent_t;
static pinevent_t s_events[1024];
static int        s_n_events = 0;

static int events_load(const char *path) {
    if (!path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    static const char *names[] = {"nmi","int","wait","busreq","reset"};
    while (fgets(line, sizeof line, f) && s_n_events < 1024) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == 0) continue;
        int phase, value;
        char pin[16];
        if (sscanf(line, "%d %15s %d", &phase, pin, &value) != 3) continue;
        int idx = -1;
        for (int i = 0; i < 5; i++) if (!strcmp(pin, names[i])) { idx = i; break; }
        if (idx < 0) { fprintf(stderr,"events: unknown pin '%s'\n", pin); continue; }
        s_events[s_n_events++] = (pinevent_t){phase, idx, value};
    }
    fclose(f);
    return s_n_events;
}

static void events_apply(z80_pins_t *p, int phase) {
    for (int i = 0; i < s_n_events; i++) {
        if (s_events[i].phase != phase) continue;
        int v = s_events[i].value;
        switch (s_events[i].pin) {
        case 0: p->nmi_n    = v ? 1 : 0; break;
        case 1: p->int_n    = v ? 1 : 0; break;
        case 2: p->wait_n   = v ? 1 : 0; break;
        case 3: p->busreq_n = v ? 1 : 0; break;
        case 4: p->reset_n  = v ? 1 : 0; break;
        }
    }
}

/* ---- The machine around the CPU (<prog>.machine): the same model as
 * scripts/perfectz80_runner.c, which describes it (duplicated, as the
 * events parser is). ---- */
typedef struct { int kind, nmi; long t0; } machine_t;   /* kind 0 none, 1 spectrum48, 2 zx81 */
static machine_t M;
static int  m_hold;             /* clock edges still held off (CLK high) */
static int  m_int = 1, m_wait = 1, m_nmi = 1;

static void machine_load(const char *path) {
    FILE *f = path ? fopen(path, "r") : NULL;
    char line[256], kind[32];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || sscanf(line, "%31s", kind) != 1) continue;
        if (!strcmp(kind, "spectrum48") && sscanf(line, "%*s %ld", &M.t0) == 1) M.kind = 1;
        else if (!strcmp(kind, "zx81") && sscanf(line, "%*s %d %ld", &M.nmi, &M.t0) == 2) M.kind = 2;
        else { fprintf(stderr, "machine: bad line '%s'\n", line); exit(2); }
        break;
    }
    fclose(f);
}
static int ula_delay(long t) {
    static const int pat[8] = {6, 5, 4, 3, 2, 1, 0, 0};
    long lp = ((t % 224) + 224) % 224;
    return lp < 128 ? pat[lp % 8] : 0;
}
static int zx81_nmi_low(long t) { long p = ((t % 207) + 207) % 207; return p >= 16 && p < 32; }
static unsigned ula_byte(int s) { return (0xA5u + 0x3Bu * (unsigned)s) & 0xFFu; }
static void machine_pre(int s) {
    if (M.kind == 2 && M.nmi && !(s & 1)) m_nmi = !zx81_nmi_low(s / 2 + M.t0);
}
static int  m_iorq_was = 1;     /* IORQ at the start of the last T-state */
static void machine_post(int s, unsigned a, int m1, int mreq, int iorq, int halt) {
    if (M.kind == 1 && !(s & 1)) {
        int port = !iorq && m1 && !(a & 1);                        /* the ULA's own */
        int contended = ((a & 0xC000) == 0x4000 && mreq && !port)  /* no access under way */
                     || (port && m_iorq_was);                      /* its T2 */
        if (m_hold == 0 && contended) m_hold = 2 * ula_delay(s / 2 + M.t0);
        m_iorq_was = iorq;
    }
    if (M.kind == 2) { m_int = (a >> 6) & 1; m_wait = !(!m_nmi && halt); }
}
static int zx81_nop(unsigned a, int m1, int mreq, int rd, int halt, unsigned byte) {
    return M.kind == 2 && !m1 && !mreq && !rd && (a & 0x8000) && halt && !(byte & 0x40);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <prog.hex> <num_phases> [nmi_phase | events_file]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    int phases = atoi(argv[2]);
    /* Back-compat with the old `[nmi_phase]` argv: if argv[3] is an
       all-digits string, treat it as a single NMI pulse at that phase.
       Otherwise it's an events sidecar path. scripts/compare_traces.py
       still uses the integer form for prog8_nmi. */
    const char *events_path = NULL;
    int  nmi_phase   = -1;
    if (argc > 3) {
        const char *p = argv[3];
        int all_digits = (*p != 0);
        for (const char *q = p; *q; q++) if (*q < '0' || *q > '9') { all_digits = 0; break; }
        if (all_digits) nmi_phase = atoi(p);
        else if (strcmp(p, "-")) events_path = p;
    }
    machine_load(argc > 4 ? argv[4] : NULL);

    z80_system_t s;
    z80_sys_init(&s);

    unsigned addr = 0;
    char tok[64];
    while (fscanf(f, "%63s", tok) == 1) {
        if (tok[0] == '@') {
            addr = (unsigned)strtoul(tok + 1, NULL, 16) & 0xFFFF;
        } else if (tok[0] == '/' || tok[0] == '#' || tok[0] == ';') {
            int ch; while ((ch = fgetc(f)) != '\n' && ch != EOF) {} /* comment line */
        } else {
            unsigned v = (unsigned)strtoul(tok, NULL, 16);
            s.mem[addr & 0xFFFF] = (uint8_t)v;
            addr++;
        }
    }
    fclose(f);

    int n_ev = events_load(events_path);
    if (events_path)
        fprintf(stderr, "tracegen: loaded %d event(s) from %s\n", n_ev, events_path);

    z80_trace_header(stdout);
    for (int i = 0; i < phases; i++) {
        /* legacy NMI-phase shorthand kept side-by-side with the events
           sidecar — if both are given the sidecar wins, but
           compare_traces.py only ever passes one or the other. */
        if (nmi_phase >= 0) s.cpu.pins.nmi_n = (i == nmi_phase) ? 0 : 1;
        z80_pins_t *pn = &s.cpu.pins;
        if (M.kind) {
            machine_pre(i);
            if (M.kind == 2) { pn->int_n = m_int; pn->wait_n = m_wait; pn->nmi_n = m_nmi; }
        }
        events_apply(pn, i);
        /* the edge into line i, unless the ULA holds CLK high: then the
           line has no edge and the data bus carries the ULA's byte */
        if (m_hold > 0) { m_hold--; pn->data_in = (uint8_t)ula_byte(i); }
        else z80_sys_phase(&s);
        if (zx81_nop(pn->addr, pn->m1_n, pn->mreq_n, pn->rd_n, pn->halt_n, s.mem[pn->addr]))
            pn->data_in = 0;
        if (M.kind) machine_post(i, pn->addr, pn->m1_n, pn->mreq_n, pn->iorq_n, pn->halt_n);
        z80_trace_rec_t r;
        z80_trace_capture(&s.cpu, &r);
        z80_trace_emit(stdout, &r);
    }
    return 0;
}
