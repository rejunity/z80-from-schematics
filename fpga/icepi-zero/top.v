// top.v - RZ80 on the IcePi Zero (Lattice ECP5, LFE5U-25F CABGA256)
//
// Two builds from this one file (EXTERNAL_BUS):
//   sbc   (0) NASCOM BASIC 4.7 (RC2014 build) on the rz80 core, console
//             over the board's FTDI USB-UART (230400 8N1). A single-board
//             computer in the Grant Searle / RC2014 sense: CPU + ROM + RAM
//             + ACIA, all inside the FPGA, with the CPU bus mirrored on the
//             40-pin header for probing.
//   chip  (1) the Z80 as a chip on the 40-pin header, clocked by an
//             external CLK: memory and I/O live outside the FPGA.
//
// The text below describes the sbc build; the header layout is shared.
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
// The Makefile sets ROM_PROTECT_TOP to the end of the ROM image rounded
// up to 1 KiB: 0x2000 for NASCOM BASIC, 0x0400 for the 1K Tiny BASIC
// (its variables and stack live at 0x0400-0x07FF).
//
// I/O map (matches scripts/basicrunner.c and tests/verilator/sim_basic.cpp):
//   IN  (0x80) -> ACIA status: bit0 RDRF (RX byte ready), bit1 TDRE (TX idle)
//   IN  (0x81) -> RX data (consumes one byte from the RX FIFO)
//   OUT (0x81) -> TX data
//   OUT (0x80) -> ACIA control (RIE/TIE decoded, rest ignored)
//   IN  (0x00) -> Tiny BASIC status: 0xFF if RX byte ready else 0x00
//   IN/OUT (0x01) -> Tiny BASIC data (same UART)
//   /INT asserted per 68B50 rules: (RIE & RDRF) | (TIE & TDRE).
//   A TX write that finds the transmitter still busy holds the CPU with
//   /WAIT until the previous byte is out (Tiny BASIC writes without
//   polling TDRE), so no byte is ever dropped.
//
// button[0] = Z80 reset (active low, board pull-up), in both builds.
//
// Header (gpio[27:0], the 40-pin Pi-style header). It has 28 user I/O,
// not the 38 of a Z80 DIP, so it carries the first 28 signals of the
// shared priority list (memory strobes first, then as much address bus
// as fits): CLK, D0..D7, /MREQ, /RD, /WR, A0..A15. /IORQ, /M1, /RFSH,
// /HALT, /BUSACK are not brought out (an I/O cycle still shows as /RD or
// /WR with /MREQ high); /WAIT, /INT, /NMI, /BUSREQ are tied inactive in
// the chip build and /RESET is button 0.
//
//   gpio[15:0]  A0..A15        gpio[24] /MREQ      gpio[26] /RD
//   gpio[23:16] D0..D7         gpio[25] CLK        gpio[27] /WR
//
// sbc: every header pin is an OUTPUT, so a logic analyser or a bus
// display can watch BASIC run; CLK is the internal Z80 clock and D the
// byte on the internal bus (the CPU's write data while it drives,
// otherwise what memory / the ACIA returned).
// chip: CLK (gpio[25], header pin 22) is an input that clocks the core,
// D0..D7 are bidirectional (driven while the Z80 writes), the rest are
// outputs. The header is 3.3 V and not 5 V tolerant.

module top #(
    parameter EXTERNAL_BUS = 0,                 // 0 = sbc, 1 = chip (see header comment)
    parameter CORE_HZ     = 4_000_000,          // sbc Z80 clock from the PLL: 4_000_000 or 3_500_000
    parameter SIM_CLK_DIV = 12,                 // simulation stand-in: 50 MHz / 12 = 4.17 MHz
    parameter BAUD        = 230_400,
    parameter ROM_FILE    = "rom.mem",
    parameter ROM_PROTECT_TOP = 16'h2000        // NASCOM 4.7 ROM is 0x0000-0x1FD9
) (
    input  wire        clk,       // 50 MHz MEMS oscillator
    input  wire        usb_rx,    // FTDI -> FPGA serial
    output wire        usb_tx,    // FPGA -> FTDI serial
    input  wire [1:0]  button,    // active low
    output wire [4:0]  led,
    inout  wire [27:0] gpio       // Z80 bus, see header comment
);
    // ---- Z80 clock: the PLL (sbc) or the CLK header pin (chip) ----
    wire clk_z80, pll_lock;
    generate if (EXTERNAL_BUS) begin : g_ext_clk
`ifdef SYNTHESIS
    // An explicit input pad: left to itself yosys turns the partly-driven
    // gpio bus into an output port and ties this undriven bit low.
    BB clk_pad (.I(1'b0), .T(1'b1), .O(clk_z80), .B(gpio[25]));
`else
    assign clk_z80  = gpio[25];
`endif
    assign pll_lock = 1'b1;
    end else begin : g_int_clk
`ifdef SYNTHESIS
    // EHXPLLL, parameters from `ecppll -i 50 -o 4 --highres` (3.5 MHz:
    // `-o 3.5`). CLKOP is the 50 MHz feedback, CLKOS the Z80 clock:
    // 50 MHz / CLKI_DIV * CLKFB_DIV * CLKOP_DIV = VCO (500 / 437.5 MHz),
    // VCO / CLKOS_DIV(125) = 4 / 3.5 MHz.
    localparam PLL_CLKI_DIV = (CORE_HZ == 3_500_000) ? 4  : 1;
    localparam PLL_CLKOP_DIV = (CORE_HZ == 3_500_000) ? 35 : 10;
    wire pll_fb;
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
    reg clk_div = 1'b0;
    always @(posedge clk) begin
        if (ckdiv == HALF - 1) begin ckdiv <= 0; clk_div <= ~clk_div; end
        else ckdiv <= ckdiv + 1'b1;
    end
    assign clk_z80  = clk_div;
    assign pll_lock = 1'b1;
`endif
    end endgenerate

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

    // z80_top's power-on reset (not a Z80 pin): low from configuration until
    // the first release of the reset above, so the registers come up 5555
    // like perfectz80's. After that the button is the RESET pin alone, which
    // keeps the registers, as on the die.
    reg cpu_por_n = 1'b0;
    always @(posedge clk_z80) if (reset_n) cpu_por_n <= 1'b1;

    // ---- Z80 ----
    wire [15:0] addr;
    wire [7:0]  data_out;
    wire        data_drive;
    wire        m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    wire [7:0]  data_in;
    wire        int_n;
    wire        wait_n;

    z80_top cpu (
        .clk        (clk_z80),
        .por_n      (cpu_por_n),
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
        .wait_n     (wait_n),
        .int_n      (int_n),
        .nmi_n      (1'b1),
        .busreq_n   (1'b1),
        .dbg_t      (),
        .dbg_m      ()
    );

    // ======================= sbc: BASIC on the FPGA =======================
    generate if (!EXTERNAL_BUS) begin : g_sbc
    // ---- header mirror (outputs only) ----
    wire [7:0] bus_d = data_drive ? data_out : data_in;
    assign gpio = {wr_n, rd_n, clk_z80, mreq_n, bus_d, addr};

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

    // TX: an OUT to the data port hands its byte to the transmitter on
    // the first rising edge of the cycle that finds it idle (tx_taken:
    // once per cycle). NASCOM BASIC checks TDRE first, so that is the
    // cycle's first edge, as before. Tiny BASIC writes without looking,
    // faster than the line drains: while the previous byte is still
    // shifting out, /WAIT holds the cycle in Tw (sampled at the falling
    // edge of Tw; IORQ and WR are low from T2, 1.5 clocks earlier).
    wire tx_wr = iowr_active && (port == 8'h81 || port == 8'h01);
    reg  tx_taken = 1'b0;
    assign wait_n = !(tx_wr && !tx_taken && tx_busy);

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
            tx_strobe <= 0; tx_taken <= 0;
            acia_ctrl <= 8'h00;
        end else begin
            prev_iord <= iord_active;
            prev_iowr <= iowr_active;
            tx_strobe <= 1'b0;

            if (fifo_push) begin
                // The backspace key of macOS terminals sends DEL (7Fh). NASCOM
                // BASIC deletes with BS (08h) and takes DEL as a Teletype
                // rubout (it reprints the deleted character), so DEL reaches
                // the CPU as BS, as in scripts/basicrunner.c.
                fifo[f_wr] <= (rx_data == 8'h7F) ? 8'h08 : rx_data;
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

            if (!tx_wr)
                tx_taken <= 1'b0;
            else if (!tx_taken && !tx_busy) begin
                tx_data   <= data_out;
                tx_strobe <= 1'b1;
                tx_taken  <= 1'b1;
            end
            if (iowr_active && !prev_iowr && port == 8'h80)
                acia_ctrl <= data_out;       // divisor/format bits ignored
        end
    end

    // 68B50 IRQ: RX byte pending (if RIE) or TX empty (if TIE). Tiny
    // BASIC never writes the control register, so it polls, as on real
    // hardware.
    assign int_n = ((rie && rx_avail) || (tie && tx_ready)) ? 1'b0 : 1'b1;

    // Data-bus input mux. INTA in IM 1 ignores the bus; 0xFF (= RST 38h)
    // is also the correct IM 0 behaviour of an open bus.
    assign data_in = inta_active ? 8'hFF : iord_active ? io_dout : ram_dout;

    // ---- LEDs: heartbeat + life signs ----
    reg [24:0] beat = 0;
    always @(posedge clk) beat <= beat + 1'b1;
    assign led[0] = beat[24];       // ~1.5 Hz heartbeat
    assign led[1] = ~usb_tx;        // TX activity
    assign led[2] = ~usb_rx_s;      // RX activity
    assign led[3] = rx_avail;       // RX FIFO non-empty
    assign led[4] = ~halt_n;        // CPU halted

    // ======================= chip: the Z80 on the header =======================
    end else begin : g_chip
    assign gpio[15:0]  = addr;
    assign gpio[24]    = mreq_n;
    // gpio[25] is the CLK input (g_ext_clk)
    assign gpio[26]    = rd_n;
    assign gpio[27]    = wr_n;
    // bidirectional data bus, driven while the Z80 writes
`ifdef SYNTHESIS
    genvar k;
    for (k = 0; k < 8; k = k + 1) begin : g_d
        BB d_pad (.I(data_out[k]), .T(~data_drive), .O(data_in[k]), .B(gpio[16 + k]));
    end
`else
    assign gpio[23:16] = data_drive ? data_out : 8'hzz;
    assign data_in = gpio[23:16];
`endif
    assign int_n   = 1'b1;
    assign wait_n  = 1'b1;
    assign usb_tx  = 1'b1;                                  // serial idle
    reg [24:0] beat = 0;
    always @(posedge clk) beat <= beat + 1'b1;
    reg [21:0] zbeat = 0;                                   // blinks while CLK runs
    always @(posedge clk_z80) zbeat <= zbeat + 1'b1;
    assign led[0] = beat[24];       // ~1.5 Hz heartbeat
    assign led[1] = zbeat[21];      // external CLK alive (~1 Hz at 4 MHz)
    assign led[2] = ~mreq_n;        // memory cycle
    assign led[3] = ~reset_n;       // held in reset
    assign led[4] = ~halt_n;        // CPU halted
    end endgenerate
endmodule
