// tb_top.v - end-to-end smoke test of the IcePi Zero RZ80 SoC.
//
// Boots the real NASCOM BASIC 4.7 ROM through the real top-level (clock
// divider, POR, BRAM, UART, ACIA shim, /INT RX path) and talks to it
// over the serial pins exactly like a terminal would:
//
//   1. wait for the cold-start "Memory top?" prompt
//   2. answer CR (accept default top-of-RAM)
//   3. wait for the banner's "Ok"
//   4. type "PRINT 123+456" + CR
//   5. expect "579"  -> PASS
//
// The UART runs at BAUD = CORE_HZ/16 (16 Z80 clocks per bit) so the
// serial side doesn't dominate simulation time. Run vvp from a directory
// containing rom.mem (top.v's ROM_FILE default), e.g. build/.

`timescale 1ns / 1ps

module tb_top;
    localparam integer CLK_IN_HZ = 50_000_000;
    localparam integer CLK_DIV   = 12;                       // PLL stand-in divider
    localparam integer CORE_HZ   = CLK_IN_HZ / CLK_DIV;      // 4.17 MHz Z80 clock
    localparam integer SIM_BAUD  = CORE_HZ / 16;
    // one UART bit = 16 Z80 clks; Z80 clk = 20 ns * CLK_DIV = 240 ns
    localparam integer BIT_NS    = 16 * 20 * CLK_DIV;

    reg clk = 0;
    always #10 clk = ~clk;              // 50 MHz

    reg  usb_rx = 1'b1;
    wire usb_tx;
    wire [4:0] led;
    wire [27:0] gpio;

    top #(.CORE_HZ(CORE_HZ), .SIM_CLK_DIV(CLK_DIV), .BAUD(SIM_BAUD)) dut (
        .clk    (clk),
        .usb_rx (usb_rx),
        .usb_tx (usb_tx),
        .button (2'b11),                // not pressed
        .led    (led),
        .gpio   (gpio)
    );

    // ---- host-side TX (drive dut's RX pin) ----
    task send_byte(input [7:0] b);
        integer i;
        begin
            usb_rx = 1'b0;              // start
            #BIT_NS;
            for (i = 0; i < 8; i = i + 1) begin
                usb_rx = b[i];          // LSB first
                #BIT_NS;
            end
            usb_rx = 1'b1;              // stop
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
            @(negedge usb_tx);          // start bit
            #(BIT_NS / 2);
            if (usb_tx == 1'b0) begin
                for (bi = 0; bi < 8; bi = bi + 1) begin
                    #BIT_NS;
                    rx_byte[bi] = usb_tx;
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
