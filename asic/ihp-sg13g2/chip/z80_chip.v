// z80_chip.v - the Z80 as a chip on ihp-sg13g2: z80_top (rtl/, unchanged) in a
// ring of 48 I/O pads, 12 a side: one per pin of the 40-pin Z80 in the DIP's
// order, and eight supply pads more. Each pin's pad sits on the side of the
// die where the original Z80 has its bond pad (Visual Z80 die scan), in the
// same order; the pads abut at their 80 um pitch. Built by chip/config.yaml
// (LibreLane Chip flow); ../config.yaml still hardens z80_top alone.
//
//   south  A5 A6 A7 A8 A9 A10 A11 A12 A13 A14 A15 CLK                  (left to right)
//   east   D4 D3 D5 D6 VCC VDD_CORE VSS_CORE_E GND_E D2 D7 D0 D1      (bottom to top)
//   north  INT NMI HALT MREQ IORQ VCC_N VDD_CORE_N GND_N RD WR BUSAK WAIT  (right to left)
//   west   BUSRQ RESET M1 RFSH VCC_W GND VSS_CORE A0 A1 A2 A3 A4       (top to bottom)
//
// The address bus floats during RESET and BUSAK, MREQ IORQ RD WR during
// BUSAK, the data bus whenever z80_top does not drive it -- as on the Z80.
// The IHP core runs at 1.2 V (VDD/VSS), the pads at 3.3 V (IOVDD/IOVSS):
// pins 11 (VCC) and 29 (GND) are the I/O supply, with a core supply pad
// beside each, and six more supply pads fill the sides to 12 -- three I/O
// supply pairs and two core supply pairs in all.

`default_nettype none

`ifdef USE_POWER_PINS
`define Z80_PAD_POWER .iovdd(IOVDD), .iovss(IOVSS), .vdd(VDD), .vss(VSS),
`else
`define Z80_PAD_POWER
`endif

module z80_chip (
`ifdef USE_POWER_PINS
    inout  wire        IOVDD, IOVSS, VDD, VSS,
`endif
    inout  wire [15:0] A,
    inout  wire [7:0]  D,
    inout  wire        CLK, RESET_n, WAIT_n, INT_n, NMI_n, BUSRQ_n,
    inout  wire        M1_n, MREQ_n, IORQ_n, RD_n, WR_n, RFSH_n, HALT_n, BUSAK_n
);

    // core-side nets are cpu_*: the pad library's netlists are
    // case-insensitive, and LVS would merge RD_n (pin) with rd_n (core)
    wire        cpu_clk, cpu_reset_n, cpu_wait_n, cpu_int_n, cpu_nmi_n, cpu_busreq_n;
    wire [15:0] addr;
    wire [7:0]  data_in, data_out;
    wire        data_drive;
    wire        cpu_m1_n, cpu_mreq_n, cpu_iorq_n, cpu_rd_n, cpu_wr_n, cpu_rfsh_n, cpu_halt_n, cpu_busack_n;

    // the Z80's tri-state rules (datasheet: RESET floats the address bus,
    // BUSAK floats the address bus and MREQ IORQ RD WR)
    wire addr_oe = cpu_busack_n && cpu_reset_n;
    wire ctl_oe  = cpu_busack_n;

    z80_top u_top (
        .clk(cpu_clk), .reset_n(cpu_reset_n),
        .addr(addr), .data_in(data_in), .data_out(data_out), .data_drive(data_drive),
        .m1_n(cpu_m1_n), .mreq_n(cpu_mreq_n), .iorq_n(cpu_iorq_n), .rd_n(cpu_rd_n), .wr_n(cpu_wr_n),
        .rfsh_n(cpu_rfsh_n), .halt_n(cpu_halt_n), .busack_n(cpu_busack_n),
        .wait_n(cpu_wait_n), .int_n(cpu_int_n), .nmi_n(cpu_nmi_n), .busreq_n(cpu_busreq_n),
        .dbg_t(), .dbg_m()
    );

    // pin 1: A11
    sg13g2_IOPadTriOut4mA pad_A11 (`Z80_PAD_POWER .pad(A[11]), .c2p(addr[11]), .c2p_en(addr_oe));
    // pin 2: A12
    sg13g2_IOPadTriOut4mA pad_A12 (`Z80_PAD_POWER .pad(A[12]), .c2p(addr[12]), .c2p_en(addr_oe));
    // pin 3: A13
    sg13g2_IOPadTriOut4mA pad_A13 (`Z80_PAD_POWER .pad(A[13]), .c2p(addr[13]), .c2p_en(addr_oe));
    // pin 4: A14
    sg13g2_IOPadTriOut4mA pad_A14 (`Z80_PAD_POWER .pad(A[14]), .c2p(addr[14]), .c2p_en(addr_oe));
    // pin 5: A15
    sg13g2_IOPadTriOut4mA pad_A15 (`Z80_PAD_POWER .pad(A[15]), .c2p(addr[15]), .c2p_en(addr_oe));
    // pin 6: CLK
    sg13g2_IOPadIn pad_CLK (`Z80_PAD_POWER .pad(CLK), .p2c(cpu_clk));
    // pin 7: D4
    sg13g2_IOPadInOut4mA pad_D4 (`Z80_PAD_POWER .pad(D[4]), .c2p(data_out[4]), .c2p_en(data_drive), .p2c(data_in[4]));
    // pin 8: D3
    sg13g2_IOPadInOut4mA pad_D3 (`Z80_PAD_POWER .pad(D[3]), .c2p(data_out[3]), .c2p_en(data_drive), .p2c(data_in[3]));
    // pin 9: D5
    sg13g2_IOPadInOut4mA pad_D5 (`Z80_PAD_POWER .pad(D[5]), .c2p(data_out[5]), .c2p_en(data_drive), .p2c(data_in[5]));
    // pin 10: D6
    sg13g2_IOPadInOut4mA pad_D6 (`Z80_PAD_POWER .pad(D[6]), .c2p(data_out[6]), .c2p_en(data_drive), .p2c(data_in[6]));
    // pin 11: VCC
    (* keep *) sg13g2_IOPadIOVdd pad_VCC (
`ifdef USE_POWER_PINS
        .iovdd(IOVDD), .iovss(IOVSS), .vdd(VDD), .vss(VSS)
`endif
    );
    // core supply: VDD_CORE
    (* keep *) sg13g2_IOPadVdd pad_VDD_CORE (
`ifdef USE_POWER_PINS
        .iovdd(IOVDD), .iovss(IOVSS), .vdd(VDD), .vss(VSS)
`endif
    );
    // extra supply, east: VSS_CORE_E
    (* keep *) sg13g2_IOPadVss pad_VSS_CORE_E (
`ifdef USE_POWER_PINS
        .iovdd(IOVDD), .iovss(IOVSS), .vdd(VDD), .vss(VSS)
`endif
    );
    // extra supply, east: GND_E
    (* keep *) sg13g2_IOPadIOVss pad_GND_E (
`ifdef USE_POWER_PINS
        .iovdd(IOVDD), .iovss(IOVSS), .vdd(VDD), .vss(VSS)
`endif
    );
    // pin 12: D2
    sg13g2_IOPadInOut4mA pad_D2 (`Z80_PAD_POWER .pad(D[2]), .c2p(data_out[2]), .c2p_en(data_drive), .p2c(data_in[2]));
    // pin 13: D7
    sg13g2_IOPadInOut4mA pad_D7 (`Z80_PAD_POWER .pad(D[7]), .c2p(data_out[7]), .c2p_en(data_drive), .p2c(data_in[7]));
    // pin 14: D0
    sg13g2_IOPadInOut4mA pad_D0 (`Z80_PAD_POWER .pad(D[0]), .c2p(data_out[0]), .c2p_en(data_drive), .p2c(data_in[0]));
    // pin 15: D1
    sg13g2_IOPadInOut4mA pad_D1 (`Z80_PAD_POWER .pad(D[1]), .c2p(data_out[1]), .c2p_en(data_drive), .p2c(data_in[1]));
    // pin 16: INT_n
    sg13g2_IOPadIn pad_INT_n (`Z80_PAD_POWER .pad(INT_n), .p2c(cpu_int_n));
    // pin 17: NMI_n
    sg13g2_IOPadIn pad_NMI_n (`Z80_PAD_POWER .pad(NMI_n), .p2c(cpu_nmi_n));
    // pin 18: HALT_n
    sg13g2_IOPadOut4mA pad_HALT_n (`Z80_PAD_POWER .pad(HALT_n), .c2p(cpu_halt_n));
    // pin 19: MREQ_n
    sg13g2_IOPadTriOut4mA pad_MREQ_n (`Z80_PAD_POWER .pad(MREQ_n), .c2p(cpu_mreq_n), .c2p_en(ctl_oe));
    // pin 20: IORQ_n
    sg13g2_IOPadTriOut4mA pad_IORQ_n (`Z80_PAD_POWER .pad(IORQ_n), .c2p(cpu_iorq_n), .c2p_en(ctl_oe));
    // extra supply, north: VCC_N
    (* keep *) sg13g2_IOPadIOVdd pad_VCC_N (
`ifdef USE_POWER_PINS
        .iovdd(IOVDD), .iovss(IOVSS), .vdd(VDD), .vss(VSS)
`endif
    );
    // extra supply, north: VDD_CORE_N
    (* keep *) sg13g2_IOPadVdd pad_VDD_CORE_N (
`ifdef USE_POWER_PINS
        .iovdd(IOVDD), .iovss(IOVSS), .vdd(VDD), .vss(VSS)
`endif
    );
    // extra supply, north: GND_N
    (* keep *) sg13g2_IOPadIOVss pad_GND_N (
`ifdef USE_POWER_PINS
        .iovdd(IOVDD), .iovss(IOVSS), .vdd(VDD), .vss(VSS)
`endif
    );
    // pin 21: RD_n
    sg13g2_IOPadTriOut4mA pad_RD_n (`Z80_PAD_POWER .pad(RD_n), .c2p(cpu_rd_n), .c2p_en(ctl_oe));
    // pin 22: WR_n
    sg13g2_IOPadTriOut4mA pad_WR_n (`Z80_PAD_POWER .pad(WR_n), .c2p(cpu_wr_n), .c2p_en(ctl_oe));
    // pin 23: BUSAK_n
    sg13g2_IOPadOut4mA pad_BUSAK_n (`Z80_PAD_POWER .pad(BUSAK_n), .c2p(cpu_busack_n));
    // pin 24: WAIT_n
    sg13g2_IOPadIn pad_WAIT_n (`Z80_PAD_POWER .pad(WAIT_n), .p2c(cpu_wait_n));
    // pin 25: BUSRQ_n
    sg13g2_IOPadIn pad_BUSRQ_n (`Z80_PAD_POWER .pad(BUSRQ_n), .p2c(cpu_busreq_n));
    // pin 26: RESET_n
    sg13g2_IOPadIn pad_RESET_n (`Z80_PAD_POWER .pad(RESET_n), .p2c(cpu_reset_n));
    // pin 27: M1_n
    sg13g2_IOPadOut4mA pad_M1_n (`Z80_PAD_POWER .pad(M1_n), .c2p(cpu_m1_n));
    // pin 28: RFSH_n
    sg13g2_IOPadOut4mA pad_RFSH_n (`Z80_PAD_POWER .pad(RFSH_n), .c2p(cpu_rfsh_n));
    // extra supply, west: VCC_W
    (* keep *) sg13g2_IOPadIOVdd pad_VCC_W (
`ifdef USE_POWER_PINS
        .iovdd(IOVDD), .iovss(IOVSS), .vdd(VDD), .vss(VSS)
`endif
    );
    // pin 29: GND
    (* keep *) sg13g2_IOPadIOVss pad_GND (
`ifdef USE_POWER_PINS
        .iovdd(IOVDD), .iovss(IOVSS), .vdd(VDD), .vss(VSS)
`endif
    );
    // core supply: VSS_CORE
    (* keep *) sg13g2_IOPadVss pad_VSS_CORE (
`ifdef USE_POWER_PINS
        .iovdd(IOVDD), .iovss(IOVSS), .vdd(VDD), .vss(VSS)
`endif
    );
    // pin 30: A0
    sg13g2_IOPadTriOut4mA pad_A0 (`Z80_PAD_POWER .pad(A[0]), .c2p(addr[0]), .c2p_en(addr_oe));
    // pin 31: A1
    sg13g2_IOPadTriOut4mA pad_A1 (`Z80_PAD_POWER .pad(A[1]), .c2p(addr[1]), .c2p_en(addr_oe));
    // pin 32: A2
    sg13g2_IOPadTriOut4mA pad_A2 (`Z80_PAD_POWER .pad(A[2]), .c2p(addr[2]), .c2p_en(addr_oe));
    // pin 33: A3
    sg13g2_IOPadTriOut4mA pad_A3 (`Z80_PAD_POWER .pad(A[3]), .c2p(addr[3]), .c2p_en(addr_oe));
    // pin 34: A4
    sg13g2_IOPadTriOut4mA pad_A4 (`Z80_PAD_POWER .pad(A[4]), .c2p(addr[4]), .c2p_en(addr_oe));
    // pin 35: A5
    sg13g2_IOPadTriOut4mA pad_A5 (`Z80_PAD_POWER .pad(A[5]), .c2p(addr[5]), .c2p_en(addr_oe));
    // pin 36: A6
    sg13g2_IOPadTriOut4mA pad_A6 (`Z80_PAD_POWER .pad(A[6]), .c2p(addr[6]), .c2p_en(addr_oe));
    // pin 37: A7
    sg13g2_IOPadTriOut4mA pad_A7 (`Z80_PAD_POWER .pad(A[7]), .c2p(addr[7]), .c2p_en(addr_oe));
    // pin 38: A8
    sg13g2_IOPadTriOut4mA pad_A8 (`Z80_PAD_POWER .pad(A[8]), .c2p(addr[8]), .c2p_en(addr_oe));
    // pin 39: A9
    sg13g2_IOPadTriOut4mA pad_A9 (`Z80_PAD_POWER .pad(A[9]), .c2p(addr[9]), .c2p_en(addr_oe));
    // pin 40: A10
    sg13g2_IOPadTriOut4mA pad_A10 (`Z80_PAD_POWER .pad(A[10]), .c2p(addr[10]), .c2p_en(addr_oe));

endmodule

`default_nettype wire
