// chip_shim.v - runs tb_sign_of_life.v (written for z80_top) on the chip:
// z80_top's ports around z80_chip (<pdk>/chip/) and its pads, so the
// program goes through the I/O cells. The data bus is a real tri-state
// bus between the D pads and the testbench's memory, which drives it
// while RD is low; data_drive is the CPU's write cycle. sign_of_life.py
// compiles it, with -DDUT=z80_chip_shim, for a run whose design is
// z80_chip, along with the core macro's own netlist.
module z80_chip_shim (
    input  wire        clk,
    input  wire        por_n,      // z80_top's power-on reset: z80_chip ties it off
    input  wire        reset_n,
    output wire [15:0] addr,
    input  wire [7:0]  data_in,
    output wire [7:0]  data_out,
    output wire        data_drive,
    output wire        m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n,
    input  wire        wait_n, int_n, nmi_n, busreq_n,
    output wire [3:0]  dbg_t,
    output wire [2:0]  dbg_m
);
    wire [7:0] D;
    assign D          = !rd_n ? data_in : 8'bz;
    assign data_out   = D;
    assign data_drive = !wr_n;
    assign dbg_t      = 4'd0;
    assign dbg_m      = 3'd0;

    z80_chip chip (
        .A(addr), .D(D),
        .CLK(clk), .RESET_n(reset_n), .WAIT_n(wait_n), .INT_n(int_n), .NMI_n(nmi_n), .BUSRQ_n(busreq_n),
        .M1_n(m1_n), .MREQ_n(mreq_n), .IORQ_n(iorq_n), .RD_n(rd_n), .WR_n(wr_n),
        .RFSH_n(rfsh_n), .HALT_n(halt_n), .BUSAK_n(busack_n)
    );

    // A chip powers up with its flip-flops at random and runs: z80_top's
    // dual-edge pin former settles on the first clock edges, and RESET
    // starts the core. A gate-level simulation starts them unknown (X)
    // instead, which the former (each half fed from the other) never
    // leaves. So the shim starts the chip in one of its power-up states,
    // the one z80_top's power-on reset gives (the chip ties por_n off):
    // the macro's por_n held low until the testbench's por_n rises.
    initial begin
        force chip.u_top.por_n = 1'b0;
        wait (por_n === 1'b1);
        release chip.u_top.por_n;
    end
endmodule
