// top.v - RZ80 SoC for the IcePi Zero (Lattice ECP5, LFE5U-25F CABGA256)
//
// NASCOM BASIC 4.7 (RC2014 build) on the rz80 core, console over the
// board's FTDI USB-UART (115200 8N1).
//
//   50 MHz board clock -> /4 divider -> 12.5 MHz core clk.
//   The core's clk runs at 2x the Z80 clock (one edge per phase), so the
//   Z80 itself runs at 6.25 MHz -- comfortably above an original 3.5 MHz
//   part, comfortably below the fabric's limits.
//
// Memory map: one 64 KiB block RAM, preloaded with the BASIC ROM image
// at bitstream load. Writes below ROM_PROTECT_TOP are dropped so the
// ROM region behaves like ROM and a button reset warm-restarts cleanly.
//
// I/O map (matches scripts/basicrunner.c and tests/verilator/sim_basic.cpp):
//   IN  (0x80) -> ACIA status: bit0 RDRF (RX byte ready), bit1 TDRE (TX idle)
//   IN  (0x81) -> RX data (consumes one byte from the RX FIFO)
//   OUT (0x81) -> TX data
//   OUT (0x80) -> ACIA control (ignored)
//   IN  (0x00) -> Tiny BASIC status: 0xFF if RX byte ready else 0x00
//   IN/OUT (0x01) -> Tiny BASIC data (same UART)
//   /INT asserted while the RX FIFO is non-empty (NASCOM's RX is
//   interrupt-driven; required, not an optimisation).
//
// button[0] = Z80 reset (active low, board pull-up).

module top #(
    parameter CLK_IN_HZ    = 50_000_000,
    parameter CLK_DIV_BITS = 2,                 // core clk = CLK_IN_HZ >> CLK_DIV_BITS
    parameter BAUD         = 115_200,
    parameter ROM_FILE     = "rom.mem",
    parameter ROM_PROTECT_TOP = 16'h2000        // NASCOM 4.7 ROM is 0x0000-0x1FD9
) (
    input  wire       clk,       // 50 MHz MEMS oscillator
    input  wire       usb_rx,    // FTDI -> FPGA serial
    output wire       usb_tx,    // FPGA -> FTDI serial
    input  wire [1:0] button,    // active low
    output wire [4:0] led
);
    localparam CORE_HZ = CLK_IN_HZ >> CLK_DIV_BITS;

    // ---- clock divider: 50 MHz -> core clk ----
    reg [CLK_DIV_BITS-1:0] ckdiv = 0;
    always @(posedge clk) ckdiv <= ckdiv + 1'b1;
    wire clk_cpu = ckdiv[CLK_DIV_BITS-1];

    // ---- power-on + button-0 reset (core clk domain) ----
    // One up-counter serves both: it starts at 0 after configuration
    // (power-on reset) and is cleared whenever button 0 is held, then
    // must count out ~5 ms after release before reset_n deasserts.
    // The stretch swallows switch bounce (the board's own counter
    // example needed a debouncer for these buttons) and is orders of
    // magnitude longer than the core's ~5-phase reset filter.
    reg [15:0] rst_cnt  = 16'd0;
    reg [1:0]  btn_sync = 2'b11;
    always @(posedge clk_cpu) begin
        btn_sync <= {btn_sync[0], button[0]};
        if (!btn_sync[1])              rst_cnt <= 16'd0;      // pressed
        else if (rst_cnt != 16'hFFFF)  rst_cnt <= rst_cnt + 1'b1;
    end
    wire reset_n = (rst_cnt == 16'hFFFF);

    // ---- Z80 core ----
    wire [15:0] addr;
    wire [7:0]  data_out;
    wire        data_drive;
    wire        m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    reg  [7:0]  data_in;
    wire        int_n;

    z80_core cpu (
        .clk        (clk_cpu),
        .cen        (1'b1),      // USE_CEN=0: core steps on every clock
        .reset_n    (reset_n),
        .addr       (addr),
        .data_in    (data_in),
        .data_out   (data_out),
        .data_drive (data_drive),
        .m1_n       (m1_n),
        .mreq_n     (mreq_n),
        .iorq_n     (iorq_n),
        .rd_n       (rd_n),
        .wr_n       (wr_n),
        .rfsh_n     (rfsh_n),
        .halt_n     (halt_n),
        .busack_n   (busack_n),
        .wait_n     (1'b1),
        .int_n      (int_n),
        .nmi_n      (1'b1),
        .busreq_n   (1'b1),
        .dbg_t      (),
        .dbg_phi    (),
        .dbg_m      ()
    );

    // ---- 64 KiB block RAM, ROM image preloaded at configuration ----
    // Synchronous read; the Z80 holds the address stable for a full
    // M-cycle (>= 6 core clks) and samples read data several edges after
    // asserting RD, so the one-cycle BRAM latency is invisible to it.
    reg [7:0] ram [0:65535];
    initial $readmemh(ROM_FILE, ram);
    reg [7:0] ram_dout;
    always @(posedge clk_cpu) begin
        if (!mreq_n && !wr_n && data_drive && (addr >= ROM_PROTECT_TOP))
            ram[addr] <= data_out;
        ram_dout <= ram[addr];
    end

    // ---- UART ----
    wire       rx_valid;
    wire [7:0] rx_data;
    reg        tx_strobe;
    reg  [7:0] tx_data;
    wire       tx_busy;
    wire       usb_rx_s;

    uart_rx #(.CLK_HZ(CORE_HZ), .BAUD(BAUD)) urx (
        .clk(clk_cpu), .reset_n(reset_n), .rx(usb_rx), .rx_sync(usb_rx_s),
        .valid(rx_valid), .data(rx_data)
    );
    uart_tx #(.CLK_HZ(CORE_HZ), .BAUD(BAUD)) utx (
        .clk(clk_cpu), .reset_n(reset_n), .strobe(tx_strobe), .data(tx_data),
        .tx(usb_tx), .busy(tx_busy)
    );

    // ---- RX FIFO (32 deep) ----
    reg [7:0] fifo [0:31];
    reg [4:0] f_rd = 0, f_wr = 0;
    reg [5:0] f_cnt = 0;
    wire rx_avail = (f_cnt != 0);
    wire [7:0] fifo_head = fifo[f_rd];

    // ---- ACIA-style I/O decode ----
    // Bus strobes hold for multiple core clocks; act once per I/O cycle
    // by edge-detecting each cycle's own "fully active" condition
    // (same convention as sim_basic.cpp). INTA (iorq+m1, rd high) never
    // matches iord_active.
    wire iord_active = !iorq_n && !rd_n;
    wire iowr_active = !iorq_n && !wr_n;
    wire inta_active = !iorq_n && !m1_n;
    reg  prev_iord = 0, prev_iowr = 0;
    reg  [7:0] io_dout = 8'hFF;
    wire [7:0] port = addr[7:0];
    wire tx_ready = !tx_busy;

    wire fifo_pop  = iord_active && !prev_iord && (port == 8'h81 || port == 8'h01) && rx_avail;
    wire fifo_push = rx_valid && (f_cnt != 6'd32);

    // 68B50 control register: bit7 RIE (RX-interrupt enable), bits 6:5
    // = 01 -> TIE (TX interrupt on TDRE). The NASCOM/z88dk ACIA driver
    // uses interrupt-driven TX: when it sees TDRE=0 it queues the byte
    // and *waits for a TDRE interrupt*, so TIE must really work -- a
    // status-only shim deadlocks the ROM's TX ring buffer at real baud.
    reg [7:0] acia_ctrl = 8'h00;
    wire rie = acia_ctrl[7];
    wire tie = (acia_ctrl[6:5] == 2'b01);

    always @(posedge clk_cpu) begin
        if (!reset_n) begin
            f_rd <= 0; f_wr <= 0; f_cnt <= 0;
            prev_iord <= 0; prev_iowr <= 0;
            tx_strobe <= 0;
            acia_ctrl <= 8'h00;
        end else begin
            prev_iord <= iord_active;
            prev_iowr <= iowr_active;
            tx_strobe <= 1'b0;

            if (fifo_push) begin
                fifo[f_wr] <= rx_data;
                f_wr <= f_wr + 1'b1;
            end
            if (fifo_pop) f_rd <= f_rd + 1'b1;
            case ({fifo_push, fifo_pop})
                2'b10: f_cnt <= f_cnt + 1'b1;
                2'b01: f_cnt <= f_cnt - 1'b1;
                default: ;
            endcase

            if (iord_active && !prev_iord) begin
                case (port)
                    8'h80:   io_dout <= {6'b0, tx_ready, rx_avail};    // NASCOM status
                    8'h00:   io_dout <= rx_avail ? 8'hFF : 8'h00;      // Tiny status
                    8'h81,
                    8'h01:   io_dout <= rx_avail ? fifo_head : 8'h00;  // data
                    default: io_dout <= 8'hFF;
                endcase
            end

            if (iowr_active && !prev_iowr) begin
                if (port == 8'h81 || port == 8'h01) begin
                    tx_data   <= data_out;
                    tx_strobe <= 1'b1;
                end else if (port == 8'h80) begin
                    acia_ctrl <= data_out;   // divisor/format bits ignored
                end
            end
        end
    end

    // 68B50 IRQ: RX byte pending (if RIE) or TX empty (if TIE). Tiny
    // BASIC never writes the control register, so it polls, as on real
    // hardware.
    assign int_n = ((rie && rx_avail) || (tie && tx_ready)) ? 1'b0 : 1'b1;

    // Data-bus input mux. INTA in IM 1 ignores the bus; 0xFF (= RST 38h)
    // is also the correct IM 0 behaviour of an open bus.
    always @(*) begin
        if (inta_active)      data_in = 8'hFF;
        else if (iord_active) data_in = io_dout;
        else                  data_in = ram_dout;
    end

    // ---- LEDs: heartbeat + life signs ----
    reg [24:0] beat = 0;
    always @(posedge clk) beat <= beat + 1'b1;
    assign led[0] = beat[24];       // ~1.5 Hz heartbeat
    assign led[1] = ~usb_tx;        // TX activity
    assign led[2] = ~usb_rx_s;      // RX activity
    assign led[3] = rx_avail;       // RX FIFO non-empty
    assign led[4] = ~halt_n;        // CPU halted
endmodule
