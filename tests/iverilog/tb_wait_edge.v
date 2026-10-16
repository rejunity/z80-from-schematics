// A short WAIT pulse at the falling-edge sample must insert one Tw even if
// WAIT is released before the following rising edge. The converse pulse,
// starting only after the falling edge, must not insert a Tw.
`timescale 1ns/1ps
`include "z80_defs.vh"

module tb_wait_edge;
    parameter USE_CEN = 1;
    reg clk = 0, cen = 1, reset_n = 0, wait_n = 1;
    wire [15:0] addr;
    wire [7:0] data_out;
    wire data_drive, m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    z80_core #(.USE_CEN(USE_CEN)) dut (
        .clk(clk), .cen(cen), .reset_n(reset_n),
        .addr(addr), .data_in(8'h00), .data_out(data_out), .data_drive(data_drive),
        .m1_n(m1_n), .mreq_n(mreq_n), .iorq_n(iorq_n), .rd_n(rd_n),
        .wr_n(wr_n), .rfsh_n(rfsh_n), .halt_n(halt_n), .busack_n(busack_n),
        .wait_n(wait_n), .int_n(1'b1), .nmi_n(1'b1), .busreq_n(1'b1),
        .dbg_t(), .dbg_phi(), .dbg_m()
    );

    task step;
        begin
            #5 clk = 1;
            #1;
            #4 clk = 0;
        end
    endtask

    task reach_t2_p;
        integer n;
        begin : search
            for (n = 0; n < 100; n = n + 1) begin
                if (dut.bus_op == `BUSOP_M1 && dut.t_state == 2 && dut.phi == 0)
                    disable search;
                step;
            end
            $fatal(1, "M1 T2.P was not reached");
        end
    endtask

    reg [15:0] pc_before;
    initial begin
        step;
        reset_n = 1;
        reach_t2_p;
        pc_before = dut.rf[`RFP_PC];

        wait_n = 0;
        step; // external falling edge: enter T2.N and capture WAIT
        if (dut.phi !== 1 || dut.wait_sampled !== 1)
            $fatal(1, "WAIT was not captured at T2 falling edge");
        wait_n = 1;
        #1;
        if (dut.wait_sampled !== 1 || dut.stall !== 1)
            $fatal(1, "WAIT decision changed after falling edge");
        if (USE_CEN) begin
            cen = 0;
            step;
            if (dut.phi !== 1 || dut.wait_sampled !== 1)
                $fatal(1, "WAIT decision was lost while CEN was low");
            cen = 1;
        end
        step; // next rising edge: insert Tw
        if (dut.t_state !== 2 || dut.phi !== 0 || dut.rf[`RFP_PC] !== pc_before)
            $fatal(1, "falling-edge WAIT pulse did not insert Tw");
        step; // next falling edge samples released WAIT
        if (dut.wait_sampled !== 0) $fatal(1, "WAIT remained latched");
        step;
        if (dut.t_state !== 3 || dut.phi !== 0)
            $fatal(1, "CPU did not leave Tw after WAIT release");

        reach_t2_p;
        step; // falling edge with WAIT inactive
        if (dut.phi !== 1 || dut.wait_sampled !== 0)
            $fatal(1, "unexpected WAIT sample");
        wait_n = 0; // too late for this T2
        step;
        if (dut.t_state !== 3 || dut.phi !== 0)
            $fatal(1, "post-falling-edge WAIT pulse inserted a false Tw");
        $display("PASS WAIT falling-edge latch USE_CEN=%0d", USE_CEN);
        $finish;
    end
endmodule
