// top.v - RZ80 BASIC SBC for the IcePi Zero (Lattice ECP5, LFE5U-25F CABGA256)
//
// NASCOM BASIC 4.7 (RC2014 build) on the rz80 core, console over the
// board's FTDI USB-UART (115200 8N1). A single-board computer in the
// Grant Searle / RC2014 sense: CPU + ROM + RAM + ACIA, all inside the
// FPGA, with the CPU bus mirrored on the 40-pin header for probing.
//
// Clock: the ECP5 PLL makes the Z80 clock, CORE_HZ = 4 MHz by default
// (a Z80A; 3.5 MHz, the Spectrum/Z80 rate, is the other supported
// value). That clock IS the Z80's CLK: z80_top runs one T-state per
// period, all core state on the rising edge and the chip's falling-edge
// pin behaviour (MREQ/RD/WR, WAIT and data sampling) on the falling
// edge (docs/clocking.md). There is no 2x clock and no clock enable; the
// whole SoC (RAM, UART, ACIA shim) lives in this one clock domain. In
// simulation the PLL is a black box, so a /SIM_CLK_DIV toggle divider
// stands in and the sim builds override CORE_HZ to match.
//
// Memory map: one 64 KiB block RAM, preloaded with the BASIC ROM image
// at bitstream load. Writes below ROM_PROTECT_TOP are dropped so the
// ROM region behaves like ROM and a button reset warm-restarts cleanly.
//
// I/O map (matches scripts/basicrunner.c and tests/verilator/sim_basic.cpp):
//   IN  (0x80) -> ACIA status: bit0 RDRF (RX byte ready), bit1 TDRE (TX idle)
//   IN  (0x81) -> RX data (consumes one byte from the RX FIFO)
//   OUT (0x81) -> TX data
//   OUT (0x80) -> ACIA control (RIE/TIE decoded, rest ignored)
//   IN  (0x00) -> Tiny BASIC status: 0xFF if RX byte ready else 0x00
//   IN/OUT (0x01) -> Tiny BASIC data (same UART)
//   /INT asserted per 68B50 rules: (RIE & RDRF) | (TIE & TDRE).
//
// button[0] = Z80 reset (active low, board pull-up).
//
// Header mirror (gpio[27:0], the 40-pin Pi-style header): the Z80 bus as
// OUTPUTS, so a logic analyser or a bus display can watch BASIC run. The
// header has 28 user I/O, not the 38 of a Z80 DIP, so this is a subset
// of the Tang Primer's full chip pinout (fpga/tang-primer-25k):
//
//   gpio[15:0]  A0..A15        gpio[24] /MREQ      gpio[26] /RD
//   gpio[23:16] D0..D7         gpio[25] /IORQ      gpio[27] /WR
//
// D shows the byte on the internal bus: the CPU's write data while it
// drives, otherwise what memory / the ACIA returned. The header is
// 3.3 V and not 5 V tolerant.

module top #(
    parameter CORE_HZ     = 4_000_000,          // Z80 clock from the PLL: 4_000_000 or 3_500_000
    parameter SIM_CLK_DIV = 12,                 // simulation stand-in: 50 MHz / 12 = 4.17 MHz
    parameter BAUD        = 115_200,
    parameter ROM_FILE    = "rom.mem",
    parameter ROM_PROTECT_TOP = 16'h2000        // NASCOM 4.7 ROM is 0x0000-0x1FD9
) (
    input  wire        clk,       // 50 MHz MEMS oscillator
    input  wire        usb_rx,    // FTDI -> FPGA serial
    output wire        usb_tx,    // FPGA -> FTDI serial
    input  wire [1:0]  button,    // active low
    output wire [4:0]  led,
    output wire [27:0] gpio       // Z80 bus mirror, see header comment
);
    // ---- Z80 clock ----
`ifdef SYNTHESIS
    // EHXPLLL, parameters from `ecppll -i 50 -o 4 --highres` (3.5 MHz:
    // `-o 3.5`). CLKOP is the 50 MHz feedback, CLKOS the Z80 clock:
    // 50 MHz / CLKI_DIV * CLKFB_DIV * CLKOP_DIV = VCO (500 / 437.5 MHz),
    // VCO / CLKOS_DIV(125) = 4 / 3.5 MHz.
    localparam PLL_CLKI_DIV = (CORE_HZ == 3_500_000) ? 4  : 1;
    localparam PLL_CLKOP_DIV = (CORE_HZ == 3_500_000) ? 35 : 10;
    wire clk_z80, pll_fb, pll_lock;
    (* FREQUENCY_PIN_CLKI="50" *)
    (* ICP_CURRENT="12" *) (* LPF_RESISTOR="8" *) (* MFG_ENABLE_FILTEROPAMP="1" *) (* MFG_GMCREF_SEL="2" *)
    EHXPLLL #(
        .PLLRST_ENA("DISABLED"), .INTFB_WAKE("DISABLED"), .STDBY_ENABLE("DISABLED"),
        .DPHASE_SOURCE("DISABLED"),
        .OUTDIVIDER_MUXA("DIVA"), .OUTDIVIDER_MUXB("DIVB"),
        .OUTDIVIDER_MUXC("DIVC"), .OUTDIVIDER_MUXD("DIVD"),
        .CLKI_DIV(PLL_CLKI_DIV),
        .CLKOP_ENABLE("ENABLED"), .CLKOP_DIV(PLL_CLKOP_DIV), .CLKOP_CPHASE(9), .CLKOP_FPHASE(0),
        .CLKOS_ENABLE("ENABLED"), .CLKOS_DIV(125), .CLKOS_CPHASE(0), .CLKOS_FPHASE(0),
        .FEEDBK_PATH("CLKOP"), .CLKFB_DIV(1)
    ) pll_i (
        .RST(1'b0), .STDBY(1'b0), .CLKI(clk),
        .CLKOP(pll_fb), .CLKOS(clk_z80), .CLKFB(pll_fb), .CLKINTFB(),
        .PHASESEL0(1'b0), .PHASESEL1(1'b0), .PHASEDIR(1'b1), .PHASESTEP(1'b1),
        .PHASELOADREG(1'b1), .PLLWAKESYNC(1'b0), .ENCLKOP(1'b0), .LOCK(pll_lock)
    );
`else
    // Simulation stand-in: toggle divider, 50 % duty (both edges matter).
    localparam HALF = SIM_CLK_DIV / 2;
    reg [$clog2(HALF)-1:0] ckdiv = 0;
    reg clk_z80 = 1'b0;
    always @(posedge clk) begin
        if (ckdiv == HALF - 1) begin ckdiv <= 0; clk_z80 <= ~clk_z80; end
        else ckdiv <= ckdiv + 1'b1;
    end
    wire pll_lock = 1'b1;
`endif

    // ---- power-on + button-0 reset ----
    // One up-counter on the always-running 50 MHz clock serves both: it
    // starts at 0 after configuration (power-on reset) and is cleared
    // whenever button 0 is held or the PLL is unlocked, then must count
    // out ~5 ms after release before board_reset_n deasserts. The stretch
    // swallows switch bounce (the board's own counter example needed a
    // debouncer for these buttons).
    reg [17:0] rst_cnt  = 18'd0;
    reg [1:0]  btn_sync = 2'b11;
    always @(posedge clk) begin
        btn_sync <= {btn_sync[0], button[0]};
        if (!btn_sync[1] || !pll_lock)  rst_cnt <= 18'd0;      // pressed / no PLL
        else if (rst_cnt != 18'h3FFFF)  rst_cnt <= rst_cnt + 1'b1;
    end
    wire board_reset_n = (rst_cnt == 18'h3FFFF);
    // Asynchronous assertion, release synchronized to the Z80 clock, so
    // z80_top's first T1 starts on a clean rising edge.
    reg [1:0] rst_sync = 2'b00;
    always @(posedge clk_z80 or negedge board_reset_n) begin
        if (!board_reset_n) rst_sync <= 2'b00;
        else                rst_sync <= {rst_sync[0], 1'b1};
    end
    wire reset_n = rst_sync[1];

    // ---- Z80 ----
    wire [15:0] addr;
    wire [7:0]  data_out;
    wire        data_drive;
    wire        m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    reg  [7:0]  data_in;
    wire        int_n;

    z80_top cpu (
        .clk        (clk_z80),
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
        .dbg_m      ()
    );

    // ---- header mirror (outputs only) ----
    wire [7:0] bus_d = data_drive ? data_out : data_in;
    assign gpio = {wr_n, rd_n, iorq_n, mreq_n, bus_d, addr};

    // ---- 64 KiB block RAM, ROM image preloaded at configuration ----
    // Synchronous read, one T-state of latency: the address is on the
    // pins from the rising edge of T1, the BRAM registers it at T2, and
    // the core takes the opcode at the rising edge after T2 (M1) or the
    // falling edge of T3 (memory read). No wait states needed.
    // Writes: WR is low across the rising edge of T3 only (it asserts at
    // T2's falling edge and releases at T3's), so each write fires once.
    reg [7:0] ram [0:65535];
    initial $readmemh(ROM_FILE, ram);
    reg [7:0] ram_dout;
    always @(posedge clk_z80) begin
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
        .clk(clk_z80), .reset_n(reset_n), .rx(usb_rx), .rx_sync(usb_rx_s),
        .valid(rx_valid), .data(rx_data)
    );
    uart_tx #(.CLK_HZ(CORE_HZ), .BAUD(BAUD)) utx (
        .clk(clk_z80), .reset_n(reset_n), .strobe(tx_strobe), .data(tx_data),
        .tx(usb_tx), .busy(tx_busy)
    );

    // ---- RX FIFO (32 deep) ----
    reg [7:0] fifo [0:31];
    reg [4:0] f_rd = 0, f_wr = 0;
    reg [5:0] f_cnt = 0;
    wire rx_avail = (f_cnt != 0);
    wire [7:0] fifo_head = fifo[f_rd];

    // ---- ACIA-style I/O decode ----
    // IORQ/RD (or WR) assert at the rising edge of T2 and hold through
    // Tw and T3, so each is seen on two or more rising edges; act once
    // per I/O cycle by edge-detecting each cycle's own "fully active"
    // condition (same convention as sim_basic.cpp). The core latches
    // read data at T3's falling edge, one T-state after io_dout settles.
    // INTA (iorq+m1, rd high) never matches iord_active.
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

    always @(posedge clk_z80) begin
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
