// z80_chip.v - the Z80 as a chip on gf180mcu: z80_top (rtl/, unchanged) in a
// ring of 40 I/O pads, one per pin of the 40-pin Z80 in the DIP's order.
// Each pad sits on the side of the die where the original Z80 has its bond
// pad (Visual Z80 die scan), in the same order; LibreLane spreads them
// along their side at the pads' own pitch. Built by chip/config.yaml
// (LibreLane Chip flow); ../config.yaml still hardens z80_top alone.
//
//   south  A5 A6 A7 A8 A9 A10 A11 A12 A13 A14 A15 CLK   (left to right)
//   east   D4 D3 D5 D6 VCC D2 D7 D0 D1                  (bottom to top)
//   north  INT NMI HALT MREQ IORQ RD WR BUSAK WAIT       (right to left)
//   west   BUSRQ RESET M1 RFSH GND A0 A1 A2 A3 A4       (top to bottom)
//
// The address bus floats during RESET and BUSAK, MREQ IORQ RD WR during
// BUSAK, the data bus whenever z80_top does not drive it -- as on the Z80.
// gf180mcu_fd_io has no separate core supply pads: the core (VDD/VSS) is
// shorted to the 5 V I/O supply (DVDD/DVSS), as in the foundry pad ring,
// so VCC and GND are the only supply pads -- exactly the Z80's two.

`default_nettype none

`ifdef USE_POWER_PINS
`define Z80_PAD_POWER .DVDD(DVDD), .DVSS(DVSS), .VDD(VDD), .VSS(VSS),
`else
`define Z80_PAD_POWER
`endif

module z80_chip (
`ifdef USE_POWER_PINS
    inout  wire        VDD, VSS, DVDD, DVSS,
`endif
    inout  wire [15:0] A,
    inout  wire [7:0]  D,
    inout  wire        CLK, RESET_n, WAIT_n, INT_n, NMI_n, BUSRQ_n,
    inout  wire        M1_n, MREQ_n, IORQ_n, RD_n, WR_n, RFSH_n, HALT_n, BUSAK_n
);

`ifdef USE_POWER_PINS
    assign VDD = DVDD;
    assign VSS = DVSS;
`endif

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
    gf180mcu_fd_io__bi_24t pad_A11 (`Z80_PAD_POWER .PAD(A[11]), .A(addr[11]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 2: A12
    gf180mcu_fd_io__bi_24t pad_A12 (`Z80_PAD_POWER .PAD(A[12]), .A(addr[12]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 3: A13
    gf180mcu_fd_io__bi_24t pad_A13 (`Z80_PAD_POWER .PAD(A[13]), .A(addr[13]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 4: A14
    gf180mcu_fd_io__bi_24t pad_A14 (`Z80_PAD_POWER .PAD(A[14]), .A(addr[14]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 5: A15
    gf180mcu_fd_io__bi_24t pad_A15 (`Z80_PAD_POWER .PAD(A[15]), .A(addr[15]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 6: CLK
    gf180mcu_fd_io__in_s pad_CLK (`Z80_PAD_POWER .PAD(CLK), .Y(cpu_clk), .PU(1'b0), .PD(1'b0));
    // pin 7: D4
    gf180mcu_fd_io__bi_24t pad_D4 (`Z80_PAD_POWER .PAD(D[4]), .A(data_out[4]), .OE(data_drive), .Y(data_in[4]), .IE(1'b1), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 8: D3
    gf180mcu_fd_io__bi_24t pad_D3 (`Z80_PAD_POWER .PAD(D[3]), .A(data_out[3]), .OE(data_drive), .Y(data_in[3]), .IE(1'b1), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 9: D5
    gf180mcu_fd_io__bi_24t pad_D5 (`Z80_PAD_POWER .PAD(D[5]), .A(data_out[5]), .OE(data_drive), .Y(data_in[5]), .IE(1'b1), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 10: D6
    gf180mcu_fd_io__bi_24t pad_D6 (`Z80_PAD_POWER .PAD(D[6]), .A(data_out[6]), .OE(data_drive), .Y(data_in[6]), .IE(1'b1), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 11: VCC
    (* keep *) gf180mcu_fd_io__dvdd pad_VCC (
`ifdef USE_POWER_PINS
        .DVDD(DVDD), .DVSS(DVSS), .VSS(VSS)   // (the cell has no VDD pin)
`endif
    );
    // pin 12: D2
    gf180mcu_fd_io__bi_24t pad_D2 (`Z80_PAD_POWER .PAD(D[2]), .A(data_out[2]), .OE(data_drive), .Y(data_in[2]), .IE(1'b1), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 13: D7
    gf180mcu_fd_io__bi_24t pad_D7 (`Z80_PAD_POWER .PAD(D[7]), .A(data_out[7]), .OE(data_drive), .Y(data_in[7]), .IE(1'b1), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 14: D0
    gf180mcu_fd_io__bi_24t pad_D0 (`Z80_PAD_POWER .PAD(D[0]), .A(data_out[0]), .OE(data_drive), .Y(data_in[0]), .IE(1'b1), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 15: D1
    gf180mcu_fd_io__bi_24t pad_D1 (`Z80_PAD_POWER .PAD(D[1]), .A(data_out[1]), .OE(data_drive), .Y(data_in[1]), .IE(1'b1), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 16: INT_n
    gf180mcu_fd_io__in_c pad_INT_n (`Z80_PAD_POWER .PAD(INT_n), .Y(cpu_int_n), .PU(1'b0), .PD(1'b0));
    // pin 17: NMI_n
    gf180mcu_fd_io__in_c pad_NMI_n (`Z80_PAD_POWER .PAD(NMI_n), .Y(cpu_nmi_n), .PU(1'b0), .PD(1'b0));
    // pin 18: HALT_n
    gf180mcu_fd_io__bi_24t pad_HALT_n (`Z80_PAD_POWER .PAD(HALT_n), .A(cpu_halt_n), .OE(1'b1), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 19: MREQ_n
    gf180mcu_fd_io__bi_24t pad_MREQ_n (`Z80_PAD_POWER .PAD(MREQ_n), .A(cpu_mreq_n), .OE(ctl_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 20: IORQ_n
    gf180mcu_fd_io__bi_24t pad_IORQ_n (`Z80_PAD_POWER .PAD(IORQ_n), .A(cpu_iorq_n), .OE(ctl_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 21: RD_n
    gf180mcu_fd_io__bi_24t pad_RD_n (`Z80_PAD_POWER .PAD(RD_n), .A(cpu_rd_n), .OE(ctl_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 22: WR_n
    gf180mcu_fd_io__bi_24t pad_WR_n (`Z80_PAD_POWER .PAD(WR_n), .A(cpu_wr_n), .OE(ctl_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 23: BUSAK_n
    gf180mcu_fd_io__bi_24t pad_BUSAK_n (`Z80_PAD_POWER .PAD(BUSAK_n), .A(cpu_busack_n), .OE(1'b1), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 24: WAIT_n
    gf180mcu_fd_io__in_c pad_WAIT_n (`Z80_PAD_POWER .PAD(WAIT_n), .Y(cpu_wait_n), .PU(1'b0), .PD(1'b0));
    // pin 25: BUSRQ_n
    gf180mcu_fd_io__in_c pad_BUSRQ_n (`Z80_PAD_POWER .PAD(BUSRQ_n), .Y(cpu_busreq_n), .PU(1'b0), .PD(1'b0));
    // pin 26: RESET_n
    gf180mcu_fd_io__in_s pad_RESET_n (`Z80_PAD_POWER .PAD(RESET_n), .Y(cpu_reset_n), .PU(1'b0), .PD(1'b0));
    // pin 27: M1_n
    gf180mcu_fd_io__bi_24t pad_M1_n (`Z80_PAD_POWER .PAD(M1_n), .A(cpu_m1_n), .OE(1'b1), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 28: RFSH_n
    gf180mcu_fd_io__bi_24t pad_RFSH_n (`Z80_PAD_POWER .PAD(RFSH_n), .A(cpu_rfsh_n), .OE(1'b1), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 29: GND
    (* keep *) gf180mcu_fd_io__dvss pad_GND (
`ifdef USE_POWER_PINS
        .DVDD(DVDD), .DVSS(DVSS), .VDD(VDD)   // (the cell has no VSS pin)
`endif
    );
    // pin 30: A0
    gf180mcu_fd_io__bi_24t pad_A0 (`Z80_PAD_POWER .PAD(A[0]), .A(addr[0]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 31: A1
    gf180mcu_fd_io__bi_24t pad_A1 (`Z80_PAD_POWER .PAD(A[1]), .A(addr[1]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 32: A2
    gf180mcu_fd_io__bi_24t pad_A2 (`Z80_PAD_POWER .PAD(A[2]), .A(addr[2]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 33: A3
    gf180mcu_fd_io__bi_24t pad_A3 (`Z80_PAD_POWER .PAD(A[3]), .A(addr[3]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 34: A4
    gf180mcu_fd_io__bi_24t pad_A4 (`Z80_PAD_POWER .PAD(A[4]), .A(addr[4]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 35: A5
    gf180mcu_fd_io__bi_24t pad_A5 (`Z80_PAD_POWER .PAD(A[5]), .A(addr[5]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 36: A6
    gf180mcu_fd_io__bi_24t pad_A6 (`Z80_PAD_POWER .PAD(A[6]), .A(addr[6]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 37: A7
    gf180mcu_fd_io__bi_24t pad_A7 (`Z80_PAD_POWER .PAD(A[7]), .A(addr[7]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 38: A8
    gf180mcu_fd_io__bi_24t pad_A8 (`Z80_PAD_POWER .PAD(A[8]), .A(addr[8]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 39: A9
    gf180mcu_fd_io__bi_24t pad_A9 (`Z80_PAD_POWER .PAD(A[9]), .A(addr[9]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));
    // pin 40: A10
    gf180mcu_fd_io__bi_24t pad_A10 (`Z80_PAD_POWER .PAD(A[10]), .A(addr[10]), .OE(addr_oe), .Y(), .IE(1'b0), .CS(1'b0), .SL(1'b0), .PU(1'b0), .PD(1'b0));

endmodule

`default_nettype wire
