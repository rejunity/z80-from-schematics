// ===========================================================================
// z80_top.v - the chip-level wrapper: one clock, one clock domain.
//
// Everything that *thinks* (z80_core: sequencer, PLA, ALU, register file)
// runs on the POSITIVE edge of `clk`, once per Z80 T-state. This wrapper is
// the only place in the design that touches the falling edge, and it does
// exactly two things with it:
//
//   1. Falling-edge input samplers. The original NMOS Z80 looks at a few
//      inputs on the falling edge of its clock (read data at the end of
//      T3/T4, INT/NMI at the rising edge of T_last -- i.e. during the
//      clock-high half). Each such input gets one native negative-edge
//      DFF here; the core consumes the sampled copy on the following
//      rising edge (a clean half-cycle path, same clock, no CDC).
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
// Reset: active-low async assert. While reset_n is low the former sits at
// the idle pin pattern (strobes high, bus not driven, BUSACK high) and the
// core holds. The first rising edge after release starts T1 of the first
// M1 at PC=0000.
// ===========================================================================
`include "z80_defs.vh"

module z80_top (
    input  wire        clk,
    input  wire        reset_n,
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
    // control inputs
    input  wire        wait_n,
    input  wire        int_n,
    input  wire        nmi_n,
    input  wire        busreq_n,
    // debug (for trace)
    output wire [3:0]  dbg_t,
    output wire [2:0]  dbg_m
);
    // ---- 1. falling-edge input samplers -------------------------------
    // Active-low control inputs reset to their INACTIVE (high) level, so
    // they map to a negedge DFF with async *set* (GF180 dffnsnq); data
    // resets to 0 (dffnrnq).
    reg [7:0] data_in_neg;
    reg       int_n_neg, nmi_n_neg;
    always @(negedge clk or negedge reset_n) begin
        if (!reset_n) begin
            data_in_neg <= 8'h00;
            int_n_neg   <= 1'b1;
            nmi_n_neg   <= 1'b1;
        end else begin
            data_in_neg <= data_in;
            int_n_neg   <= int_n;
            nmi_n_neg   <= nmi_n;
        end
    end

    // ---- the posedge-only core -----------------------------------------
    // Pin images, packed {addr, data_out, data_drive, m1_n, mreq_n, iorq_n,
    // rd_n, wr_n, rfsh_n, halt_n, busack_n} -- see PIN_* below.
    localparam PW = 16 + 8 + 1 + 8;
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

    z80_core u_core (
        .clk(clk), .reset_n(reset_n),
        // rising-edge inputs (live) + falling-edge-sampled copies
        .data_in(data_in),   .data_in_neg(data_in_neg),
        .wait_n(wait_n),     .busreq_n(busreq_n),
        .int_n_neg(int_n_neg),
        .nmi_n(nmi_n),       .nmi_n_neg(nmi_n_neg),
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
        .dbg_t(dbg_t), .dbg_m(dbg_m)
    );

    // ---- 2. dual-edge output former -------------------------------------
    wire [PW-1:0] pins_pos = {addr_pos, dout_pos, drive_pos, m1_pos, mreq_pos,
                              iorq_pos, rd_pos, wr_pos, rfsh_pos, halt_pos, busack_pos};
    wire [PW-1:0] pins_neg = {addr_neg, dout_neg, drive_neg, m1_neg, mreq_neg,
                              iorq_neg, rd_neg, wr_neg, rfsh_neg, halt_neg, busack_neg};
    // idle pattern: addr 0, data 0, not driving, all active-low strobes high
    localparam [PW-1:0] PINS_IDLE = {16'h0000, 8'h00, 1'b0, 8'b1111_1111};

    reg [PW-1:0] pin_a;     // rising-edge half of the former
    reg [PW-1:0] pin_b;     // falling-edge half of the former
    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) pin_a <= PINS_IDLE;
        else          pin_a <= pins_pos ^ pin_b;
    end
    always @(negedge clk or negedge reset_n) begin
        if (!reset_n) pin_b <= {PW{1'b0}};
        else          pin_b <= pins_neg ^ pin_a;
    end
    wire [PW-1:0] pins = pin_a ^ pin_b;

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
