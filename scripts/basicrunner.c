/* basicrunner.c - run a Z80 ROM image (Intel HEX or raw binary) on our C
   model with an emulated 68B50 ACIA at ports 0x80 (status/control) and
   0x81 (data), wired to host stdin/stdout. Tuned for NASCOM/RC2014 BASIC
   and Tiny BASIC ROMs.

   Usage:
       basicrunner [--bin] <rom.{hex,bin}> [--start ADDR] [--max-instr N]

   The status register returns RDRF (bit 0) and TDRE (bit 1) per the 6850
   datasheet; reads from the data register return one byte from stdin
   (blocking if none is available); writes go to stdout. Other ports return
   0xFF on read and are ignored on write. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <sys/select.h>
#include "z80_sim.h"

#define ACIA_STATUS 0x80
#define ACIA_DATA   0x81
#define STATUS_RDRF 0x01     /* receive data register full */
#define STATUS_TDRE 0x02     /* transmit data register empty (always set here) */

static struct termios saved_termios;
static int            termios_saved = 0;

static void restore_termios(void) {
    if (termios_saved) tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}
static void on_signal(int sig) { restore_termios(); _exit(128 + sig); }
static void enable_raw_stdin(void) {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
        termios_saved = 1;
        atexit(restore_termios);
        struct termios t = saved_termios;
        t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        t.c_cc[VMIN]  = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
}

/* one-byte lookahead so RDRF reports accurately even when stdin is a pipe
   that's been closed (select() returns readable + read() returns 0 = EOF) */
static int stdin_pending = -1;
static int stdin_eof     = 0;

static int stdin_byte_available(void) {
    if (stdin_pending >= 0) return 1;
    if (stdin_eof) return 0;
    fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = { 0, 0 };
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) return 0;
    unsigned char ch;
    ssize_t r = read(STDIN_FILENO, &ch, 1);
    if (r == 0) { stdin_eof = 1; return 0; }
    if (r < 0)  return 0;
    stdin_pending = ch;
    return 1;
}

static int stdin_consume_byte(void) {
    if (stdin_pending >= 0) {
        int b = stdin_pending; stdin_pending = -1; return b;
    }
    if (stdin_eof) return 0;
    unsigned char ch;
    ssize_t r = read(STDIN_FILENO, &ch, 1);
    if (r <= 0) { stdin_eof = 1; return 0; }
    return ch;
}

/* Intel HEX loader: ignores end-of-file marker, treats checksum errors as fatal */
static size_t load_intel_hex(const char* path, uint8_t* mem) {
    FILE* f = fopen(path, "r"); if (!f) { perror(path); exit(2); }
    char line[600];
    size_t loaded = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] != ':') continue;
        unsigned len, addr, type;
        if (sscanf(line + 1, "%2x%4x%2x", &len, &addr, &type) != 3) continue;
        if (type == 1) break;                                    /* EOF */
        if (type != 0) continue;                                  /* skip ext seg / start */
        unsigned sum = (unsigned)(len + (addr >> 8) + (addr & 0xFF) + type);
        for (unsigned i = 0; i < len; i++) {
            unsigned b;
            if (sscanf(line + 9 + 2*i, "%2x", &b) != 1) { fprintf(stderr,"bad hex\n"); exit(2); }
            mem[(addr + i) & 0xFFFF] = (uint8_t)b;
            sum += b;
            loaded++;
        }
        unsigned cks;
        if (sscanf(line + 9 + 2*len, "%2x", &cks) == 1) {
            if (((sum + cks) & 0xFF) != 0)
                fprintf(stderr, "warn: hex checksum mismatch at %04x\n", addr);
        }
    }
    fclose(f);
    return loaded;
}

static size_t load_raw_bin(const char* path, uint8_t* mem, uint16_t at) {
    FILE* f = fopen(path, "rb"); if (!f) { perror(path); exit(2); }
    size_t n = fread(mem + at, 1, 0x10000 - at, f);
    fclose(f);
    return n;
}

int main(int argc, char** argv) {
    const char* rom = NULL;
    int is_bin = 0;
    uint16_t load_at = 0x0000;
    long long max_instr = 5000000000LL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bin"))                  is_bin = 1;
        else if (!strcmp(argv[i], "--start") && i+1<argc) load_at = (uint16_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--max-instr") && i+1<argc) max_instr = atoll(argv[++i]);
        else if (!rom)                                  rom = argv[i];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }
    if (!rom) { fprintf(stderr, "usage: %s [--bin] <rom> [--start ADDR] [--max-instr N]\n", argv[0]); return 2; }

    z80_system_t* s = malloc(sizeof *s);
    z80_sys_init(s);
    size_t loaded;
    if (is_bin) loaded = load_raw_bin(rom, s->mem, load_at);
    else        loaded = load_intel_hex(rom, s->mem);
    fprintf(stderr, "[basicrunner] loaded %zu bytes from %s%s\n",
            loaded, rom, is_bin ? " (binary)" : " (intel hex)");

    z80_set_pc(&s->cpu, 0x0000);
    s->cpu.rf[RFP_SP] = 0xFFFF;

    enable_raw_stdin();

    long long n = 0;
    /* Bus pins stay asserted across multiple phases of a single M-cycle;
       only act on each side-effect once per cycle by detecting the leading
       (falling) edge of MREQ/IORQ + RD/WR. Data-bus reads are an exception:
       we MUST drive data_in on every phase where the read is active so the
       latch picks it up at the right phase. */
    int prev_mem_wr = 1, prev_io_wr = 1, prev_io_rd = 1;
    int latched_data_byte = -1;            /* sticky data for the current IORQ read cycle */
    /* The 6850 ACIA's IRQ output is wired to the Z80 /INT pin in this ROM's
       hardware: IRQ stays asserted while RDRF=1 (or TDRE=1 with TX-int
       enabled, which this ROM doesn't keep on). NASCOM BASIC RX is fully
       interrupt-driven (acia_rxa polls a software queue filled by the
       acia_interrupt ISR), so we MUST drive /INT when stdin has a byte. */
    s->cpu.pins.int_n = 1;
    while (n++ < max_instr) {
        z80_phase_step(&s->cpu);
        z80_t* c = &s->cpu;
        /* /INT mirrors ACIA RDRF: drop it while a stdin byte is queued */
        c->pins.int_n = stdin_byte_available() ? 0 : 1;
        int mreq = !c->pins.mreq_n, iorq = !c->pins.iorq_n;
        int rd   = !c->pins.rd_n,    wr   = !c->pins.wr_n;
        int mem_wr_active = mreq && wr;
        int io_wr_active  = iorq && wr;
        int io_rd_active  = iorq && rd;

        if (mem_wr_active && !prev_mem_wr)
            s->mem[c->pins.addr] = c->pins.data_out;
        if (io_wr_active  && !prev_io_wr) {
            uint8_t port = c->pins.addr & 0xFF;
            /* Data ports: NASCOM ACIA at 0x81, Tiny BASIC at 0x01 */
            if (port == ACIA_DATA || port == 0x01) { putchar((int)c->pins.data_out); fflush(stdout); }
            /* Status writes (control reg): port 0x80 NASCOM, port 0x00 Tiny BASIC - both ignored */
        }
        if (io_rd_active && !prev_io_rd) {
            uint8_t port = c->pins.addr & 0xFF;
            int has = stdin_byte_available();
            if (port == ACIA_STATUS) {
                /* NASCOM BASIC: bit 0 = RDRF, bit 1 = TDRE */
                latched_data_byte = STATUS_TDRE | (has ? STATUS_RDRF : 0);
            } else if (port == 0x00) {
                /* Tiny BASIC: any non-zero = ready (it does ANA A; JZ) */
                latched_data_byte = has ? 0xFF : 0x00;
            } else if (port == ACIA_DATA || port == 0x01) {
                latched_data_byte = stdin_consume_byte();
                if (getenv("BASIC_DEBUG"))
                    fprintf(stderr, "[in:%02x@PC=%04x]", latched_data_byte, c->rf[RFP_PC]);
            } else {
                latched_data_byte = 0xFF;
            }
        }
        if (!io_rd_active) latched_data_byte = -1;

        /* drive data_in for the read currently in progress */
        if (mreq && rd) {
            c->pins.data_in = s->mem[c->pins.addr];
        } else if (io_rd_active && latched_data_byte >= 0) {
            c->pins.data_in = (uint8_t)latched_data_byte;
        } else {
            c->pins.data_in = 0;
        }
        prev_mem_wr = mem_wr_active;
        prev_io_wr  = io_wr_active;
        prev_io_rd  = io_rd_active;
    }
    fprintf(stderr, "[basicrunner] max_instr reached at PC=%04x\n", s->cpu.rf[RFP_PC]);
    free(s);
    return 0;
}
