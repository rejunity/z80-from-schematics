/* perfectz80_runner.c - run a .hex program through the perfectz80 gate-level
   Z80 simulator (Brian Silverman et al's transistor-level netlist port of the
   Visual Z80 die scan; MIT) and dump a per-half-cycle pin trace in the same
   shape as scripts/tracegen.c / tb_z80.v so timing can be compared.

   Output columns (tab-separated for legibility, space-separated as in
   tracegen):
     phase addr data_o data_i mreq iorq rd wr m1 rfsh halt

   Usage: perfectz80_runner <prog.hex> <num_phases> [events_file|-] [machine_file]

   Notes: perfectz80 is gate-level slow (~10k phases/s here). Keep `num_phases`
   small. Reset itself consumes several cycles internally inside
   cpu_initAndResetChip; the first dumped phase is the moment reset releases,
   so cycle counts align with our tracegen's "reset state" line.

   Events file (optional): per-line `<phase> <pin> <value>` events that drive
   the gate-level netlist's input pins via cpu_writeNMI / cpu_writeINT /
   cpu_writeWAIT / cpu_writeRESET / cpu_writeBUSRQ. This is the SAME format
   consumed by scripts/tracegen.c — a single sidecar exercises both
   harnesses identically.

   Machine file (optional, `<prog>.machine`, the same parser and model as
   scripts/tracegen.c and the iverilog testbenches): the logic of a
   machine built around the Z80, reacting to its pins every half-cycle --
   the ZX Spectrum 48K's ULA holding CLK high through contended accesses,
   the ZX80 / ZX81's display tricks and the ZX81's NMI and WAIT
   generators. See MACHINE below. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "refs/perfectz80/perfectz80.h"

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

static void events_apply(void *st, int phase) {
    for (int i = 0; i < s_n_events; i++) {
        if (s_events[i].phase != phase) continue;
        bool high = s_events[i].value ? true : false;
        switch (s_events[i].pin) {
        case 0: cpu_writeNMI  (st, high); break;
        case 1: cpu_writeINT  (st, high); break;
        case 2: cpu_writeWAIT (st, high); break;
        case 3: cpu_writeBUSRQ(st, high); break;
        case 4: cpu_writeRESET(st, high); break;
        }
    }
}

/* ---- The machine around the CPU (<prog>.machine), shared with
 * scripts/tracegen.c and tests/iverilog/tb_z80*.v (duplicated, as the
 * events parser is). One line, `#` comments:
 *
 *   spectrum48 <t>    ZX Spectrum 16K / 48K: the ULA holds CLK high
 *   zx81 <nmi> <t>    ZX80 / ZX81 (<nmi> 1: the ZX81's NMI generator on)
 *
 * <t> is the ULA's T-state count at trace line 0. Every half-cycle s
 * (trace line), from the pins after its edge, the machine sets the inputs
 * the next edge samples and decides whether that edge happens at all:
 *
 * spectrum48 -- at the start of a T-state (an even line, the clock just
 *   risen) the ULA holds CLK high for 6, 5, 4, 3, 2, 1, 0, 0 T-states by
 *   its T-state count within the first 128 of each 224-T line (none in
 *   the other 96: its display fetches) when the address on the bus is
 *   contended, 4000-7FFF, with MREQ high -- the first T-state of a memory
 *   access, every T-state of an internal or I/O cycle, the refresh's
 *   first -- or when an I/O cycle (IORQ low, M1 high) to an even port,
 *   the ULA's own, is in its first T-state with IORQ low (T2), its later
 *   ones then free. That is the 48K ULA's documented behaviour (the
 *   address alone, MREQ or not, outside the accesses it delays): pc:4,
 *   hl:3, hl:1 for each internal T-state, and the I/O ports' N:1 C:3,
 *   C:1 C:3, C:1 C:1 C:1 C:1, N:4 (worldofspectrum.org, "Contended memory"
 *   and the Spectrum FAQ's I/O contention table; en.wikipedia.org/wiki/
 *   Contended_memory; the 48K Service Manual: the ULA "inhibits the CPU
 *   clock"). Held lines are trace lines with no clock edge; the data bus
 *   carries the ULA's bytes there (a decoy that changes every line: no
 *   model may take it).
 * zx81 -- (CLK wiki "The ZX80 and ZX81", git.applefritter.com/6502/CLK)
 *   an opcode fetch (M1 MREQ RD low) from 8000-FFFF while HALT is high
 *   whose byte has bit 6 clear gets 00 (NOP) on the data bus, the byte
 *   going to the video circuit instead; INT is wired to A6 (an interrupt
 *   when the refresh address, I:R, has bit 6 clear); with <nmi> the NMI
 *   generator pulls NMI low at counts 16-31 of its 207-T line (with the
 *   horizontal sync), and WAIT is low while NMI is low and HALT high. ---- */
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
static int ula_delay(long t) {           /* T-states the 48K ULA holds CLK at its count t */
    static const int pat[8] = {6, 5, 4, 3, 2, 1, 0, 0};
    long lp = ((t % 224) + 224) % 224;
    return lp < 128 ? pat[lp % 8] : 0;
}
static int zx81_nmi_low(long t) { long p = ((t % 207) + 207) % 207; return p >= 16 && p < 32; }
static unsigned ula_byte(int s) { return (0xA5u + 0x3Bu * (unsigned)s) & 0xFFu; }
/* before line s: the generator's NMI (it changes with the clock's rise) */
static void machine_pre(int s) {
    if (M.kind == 2 && M.nmi && !(s & 1)) m_nmi = !zx81_nmi_low(s / 2 + M.t0);
}
/* after line s, from its pins: the inputs for line s + 1, and the hold */
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
/* the ZX80 / ZX81 forces an opcode fetch's byte to 00 */
static int zx81_nop(unsigned a, int m1, int mreq, int rd, int halt, unsigned byte) {
    return M.kind == 2 && !m1 && !mreq && !rd && (a & 0x8000) && halt && !(byte & 0x40);
}

static void load_hex(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); exit(2); }
    unsigned addr = 0; char tok[64];
    while (fscanf(f, "%63s", tok) == 1) {
        if (tok[0] == '@') {
            addr = (unsigned)strtoul(tok + 1, NULL, 16) & 0xFFFF;
        } else if (tok[0] == '/' || tok[0] == '#' || tok[0] == ';') {
            /* Skip the rest of the comment line. Matches the C tracegen
             * loader in scripts/tracegen.c so the same .hex file produces
             * identical memory images on both sides. */
            int ch; while ((ch = fgetc(f)) != '\n' && ch != EOF) {}
        } else {
            cpu_memory[addr & 0xFFFF] = (unsigned char)strtoul(tok, NULL, 16);
            addr++;
        }
    }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr,"usage: %s <prog.hex> <num_phases> [events_file|-] [machine_file]\n", argv[0]); return 2; }
    int phases = atoi(argv[2]);
    const char *events_path = (argc > 3 && strcmp(argv[3], "-")) ? argv[3] : NULL;
    machine_load(argc > 4 ? argv[4] : NULL);
    memset(cpu_memory, 0, sizeof cpu_memory);
    load_hex(argv[1]);
    int n_ev = events_load(events_path);
    if (events_path)
        fprintf(stderr, "perfectz80: loaded %d event(s) from %s\n", n_ev, events_path);

    void* st = cpu_initAndResetChip();
    /* After initAndReset, perfectz80 is past reset. We dump from here. */
    /* The interrupt-acknowledge byte: 00, the byte tracegen (z80_sim.c),
       tb_z80.v, tb_z80_netlist.v and the Verilator harness see whenever
       nothing is read (perfectz80's own default is E0), so an IM2 table
       read lands on {I, 00} in every harness. */
    cpu_setIntVec(0x00);
    /* Every input starts high and keeps the level of its last event, as
       in the C tracegen and the RTL testbenches (+<pin>_lo / +<pin>_hi):
       "30 wait 0" then "38 wait 1" holds WAIT low from phase 30 to 37. */
    cpu_writeNMI(st, true); cpu_writeINT(st, true);
    cpu_writeWAIT(st, true);
    int held = 0, nop_addr = -1;             /* line reached without an edge; the byte a NOP stands in for */
    uint8_t nop_byte = 0;
    for (int i = 0; i < phases; i++) {
        /* row i is the comparison's line s = i - 1 (PZ_OFFSET): the machine
           reacts to it, and what it sets is sampled entering line i */
        int s = i - 1;
        if (s >= 0 && M.kind) {
            unsigned a = cpu_readAddressBus(st);
            int m1 = cpu_readM1(st), mreq = cpu_readMREQ(st), rd = cpu_readRD(st), halt = cpu_readHALT(st);
            if (held) cpu_writeDataBus(st, (uint8_t)ula_byte(s));
            /* the NOP: in place of the byte while the fetch lasts, so
               perfectz80's bus puts 00 on after each rising edge in it */
            unsigned byte = (nop_addr == (int)a) ? nop_byte : cpu_memory[a & 0xFFFF];
            if (zx81_nop(a, m1, mreq, rd, halt, byte)) {
                if (nop_addr != (int)a) { nop_addr = (int)a; nop_byte = cpu_memory[a]; cpu_memory[a] = 0; }
            } else if (nop_addr >= 0) { cpu_memory[nop_addr] = nop_byte; nop_addr = -1; }
            machine_post(s, a, m1, mreq, cpu_readIORQ(st), halt);
        }
        if (M.kind) {
            machine_pre(i);
            if (M.kind == 2) { cpu_writeINT(st, m_int); cpu_writeWAIT(st, m_wait); cpu_writeNMI(st, m_nmi); }
        }
        events_apply(st, i);

        /* present read data from our memory just like the iverilog/Verilator
           harnesses do (so all three see the same bus). perfectz80's internal
           memory accessor is already wired to cpu_memory, so for memory cycles
           we don't have to drive the data bus ourselves; for I/O reads we
           drive 0 to match tracegen. */
        if (!cpu_readMREQ(st) && cpu_readRD(st) == 0) {
            /* nothing to do — perfectz80 handles its own mem read */
        }
        if (!cpu_readIORQ(st) && cpu_readRD(st) == 0) {
            cpu_writeDataBus(st, 0);
        }
        unsigned addr   = cpu_readAddressBus(st);
        unsigned data_o = cpu_readDataBus(st);
        /* data_i is the value that will be latched next; for memory reads we
           use cpu_memory (matches our harness). */
        unsigned data_i = 0;
        if (!cpu_readMREQ(st) && !cpu_readRD(st)) data_i = cpu_memory[addr & 0xFFFF];
        printf("%d %04x %02x %02x %u %u %u %u %u %u %u\n",
            i, addr, data_o, data_i,
            cpu_readMREQ(st)?1:0, cpu_readIORQ(st)?1:0,
            cpu_readRD(st)?1:0,  cpu_readWR(st)?1:0,
            cpu_readM1(st)?1:0,  cpu_readRFSH(st)?1:0,
            cpu_readHALT(st)?1:0);
        /* the edge into line i: held off while the ULA holds CLK high */
        held = m_hold > 0;
        if (held) m_hold--;
        else cpu_step(st);
    }
    cpu_destroyChip(st);
    return 0;
}
