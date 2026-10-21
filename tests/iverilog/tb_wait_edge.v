// A short WAIT pulse at the falling-edge sample must insert one Tw even if
// WAIT is released before the following rising edge. The converse pulse,
// starting only after the falling edge, must not insert a Tw.
//
// Single Z80-rate clock: one T-state per period, clock-high = ".P",
// clock-low = ".N". WAIT is taken by z80_top's falling-edge sampler.
`timescale 1ns/1ps
`include "z80_defs.vh"

module tb_wait_edge;
    reg clk = 0, reset_n = 1, wait_n = 1;
    wire [15:0] addr;
    wire [7:0] data_out;
    wire data_drive, m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    z80_top dut (
        .clk(clk), .por_n(reset_n), .reset_n(reset_n),
        .addr(addr), .data_in(8'h00), .data_out(data_out), .data_drive(data_drive),
        .m1_n(m1_n), .mreq_n(mreq_n), .iorq_n(iorq_n), .rd_n(rd_n),
        .wr_n(wr_n), .rfsh_n(rfsh_n), .halt_n(halt_n), .busack_n(busack_n),
        .wait_n(wait_n), .int_n(1'b1), .nmi_n(1'b1), .busreq_n(1'b1),
        .dbg_t(), .dbg_m()
    );
    wire phi = ~clk;   // valid once settled after an edge

    // half a T-state: one clock edge, then settle
    task step;
        begin
            #4 clk = ~clk;
            #1;
        end
    endtask

    task reach_t2_p;
        integer n;
        begin : search
            for (n = 0; n < 100; n = n + 1) begin
                if (dut.u_core.bus_op == `BUSOP_M1 && dut.u_core.t_state == 2 && phi == 0)
                    disable search;
                step;
            end
            $fatal(1, "M1 T2.P was not reached");
        end
    endtask

    reg [15:0] pc_before;
    initial begin
        #1 reset_n = 0;   // real falling edge on reset_n: async-resets both former halves
        step;
        reset_n = 1;
        reach_t2_p;
        pc_before = dut.u_core.rfl[`RFP_PC];

        wait_n = 0;
        step; // falling edge of T2: z80_top captures WAIT
        if (phi !== 1 || dut.u_core.wait_sampled !== 1)
            $fatal(1, "WAIT was not captured at T2 falling edge");
        wait_n = 1;
        #1;
        if (dut.u_core.wait_sampled !== 1 || dut.u_core.stall !== 1)
            $fatal(1, "WAIT decision changed after falling edge");
        step; // next rising edge: insert Tw
        if (dut.u_core.t_state !== 2 || phi !== 0 || dut.u_core.rfl[`RFP_PC] !== pc_before)
            $fatal(1, "falling-edge WAIT pulse did not insert Tw");
        step; // next falling edge samples released WAIT
        if (dut.u_core.wait_sampled !== 0) $fatal(1, "WAIT remained latched");
        step;
        if (dut.u_core.t_state !== 3 || phi !== 0)
            $fatal(1, "CPU did not leave Tw after WAIT release");

        reach_t2_p;
        step; // falling edge with WAIT inactive
        if (phi !== 1 || dut.u_core.wait_sampled !== 0)
            $fatal(1, "unexpected WAIT sample");
        wait_n = 0; // too late for this T2
        step;
        if (dut.u_core.t_state !== 3 || phi !== 0)
            $fatal(1, "post-falling-edge WAIT pulse inserted a false Tw");
        $display("PASS WAIT falling-edge latch");
        $finish;
    end
endmodule
