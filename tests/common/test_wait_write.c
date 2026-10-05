/* Each held write must have one continuous WR pulse across every Tw. */
#include "z80_sim.h"
#include "test_util.h"

static z80_system_t s;

static void check_write(unsigned op, uint16_t address)
{
    CHECK(s.cpu.pins.addr == address && s.cpu.pins.data_out == 0x55 &&
          s.cpu.pins.data_drive && !s.cpu.pins.wr_n && s.cpu.pins.rd_n &&
          s.cpu.pins.m1_n && s.cpu.pins.rfsh_n &&
          s.cpu.pins.mreq_n == (op != BUSOP_MWR) &&
          s.cpu.pins.iorq_n == (op != BUSOP_IOWR),
          "write stable: op=%u T=%u phi=%u WR=%u", op, s.cpu.t_state,
          s.cpu.phi, s.cpu.pins.wr_n);
}

static void held_write(unsigned op, unsigned sample_t, uint16_t address)
{
    int n;
    for (n = 0; n < 300; ++n) {
        if ((unsigned)s.cpu.bus_op == op && s.cpu.t_state == sample_t && !s.cpu.phi)
            break;
        z80_sys_phase(&s);
    }
    CHECK(n < 300, "write sample phase reached");
    if (n == 300) return;
    if (op == BUSOP_MWR) CHECK(s.cpu.pins.wr_n, "WR inactive before initial T2.N");
    s.cpu.pins.wait_n = 0;
    z80_sys_phase(&s);
    check_write(op, address);
    for (n = 0; n < 40; ++n) {
        z80_sys_phase(&s);
        check_write(op, address);
        CHECK(s.cpu.t_state == sample_t, "write remains in Tw");
    }
    s.cpu.pins.wait_n = 1;
    for (n = 0; n < 3; ++n) {
        z80_sys_phase(&s);
        check_write(op, address);
    }
    z80_sys_phase(&s);
    CHECK(s.cpu.pins.wr_n && s.cpu.pins.mreq_n && s.cpu.pins.iorq_n,
          "write ends after WAIT release");
}

int main(void)
{
    const uint8_t prog[] = {0x3e, 0x55, 0x32, 0x00, 0x40, 0xd3, 0x20, 0x76};
    z80_sys_init(&s);
    z80_sys_load(&s, 0, prog, sizeof prog);
    held_write(BUSOP_MWR, 2, 0x4000);
    held_write(BUSOP_IOWR, 3, 0x5520);
    TEST_SUMMARY();
}
