// sim_top.cpp - Verilator smoke test of the IcePi Zero RZ80 SoC top.
//
// Same script as tb_top.v, but ~100x faster: boots the real NASCOM
// BASIC 4.7 ROM through the real top-level (clock divider, POR, BRAM,
// UART, ACIA shim, /INT RX path), talking over the serial *pins*:
//
//   1. wait for cold-start "Memory top?"  -> answer CR
//   2. wait for the banner "Ok"           -> type "PRINT 123+457", DEL, "6", CR
//                                         (BS echoed: top.v maps DEL to BS)
//   3. wait for "579"                     -> press button 0 (~2 ms, with
//      a bouncy release, like the real switch)
//   4. wait for warm-start "C|W"          -> answer "W"
//   5. wait for "Ok"                      -> PASS (exit 0)
//
// Build with -GBAUD=260417 so one UART bit is 16 Z80 clocks. Run from
// a directory containing rom.mem (top.v's ROM_FILE default), e.g. build/.
//
// `Vtop tiny` (make sim_tiny) runs the 1K Tiny BASIC script below.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <queue>
#include "Vtop.h"
#include "verilated.h"

// SIM_BAUD must match top's BAUD parameter (-GBAUD). Fast default:
// 16 Z80 clks per bit. Build with -DSIM_BAUD=240000 -GBAUD=240000 (make
// sim_real) for the hardware's pacing, 17.36 Z80 clocks a bit (230400
// baud at 4 MHz), which exercises the ROM's interrupt-driven TX path that
// fast baud never enters.
#ifndef SIM_BAUD
#define SIM_BAUD 260417
#endif
static const int CLK_DIV   = 12;       // top.v PLL stand-in: 50 MHz / 12 = 4.17 MHz Z80 clock
static const int CORE_HZ   = 50000000 / CLK_DIV;
// A bit is CORE_HZ / SIM_BAUD Z80 clocks, which need not be whole (17.36
// at the hardware's 230400 baud from 4 MHz). The host side keeps its bit
// edges on the exact rate with a phase accumulator over eval steps, as a
// real UART bridge's own clock would.
static const long long STEPS_PER_SEC = 2LL * CLK_DIV * CORE_HZ;  // eval steps per second
static const int BIT_TICKS =           // one bit in eval steps, rounded (pauses)
    (int)((STEPS_PER_SEC + SIM_BAUD / 2) / SIM_BAUD);
struct BitClock {                      // step() is true when a bit time has passed
    long long acc = 0;
    void start(long long phase) { acc = phase; }
    bool step() {
        acc += SIM_BAUD;
        if (acc < STEPS_PER_SEC) return false;
        acc -= STEPS_PER_SEC;
        return true;
    }
};

// `Vtop tiny` (make sim_tiny) runs the 1K Tiny BASIC ROM instead (model
// built with RAM from 0x0400): one line typed per ">" prompt, and the
// whole console transcript must equal the C model's byte for byte
// (scripts/basicrunner, same ROM, same lines). Tiny BASIC writes without
// polling TDRE, so a byte the UART dropped shows up as a mismatch.
// Lines are typed one character per echo, as a person types: basic1K
// takes ~430 Z80 clocks per input character (~2,000 when it tokenizes a
// keyword), slower than back-to-back characters arrive at this baud, and
// its only input buffer is the SoC's 32-byte RX FIFO.
static const char* const TINY_LINES[] = {
    "PRINT 1+2",
    "10 FOR I=1 TO 12",
    "20 PRINT I*1111",
    "30 NEXT I",
    "RUN",                     // basic1K reports the end of the run as E657
    "PRINT \"THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789\"",
    "PRINT \"DONE-TINY\"",
};
static const int TINY_N = sizeof(TINY_LINES) / sizeof(TINY_LINES[0]);
static const char TINY_EXPECT[] =
    "\r\n"
    ">PRINT 1+2\r\r\n"
    "3\r\n"
    "\r\n"
    ">10 FOR I=1 TO 12\r\r\n"
    ">20 PRINT I*1111\r\r\n"
    ">30 NEXT I\r\r\n"
    ">RUN\r\r\n"
    "1111\r\n"
    "2222\r\n"
    "3333\r\n"
    "4444\r\n"
    "5555\r\n"
    "6666\r\n"
    "7777\r\n"
    "8888\r\n"
    "9999\r\n"
    "11110\r\n"
    "12221\r\n"
    "13332\r\n"
    "\r\n"
    "E657\r\n"
    ">PRINT \"THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789\"\r\r\n"
    "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789\r\n"
    "\r\n"
    ">PRINT \"DONE-TINY\"\r\r\n"
    "DONE-TINY\r\n"
    "\r\n"
    ">";

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    const bool tiny = argc > 1 && strcmp(argv[argc - 1], "tiny") == 0;
    Vtop* t = new Vtop;
    t->button = 3;      // not pressed
    t->usb_rx = 1;

    // host -> DUT serial: queue of pending line levels, one a bit
    std::queue<int> tx_levels;
    auto send_byte = [&](uint8_t b) {
        tx_levels.push(0);                              // start
        for (int i = 0; i < 8; i++) tx_levels.push((b >> i) & 1);
        tx_levels.push(1);                              // stop
        tx_levels.push(1);                              // idle gap
    };
    auto send_line = [&](const char* s) {
        while (*s) send_byte((uint8_t)*s++);
        send_byte('\r');
    };

    // DUT -> host serial decoder (samples usb_tx every eval step)
    int  rx_state = 0, rx_bit = 0;
    uint8_t rx_sh = 0;
    std::string tail;
    int stage = 0;
    std::string all;          // tiny: the whole transcript
    int tiny_next = 0;        // tiny: next line to type
    const char* typing = nullptr;   // tiny: rest of the line being typed

    long long tx_wait = 0;
    BitClock tx_clk, rx_clk;
    bool tx_run = false;
    long long steps = 0;
    long long btn_timer = 0;      // >0: button-0 press in progress
    const long long BTN_PRESS  = 200000;   // ~2 ms in half-clk steps
    const long long BTN_BOUNCE = 20000;    // bouncy tail of the release
    const long long MAX_STEPS = tiny ? 400000000LL : 4000000000LL;   // hard stop

    while (steps < MAX_STEPS) {
        // button-0 press with a bouncing release: solid low, then
        // chatter between low/high, then released. The reset stretch
        // in top.v must swallow the chatter and emit ONE clean reset.
        if (btn_timer > 0) {
            btn_timer--;
            if (btn_timer == 0)              t->button = 3;  // released
            else if (btn_timer < BTN_BOUNCE) t->button = ((btn_timer / 1000) & 1) ? 3 : 2;
            else                             t->button = 2;  // held
        }

        t->clk = !t->clk;
        t->eval();
        steps++;

        // drive host->DUT serial
        if (tx_wait > 0) {
            tx_wait--;
        } else if (!tx_levels.empty()) {
            if (!tx_run || tx_clk.step()) {     // the next level, a bit after the last
                if (!tx_run) tx_clk.start(0);
                tx_run = true;
                t->usb_rx = tx_levels.front();
                tx_levels.pop();
            }
        } else if (tx_run && tx_clk.step()) {
            tx_run = false;                     // the last level has had its bit
        }

        // decode DUT->host serial
        switch (rx_state) {
            case 0:
                if (t->usb_tx == 0) { rx_state = 1; rx_clk.start(STEPS_PER_SEC / 2); }
                break;
            case 1:      // middle of start bit
                if (rx_clk.step()) {
                    if (t->usb_tx == 0) { rx_state = 2; rx_bit = 0; rx_sh = 0; }
                    else rx_state = 0;
                }
                break;
            case 2:
                if (rx_clk.step()) {
                    rx_sh |= (t->usb_tx & 1) << rx_bit;
                    if (++rx_bit == 8) rx_state = 3;
                }
                break;
            case 3:      // stop bit
                if (rx_clk.step()) {
                    putchar(rx_sh); fflush(stdout);
                    tail.push_back((char)rx_sh);
                    if (tail.size() > 256) tail.erase(0, tail.size() - 256);
                    rx_state = 0;

                    if (tiny) {
                        all.push_back((char)rx_sh);
                        if (typing && rx_sh == (uint8_t)*typing) {      // its echo: type the next one
                            if (*++typing) send_byte((uint8_t)*typing);
                            else { send_byte('\r'); typing = nullptr; }
                        } else if (!typing && all.size() >= 3 && all.compare(all.size() - 3, 3, "\r\n>") == 0) {
                            if (tiny_next < TINY_N) {
                                typing = TINY_LINES[tiny_next++];
                                tx_wait = 20LL * BIT_TICKS;
                                send_byte((uint8_t)*typing);
                            } else {
                                size_t n = 0;
                                while (n < all.size() && TINY_EXPECT[n] && all[n] == TINY_EXPECT[n]) n++;
                                bool ok = all == TINY_EXPECT;
                                if (ok) printf("\n[sim_top] PASS: Tiny BASIC transcript matches the C model (%zu bytes, %lld half-clks)\n", all.size(), steps);
                                else    printf("\n[sim_top] FAIL: Tiny BASIC transcript differs from the C model at byte %zu of %zu\n", n, sizeof(TINY_EXPECT) - 1);
                                delete t;
                                return ok ? 0 : 1;
                            }
                        }
                    } else if (stage == 0 && tail.find("Memory top?") != std::string::npos) {
                        stage = 1; tail.clear();
                        tx_wait = 20LL * BIT_TICKS;
                        send_byte('\r');
                    } else if (stage == 1 && tail.find("Ok") != std::string::npos) {
                        stage = 2; tail.clear();
                        tx_wait = 20LL * BIT_TICKS;
                        send_line("PRINT 123+457\x7f" "6");     // DEL: macOS backspace key
                    } else if (stage == 2 && tail.find("579") != std::string::npos) {
                        // BASIC echoes the DEL it received as BS; a raw DEL would be a
                        // rubout, echoing the deleted "7" instead
                        if (tail.find("457\b6") == std::string::npos) {
                            printf("\n[sim_top] FAIL: DEL was not edited as backspace (no 457<BS>6 echo)\n");
                            delete t;
                            return 1;
                        }
                        printf("\n[sim_top] 123+457<DEL>6 = 579 OK -- pressing button 0 (reset)\n");
                        tail.clear();
                        btn_timer = BTN_PRESS;
                        stage = 4;
                    } else if (stage == 4 && tail.find("C|W") != std::string::npos) {
                        stage = 5; tail.clear();
                        tx_wait = 20LL * BIT_TICKS;
                        send_byte('W');
                    } else if (stage == 5 && tail.find("Ok") != std::string::npos) {
                        printf("\n[sim_top] PASS: boot, arithmetic, button warm-restart all good (%lld half-clks)\n", steps);
                        delete t;
                        return 0;
                    }
                }
                break;
        }
    }
    if (tiny) printf("\n[sim_top] FAIL: timeout, Tiny BASIC: %d of %d lines typed, %zu bytes received\n", tiny_next, TINY_N, all.size());
    else      printf("\n[sim_top] FAIL: timeout at stage %d\n", stage);
    delete t;
    return 1;
}
