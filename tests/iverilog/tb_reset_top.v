// Sweep RESET release across CLK, including edges still in the sampler pipeline.
`timescale 1ns/1ps
`include "z80_defs.vh"
module tb_reset_top;
    reg clk = 0, z80_clk = 0, z80_wait_n = 1;
    always #10 clk = ~clk;
    always #500 z80_clk = ~z80_clk;
    reg reset_n=0;
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
        .z80_int_n(1'b1), .z80_nmi_n(1'b1),
        .z80_busreq_n(1'b1), .z80_reset_n(reset_n)
    );

    integer offset, n, i, wait_samples, fresh_edges;
    assign z80_d = !z80_mreq_n && !z80_rd_n ? mem[z80_a] : 8'hzz;
    reg checking=0, saw_fetch, reached;
    always @(posedge z80_clk) if(checking && reset_n) fresh_edges=fresh_edges+1;
    initial begin
        force dut.rst_cnt=16'hffff;
        for(i=0;i<65536;i=i+1) mem[i]=0;
        mem[0]=8'hc3; mem[1]=0; mem[2]=1; mem[16'h100]=8'h76;
        // 5 ns steps cover the whole 1 MHz clock and all 20 ns sampler phases.
        for(offset=1;offset<1000;offset=offset+5) begin
            reset_n=0; checking=0; z80_wait_n=1;
            repeat(4) @(negedge z80_clk);
            @(posedge z80_clk); #(offset);
            fresh_edges=0; checking=1; reset_n=1;
            saw_fetch=0; reached=0; wait_samples=0;
            for(n=0;n<1500 && !reached;n=n+1) begin
                @(negedge clk);
                if(!z80_m1_n && fresh_edges==0)
                    $fatal(1,"RESET used a pre-release CLK edge, offset=%0d",offset);
                if(!z80_mreq_n && !z80_rd_n) begin
                    if(!saw_fetch) begin
                        if(z80_a!==0) $fatal(1,"first fetch address not zero");
                        saw_fetch=1;
                    end
                    if(z80_a==16'h100) reached=1;
                end
                // Stretch the first fetch. Supply JP throughout its read window.
                if(saw_fetch && dut.cpu.bus_op==`BUSOP_M1 && dut.cpu.t_state==2 &&
                   dut.cpu.phi==0 && dut.core_ce && wait_samples<3)
                    wait_samples=wait_samples+1;
                z80_wait_n=!(saw_fetch && z80_a==0 && wait_samples<3);
            end
            if(!saw_fetch || !reached || wait_samples!=3) $fatal(1,"first JP lost after reset, offset=%0d",offset);
        end
        $display("PASS RESET release: 200 phases, first opcode JP 0100 with WAIT");
        $finish;
    end
endmodule
