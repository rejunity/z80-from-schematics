// top.v - RZ80 for the pico-ice (tinyVision.ai: Lattice iCE40UP5K-SG48 + RP2040)
//
// Two builds from this one file, selected by EXTERNAL_BUS (see Makefile:
// `make sbc`, `make chip`):
//
//   EXTERNAL_BUS=0  "sbc": a BASIC single-board computer. NASCOM BASIC 4.7
//     (RC2014 build) in an 8 KiB block-RAM ROM, 32 KiB RAM in one SPRAM,
//     68B50-style ACIA at 115200 8N1 on the RP2040's USB-UART bridge, Z80
//     clock from the PLL (CORE_HZ, 4 MHz). The headers carry the Z80 bus
//     as OUTPUTS in the chip-build pinout below (CLK included), so a logic
//     analyser sees what a Z80 would drive; /RESET (SW2) is the one input.
//
//   EXTERNAL_BUS=1  "chip": a Z80 chip replacement. The headers are the
//     live CPU bus; CLK comes IN on Pmod 1 pin 10 (ICE44) and is the clock
//     of z80_top; memory and peripherals are whatever is wired to the
//     headers. The UART is unused. 3.3 V I/O, not 5 V tolerant: use level
//     translators towards 5 V Z80 hardware.
//
// Header pinout (pico-ice.pcf; README "Pinout"). The board has 25 free
// header pins, so it carries the first 25 signals of the shared priority
// list -- CLK, D0-D7, /MREQ, /RD, /WR, A0-A12:
//
//   Pmod 1  pins 1-4  A0-A3          Pmod 2  D0-D7
//           pins 7-10 /MREQ, /RD, /WR, CLK
//   Pmod 3  pins 3-4, 7-10  A4-A9 (pins 1-2: the UART to the RP2040)
//   ICE6 / ICE9 / ICE11  A10 / A11 / A12   (left long header)
//
// The strobes and D sit on the two iCE40-only Pmods; Pmod 3 doubles as
// RP2040 GPIO0-7 (inputs with pull-downs under the stock firmware), which
// pull its pins low while the FPGA is unconfigured -- harmless for the
// address bits it carries, not for /MREQ, /RD, /WR.
//
// A13-A15, /IORQ, /M1, /RFSH, /HALT, /BUSACK are not brought out; /WAIT,
// /INT, /NMI, /BUSREQ are tied inactive. /RESET is the board's SW2 button,
// whose net is also a header hole (ICE10, 10k pull-up), so an external
// open-drain /RESET can be wired there.
//
// Clocking. The Z80 clock is a real clock, one T-state per period:
// z80_top keeps all core state on its rising edge and reproduces the
// chip's falling-edge pin behaviour (MREQ/RD/WR edges, WAIT / read-data /
// INT / NMI sampling) with native negative-edge flops (iCE40 logic tiles
// have a per-tile clock inversion, SB_DFFN*; docs/clocking.md). No 2x
// clock, no clock enable, nothing resamples the external clock.
//
//   - Board clock: the RP2040 exports 12 MHz (48 MHz USB PLL / 4, from its
//     12 MHz crystal) on GPIO24 to ICE35, the iCE40's PLL pad -- the FPGA's
//     only clock source on this board besides the UP5K's imprecise internal
//     oscillators (not used).
//   - sbc: SB_PLL40_2_PAD on ICE35: port A passes the 12 MHz reference to
//     a global (board clock: reset stretch, heartbeat), port B makes
//     24 MHz (VCO 768 MHz / 32); a toggle divider /6 turns that into
//     4.000 MHz at 50 % duty, which SB_GB puts on a global net. The PLL
//     cannot go below 16 MHz and 12 MHz / 3 has no 50 % duty without
//     using both input edges, hence PLL + divider. CORE_HZ = 3_500_000
//     sets the PLL to 21 MHz -> 3.5 MHz. The fabric-divided clock feeds
//     nothing but z80_top, the memories and the UART/ACIA, all in this
//     one domain. In simulation the PLL is a black box, so its output is
//     replaced by the 12 MHz input itself (x1): the sim Z80 runs at
//     12 / 6 = 2 MHz and the sim builds override CORE_HZ to match.
//   - chip: CLK on ICE44 (global-buffer input pad G6, SB_GB_IO) drives
//     z80_top directly. Every rising edge is one T-state; opcodes are taken
//     at the rising edge of T3, read data at the falling edge of T3, the
//     Z80's own sampling points. Holding CLK stops the CPU, as on a CMOS
//     Z80. The 12 MHz board clock still runs the reset stretch.
//
// RESET: power-on + SW2, debounced and stretched (~5.5 ms) on the 12 MHz
// board clock; asynchronous assertion, release synchronized to the Z80
// clock so the first T1 always starts on a clean rising edge.
//
// sbc memory map (RC2014 "Classic": 8K ROM, 32K RAM):
//   0x0000-0x1FFF  ROM: 16 EBR (SB_RAM40_4K), preloaded from ROM_FILE.
//                  Writes at or above ROM_PROTECT_TOP land. The Makefile
//                  sets it to the end of the ROM image rounded up to 1 KiB:
//                  0x2000 for NASCOM BASIC (none, a true ROM), 0x0400 for
//                  the 1K Tiny BASIC (its variables and stack live at
//                  0x0400-0x07FF).
//   0x2000-0x7FFF  unmapped (reads 0xFF, writes ignored)
//   0x8000-0xFFFF  RAM: one SB_SPRAM256KA (16K x 16; A0 picks the byte
//                  via MASKWREN). SPRAM cannot be preloaded and powers up
//                  with arbitrary contents; BASIC sizes and clears it.
// Both memories read synchronously with one T-state of latency, like the
// IcePi port: address on the pins from the rising edge of T1, registered
// at T2, consumed by the core at the rising edge of T3 (M1) or the
// falling edge of T3 (memory read). No wait states. WR is low across the
// rising edge of T3 only, so each write fires once.
//
// I/O map / interrupt semantics: identical to fpga/icepi-zero/top.v --
// ACIA status/control at 0x80, data at 0x81 (Tiny BASIC's 0x00/0x01 too),
// /INT = (RIE & RDRF) | (TIE & TDRE), IM 1 (INTA reads 0xFF = RST 38h).
// A TX write that finds the transmitter busy holds the CPU with /WAIT
// until the previous byte is out (Tiny BASIC writes without polling TDRE),
// so no byte is dropped.
//
// RGB LED (active low, SB_RGBA_DRV): green = heartbeat (board clock alive),
// blue = serial activity (sbc) / /MREQ activity (chip), red = Z80 in reset
// or halted.

module top #(
    parameter CORE_HZ  = 4_000_000,             // sbc Z80 clock: 4_000_000 or 3_500_000; sim overrides to 2_000_000
    parameter BAUD     = 115_200,
    parameter ROM_FILE = "rom.mem",             // 8 KiB image for 0x0000-0x1FFF (hex2mem.py ... 8192)
    parameter ROM_PROTECT_TOP = 16'h2000,       // first writable ROM-window address; 0x2000 = none
    parameter EXTERNAL_BUS = 1'b1               // 1: chip build (headers are the live CPU bus); 0: BASIC sbc build
) (
    input  wire        clk,         // ICE35: 12 MHz from the RP2040 (GPIO24)
    input  wire        uart_rx,     // ICE27 <- RP2040 GPIO0 (UART0 TX)
    output wire        uart_tx,     // ICE25 -> RP2040 GPIO1 (UART0 RX)
    input  wire        btn_n,       // ICE10: SW2, active low = Z80 /RESET
    output wire        led_r,       // RGB LED, active low
    output wire        led_g,
    output wire        led_b,

    // Z80 bus on the headers (pinout in pico-ice.pcf). chip build: CLK in,
    // D bidirectional, the rest out. sbc build: everything out.
    inout  wire        z80_clk,     // chip: input; sbc: output
    inout  wire [7:0]  z80_d,       // chip: bidirectional; sbc: output
    output wire [12:0] z80_a,
    output wire        z80_mreq_n,
    output wire        z80_rd_n,
    output wire        z80_wr_n
);
    // ---- clocks ----
    // Each build has exactly one Z80 clock source and no clock mux in the
    // netlist. The header pins are separate ports (CLK) or per-bit SB_IOs
    // (D), never one mixed-direction vector: yosys otherwise makes the
    // whole vector an output and ties the read-only bit to a constant.
    wire clk_board;     // 12 MHz, always running
    wire clk_z80;       // the Z80 clock
    wire pll_lock;
    generate if (!EXTERNAL_BUS) begin : g_clk_sbc
        wire clk_pll;   // 6 x CORE_HZ
`ifdef SYNTHESIS
        // icepll -i 12 -o 24: DIVR 0, DIVF 63 (VCO 768 MHz), DIVQ 5 (/32).
        // 21 MHz for CORE_HZ 3.5 MHz: DIVF 55 (VCO 672 MHz), same DIVQ.
        localparam [6:0] PLL_DIVF = (CORE_HZ == 3_500_000) ? 7'd55 : 7'd63;
        SB_PLL40_2_PAD #(
            .FEEDBACK_PATH("SIMPLE"),
            .DIVR(4'd0), .DIVF(PLL_DIVF), .DIVQ(3'd5), .FILTER_RANGE(3'd1)
        ) pll (
            .PACKAGEPIN(clk),
            .PLLOUTGLOBALA(clk_board),  // the 12 MHz reference, on a global
            .PLLOUTGLOBALB(clk_pll),    // 24 MHz
            .RESETB(1'b1), .BYPASS(1'b0), .LOCK(pll_lock)
        );
`else
        // Simulation: the PLL is a black box; x1 stand-in (see header).
        assign clk_board = clk;
        assign clk_pll   = clk;
        assign pll_lock  = 1'b1;
`endif
        // /6 toggle divider: 50 % duty (both edges matter).
        reg [1:0] ckdiv   = 2'd0;
        reg       clk_div = 1'b0;
        always @(posedge clk_pll) begin
            if (ckdiv == 2'd2) begin ckdiv <= 2'd0; clk_div <= ~clk_div; end
            else ckdiv <= ckdiv + 1'b1;
        end
`ifdef SYNTHESIS
        SB_GB gb_z80 (.USER_SIGNAL_TO_GLOBAL_BUFFER(clk_div), .GLOBAL_BUFFER_OUTPUT(clk_z80));
`else
        assign clk_z80 = clk_div;
`endif
        assign z80_clk = clk_div;       // CLK header pin: the Z80 clock, out
    end else begin : g_clk_chip
        assign clk_board = clk;
        assign pll_lock  = 1'b1;
`ifdef SYNTHESIS
        // CLK pad straight onto the global network (ICE44 = G6). Weak
        // pull-up: with nothing attached CLK sits high and the CPU stops.
        SB_GB_IO #(.PIN_TYPE(6'b0000_01), .PULLUP(1'b1)) clk_pad (
            .PACKAGE_PIN(z80_clk), .GLOBAL_BUFFER_OUTPUT(clk_z80)
        );
`else
        assign clk_z80 = z80_clk;
`endif
    end endgenerate

    // ---- power-on + SW2 reset ----
    // Counted on the always-running 12 MHz board clock (the Z80 clock may
    // be external and stopped): a press -- or the PLL not yet locked --
    // clears the counter, and board_reset_n stays low until ~5.5 ms after
    // the last bounce. Registered, so the asynchronous reset below never
    // sees a decoder glitch.
    reg [15:0] rst_cnt   = 16'd0;
    reg [1:0]  btn_sync  = 2'b11;
    reg [1:0]  lock_sync = 2'b00;
    reg        board_reset_n = 1'b0;
    always @(posedge clk_board) begin
        btn_sync  <= {btn_sync[0], btn_n};
        lock_sync <= {lock_sync[0], pll_lock};
        if (!btn_sync[1] || !lock_sync[1]) rst_cnt <= 16'd0;   // pressed / PLL not locked
        else if (rst_cnt != 16'hFFFF)      rst_cnt <= rst_cnt + 1'b1;
        board_reset_n <= (rst_cnt == 16'hFFFF);
    end
    // Asynchronous assertion, release synchronized to the Z80 clock:
    // z80_top's first T1 then starts on a clean rising edge.
    reg [1:0] rst_sync = 2'b00;
    always @(posedge clk_z80 or negedge board_reset_n) begin
        if (!board_reset_n) rst_sync <= 2'b00;
        else                rst_sync <= {rst_sync[0], 1'b1};
    end
    wire cpu_reset_n = rst_sync[1];

    // ---- Z80 core ----
    wire [15:0] addr;
    wire [7:0]  data_out;
    wire        data_drive;
    wire        m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    reg  [7:0]  data_in;
    wire        soc_int_n;
    wire        soc_wait_n;

    // chip: no /WAIT, /INT, /NMI, /BUSREQ pins -> tied inactive.
    // sbc: /INT and /WAIT are the ACIA's.
    z80_top cpu (
        .clk        (clk_z80),
        .reset_n    (cpu_reset_n),
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
        .wait_n     (EXTERNAL_BUS ? 1'b1 : soc_wait_n),
        .int_n      (EXTERNAL_BUS ? 1'b1 : soc_int_n),
        .nmi_n      (1'b1),
        .busreq_n   (1'b1),
        .dbg_t      (),
        .dbg_m      ()
    );

    // ---- header drivers ----
    // Address and strobes: outputs in both builds (BUSREQ is tied off, so
    // the core never releases the bus).
    assign z80_a      = addr[12:0];
    assign z80_mreq_n = mreq_n;
    assign z80_rd_n   = rd_n;
    assign z80_wr_n   = wr_n;

    // D: chip = the live data bus, driven only while the CPU drives it;
    // sbc = the byte on the internal bus (the CPU's write data while it
    // drives, otherwise what memory / the ACIA returned).
    wire [7:0] ext_d;   // chip: header D as read by z80_top
    generate if (EXTERNAL_BUS) begin : g_d_chip
`ifdef SYNTHESIS
        genvar i;
        for (i = 0; i < 8; i = i + 1) begin : g_bit
            // output + tristate enable, both combinational; input direct.
            // Weak pull-up: an undriven bus reads 0xFF.
            SB_IO #(.PIN_TYPE(6'b1010_01), .PULLUP(1'b1)) d_pad (
                .PACKAGE_PIN  (z80_d[i]),
                .OUTPUT_ENABLE(data_drive),
                .D_OUT_0      (data_out[i]),
                .D_IN_0       (ext_d[i])
            );
        end
`else
        assign z80_d = data_drive ? data_out : 8'hzz;
        assign ext_d = z80_d;
`endif
    end else begin : g_d_sbc
        assign z80_d = data_drive ? data_out : data_in;
        assign ext_d = 8'hFF;
    end endgenerate

    // ---- sbc memory: 8 KiB ROM (EBR) + 32 KiB RAM (SPRAM) ----
    wire [7:0] mem_dout;
    generate if (!EXTERNAL_BUS) begin : g_mem
        // ROM: inferred block RAM, preloaded at configuration. Writes at
        // or above ROM_PROTECT_TOP land (WR is low across exactly one
        // rising edge, T3); ROM_PROTECT_TOP = 0x2000 leaves no write port.
        localparam ROM_WRITABLE = (ROM_PROTECT_TOP < 16'h2000);
        reg [7:0] rom [0:8191];
        initial $readmemh(ROM_FILE, rom);
        reg [7:0] rom_q;
        always @(posedge clk_z80) begin
            if (ROM_WRITABLE && !mreq_n && !wr_n && data_drive &&
                addr[15:13] == 3'd0 && addr >= ROM_PROTECT_TOP)
                rom[addr[12:0]] <= data_out;
            rom_q <= rom[addr[12:0]];
        end

        // RAM write: WR is low across exactly one rising edge (T3).
        wire        ram_we = !mreq_n && !wr_n && data_drive && addr[15];
        wire [15:0] ram_q;
`ifdef SYNTHESIS
        SB_SPRAM256KA ram (
            .ADDRESS    (addr[14:1]),
            .DATAIN     ({data_out, data_out}),
            .MASKWREN   (addr[0] ? 4'b1100 : 4'b0011),     // nibble enables
            .WREN       (ram_we),
            .CHIPSELECT (1'b1),
            .CLOCK      (clk_z80),
            .STANDBY    (1'b0),
            .SLEEP      (1'b0),
            .POWEROFF   (1'b1),                             // active low
            .DATAOUT    (ram_q)
        );
`else
        // Behavioural SB_SPRAM256KA: synchronous read, nibble-masked write.
        // DATAOUT just holds during a write here (yosys' model makes it X);
        // nothing reads it in the T-state after a write. Zeroed so the
        // simulation is X-free; the real SPRAM powers up arbitrary.
        reg [15:0] spram [0:16383];
        reg [15:0] spram_q = 16'h0000;
        integer k;
        initial for (k = 0; k < 16384; k = k + 1) spram[k] = 16'h0000;
        always @(posedge clk_z80) begin
            if (ram_we) begin
                if (addr[0]) spram[addr[14:1]][15:8] <= data_out;
                else         spram[addr[14:1]][7:0]  <= data_out;
            end else begin
                spram_q <= spram[addr[14:1]];
            end
        end
        assign ram_q = spram_q;
`endif
        // Region of the address the memories registered at this edge.
        reg sel_ram = 1'b0, sel_rom = 1'b0, sel_hi = 1'b0;
        always @(posedge clk_z80) begin
            sel_ram <= addr[15];
            sel_rom <= (addr[15:13] == 3'b000);
            sel_hi  <= addr[0];
        end
        assign mem_dout = sel_ram ? (sel_hi ? ram_q[15:8] : ram_q[7:0]) :
                          sel_rom ? rom_q : 8'hFF;
    end else begin : g_nomem
        assign mem_dout = 8'hFF;
    end endgenerate

    // ---- UART ----
    wire       rx_valid;
    wire [7:0] rx_data;
    reg        tx_strobe;
    reg  [7:0] tx_data;
    wire       tx_busy;
    wire       uart_rx_s;

    uart_rx #(.CLK_HZ(CORE_HZ), .BAUD(BAUD)) urx (
        .clk(clk_z80), .reset_n(cpu_reset_n), .rx(uart_rx), .rx_sync(uart_rx_s),
        .valid(rx_valid), .data(rx_data)
    );
    uart_tx #(.CLK_HZ(CORE_HZ), .BAUD(BAUD)) utx (
        .clk(clk_z80), .reset_n(cpu_reset_n), .strobe(tx_strobe), .data(tx_data),
        .tx(uart_tx), .busy(tx_busy)
    );

    // ---- RX FIFO (32 deep) ----
    reg [7:0] fifo [0:31];
    reg [4:0] f_rd = 0, f_wr = 0;
    reg [5:0] f_cnt = 0;
    wire rx_avail = (f_cnt != 0);
    wire [7:0] fifo_head = fifo[f_rd];

    // ---- ACIA-style I/O decode (see fpga/icepi-zero/top.v) ----
    // IORQ/RD (or WR) assert at the rising edge of T2 and hold through Tw
    // and T3; act once per I/O cycle on the edge of each cycle's own
    // "fully active" condition. INTA (iorq+m1, rd high) never matches
    // iord_active.
    wire iord_active = !iorq_n && !rd_n;
    wire iowr_active = !iorq_n && !wr_n;
    wire inta_active = !iorq_n && !m1_n;
    reg  prev_iord = 0, prev_iowr = 0;
    reg  [7:0] io_dout = 8'hFF;
    wire [7:0] port = addr[7:0];
    wire tx_ready = !tx_busy;

    wire fifo_pop  = iord_active && !prev_iord && (port == 8'h81 || port == 8'h01) && rx_avail;
    wire fifo_push = rx_valid && (f_cnt != 6'd32);

    // TX flow control (see fpga/icepi-zero/top.v): an OUT to the data port
    // hands its byte over on the first rising edge of the cycle that finds
    // the transmitter idle, once per cycle; while the previous byte is
    // still shifting out, /WAIT holds the cycle in Tw.
    wire tx_wr = !EXTERNAL_BUS && iowr_active && (port == 8'h81 || port == 8'h01);
    reg  tx_taken = 1'b0;
    assign soc_wait_n = !(tx_wr && !tx_taken && tx_busy);

    // 68B50 control register: bit7 RIE, bits 6:5 = 01 -> TIE. The
    // NASCOM/z88dk ACIA driver's TX is interrupt-driven, so TIE must
    // really work (a status-only shim deadlocks TX at real baud).
    reg [7:0] acia_ctrl = 8'h00;
    wire rie = acia_ctrl[7];
    wire tie = (acia_ctrl[6:5] == 2'b01);

    always @(posedge clk_z80) begin
        if (!cpu_reset_n) begin
            f_rd <= 0; f_wr <= 0; f_cnt <= 0;
            prev_iord <= 0; prev_iowr <= 0;
            tx_strobe <= 0; tx_taken <= 0;
            acia_ctrl <= 8'h00;
        end else begin
            prev_iord <= iord_active;
            prev_iowr <= iowr_active;
            tx_strobe <= 1'b0;

            if (!EXTERNAL_BUS && fifo_push) begin
                fifo[f_wr] <= rx_data;
                f_wr <= f_wr + 1'b1;
            end
            if (!EXTERNAL_BUS && fifo_pop) f_rd <= f_rd + 1'b1;
            case ({!EXTERNAL_BUS && fifo_push, !EXTERNAL_BUS && fifo_pop})
                2'b10: f_cnt <= f_cnt + 1'b1;
                2'b01: f_cnt <= f_cnt - 1'b1;
                default: ;
            endcase

            if (!EXTERNAL_BUS && iord_active && !prev_iord) begin
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
            if (!EXTERNAL_BUS && iowr_active && !prev_iowr && port == 8'h80)
                acia_ctrl <= data_out;       // divisor/format bits ignored
        end
    end

    // 68B50 IRQ: RX byte pending (if RIE) or TX empty (if TIE).
    assign soc_int_n = ((rie && rx_avail) || (tie && tx_ready)) ? 1'b0 : 1'b1;

    // Data-bus input mux. INTA in IM 1 ignores the bus; 0xFF = RST 38h.
    always @(*) begin
        if (EXTERNAL_BUS)     data_in = ext_d;
        else if (inta_active) data_in = 8'hFF;
        else if (iord_active) data_in = io_dout;
        else                  data_in = mem_dout;
    end

    // ---- RGB LED ----
    reg [23:0] beat = 0;
    always @(posedge clk_board) beat <= beat + 1'b1;
    wire led_g_on = beat[23];                                   // ~0.7 s on / off
    wire led_b_on = EXTERNAL_BUS ? !mreq_n                      // bus activity
                                 : (!uart_tx || !uart_rx_s);    // serial activity
    wire led_r_on = !cpu_reset_n || !halt_n;
`ifdef SYNTHESIS
    // RGB0..2 are the UP5K's constant-current LED sinks; the board's 1k
    // series resistors set the actual current, the driver's 2 mA limit
    // (half-current mode, one step) never binds.
    SB_RGBA_DRV #(
        .CURRENT_MODE("0b1"),
        .RGB0_CURRENT("0b000001"),
        .RGB1_CURRENT("0b000001"),
        .RGB2_CURRENT("0b000001")
    ) rgb (
        .CURREN(1'b1), .RGBLEDEN(1'b1),
        .RGB0PWM(led_g_on), .RGB1PWM(led_b_on), .RGB2PWM(led_r_on),
        .RGB0(led_g), .RGB1(led_b), .RGB2(led_r)
    );
`else
    assign led_g = !led_g_on;
    assign led_b = !led_b_on;
    assign led_r = !led_r_on;
`endif
endmodule
