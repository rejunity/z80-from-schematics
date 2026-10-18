// uart.v - minimal 8N1 UART for the RZ80 SoC (copied unchanged from the
// IcePi Zero port, branch fpga-icepi-zero-basic).
// TX: strobe+data in, busy out. RX: mid-bit sampled, valid strobe out.
// Both run off the core clock; divisor computed from CLK_HZ / BAUD.

module uart_tx #(
    parameter CLK_HZ = 12_500_000,
    parameter BAUD   = 115_200
) (
    input  wire       clk,
    input  wire       reset_n,
    input  wire       strobe,     // load data, start transmission
    input  wire [7:0] data,
    output wire       tx,
    output wire       busy
);
    localparam integer DIV = (CLK_HZ + BAUD / 2) / BAUD;
    localparam integer CW  = $clog2(DIV);

    reg [CW-1:0] baud_cnt = 0;
    reg [3:0]    bit_idx  = 0;   // 0 idle; 1..10 = start,8 data,stop
    reg [9:0]    shifter  = 10'h3FF;

    assign tx   = shifter[0];
    assign busy = (bit_idx != 0);

    always @(posedge clk) begin
        if (!reset_n) begin
            baud_cnt <= 0; bit_idx <= 0; shifter <= 10'h3FF;
        end else if (bit_idx == 0) begin
            if (strobe) begin
                shifter  <= {1'b1, data, 1'b0};  // stop, data LSB-first, start
                bit_idx  <= 4'd10;
                baud_cnt <= DIV - 1;
            end
        end else if (baud_cnt != 0) begin
            baud_cnt <= baud_cnt - 1'b1;
        end else begin
            shifter  <= {1'b1, shifter[9:1]};
            bit_idx  <= bit_idx - 1'b1;
            baud_cnt <= DIV - 1;
        end
    end
endmodule

module uart_rx #(
    parameter CLK_HZ = 12_500_000,
    parameter BAUD   = 115_200
) (
    input  wire       clk,
    input  wire       reset_n,
    input  wire       rx,
    output wire       rx_sync,    // synchronized rx, for activity LEDs
    output reg        valid,      // 1-clk strobe when a byte lands
    output reg  [7:0] data
);
    localparam integer DIV = (CLK_HZ + BAUD / 2) / BAUD;
    localparam integer CW  = $clog2(DIV);

    reg [1:0] sync = 2'b11;
    always @(posedge clk) sync <= {sync[0], rx};
    assign rx_sync = sync[1];

    localparam S_IDLE  = 2'd0, S_START = 2'd1, S_DATA = 2'd2, S_STOP = 2'd3;
    reg [1:0]    state    = S_IDLE;
    reg [CW-1:0] baud_cnt = 0;
    reg [2:0]    bit_idx  = 0;
    reg [7:0]    shifter  = 0;

    always @(posedge clk) begin
        valid <= 1'b0;
        if (!reset_n) begin
            state <= S_IDLE;
        end else begin
            case (state)
                S_IDLE: if (!rx_sync) begin          // start-bit edge
                    baud_cnt <= DIV / 2;             // to mid start bit
                    state    <= S_START;
                end
                S_START: if (baud_cnt != 0) baud_cnt <= baud_cnt - 1'b1;
                else if (rx_sync) state <= S_IDLE;   // false start
                else begin
                    baud_cnt <= DIV - 1;
                    bit_idx  <= 0;
                    state    <= S_DATA;
                end
                S_DATA: if (baud_cnt != 0) baud_cnt <= baud_cnt - 1'b1;
                else begin
                    shifter  <= {rx_sync, shifter[7:1]};  // LSB first
                    baud_cnt <= DIV - 1;
                    if (bit_idx == 3'd7) state <= S_STOP;
                    else bit_idx <= bit_idx + 1'b1;
                end
                S_STOP: if (baud_cnt != 0) baud_cnt <= baud_cnt - 1'b1;
                else begin
                    if (rx_sync) begin               // stop bit good
                        data  <= shifter;
                        valid <= 1'b1;
                    end                               // else framing error: drop
                    state <= S_IDLE;
                end
            endcase
        end
    end
endmodule
