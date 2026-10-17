// sim_top.cpp - Verilator smoke test of the pico-ice RZ80 SoC (sbc build).
//
// Same script as fpga/icepi-zero/sim_top.cpp: boots the real NASCOM
// BASIC 4.7 ROM through the real top-level (clock divider, POR, EBR ROM,
// behavioural SPRAM, UART, ACIA shim, /INT RX path), talking over the
// serial *pins*:
//
//   1. wait for cold-start "Memory top?"  -> answer CR
//   2. wait for the banner "Ok"           -> type "PRINT 123+456" CR
//   3. wait for "579"                     -> press SW2 (~8 ms, with a
//      bouncy release, like the real switch)
//   4. wait for warm-start "C|W"          -> answer "W"
//   5. wait for "Ok"                      -> PASS (exit 0)
//
// The PLL is a black box in simulation: top.v replaces its 24 MHz output
// with the 12 MHz input, so the Z80 clock is clk / 6 = 2 MHz here. Build
// with -GBAUD / -DSIM_BAUD (see Makefile); run from a directory containing
// rom.mem (top.v's ROM_FILE default), e.g. build/.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <queue>
#include "Vtop.h"
#include "verilated.h"

// SIM_BAUD must match top's BAUD parameter (-GBAUD). Fast default:
// 16 Z80 clks per bit. `make sim_real` builds with SIM_BAUD=57143, i.e.
// 35 Z80 clocks per bit at the sim's 2 MHz -- the hardware's pacing
// (115200 baud at 4 MHz) -- which exercises the ROM's interrupt-driven
// TX path that fast baud never enters.
#ifndef SIM_BAUD
#define SIM_BAUD 125000
#endif
static const int CLK_HZ    = 12000000; // board clock (RP2040 GPIO24 -> ICE35)
static const int CLK_DIV   = 6;        // top.v: Z80 clock = PLL output / 6, PLL = x1 in sim
static const int CORE_HZ   = CLK_HZ / CLK_DIV;   // 2 MHz
static const int BIT_TICKS =           // in half-input-clk (eval) steps
    ((CORE_HZ + SIM_BAUD / 2) / SIM_BAUD) * CLK_DIV * 2;

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vtop* t = new Vtop;
    t->btn_n = 1;       // SW2 not pressed (board pull-up)
    t->uart_rx = 1;

    // host -> DUT serial: queue of pending line levels, one per BIT_TICKS
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

    // DUT -> host serial decoder (samples uart_tx every eval step)
    int  rx_state = 0, rx_cnt = 0, rx_bit = 0;
    uint8_t rx_sh = 0;
    std::string tail;
    int stage = 0;

    long long tx_wait = 0, tx_cnt = 0;
    long long steps = 0;
    long long btn_timer = 0;      // >0: SW2 press in progress
    const long long BTN_PRESS  = 200000;   // ~8 ms in half-clk steps
    const long long BTN_BOUNCE = 20000;    // bouncy tail of the release
    const long long MAX_STEPS = 4000000000LL;   // hard stop

    while (steps < MAX_STEPS) {
        // SW2 press with a bouncing release: solid low, then chatter
        // between low/high, then released. The reset stretch in top.v
        // must swallow the chatter and emit ONE clean reset.
        if (btn_timer > 0) {
            btn_timer--;
            if (btn_timer == 0)              t->btn_n = 1;   // released
            else if (btn_timer < BTN_BOUNCE) t->btn_n = (btn_timer / 1000) & 1;
            else                             t->btn_n = 0;   // held
        }

        t->clk = !t->clk;
        t->eval();
        steps++;

        // drive host->DUT serial
        if (tx_wait > 0) {
            tx_wait--;
        } else if (!tx_levels.empty()) {
            if (--tx_cnt <= 0) {
                t->uart_rx = tx_levels.front();
                tx_levels.pop();
                tx_cnt = BIT_TICKS;
            }
        }

        // decode DUT->host serial
        switch (rx_state) {
            case 0:
                if (t->uart_tx == 0) { rx_state = 1; rx_cnt = BIT_TICKS / 2; }
                break;
            case 1:      // middle of start bit
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
            case 3:      // stop bit
                if (--rx_cnt == 0) {
                    putchar(rx_sh); fflush(stdout);
                    tail.push_back((char)rx_sh);
                    if (tail.size() > 256) tail.erase(0, tail.size() - 256);
                    rx_state = 0;

                    if (stage == 0 && tail.find("Memory top?") != std::string::npos) {
                        stage = 1; tail.clear();
                        tx_wait = 20LL * BIT_TICKS;
                        send_byte('\r');
                    } else if (stage == 1 && tail.find("Ok") != std::string::npos) {
                        stage = 2; tail.clear();
                        tx_wait = 20LL * BIT_TICKS;
                        send_line("PRINT 123+456");
                    } else if (stage == 2 && tail.find("579") != std::string::npos) {
                        printf("\n[sim_top] 123+456=579 OK -- pressing SW2 (reset)\n");
                        tail.clear();
                        btn_timer = BTN_PRESS;
                        stage = 4;
                    } else if (stage == 4 && tail.find("C|W") != std::string::npos) {
                        stage = 5; tail.clear();
                        tx_wait = 20LL * BIT_TICKS;
                        send_byte('W');
                    } else if (stage == 5 && tail.find("Ok") != std::string::npos) {
                        printf("\n[sim_top] PASS: boot, arithmetic, SW2 warm-restart all good (%lld half-clks)\n", steps);
                        delete t;
                        return 0;
                    }
                }
                break;
        }
    }
    printf("\n[sim_top] FAIL: timeout at stage %d\n", stage);
    delete t;
    return 1;
}
