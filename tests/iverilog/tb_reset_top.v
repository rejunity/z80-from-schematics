// Sweep header /RESET release across the external Z80 clock. Whatever the
// phase of the release, the first opcode fetch must be a clean M1 at 0000
// starting on a rising edge after the release (never on an edge that
// preceded it), and WAIT stretching of that first fetch must not lose it.
`timescale 1ns/1ps
`include "z80_defs.vh"
module tb_reset_top;
    reg clk = 0, clk_ext = 0;
    tri z80_clk = clk_ext;           // header CLK pin is inout in top.v
    reg wait_drv = 1;
    tri z80_wait_n = wait_drv, hi = 1'b1; // inout header pins need nets
    always #10 clk = ~clk;
    always #500 clk_ext = ~clk_ext;
    reg reset_n = 0;
    reg [7:0] mem [0:65535];
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
        .z80_int_n(hi), .z80_nmi_n(1'b1),
        .z80_busreq_n(hi), .z80_reset_n(reset_n)
    );

    integer offset, n, i, wait_samples, fresh_edges;
    assign z80_d = !z80_mreq_n && !z80_rd_n ? mem[z80_a] : 8'hzz;
    reg checking = 0, saw_fetch, reached;
    always @(posedge z80_clk) if (checking && reset_n) fresh_edges = fresh_edges + 1;

    // Stretch the first fetch by three Tw: drive WAIT low half a period
    // before each of the first three falling-edge sample points of M1 T2
    // at address 0 (t_state stays 2 through inserted Tw), high before the
    // fourth. JP 0100 must still be read correctly through the stretch.
    always @(posedge z80_clk) begin
        #1;
        if (checking && saw_fetch && z80_a == 16'h0000 &&
            dut.cpu.u_core.bus_op == `BUSOP_M1 && dut.cpu.u_core.t_state == 4'd2 &&
            wait_samples < 3) begin
            wait_drv = 0;
            wait_samples = wait_samples + 1;
        end else
            wait_drv = 1;
    end

    initial begin
        for (i = 0; i < 65536; i = i + 1) mem[i] = 0;
        mem[0] = 8'hc3; mem[1] = 0; mem[2] = 1; mem[16'h100] = 8'h76;
        wait (dut.board_reset_n);          // power-on stretch (~5 ms)
        // 5 ns steps cover the whole 1 MHz clock period.
        for (offset = 1; offset < 1000; offset = offset + 5) begin
            reset_n = 0; checking = 0; wait_samples = 0;
            repeat (4) @(negedge z80_clk);
            @(posedge z80_clk); #(offset);
            fresh_edges = 0; checking = 1; reset_n = 1;
            saw_fetch = 0; reached = 0;
            for (n = 0; n < 1500 && !reached; n = n + 1) begin
                @(negedge clk);
                if (!z80_m1_n && fresh_edges == 0)
                    $fatal(1, "RESET used a pre-release CLK edge, offset=%0d", offset);
                if (!z80_mreq_n && !z80_rd_n) begin
                    if (!saw_fetch) begin
                        if (z80_a !== 0) $fatal(1, "first fetch address not zero, offset=%0d", offset);
                        saw_fetch = 1;
                    end
                    if (z80_a == 16'h100) reached = 1;
                end
            end
            if (!saw_fetch || !reached || wait_samples != 3)
                $fatal(1, "first JP lost after reset, offset=%0d fetch=%0d reached=%0d waits=%0d",
                       offset, saw_fetch, reached, wait_samples);
        end
        $display("PASS RESET release: 200 phases, first opcode JP 0100 with WAIT");
        $finish;
    end
endmodule
