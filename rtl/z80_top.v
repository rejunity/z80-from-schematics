// ===========================================================================
// SPDX-FileCopyrightText: 2026 ReJ aka Renaldas Zioma
// SPDX-License-Identifier: CERN-OHL-W-2.0
// Source Location: https://github.com/rejunity/z80-from-schematics
// See HARDWARE_NOTICE.md for the section 4.1 Product notice.
//
// z80_top.v - the chip-level wrapper: one clock, one clock domain.
//
// Everything that *thinks* (z80_core: sequencer, PLA, ALU, register file)
// runs on the POSITIVE edge of `clk`, once per Z80 T-state. This wrapper is
// the only place in the design that touches the falling edge, and it does
// exactly two things with it:
//
//   1. Falling-edge input samplers. The original NMOS Z80 looks at a few
//      inputs on the falling edge of its clock (read data at the end of
//      T3/T4, WAIT in T2 / Tw). Each such input gets one native
//      negative-edge DFF here; the core consumes the sampled copy on the
//      following rising edge (a clean half-cycle path, same clock, no
//      CDC). INT is latched on the rising edge that starts the last
//      T-state (Zilog PS0178 Figure 8, TsINT/ThINT; perfectz80), NMI by
//      its own falling edge, of any width (TwNMI): see below.
//
//   2. Dual-edge output former. Bus pins change on BOTH edges of the Z80
//      clock (MREQ asserts on a falling edge, deasserts on a rising edge,
//      etc). The core supplies two images per pin -- the value for the
//      clock-high half of the upcoming T-state (`*_pos`, computed from its
//      next-state logic) and the value for the clock-low half of the
//      current T-state (`*_neg`). The former is the classic glitch-free
//      two-flop XOR structure:
//
//          a <= pos ^ b   @ posedge          out = a ^ b
//          b <= neg ^ a   @ negedge
//
//      After a rising edge  out = (pos^b)^b = pos; after a falling edge
//      out = a^(neg^a) = neg. Only one XOR input changes per edge, so the
//      pin makes exactly one transition per edge -- no clock-as-data mux,
//      no select/data race on WR#, no inverted clock net.
//
// Both flop banks hang off the SAME clock tree (opposite edges) -- do not
// build a clk_n = ~clk. GF180 provides native negedge cells
// (gf180mcu_fd_sc_mcu{7,9}t5v0__dffnq / dffnrnq / dffnsnq); check the
// mapped netlist for them. See docs/clocking.md.
//
// Two resets:
//  - por_n, the power-on reset: active-low async assert of every flop to
//    its power-up value (the register file to 5555, as perfectz80 comes
//    up). Not a Z80 pin: testbenches pulse it at time 0, FPGA tops drive
//    it from their power-on logic, the ASIC chip ties it off (registers
//    come up random, like the NMOS part's).
//  - reset_n, the RESET pin: sampled on every rising edge, as the die
//    does (a master-slave flip-flop), and acted on by the core
//    synchronously -- it finishes the T-state in hand and one more, keeps
//    the registers, and starts T1 of an M1 at 0000 on the third rising
//    edge after release (z80_core "RESET"; docs/timing.md). Power-on
//    leaves the core in that reset until the pin is seen high.
// ===========================================================================
`include "z80_defs.vh"

module z80_top (
    input  wire        clk,
    input  wire        por_n,           // power-on reset (not a Z80 pin)
    input  wire        reset_n,         // the RESET pin
    // bus
    output wire [15:0] addr,
    input  wire [7:0]  data_in,
    output wire [7:0]  data_out,
    output wire        data_drive,
    output wire        m1_n,
    output wire        mreq_n,
    output wire        iorq_n,
    output wire        rd_n,
    output wire        wr_n,
    output wire        rfsh_n,
    output wire        halt_n,
    output wire        busack_n,
    // the die's pad enables: the address pads, and MREQ IORQ RD WR (both
    // float in a bus grant, the address also in the reset idle); the data
    // pads follow data_drive. M1 RFSH HALT BUSACK are always driven.
    output wire        addr_drive,
    output wire        ctl_drive,
    // control inputs
    input  wire        wait_n,
    input  wire        int_n,
    input  wire        nmi_n,
    input  wire        busreq_n,
    // debug (for trace)
    output wire [3:0]  dbg_t,
    output wire [2:0]  dbg_m
);
    // ---- 1. input samplers ----------------------------------------------
    // NMI pulse catcher. The NMOS Z80 takes an NMI pulse of any width
    // (TwNMI: 80 ns at 4 MHz, under half a clock), not only one that
    // spans a clock edge: perfectz80's NMI flip-flop is set by the pin's
    // falling edge itself. nmi_hold is that flip-flop, clocked by the pin,
    // and holds NMI asserted for the samplers until the next rising edge
    // has seen it, so the core's edge detect below sees every pulse --
    // exactly as before for any pulse that spans a clock edge.
    reg nmi_hold, nmi_hold_clr;
    always @(negedge nmi_n or posedge nmi_hold_clr) begin
        if (nmi_hold_clr) nmi_hold <= 1'b0;
        else              nmi_hold <= 1'b1;
    end
    always @(posedge clk or negedge por_n) begin
        if (!por_n) nmi_hold_clr <= 1'b1;
        else          nmi_hold_clr <= nmi_hold;
    end
    wire nmi_n_held = nmi_n & ~nmi_hold;

    // Falling-edge samplers. Active-low control inputs reset to their
    // INACTIVE (high) level, so they map to a negedge DFF with async *set*
    // (GF180 dffnsnq); data resets to 0 (dffnrnq).
    reg [7:0] data_in_neg;
    reg       nmi_n_neg, wait_n_neg;
    always @(negedge clk or negedge por_n) begin
        if (!por_n) begin
            data_in_neg <= 8'h00;
            nmi_n_neg   <= 1'b1;
            wait_n_neg  <= 1'b1;
        end else begin
            data_in_neg <= data_in;
            nmi_n_neg   <= nmi_n_held;
            wait_n_neg  <= wait_n;
        end
    end

    // RESET: the level at the rising edge that starts a T-state (the die's
    // master-slave flip-flop, perfectz80 nodes 132 / 136), 1 = in reset.
    // Power-on holds it until the pin has been seen high.
    reg res_rise;
    always @(posedge clk or negedge por_n) begin
        if (!por_n) res_rise <= 1'b1;
        else        res_rise <= ~reset_n;
    end

    // BUSREQ: the level at the rising edge that starts a T-state (PS0178
    // TsBUSREQ / ThBUSREQ; perfectz80): in an M-cycle's last T-state it
    // decides the grant, in a granted T-state the release.
    reg busreq_rise;
    always @(posedge clk or negedge por_n) begin
        if (!por_n) busreq_rise <= 1'b1;
        else        busreq_rise <= busreq_n;
    end

    // INT: the level at the rising edge that starts a T-state, which the
    // core reads at the rising edge that ends it -- in the last T-state,
    // the datasheet's sample point.
    reg int_n_rise;
    always @(posedge clk or negedge por_n) begin
        if (!por_n) int_n_rise <= 1'b1;
        else          int_n_rise <= int_n;
    end

    // ---- the posedge-only core -----------------------------------------
    // Pin images, packed {addr_drive, ctl_drive, addr, data_out, data_drive,
    // m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n} -- see the
    // assigns below.
    localparam PW = 2 + 16 + 8 + 1 + 8;
    wire [15:0] addr_pos,  addr_neg;
    wire [7:0]  dout_pos,  dout_neg;
    wire        drive_pos, drive_neg;
    wire        m1_pos,    m1_neg;
    wire        mreq_pos,  mreq_neg;
    wire        iorq_pos,  iorq_neg;
    wire        rd_pos,    rd_neg;
    wire        wr_pos,    wr_neg;
    wire        rfsh_pos,  rfsh_neg;
    wire        halt_pos,  halt_neg;
    wire        busack_pos, busack_neg;
    wire        aen_pos,   aen_neg;
    wire        cen_pos,   cen_neg;

    z80_core u_core (
        .clk(clk), .por_n(por_n), .res_s(res_rise),
        // rising-edge inputs (live) + falling-edge-sampled copies
        .data_in(data_in),   .data_in_neg(data_in_neg),
        .busreq_n(busreq_rise),
        .int_n_rise(int_n_rise), .wait_n_neg(wait_n_neg),
        .nmi_n(nmi_n_held),  .nmi_n_neg(nmi_n_neg),
        // pin images: clock-high half of the next T-state / clock-low half
        // of the current T-state
        .addr_pos(addr_pos),       .addr_neg(addr_neg),
        .data_out_pos(dout_pos),   .data_out_neg(dout_neg),
        .data_drive_pos(drive_pos),.data_drive_neg(drive_neg),
        .m1_n_pos(m1_pos),         .m1_n_neg(m1_neg),
        .mreq_n_pos(mreq_pos),     .mreq_n_neg(mreq_neg),
        .iorq_n_pos(iorq_pos),     .iorq_n_neg(iorq_neg),
        .rd_n_pos(rd_pos),         .rd_n_neg(rd_neg),
        .wr_n_pos(wr_pos),         .wr_n_neg(wr_neg),
        .rfsh_n_pos(rfsh_pos),     .rfsh_n_neg(rfsh_neg),
        .halt_n_pos(halt_pos),     .halt_n_neg(halt_neg),
        .busack_n_pos(busack_pos), .busack_n_neg(busack_neg),
        .addr_drive_pos(aen_pos),  .addr_drive_neg(aen_neg),
        .ctl_drive_pos(cen_pos),   .ctl_drive_neg(cen_neg),
        .dbg_t(dbg_t), .dbg_m(dbg_m)
    );

    // ---- 2. dual-edge output former -------------------------------------
    wire [PW-1:0] pins_pos = {aen_pos, cen_pos, addr_pos, dout_pos, drive_pos, m1_pos, mreq_pos,
                              iorq_pos, rd_pos, wr_pos, rfsh_pos, halt_pos, busack_pos};
    wire [PW-1:0] pins_neg = {aen_neg, cen_neg, addr_neg, dout_neg, drive_neg, m1_neg, mreq_neg,
                              iorq_neg, rd_neg, wr_neg, rfsh_neg, halt_neg, busack_neg};
    // idle pattern (power-on: the reset idle): the address floating, the
    // strobes driven high, addr 0, data 0, the data bus not driven
    localparam [PW-1:0] PINS_IDLE = {1'b0, 1'b1, 16'h0000, 8'h00, 1'b0, 8'b1111_1111};

    reg [PW-1:0] pin_a;     // rising-edge half of the former
    reg [PW-1:0] pin_b;     // falling-edge half of the former
    always @(posedge clk or negedge por_n) begin
        if (!por_n) pin_a <= PINS_IDLE;
        else          pin_a <= pins_pos ^ pin_b;
    end
    always @(negedge clk or negedge por_n) begin
        if (!por_n) pin_b <= {PW{1'b0}};
        else          pin_b <= pins_neg ^ pin_a;
    end
    wire [PW-1:0] pins = pin_a ^ pin_b;

    assign addr_drive = pins[34];
    assign ctl_drive  = pins[33];
    assign addr       = pins[32:17];
    assign data_out   = pins[16:9];
    assign data_drive = pins[8];
    assign m1_n       = pins[7];
    assign mreq_n     = pins[6];
    assign iorq_n     = pins[5];
    assign rd_n       = pins[4];
    assign wr_n       = pins[3];
    assign rfsh_n     = pins[2];
    assign halt_n     = pins[1];
    assign busack_n   = pins[0];
endmodule
