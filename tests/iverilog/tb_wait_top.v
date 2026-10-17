// External /WAIT on the Tang Primer header reaches z80_top's falling-edge
// sampler directly: a pulse that is low across the falling edge of T2 (or
// of a Tw) inserts a wait state; a pulse that only spans the rising edge
// does not. The header CLK (z80_clk) is the real Z80 clock here, 1 MHz.
`timescale 1ns/1ps
module tb_wait_top;
    reg clk = 0, clk_ext = 0, z80_reset_n = 0;
    tri z80_clk = clk_ext;           // header CLK pin is inout in top.v
    reg wait_drv = 1;
    tri z80_wait_n = wait_drv, hi = 1'b1; // inout header pins need nets
    always #10  clk = ~clk;          // 50 MHz crystal (POR counter only)
    always #500 clk_ext = ~clk_ext;  // 1 MHz external Z80 clock
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
        .z80_busreq_n(hi), .z80_reset_n(z80_reset_n)
    );
    assign z80_d = (!z80_mreq_n && !z80_rd_n) ? 8'h00 : 8'hzz;  // NOPs

    // Width of the next M1 (M1 low = T1 .. end of T2 plus any Tw), in T.
    // t0 is the rising edge of T1; the falling edge of T2 is t0 + 1500 ns.
    realtime t0, t_m1_end = 0;
    integer t_len;
    always @(posedge z80_m1_n) t_m1_end = $realtime;   // M1 may end before the pulse does
    task pulse_and_measure(input integer lo_ns, input integer hi_ns, input integer want, input [8*48-1:0] what);
        begin
            @(negedge z80_m1_n); t0 = $realtime;
            #(lo_ns); wait_drv = 0;
            #(hi_ns - lo_ns); wait_drv = 1;
            wait (t_m1_end > t0);
            t_len = (t_m1_end - t0) / 1000;
            if (t_len !== want)
                $fatal(1, "%0s: M1 lasted %0d T, expected %0d", what, t_len, want);
        end
    endtask

    initial begin
        #10_000_000;
        $fatal(1, "tb_wait_top timed out");
    end

    initial begin
        wait (dut.board_reset_n);          // power-on stretch (~5 ms)
        repeat (2) @(posedge z80_clk);
        #1 z80_reset_n = 1;
        @(negedge z80_m1_n);               // let the first fetch go by
        // WAIT low 1300..1700 ns: spans T2's falling edge (1500) but is
        // high again at Tw's rising edge (2000). One Tw inserted.
        pulse_and_measure(1300, 1700, 3, "WAIT across T2 falling edge");
        // WAIT low 1600..2400 ns: spans Tw's would-be rising edge (2000)
        // but neither falling edge (1500, 2500). No Tw: the pin is sampled
        // on falling edges only.
        pulse_and_measure(1600, 2400, 2, "WAIT between falling edges");
        // WAIT low 1300..2700 ns: T2 and Tw1 falling edges. Two Tw.
        pulse_and_measure(1300, 2700, 4, "WAIT across two falling edges");
        // Unstretched cycle afterwards.
        pulse_and_measure(0, 1, 2, "no WAIT");
        $display("PASS Tang Primer external WAIT: falling-edge sample via the header clock");
        $finish;
    end
endmodule
