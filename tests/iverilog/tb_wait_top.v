// Verify CLK and WAIT remain aligned through the Tang Primer sampler.
`timescale 1ns/1ps
module tb_wait_top;
    reg clk = 0, z80_clk = 0, z80_wait_n = 1;
    always #10 clk = ~clk;
    wire [15:0] z80_a;
    tri [7:0] z80_d;
    wire z80_m1_n, z80_mreq_n, z80_iorq_n, z80_rd_n, z80_wr_n;
    wire z80_rfsh_n, z80_halt_n, uart_tx;
    top #(.ROM_FILE("tests/iverilog/zero.mem")) dut (
        .clk(clk), .uart_rx(1'b1), .uart_tx(uart_tx), .key(2'b00),
        .z80_clk(z80_clk), .z80_a(z80_a), .z80_d(z80_d),
        .z80_m1_n(z80_m1_n), .z80_mreq_n(z80_mreq_n),
        .z80_iorq_n(z80_iorq_n), .z80_rd_n(z80_rd_n),
        .z80_wr_n(z80_wr_n), .z80_rfsh_n(z80_rfsh_n),
        .z80_halt_n(z80_halt_n), .z80_wait_n(z80_wait_n),
        .z80_int_n(1'b1), .z80_nmi_n(1'b1),
        .z80_busreq_n(1'b1), .z80_reset_n(1'b0)
    );

    task wait_for_falling_enable;
        integer n;
        begin : search
            for (n = 0; n < 10; n = n + 1) begin
                @(negedge clk);
                if (dut.core_ce && dut.z80_clk_sync[1] == 0)
                    disable search;
            end
            $fatal(1, "synchronized falling CLK edge was not detected");
        end
    endtask

    initial begin
        // Establish a high CLK level, then pulse WAIT across the first
        // sampler tick after the falling edge. Release it before core_ce.
        @(negedge clk); #1 z80_clk = 1;
        repeat (5) @(negedge clk);
        #1 z80_wait_n = 0; z80_clk = 0;
        @(posedge clk); #1 z80_wait_n = 1;
        wait_for_falling_enable;
        if (dut.core_wait_n !== 0)
            $fatal(1, "WAIT sample was lost before falling-edge enable");

        // A pulse starting after the first sampler tick is too late.
        @(negedge clk); #1 z80_clk = 1;
        repeat (5) @(negedge clk);
        #1 z80_clk = 0;
        @(posedge clk); #1 z80_wait_n = 0;
        wait_for_falling_enable;
        if (dut.core_wait_n !== 1)
            $fatal(1, "late WAIT pulse was paired with earlier CLK edge");
        $display("PASS Tang Primer WAIT/CLK sampler alignment");
        $finish;
    end
endmodule
