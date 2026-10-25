// tb_uart.v - uart.v at the board's true clock-to-baud ratio.
//   TX: every bit edge against its ideal time (k * CLK_HZ / BAUD clocks
//       after the start edge), the bytes as an ideal receiver reads them,
//       and the rate over 200 back-to-back characters.
//   RX: a sender off by -3 .. +3 % (the gate) and by 4, 4.5 and 5 %
//       (reported), its start edge at 16 phases of the core clock, 0 to 2
//       bits of idle between bytes; every byte must arrive, and each
//       sample's distance from its bit's centre is measured.
// iverilog -g2012 -P tb_uart.CLK_HZ=4000000 -P tb_uart.BAUD=230400 \
//   -o tb_uart.vvp uart.v tb_uart.v && vvp -n tb_uart.vvp
`timescale 1ns/1fs
module tb_uart;
    parameter integer CLK_HZ = 4_000_000;
    parameter integer BAUD   = 230_400;
    localparam real TCLK = 1.0e9 / CLK_HZ;     // ns
    localparam real R    = 1.0 * CLK_HZ / BAUD; // core clocks a bit

    reg clk = 0;
    always #(TCLK / 2.0) clk = ~clk;
    reg reset_n = 0;

    // ---- TX ----
    reg        strobe = 0;
    reg  [7:0] tx_data = 0;
    wire       tx, busy;
    uart_tx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) utx (
        .clk(clk), .reset_n(reset_n), .strobe(strobe), .data(tx_data), .tx(tx), .busy(busy));

    // ---- RX ----
    reg        line = 1;
    wire       rx_sync, valid;
    wire [7:0] rx_data;
    uart_rx #(.CLK_HZ(CLK_HZ), .BAUD(BAUD)) urx (
        .clk(clk), .reset_n(reset_n), .rx(line), .rx_sync(rx_sync), .valid(valid), .data(rx_data));

    integer cyc = 0;
    always @(posedge clk) cyc <= cyc + 1;

    integer fails = 0;

    // ---- TX: edges and bytes ----
    reg [7:0] sent [0:255];
    integer   nsent = 0;
    integer   s0 = -1, nrecv_tx = 0, last_tx = 1, s0_first = -1, s0_last = -1, nstarts = 0;
    real      worst_tx = 0.0, worst_tx_early = 0.0;
    reg [9:0] frame;
    integer   k, bit_k;
    real      d;
    // tx changes only after posedges: record the edge index of each change
    always @(posedge clk) begin
        if (tx !== last_tx) begin
            if (s0 < 0 || cyc - s0 > 10.0 * R - 1.0) begin
                if (tx !== 1'b0) begin $display("FAIL: TX rose where a start bit should begin"); fails = fails + 1; end
                s0 = cyc;                                    // a start edge
                nstarts = nstarts + 1;
                if (nstarts == 7) s0_first = cyc;            // the back-to-back run
                s0_last = cyc;
            end else begin
                bit_k = $rtoi((cyc - s0) / R + 0.5);         // the nearest boundary
                d = (cyc - s0) - bit_k * R;                  // clocks late
                if (d < -1.0e-9 || d >= 1.0) begin
                    $display("FAIL: TX edge %0d clocks after the start, boundary %0d ideal at %0.3f", cyc - s0, bit_k, bit_k * R);
                    fails = fails + 1;
                end
                if (d > worst_tx) worst_tx = d;
            end
            last_tx = tx;
        end
    end
    // an ideal receiver reads the TX line at each bit's centre
    always @(negedge tx) if (s0 < 0 || cyc - s0 > 10.0 * R - 1.0) fork : rd
        reg [7:0] b; integer j; real t0;
        t0 = $realtime;
        for (j = 0; j < 10; j = j + 1) begin
            #((j == 0 ? 0.5 : 1.0) * R * TCLK);
            if (j == 0 && tx !== 1'b0) begin $display("FAIL: TX start bit not low at its centre"); fails = fails + 1; end
            if (j >= 1 && j <= 8) b[j - 1] = tx;
            if (j == 9) begin
                if (tx !== 1'b1) begin $display("FAIL: TX stop bit not high"); fails = fails + 1; end
                if (b !== sent[nrecv_tx]) begin
                    $display("FAIL: TX sent %02h, read %02h", sent[nrecv_tx], b); fails = fails + 1;
                end
                nrecv_tx = nrecv_tx + 1;
            end
        end
    join_none

    task send_tx(input [7:0] b);
        begin
            while (busy) @(posedge clk);
            sent[nsent] = b; nsent = nsent + 1;
            tx_data <= b; strobe <= 1'b1;
            @(posedge clk); strobe <= 1'b0;
            @(posedge clk);
        end
    endtask

    // ---- RX: a sender off by e ----
    integer nrx = 0, rx_fail = 0;
    reg [7:0] expect_q [0:4095];
    integer   qh = 0, qt = 0;
    real      tb_start, tb_bit, min_margin, max_dev;
    always @(posedge clk) if (valid) begin
        if (qh == qt || rx_data !== expect_q[qh % 4096]) begin
            rx_fail = rx_fail + 1;
        end
        if (qh != qt) qh = qh + 1;
        nrx = nrx + 1;
    end
    // each sample reads the line as it was two posedges earlier
    integer bitno;
    real    smp, dev;
    always @(posedge clk) if (urx.state != 2'd0 && urx.bit_end) begin
        smp = $realtime - 2.0 * TCLK;             // the line as the sync flops took it
        bitno = (urx.state == 2'd1) ? 0 : (urx.state == 2'd2) ? urx.bit_idx + 1 : 9;
        dev = (smp - (tb_start + (bitno + 0.5) * tb_bit)) / tb_bit;
        if (dev < 0) dev = -dev;
        if (dev > max_dev) max_dev = dev;
        if (0.5 - dev < min_margin) min_margin = 0.5 - dev;
    end

    task send_rx(input [7:0] b, input real e, input real phase, input real gap_bits);
        integer j;
        begin
            tb_bit = 1.0e9 / (BAUD * (1.0 + e));
            @(posedge clk); #(phase);
            expect_q[qt % 4096] = b; qt = qt + 1;
            tb_start = $realtime;
            line = 0; #(tb_bit);
            for (j = 0; j < 8; j = j + 1) begin line = b[j]; #(tb_bit); end
            line = 1; #(tb_bit);                  // stop
            #(gap_bits * tb_bit);
        end
    endtask

    integer pi, bi, ei, n0, seed;
    real    es [0:12];
    real    e, gap;
    reg [7:0] pat;
    integer t_first, t_last;
    initial begin
        seed = 1;
        es[0] = -0.03; es[1] = -0.02; es[2] = -0.01; es[3] = 0.0; es[4] = 0.01; es[5] = 0.02; es[6] = 0.03;
        es[7] = -0.04; es[8] = 0.04; es[9] = -0.045; es[10] = 0.045; es[11] = -0.05; es[12] = 0.05;
        repeat (4) @(posedge clk);
        reset_n = 1;
        repeat (4) @(posedge clk);

        // TX: patterns, then 200 back-to-back
        send_tx(8'h55); send_tx(8'hAA); send_tx(8'h00); send_tx(8'hFF); send_tx(8'h01); send_tx(8'h80);
        t_first = -1;
        for (bi = 0; bi < 200; bi = bi + 1) begin
            send_tx($random(seed));
            if (bi == 0) t_first = s0;
        end
        while (busy) @(posedge clk);
        repeat (40) @(posedge clk);
        t_last = cyc;
        $display("TX  %0d Hz / %0d baud = %0.4f clocks a bit: %0d bytes read back, every edge late by under one clock (worst %0.3f clocks = %0.1f %% of a bit)",
                 CLK_HZ, BAUD, R, nrecv_tx, worst_tx, 100.0 * worst_tx / R);
        $display("TX  200 back-to-back: a character every %0.2f clocks (10 bits = %0.2f; the rest is the strobe after busy drops)",
                 1.0 * (s0_last - s0_first) / 199.0, 10.0 * R);
        if (nrecv_tx != nsent) begin $display("FAIL: TX %0d of %0d bytes read", nrecv_tx, nsent); fails = fails + 1; end

        // RX
        for (ei = 0; ei < 13; ei = ei + 1) begin
            e = es[ei];
            min_margin = 1.0; max_dev = 0.0; n0 = nrx; rx_fail = 0;
            for (pi = 0; pi < 16; pi = pi + 1)
                for (bi = 0; bi < 6; bi = bi + 1) begin
                    pat = (bi == 0) ? 8'h00 : (bi == 1) ? 8'hFF : (bi == 2) ? 8'h55 : (bi == 3) ? 8'hAA : $random(seed);
                    gap = (bi % 3 == 0) ? 0.0 : (bi % 3 == 1) ? 0.37 : 2.0;
                    send_rx(pat, e, TCLK * pi / 16.0, gap);
                end
            repeat (60) @(posedge clk);
            qh = qt;                              // drop what never arrived
            if (nrx - n0 == 96 && rx_fail == 0 && e >= -0.0301 && e <= 0.0301)
                $display("RX  sender %+5.1f %%: 96 / 96 bytes, every sample within %4.1f %% of a bit from its centre (margin %4.1f %%)%s",
                         100.0 * e, 100.0 * max_dev, 100.0 * min_margin,
                         (e >= -0.0301 && e <= 0.0301) ? "" : "  (outside the gate)");
            else
                $display("RX  sender %+5.1f %%: %0d / 96 bytes%s", 100.0 * e, nrx - n0 - rx_fail,
                         (e >= -0.0301 && e <= 0.0301) ? "" : "  (outside the gate)");
            if (e >= -0.0301 && e <= 0.0301 && (nrx - n0 != 96 || rx_fail != 0)) begin
                $display("FAIL: RX at %+0.1f %%", 100.0 * e); fails = fails + 1;
            end
        end
        if (fails == 0) $display("PASS: uart.v at %0d Hz / %0d baud", CLK_HZ, BAUD);
        else            $display("FAIL: %0d checks", fails);
        $finish;
    end
endmodule
