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

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
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

    long long tx_wait = 0, tx_cnt = 0;
    long long steps = 0;
    long long btn_timer = 0;
    const long long BTN_PRESS  = 200000;   // ~2 ms in half-clk steps
    const long long BTN_BOUNCE = 20000;
    const long long MAX_STEPS = 4000000000LL;

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

                    if (stage == 0 && tail.find("Memory top?") != std::string::npos) {
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
    printf("\n[sim_top] FAIL: timeout at stage %d\n", stage);
    delete t;
    return 1;
}
