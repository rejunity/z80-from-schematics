/* ============================================================================
 * pinhash.c - phase-identity probe: runs a program through the C model and
 * folds every phase's pins into a rolling hash, printed at checkpoints with
 * the register file. Two builds of the model (e.g. a reference commit and the
 * work tree) are phase-identical on a run when their outputs are identical;
 * scripts/phase_identity.sh builds both and diffs them.
 *
 *   pinhash [options] <prog>
 *
 * Programs:
 *   *.com      CP/M: loaded at 0x0100, BDOS console at 0x0005, exit when PC
 *              reaches 0x0000 at an instruction boundary (as zexrunner;
 *              not under -r, where RESET and interrupts lead there too)
 *   *.tap      z80test: CODE block at its load address, RST 10 / CHAN-OPEN
 *              stubs, Spectrum ULA port idle pattern (as z80test_runner)
 *   *.hex      tracegen format, run from PC=0; <prog>.events applies if it
 *              exists, *nmi* programs get compare_traces.py's NMI at phase 30
 *   rand:SEED  64K of random memory and I/O space, run from reset
 *
 * Options:
 *   -n N       stop after N phases (default: program end; 400 for .hex)
 *   -c N       checkpoint every N phases (default 1000000)
 *   -e FILE    pin events sidecar (tracegen's `<phase> <pin> <value>`)
 *   -r SEED[:SCALE]  random INT / NMI / WAIT / BUSREQ / RESET activity, its
 *              mean intervals scaled by SCALE (default 1)
 *   -v         drive a random byte during interrupt acknowledge (M1+IORQ);
 *              the library's z80_sys_phase drives 0 there
 *   -b         random bus: every memory / I/O read and interrupt acknowledge
 *              returns a random byte, so the core decodes a random stream
 *              (own and tick modes)
 *   -m MODE    sys:  the library's z80_sys_phase (memory + I/O model)
 *              own:  this file's bus model around z80_phase_step
 *              tick: this file's bus model around z80_tick (pin word API)
 *              fast: the same around cfast's z80f_tick (built -DZ80_FAST)
 *              run:  the same inlined in cfast's z80f_run (-DZ80_FAST_RUN;
 *                    with -DZ80_SINGLE from the one-header build)
 *   -d A:B     dump the trace lines (tracegen format) of phases [A, B)
 *   -x         leave the pad drivers out of the hash (a reference without)
 *   -q         don't print the program's console output
 *
 * A phase's hash word holds the 14 trace columns compare_traces.py checks
 * against the RTL (t phi m addr data_o data_i mreq iorq rd wr m1 rfsh halt
 * busack) plus data_drive, and -- in a build against a model that has them,
 * -DPINHASH_DRIVES (the Makefile and phase_identity.py set it from
 * cmodel/z80.h) -- the die's other pad drivers, addr_drive and ctl_drive,
 * read from the model's pins in every front end. -DPINHASH_ARCH (set the
 * same way when cmodel/z80.h has z80_arch): the checkpoints' registers and
 * the console hooks' as the program sees them, the writes the model makes
 * in the next M1 made (docs/timing.md "Register write-back").
 * ==========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "z80_sim.h"
#if defined(Z80_FAST_RUN) && defined(Z80_SINGLE)
#define Z80_FAST
#define Z80_IMPL
#include "z80_cfast.h"           /* cfast's one-header build */
#elif defined(Z80_FAST_RUN)
#define Z80_FAST
#include "z80_fast.c"            /* cfast, compiled into this unit */
#elif defined(Z80_FAST)
#include "z80_fast.h"
#endif
#ifdef Z80_FAST
static z80f_t fast_core;
#endif
#ifdef PINHASH_ARCH
#define ARCH(c) z80_arch(c)
#else
#define ARCH(c) (c)
#endif

enum { PROG_COM, PROG_TAP, PROG_HEX, PROG_RAND };
enum { MODE_SYS, MODE_OWN, MODE_TICK, MODE_FAST, MODE_RUN };

static z80_system_t *S;
static int      prog_kind;
static int      mode = MODE_SYS;
static int      inta_rand = 0;
static int      rand_bus = 0;
static int      quiet = 0;
static uint64_t seed_v = 0;

/* ---- deterministic PRNG (splitmix64) ---- */
static uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
static uint64_t rng_state;
static uint32_t rnd(void) { rng_state = mix64(rng_state); return (uint32_t)(rng_state >> 32); }

/* ---- pin events sidecar (same format as tracegen / perfectz80_runner) ---- */
typedef struct { uint64_t phase; int pin; int value; } pinevent_t;
static pinevent_t *events;
static int n_events, cap_events;
enum { PIN_NMI, PIN_INT, PIN_WAIT, PIN_BUSREQ, PIN_RESET, PIN_COUNT };
static const char *pin_names[PIN_COUNT] = { "nmi", "int", "wait", "busreq", "reset" };

static void events_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "pinhash: cannot open %s\n", path); exit(2); }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == 0) continue;
        unsigned long long phase; int value; char pin[16];
        if (sscanf(line, "%llu %15s %d", &phase, pin, &value) != 3) continue;
        int idx = -1;
        for (int i = 0; i < PIN_COUNT; i++) if (!strcmp(pin, pin_names[i])) idx = i;
        if (idx < 0) { fprintf(stderr, "pinhash: unknown pin '%s'\n", pin); continue; }
        if (n_events == cap_events) {
            cap_events = cap_events ? 2 * cap_events : 64;
            events = realloc(events, (size_t)cap_events * sizeof *events);
        }
        events[n_events++] = (pinevent_t){ (uint64_t)phase, idx, value };
    }
    fclose(f);
}

/* Input pin levels (1 = high = inactive), owned by this harness and copied
   into the core before every phase. */
static int pin_lvl[PIN_COUNT] = { 1, 1, 1, 1, 1 };

/* Random activity: mean phases between assertions and the longest hold. */
static int      rand_ev = 0;
static uint32_t rate[PIN_COUNT] = { 30000, 3000, 40, 25000, 4000000 };
static uint32_t hold[PIN_COUNT] = { 8, 200, 6, 60, 24 };
static uint32_t hold_left[PIN_COUNT];

static void random_events(void)
{
    for (int p = 0; p < PIN_COUNT; p++) {
        if (hold_left[p]) {
            if (--hold_left[p] == 0) pin_lvl[p] = 1;
        } else if (rnd() % rate[p] == 0) {
            pin_lvl[p] = 0;
            hold_left[p] = 1 + rnd() % hold[p];
        }
    }
}

static long long nmi_phase = -1;   /* compare_traces.py's 1-phase NMI pulse */

static void apply_inputs(uint64_t ph)
{
    if (rand_ev) random_events();
    for (int i = 0; i < n_events; i++)
        if (events[i].phase == ph) pin_lvl[events[i].pin] = events[i].value ? 1 : 0;
    if (nmi_phase >= 0) pin_lvl[PIN_NMI] = ((long long)ph == nmi_phase) ? 0 : 1;
    z80_pins_t *p = &S->cpu.pins;
    p->nmi_n    = pin_lvl[PIN_NMI];
    p->int_n    = pin_lvl[PIN_INT];
    p->wait_n   = pin_lvl[PIN_WAIT];
    p->busreq_n = pin_lvl[PIN_BUSREQ];
    p->reset_n  = pin_lvl[PIN_RESET];
}

/* a random byte that depends only on the seed and the phase */
static uint8_t bus_rnd(uint64_t ph)
{
    return (uint8_t)mix64(seed_v ^ (ph * 0x5851F42D4C957F2Dull));
}

/* the byte an interrupting device puts on the bus in an INTA cycle */
static uint8_t inta_byte(uint64_t ph)
{
    return inta_rand ? bus_rnd(ph) : 0;
}

/* ---- the bus model of z80_sys_phase, with the INTA byte added ---- */
static void own_phase(uint64_t ph)
{
    z80_t *c = &S->cpu;
    z80_phase_step(c);
    if (!c->pins.mreq_n && !c->pins.wr_n)
        S->mem[c->pins.addr] = c->pins.data_out;
    else if (!c->pins.iorq_n && !c->pins.wr_n)
        S->io[c->pins.addr] = c->pins.data_out;
    if (!c->pins.mreq_n && !c->pins.rd_n)
        c->pins.data_in = rand_bus ? bus_rnd(ph) : S->mem[c->pins.addr];
    else if (!c->pins.iorq_n && !c->pins.rd_n)
        c->pins.data_in = rand_bus ? bus_rnd(ph) : S->io_ula_idle
            ? ((c->pins.addr & 1) ? 0xFFu : 0xBFu) : S->io[c->pins.addr];
    else if (!c->pins.iorq_n && !c->pins.m1_n)
        c->pins.data_in = inta_byte(ph);
    else
        c->pins.data_in = 0;
}

/* ---- one phase's trace columns ---- */
typedef struct {
    uint8_t t, phi, m, data_o, data_i;
    uint16_t addr;
    uint8_t mreq, iorq, rd, wr, m1, rfsh, halt, busack, drive;
    uint8_t addr_drive, ctl_drive;
} cols_t;

static int no_drives;   /* -x */

/* the pad drivers, from the model's pins (cfast leaves them driven in its
   fast mode, which never runs in a bus grant or the reset idle) */
static void cols_drives(cols_t *k)
{
#ifdef PINHASH_DRIVES
    k->addr_drive = S->cpu.pins.addr_drive; k->ctl_drive = S->cpu.pins.ctl_drive;
#else
    k->addr_drive = k->ctl_drive = 0;
#endif
    if (no_drives) k->addr_drive = k->ctl_drive = 0;
}

static void cols_from_struct(cols_t *k)
{
    const z80_t *c = &S->cpu;
    k->t = c->t_state; k->phi = c->phi; k->m = c->m_cycle;
    k->addr = c->pins.addr; k->data_o = c->pins.data_out; k->data_i = c->pins.data_in;
    k->mreq = c->pins.mreq_n; k->iorq = c->pins.iorq_n; k->rd = c->pins.rd_n;
    k->wr = c->pins.wr_n; k->m1 = c->pins.m1_n; k->rfsh = c->pins.rfsh_n;
    k->halt = c->pins.halt_n; k->busack = c->pins.busack_n; k->drive = c->pins.data_drive;
    cols_drives(k);
}

#ifdef Z80_PIN_M1
/* ---- tick mode: the bus model above on the pin word ---- */
static uint64_t tick_pins;

static uint64_t tick_inputs(void)
{
    uint64_t p = tick_pins & ~(Z80_INT | Z80_NMI | Z80_WAIT | Z80_BUSREQ | Z80_RES);
    if (!pin_lvl[PIN_INT])    p |= Z80_INT;
    if (!pin_lvl[PIN_NMI])    p |= Z80_NMI;
    if (!pin_lvl[PIN_WAIT])   p |= Z80_WAIT;
    if (!pin_lvl[PIN_BUSREQ]) p |= Z80_BUSREQ;
    if (!pin_lvl[PIN_RESET])  p |= Z80_RES;
    return p;
}

/* the bus model on the pins a tick returned; sets D for the next phase */
static void tick_bus(uint64_t ph, uint64_t *pp, cols_t *k)
{
    uint64_t p = *pp;
    uint16_t a = Z80_GET_ADDR(p);
    const z80_t *c = &S->cpu;
    k->t = c->t_state; k->phi = c->phi; k->m = c->m_cycle;
    k->addr = a; k->data_o = Z80_GET_DATA(p);
    k->mreq = !(p & Z80_MREQ); k->iorq = !(p & Z80_IORQ); k->rd = !(p & Z80_RD);
    k->wr = !(p & Z80_WR); k->m1 = !(p & Z80_M1); k->rfsh = !(p & Z80_RFSH);
    k->halt = !(p & Z80_HALT); k->busack = !(p & Z80_BUSACK); k->drive = (p & Z80_DOE) != 0;
    cols_drives(k);
    uint8_t d;
    if ((p & (Z80_MREQ | Z80_WR)) == (Z80_MREQ | Z80_WR))
        S->mem[a] = Z80_GET_DATA(p);
    else if ((p & (Z80_IORQ | Z80_WR)) == (Z80_IORQ | Z80_WR))
        S->io[a] = Z80_GET_DATA(p);
    if ((p & (Z80_MREQ | Z80_RD)) == (Z80_MREQ | Z80_RD))
        d = rand_bus ? bus_rnd(ph) : S->mem[a];
    else if ((p & (Z80_IORQ | Z80_RD)) == (Z80_IORQ | Z80_RD))
        d = rand_bus ? bus_rnd(ph) : S->io_ula_idle ? ((a & 1) ? 0xFFu : 0xBFu) : S->io[a];
    else if ((p & (Z80_IORQ | Z80_M1)) == (Z80_IORQ | Z80_M1))
        d = inta_byte(ph);
    else
        d = 0;
    Z80_SET_DATA(p, d);
    k->data_i = d;
    *pp = tick_pins = p;
}

static void tick_phase(uint64_t ph, cols_t *k)
{
#ifdef Z80_FAST
    uint64_t p = (mode == MODE_FAST) ? z80f_tick(&fast_core, tick_inputs())
                                     : z80_tick(&S->cpu, tick_inputs());
#else
    uint64_t p = z80_tick(&S->cpu, tick_inputs());
#endif
    tick_bus(ph, &p, k);
}
#endif

static uint64_t hash_word(const cols_t *k)
{
    return (uint64_t)k->addr
         | (uint64_t)k->data_o << 16 | (uint64_t)k->data_i << 24
         | (uint64_t)(k->t & 0xF) << 32 | (uint64_t)(k->phi & 1) << 36
         | (uint64_t)k->m << 40
         | (uint64_t)(k->mreq & 1) << 48 | (uint64_t)(k->iorq & 1) << 49
         | (uint64_t)(k->rd & 1) << 50   | (uint64_t)(k->wr & 1) << 51
         | (uint64_t)(k->m1 & 1) << 52   | (uint64_t)(k->rfsh & 1) << 53
         | (uint64_t)(k->halt & 1) << 54 | (uint64_t)(k->busack & 1) << 55
         | (uint64_t)(k->drive & 1) << 56 | (uint64_t)(k->t >> 4) << 57
         | (uint64_t)(k->addr_drive & 1) << 58 | (uint64_t)(k->ctl_drive & 1) << 59;
}

static void dump_line(const cols_t *k)
{
    printf("%u %u %u %04x %02x %02x %u %u %u %u %u %u %u %u %u",
           k->t, k->phi, k->m, k->addr, k->data_o, k->data_i,
           k->mreq, k->iorq, k->rd, k->wr, k->m1, k->rfsh, k->halt, k->busack, k->drive);
#ifdef PINHASH_DRIVES
    if (!no_drives) printf(" %u %u", k->addr_drive, k->ctl_drive);   /* pad drivers */
#endif
    printf("\n");
}

static void checkpoint(char tag, uint64_t ph, uint64_t h)
{
    const z80_t *c = ARCH(&S->cpu);
    printf("%c %llu %016llx ic=%llu cy=%llu pc=%04x sp=%04x af=%04x bc=%04x de=%04x hl=%04x"
           " ix=%04x iy=%04x wz=%04x af'=%04x bc'=%04x de'=%04x hl'=%04x i=%02x r=%02x"
           " iff=%u%u im=%u halt=%u q=%02x\n",
           tag, (unsigned long long)ph, (unsigned long long)h,
           (unsigned long long)c->instr_count, (unsigned long long)c->cycle,
           c->rf[RFP_PC], c->rf[RFP_SP], c->rf[RFP_AF], c->rf[RFP_BC], c->rf[RFP_DE],
           c->rf[RFP_HL], c->rf[RFP_IX], c->rf[RFP_IY], c->rf[RFP_WZ], c->rf[RFP_AF2],
           c->rf[RFP_BC2], c->rf[RFP_DE2], c->rf[RFP_HL2], c->reg_i, c->reg_r,
           c->iff1, c->iff2, c->im, c->halted, c->q);
}

/* ---- program loaders ---- */
static void load_com(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "pinhash: cannot open %s\n", path); exit(2); }
    if (fread(&S->mem[0x0100], 1, 0x10000 - 0x0100, f) == 0) { fprintf(stderr, "pinhash: empty %s\n", path); exit(2); }
    fclose(f);
    S->mem[0x0000] = 0xC3; S->mem[0x0001] = 0x00; S->mem[0x0002] = 0x00; /* JP 0000 */
    S->mem[0x0005] = 0xC9;                                               /* RET */
    z80_set_pc(&S->cpu, 0x0100);
    S->cpu.rf[RFP_SP] = 0xFFFE;
}

static void load_tap(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "pinhash: cannot open %s\n", path); exit(2); }
    static uint8_t tap[1 << 20];
    size_t total = fread(tap, 1, sizeof tap, f);
    fclose(f);
    size_t off = 0; int found = 0; uint16_t code_addr = 0, start = 0;
    int loaded = 0;
    while (off + 2 < total && !loaded) {
        size_t blen = (size_t)tap[off] | ((size_t)tap[off + 1] << 8);
        if (off + 2 + blen > total || blen < 2) break;
        uint8_t flag = tap[off + 2];
        const uint8_t *body = &tap[off + 3];
        size_t body_len = blen - 2;
        if (flag == 0x00 && body_len >= 14) {
            if (body[0] == 3) { code_addr = (uint16_t)(body[13] | (body[14] << 8)); found = 1; }
        } else if (flag == 0xFF && found) {
            size_t copy = body_len;
            if ((size_t)code_addr + copy > 0x10000) copy = 0x10000 - code_addr;
            memcpy(&S->mem[code_addr], body, copy);
            start = code_addr; loaded = 1;
        }
        off += 2 + blen;
    }
    if (!loaded) { fprintf(stderr, "pinhash: no CODE block in %s\n", path); exit(2); }
    S->mem[0x0010] = 0xC9;
    S->mem[0x1601] = 0xC9;
    memset(S->io, 0xFF, sizeof S->io);
    S->io_ula_idle = 1;
    S->cpu.rf[RFP_SP] = 0xFFEE;
    S->mem[0xFFEE] = 0x00; S->mem[0xFFEF] = 0x00;
    z80_set_pc(&S->cpu, start);
}

static void load_hex(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "pinhash: cannot open %s\n", path); exit(2); }
    unsigned addr = 0; char tok[64];
    while (fscanf(f, "%63s", tok) == 1) {
        if (tok[0] == '@') addr = (unsigned)strtoul(tok + 1, NULL, 16) & 0xFFFF;
        else if (tok[0] == '/' || tok[0] == '#' || tok[0] == ';') {
            int ch; while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
        } else S->mem[addr++ & 0xFFFF] = (uint8_t)strtoul(tok, NULL, 16);
    }
    fclose(f);
}

static void load_rand(uint64_t seed)
{
    uint64_t x = seed * 0xD1342543DE82EF95ull + 1;
    for (int i = 0; i < 65536; i++) { x = mix64(x); S->mem[i] = (uint8_t)x; S->io[i] = (uint8_t)(x >> 8); }
}

static void console(void)
{
    if (quiet) return;
    const z80_t *c = &S->cpu;
    if (c->rf[RFP_PC] == 0x0005 || c->rf[RFP_PC] == 0x0010) c = ARCH(c);
    if (prog_kind == PROG_COM && c->rf[RFP_PC] == 0x0005) {
        uint8_t fn = (uint8_t)c->rf[RFP_BC];
        if (fn == 2) putc((int)(c->rf[RFP_DE] & 0xFF), stderr);
        else if (fn == 9) {
            uint16_t de = c->rf[RFP_DE];
            for (int g = 0; g < 65536 && S->mem[de] != '$'; g++) putc(S->mem[de++], stderr);
        }
    } else if (prog_kind == PROG_TAP && c->rf[RFP_PC] == 0x0010) {
        putc((int)(c->rf[RFP_AF] >> 8), stderr);
    }
}

/* the run: phases done, rolling hash, instructions seen */
static unsigned long long max_ph = 0, every = 1000000, dump_a = 1, dump_b = 0;
static uint64_t ph, h = 0x243F6A8885A308D3ull, last_ic;

/* after a phase: hash it, checkpoint, react to an instruction boundary;
   nonzero when the run is over */
static int phase_done(const cols_t *k)
{
    h = (h ^ hash_word(k)) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    if (ph >= dump_a && ph < dump_b) dump_line(k);
    ph++;
    if (ph % every == 0) checkpoint('C', ph, h);
    if (S->cpu.instr_count != last_ic) {        /* instruction boundary */
        last_ic = S->cpu.instr_count;
        /* program exit, unless random RESET / interrupts may land there */
        if ((prog_kind == PROG_COM || prog_kind == PROG_TAP) && !rand_ev &&
            S->cpu.rf[RFP_PC] == 0x0000)
            return 1;
        console();
    }
    return max_ph && ph >= max_ph;
}

#ifdef Z80_FAST_RUN
/* run mode: the bus model and the tail inlined between cfast's steps */
static int run_phase(uint64_t *pins)
{
    cols_t k;
    tick_bus(ph, pins, &k);
    if (phase_done(&k)) return 1;
    apply_inputs(ph);
    *pins = tick_inputs();
    return 0;
}
#define ZF_BUS(p)      do { ZF_SYNC_STATE(); if (run_phase(&(p))) zf_exit = 1; } while (0)
#define ZF_BOUNDARY(c) 0
#ifdef Z80_SINGLE
#include "z80_cfast.h"           /* again, with ZF_BUS: z80f_run */
#else
#include "z80_fast_run.h"
#endif
#endif

int main(int argc, char **argv)
{
    const char *ev_path = NULL, *prog = NULL;
    double scale = 1.0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : "";
        if      (!strcmp(a, "-n")) { max_ph = strtoull(v, NULL, 0); i++; }
        else if (!strcmp(a, "-c")) { every = strtoull(v, NULL, 0); i++; }
        else if (!strcmp(a, "-e")) { ev_path = v; i++; }
        else if (!strcmp(a, "-r")) {
            rand_ev = 1; seed_v = strtoull(v, NULL, 0);
            const char *colon = strchr(v, ':');
            if (colon) scale = atof(colon + 1);
            i++;
        }
        else if (!strcmp(a, "-v")) inta_rand = 1;
        else if (!strcmp(a, "-b")) rand_bus = 1;
        else if (!strcmp(a, "-q")) quiet = 1;
        else if (!strcmp(a, "-x")) no_drives = 1;
        else if (!strcmp(a, "-m")) {
            if      (!strcmp(v, "sys"))  mode = MODE_SYS;
            else if (!strcmp(v, "own"))  mode = MODE_OWN;
            else if (!strcmp(v, "tick")) mode = MODE_TICK;
            else if (!strcmp(v, "fast")) mode = MODE_FAST;
            else if (!strcmp(v, "run"))  mode = MODE_RUN;
            else { fprintf(stderr, "pinhash: unknown mode %s\n", v); return 2; }
            i++;
        }
        else if (!strcmp(a, "-d")) { sscanf(v, "%llu:%llu", &dump_a, &dump_b); i++; }
        else prog = a;
    }
    if (!prog) { fprintf(stderr, "usage: %s [options] <prog.com|.tap|.hex|rand:SEED>\n", argv[0]); return 2; }
    if (rand_bus && mode == MODE_SYS) { fprintf(stderr, "pinhash: -b needs -m own or tick\n"); return 2; }
#ifndef Z80_PIN_M1
    if (mode == MODE_TICK) { fprintf(stderr, "pinhash: this build has no z80_tick\n"); return 2; }
#endif
#ifndef Z80_FAST
    if (mode == MODE_FAST) { fprintf(stderr, "pinhash: built without cfast (-DZ80_FAST)\n"); return 2; }
#endif
#ifndef Z80_FAST_RUN
    if (mode == MODE_RUN) { fprintf(stderr, "pinhash: built without -DZ80_FAST_RUN\n"); return 2; }
#endif
    if (every == 0) every = 1000000;

    S = malloc(sizeof *S);
    z80_sys_init(S);
    size_t len = strlen(prog);
    if (!strncmp(prog, "rand:", 5)) { prog_kind = PROG_RAND; load_rand(strtoull(prog + 5, NULL, 0)); }
    else if (len > 4 && !strcmp(prog + len - 4, ".com")) { prog_kind = PROG_COM; load_com(prog); }
    else if (len > 4 && !strcmp(prog + len - 4, ".tap")) { prog_kind = PROG_TAP; load_tap(prog); }
    else {
        prog_kind = PROG_HEX; load_hex(prog);
        if (!ev_path && len > 4) {
            static char side[1024];
            snprintf(side, sizeof side, "%.*s.events", (int)(len - 4), prog);
            FILE *t = fopen(side, "r");
            if (t) { fclose(t); ev_path = side; }
            else if (strstr(prog, "nmi")) nmi_phase = 30;
        }
        if (!max_ph) max_ph = 400;
    }
    if (ev_path) events_load(ev_path);
    if (rand_ev) {
        rng_state = seed_v ^ 0xA0761D6478BD642Full;
        for (int p = 0; p < PIN_COUNT; p++) {
            double r = rate[p] * scale;
            rate[p] = r < 1 ? 1 : (uint32_t)r;
        }
    }

#ifdef Z80_FAST
    z80f_init(&fast_core, &S->cpu);
#endif
    printf("# pinhash %s\n", prog);
    last_ic = S->cpu.instr_count;
#ifdef Z80_FAST_RUN
    if (mode == MODE_RUN) {
        apply_inputs(0);
        z80f_run(&fast_core, tick_inputs());
    } else
#endif
    for (;;) {
        if (max_ph && ph >= max_ph) break;
        apply_inputs(ph);
        cols_t k;
        if (mode == MODE_SYS)      { z80_sys_phase(S); cols_from_struct(&k); }
        else if (mode == MODE_OWN) { own_phase(ph); cols_from_struct(&k); }
#ifdef Z80_PIN_M1
        else                       tick_phase(ph, &k);   /* tick, fast */
#endif
        if (phase_done(&k)) break;
    }
    checkpoint('E', ph, h);
#if defined(Z80_FAST) && defined(ZF_STATS)
    {
        extern unsigned long long zf_slow_ticks, zf_resumes;
        fprintf(stderr, "cfast: %llu of %llu phases slow, %llu resumptions\n",
                zf_slow_ticks, (unsigned long long)ph, zf_resumes);
    }
#endif
    free(S);
    return 0;
}
