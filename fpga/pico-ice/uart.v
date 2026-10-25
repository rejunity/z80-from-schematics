// uart.v - minimal 8N1 UART for the RZ80 SoC.
// TX: strobe+data in, busy out. RX: mid-bit sampled, valid strobe out.
//
// Both run off the core clock, whose rate need not be a whole multiple of
// the baud rate: 230400 baud from a 4 MHz Z80 clock is 17.36 clocks a bit.
// The bit timer is a phase accumulator: it adds BAUD / G every clock and a
// bit ends where it passes CLK_HZ / G (G = gcd(CLK_HZ, BAUD); 36 of 625 at
// 4 MHz, 288 of 4375 at 3.5 MHz). The average rate is exact, and every bit
// edge lands within one core clock of its ideal time. Where CLK_HZ is a
// whole multiple of BAUD this is a plain divide-by-N counter.

module uart_tx #(
    parameter CLK_HZ = 4_000_000,
    parameter BAUD   = 230_400
) (
    input  wire       clk,
    input  wire       reset_n,
    input  wire       strobe,     // load data, start transmission
    input  wire [7:0] data,
    output wire       tx,
    output wire       busy
);
    function integer gcd;
        input integer a, b;
        integer x, y, t;
        begin
            x = a; y = b;
            while (y != 0) begin t = x % y; x = y; y = t; end
            gcd = x;
        end
    endfunction

    localparam integer G   = gcd(CLK_HZ, BAUD);
    localparam integer INC = BAUD / G;
    localparam integer MOD = CLK_HZ / G;
    localparam integer AW  = $clog2(MOD + INC);

    reg  [AW-1:0] acc     = 0;
    reg  [3:0]    bit_idx = 0;   // 0 idle; 10..1 = start, 8 data, stop
    reg  [9:0]    shifter = 10'h3FF;
    wire [AW-1:0] sum     = acc + INC[AW-1:0];
    wire          bit_end = (sum >= MOD[AW-1:0]);

    assign tx   = shifter[0];
    assign busy = (bit_idx != 0);

    always @(posedge clk) begin
        if (!reset_n) begin
            acc <= 0; bit_idx <= 0; shifter <= 10'h3FF;
        end else if (bit_idx == 0) begin
            if (strobe) begin
                shifter <= {1'b1, data, 1'b0};  // stop, data LSB-first, start
                bit_idx <= 4'd10;
                acc     <= 0;                   // the start bit begins now
            end
        end else if (bit_end) begin
            acc     <= sum - MOD[AW-1:0];
            shifter <= {1'b1, shifter[9:1]};
            bit_idx <= bit_idx - 1'b1;
        end else begin
            acc <= sum;
        end
    end
endmodule

module uart_rx #(
    parameter CLK_HZ = 4_000_000,
    parameter BAUD   = 230_400
) (
    input  wire       clk,
    input  wire       reset_n,
    input  wire       rx,
    output wire       rx_sync,    // synchronized rx, for activity LEDs
    output reg        valid,      // 1-clk strobe when a byte lands
    output reg  [7:0] data
);
    function integer gcd;
        input integer a, b;
        integer x, y, t;
        begin
            x = a; y = b;
            while (y != 0) begin t = x % y; x = y; y = t; end
            gcd = x;
        end
    endfunction

    localparam integer G   = gcd(CLK_HZ, BAUD);
    localparam integer INC = BAUD / G;
    localparam integer MOD = CLK_HZ / G;
    localparam integer AW  = $clog2(MOD + INC);
    // The FSM sees the start edge 2 to 3 clocks after the line falls (the
    // two synchronizer flops), and each sample reads the line as it was 2
    // clocks earlier. Starting the accumulator at half a bit plus one
    // clock's worth of phase puts every sample within one clock of its
    // bit's centre: 5.8 % of a bit at 4 MHz, 6.6 % at 3.5 MHz, which
    // leaves a sender up to about 4.5 % off (tb_uart.v measures it).
    localparam integer HALF = MOD / 2 + INC;

    reg [1:0] sync = 2'b11;
    always @(posedge clk) sync <= {sync[0], rx};
    assign rx_sync = sync[1];

    localparam S_IDLE  = 2'd0, S_START = 2'd1, S_DATA = 2'd2, S_STOP = 2'd3;
    reg  [1:0]    state   = S_IDLE;
    reg  [AW-1:0] acc     = 0;
    reg  [2:0]    bit_idx = 0;
    reg  [7:0]    shifter = 0;
    wire [AW-1:0] sum     = acc + INC[AW-1:0];
    wire          bit_end = (sum >= MOD[AW-1:0]);   // sample here

    initial valid = 1'b0;

    always @(posedge clk) begin
        valid <= 1'b0;
        if (!reset_n) begin
            state <= S_IDLE;
        end else if (state == S_IDLE) begin
            if (!rx_sync) begin                      // start-bit edge
                acc   <= HALF[AW-1:0];
                state <= S_START;
            end
        end else if (!bit_end) begin
            acc <= sum;
        end else begin
            acc <= sum - MOD[AW-1:0];
            case (state)
                S_START: if (rx_sync) state <= S_IDLE;   // false start
                else begin
                    bit_idx <= 0;
                    state   <= S_DATA;
                end
                S_DATA: begin
                    shifter <= {rx_sync, shifter[7:1]};  // LSB first
                    if (bit_idx == 3'd7) state <= S_STOP;
                    else bit_idx <= bit_idx + 1'b1;
                end
                default: begin                            // S_STOP
                    if (rx_sync) begin                    // stop bit good
                        data  <= shifter;
                        valid <= 1'b1;
                    end                                   // else framing error: drop
                    state <= S_IDLE;
                end
            endcase
        end
    end
endmodule
