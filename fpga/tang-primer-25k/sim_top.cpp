// sim_top.cpp - Verilator smoke test of the Tang Primer 25K RZ80 SoC.
//
// Same script as fpga/icepi-zero/sim_top.cpp, adapted to this board's
// top: `key` buttons are ACTIVE HIGH and there is no led port.
//
//   1. wait for cold-start "Memory top?"  -> answer CR
//   2. wait for the banner "Ok"           -> type "PRINT 123+456" CR
//   3. wait for "579"                     -> press key[0] (~2 ms, with
//      a bouncy release, like the real switch)
//   4. wait for warm-start "C|W"          -> answer "W"
//   5. wait for "Ok"                      -> PASS (exit 0)
//
// Build with -GBAUD / -DSIM_BAUD (see Makefile); run from a directory
// containing rom.mem, e.g. build/.
//
// `Vtop tiny` (make sim_tiny) runs the 1K Tiny BASIC script below.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <queue>
#include "Vtop.h"
#include "verilated.h"

#ifndef SIM_BAUD
#define SIM_BAUD 260417
#endif
static const int CLK_DIV   = 12;       // top.v sim stand-in for PLLA+CLKDIV: 50 MHz / 12
static const int CORE_HZ   = 50000000 / CLK_DIV;   // 4.17 MHz Z80 clock
static const int BIT_TICKS =           // in half-input-clk (eval) steps
    ((CORE_HZ + SIM_BAUD / 2) / SIM_BAUD) * CLK_DIV * 2;

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
    t->key = 0;         // buttons idle low (active high on this board)
    t->uart_rx = 1;
    t->z80_reset_n = 1; // header /RESET and /NMI are inputs in the sbc build too (pull-ups)
    t->z80_nmi_n = 1;

    std::queue<int> tx_levels;
    auto send_byte = [&](uint8_t b) {
        tx_levels.push(0);
        for (int i = 0; i < 8; i++) tx_levels.push((b >> i) & 1);
        tx_levels.push(1);
        tx_levels.push(1);
    };
    auto send_line = [&](const char* s) {
        while (*s) send_byte((uint8_t)*s++);
        send_byte('\r');
    };

    int  rx_state = 0, rx_cnt = 0, rx_bit = 0;
    uint8_t rx_sh = 0;
    std::string tail;
    int stage = 0;
    std::string all;          // tiny: the whole transcript
    int tiny_next = 0;        // tiny: next line to type
    const char* typing = nullptr;   // tiny: rest of the line being typed

    long long tx_wait = 0, tx_cnt = 0;
    long long steps = 0;
    long long btn_timer = 0;
    const long long BTN_PRESS  = 200000;   // ~2 ms in half-clk steps
    const long long BTN_BOUNCE = 20000;
    const long long MAX_STEPS = tiny ? 400000000LL : 4000000000LL;

    while (steps < MAX_STEPS) {
        // key[0] press with a bouncing release (active HIGH here)
        if (btn_timer > 0) {
            btn_timer--;
            if (btn_timer == 0)              t->key = 0;  // released
            else if (btn_timer < BTN_BOUNCE) t->key = ((btn_timer / 1000) & 1) ? 0 : 1;
            else                             t->key = 1;  // held
        }

        t->clk = !t->clk;
        t->eval();
        steps++;

        if (tx_wait > 0) {
            tx_wait--;
        } else if (!tx_levels.empty()) {
            if (--tx_cnt <= 0) {
                t->uart_rx = tx_levels.front();
                tx_levels.pop();
                tx_cnt = BIT_TICKS;
            }
        }

        switch (rx_state) {
            case 0:
                if (t->uart_tx == 0) { rx_state = 1; rx_cnt = BIT_TICKS / 2; }
                break;
            case 1:
                if (--rx_cnt == 0) {
                    if (t->uart_tx == 0) { rx_state = 2; rx_bit = 0; rx_sh = 0; rx_cnt = BIT_TICKS; }
                    else rx_state = 0;
                }
                break;
            case 2:
                if (--rx_cnt == 0) {
                    rx_sh |= (t->uart_tx & 1) << rx_bit;
                    rx_cnt = BIT_TICKS;
                    if (++rx_bit == 8) rx_state = 3;
                }
                break;
            case 3:
                if (--rx_cnt == 0) {
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
                        send_line("PRINT 123+456");
                    } else if (stage == 2 && tail.find("579") != std::string::npos) {
                        printf("\n[sim_top] 123+456=579 OK -- pressing key[0] (reset)\n");
                        tail.clear();
                        btn_timer = BTN_PRESS;
                        stage = 4;
                    } else if (stage == 4 && tail.find("C|W") != std::string::npos) {
                        stage = 5; tail.clear();
                        tx_wait = 20LL * BIT_TICKS;
                        send_byte('W');
                    } else if (stage == 5 && tail.find("Ok") != std::string::npos) {
                        printf("\n[sim_top] PASS: boot, arithmetic, key warm-restart all good (%lld half-clks)\n", steps);
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
