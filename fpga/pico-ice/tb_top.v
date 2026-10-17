// tb_top.v - end-to-end smoke test of the pico-ice RZ80 SoC (sbc build).
//
// Boots the real NASCOM BASIC 4.7 ROM through the real top-level (clock
// divider, POR, EBR ROM, behavioural SPRAM, UART, ACIA shim, /INT RX
// path) and talks to it over the serial pins exactly like a terminal:
//
//   1. wait for the cold-start "Memory top?" prompt
//   2. answer CR (accept default top-of-RAM)
//   3. wait for the banner's "Ok"
//   4. type "PRINT 123+456" + CR
//   5. expect "579"  -> PASS
//
// The PLL is a black box in simulation; top.v replaces its output with the
// 12 MHz input, so the Z80 clock is 12 MHz / 6 = 2 MHz here. The UART runs
// at BAUD = CORE_HZ/16 (16 Z80 clocks per bit) so the serial side doesn't
// dominate simulation time. Run vvp from a directory containing rom.mem
// (top.v's ROM_FILE default), e.g. build/.

`timescale 1ns / 1ps

module tb_top;
    localparam integer CLK_IN_HZ = 12_000_000;               // RP2040 GPIO24 -> ICE35
    localparam integer CLK_DIV   = 6;                        // Z80 clock = PLL (x1 in sim) / 6
    localparam integer CORE_HZ   = CLK_IN_HZ / CLK_DIV;      // 2 MHz
    localparam integer SIM_BAUD  = CORE_HZ / 16;
    // one UART bit = 16 Z80 clks; Z80 clk = 1e9 / CORE_HZ ns
    localparam integer BIT_NS    = 16 * (1_000_000_000 / CORE_HZ);

    localparam real    HALF_NS   = 1.0e9 / CLK_IN_HZ / 2.0;  // 41.667 ns

    reg clk = 0;
    always #(HALF_NS) clk = ~clk;       // 12 MHz

    reg  uart_rx = 1'b1;
    wire uart_tx;
    wire led_r, led_g, led_b;
    wire z80_clk;
    wire [7:0]  z80_d;
    wire [12:0] z80_a;
    wire z80_mreq_n, z80_rd_n, z80_wr_n;

    top #(.CORE_HZ(CORE_HZ), .BAUD(SIM_BAUD), .EXTERNAL_BUS(1'b0)) dut (
        .clk        (clk),
        .uart_rx    (uart_rx),
        .uart_tx    (uart_tx),
        .btn_n      (1'b1),             // SW2 not pressed
        .led_r      (led_r),
        .led_g      (led_g),
        .led_b      (led_b),
        .z80_clk    (z80_clk),
        .z80_d      (z80_d),
        .z80_a      (z80_a),
        .z80_mreq_n (z80_mreq_n),
        .z80_rd_n   (z80_rd_n),
        .z80_wr_n   (z80_wr_n)
    );

    // ---- host-side TX (drive dut's RX pin) ----
    task send_byte(input [7:0] b);
        integer i;
        begin
            uart_rx = 1'b0;             // start
            #BIT_NS;
            for (i = 0; i < 8; i = i + 1) begin
                uart_rx = b[i];         // LSB first
                #BIT_NS;
            end
            uart_rx = 1'b1;             // stop
            #(2 * BIT_NS);
        end
    endtask

    task send_line(input [8*32-1:0] s, input integer len);
        integer i;
        begin
            for (i = len - 1; i >= 0; i = i - 1)
                send_byte(s[8*i +: 8]);
            send_byte(8'h0D);           // CR terminates a BASIC line
        end
    endtask

    // ---- host-side RX (decode dut's TX pin) ----
    reg [7:0]  rx_byte;
    reg [255:0] tail = 0;               // last 32 chars, newest in LSB byte
    integer bi;
    integer stage = 0;

    initial begin
        forever begin
            @(negedge uart_tx);         // start bit
            #(BIT_NS / 2);
            if (uart_tx == 1'b0) begin
                for (bi = 0; bi < 8; bi = bi + 1) begin
                    #BIT_NS;
                    rx_byte[bi] = uart_tx;
                end
                #BIT_NS;                // stop bit
                $write("%c", rx_byte);
                tail = {tail[247:0], rx_byte};

                case (stage)
                    0: if (tail[87:0] == "Memory top?") begin   // 11 chars
                        stage = 1;
                        fork begin
                            #(20 * BIT_NS);
                            send_byte(8'h0D);
                        end join_none
                    end
                    1: if (tail[15:0] == "Ok") begin
                        stage = 2;
                        fork begin
                            #(20 * BIT_NS);
                            send_line("PRINT 123+456", 13);
                        end join_none
                    end
                    2: if (tail[23:0] == "579") begin
                        $display("\n[tb_top] PASS: BASIC booted over UART and computed 123+456=579");
                        $finish;
                    end
                endcase
            end
        end
    end

    initial begin
        #2_000_000_000;                 // 2 s of sim time
        $display("\n[tb_top] FAIL: timeout (stage %0d)", stage);
        $fatal;
    end
endmodule
