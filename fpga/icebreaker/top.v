// top.v - RZ80 for the iCEBreaker (Lattice iCE40UP5K-SG48)
//
// Two builds from this one file, selected by EXTERNAL_BUS (see Makefile:
// `make sbc`, `make chip`):
//
//   EXTERNAL_BUS=0  "sbc": a BASIC single-board computer. NASCOM BASIC 4.7
//     (RC2014 build) in an 8 KiB block-RAM ROM, 32 KiB of SPRAM, and a
//     68B50-style ACIA on the FT2232H's second channel (the board's USB
//     serial port) at 115200 8N1; Z80 clock 4 MHz from the PLL. The PMODs
//     carry the bus as OUTPUTS in the chip-build pinout below (CLK
//     included), so a logic analyser sees what a Z80 would drive.
//
//   EXTERNAL_BUS=1  "chip": a Z80 chip replacement. The PMODs are the live
//     CPU bus; CLK comes IN on P1A10 and is the clock of z80_top; memory
//     and peripherals are whatever is wired to the PMODs. The USB UART is
//     unused (TX idles high).
//
// PMOD pinout. The three PMODs have 24 user I/O, so they carry the first
// 24 signals of the pinout order shared by all our boards (strobes first,
// then as much address bus as fits): CLK, D0-D7, /MREQ, /RD, /WR, A0-A11.
// One byte per PMOD (table in icebreaker.pcf):
//
//   P1A  A8-A11, /MREQ, /RD, /WR, CLK (P1A10 = pin 44, a GBIN)
//   P1B  D0-D7
//   P2   A0-A7   (the snap-off button/LED board's PMOD: snap it off for
//                 the chip build)
//
// A12-A15, /IORQ, /M1, /RFSH, /HALT, /BUSACK are not brought out (the chip
// build is a 4 KiB-window memory-only Z80); /WAIT, /INT, /NMI, /BUSREQ are
// tied inactive; /RESET is the user button. All PMOD I/O is 3.3 V and NOT
// 5 V tolerant: use level translators towards a 5 V Z80 system.
//
// Clocking. The Z80 clock is a real clock, one T-state per period:
// z80_top keeps all core state on its rising edge and reproduces the
// chip's falling-edge pin behaviour (MREQ/RD/WR edges, read-data / WAIT /
// INT / NMI sampling) with native negative-edge flops (docs/clocking.md).
// No 2x clock, no clock enable, nothing resamples the Z80 clock.
//
//   - sbc: the iCE40 PLL cannot go below 16 MHz, so 4 MHz is the PLL plus
//     a fabric divider: 12 MHz (pin 35) -> SB_PLL40_2_PAD -> 48 MHz ->
//     /12 toggle divider (6 PLL periods high, 6 low: exact 50 % duty) ->
//     SB_GB -> 4.000 MHz on a global net. CORE_HZ = 3_500_000 picks a
//     42 MHz PLL output instead (3.500 MHz). The PLL's port A passes the
//     12 MHz reference through onto a second global net, which clocks the
//     reset stretch and the heartbeat. In simulation the PLL is a black
//     box: the testbench clock stands in for its 48 MHz output (and for
//     the 12 MHz domain), so the real /12 divider runs and CORE_HZ needs
//     no override.
//   - chip: CLK on P1A10 (pin 44, a global-buffer input pin) enters the
//     global network through SB_GB_IO and clocks z80_top directly. Every
//     rising edge is one T-state; read data must be valid around the
//     clock edge the Z80 samples it on (rising edge ending T2 for M1, the
//     falling edge of T3 for MRD), the Z80's own setup/hold contract --
//     with one difference: the core decodes the M1 opcode in the same
//     clock, so it needs ~58 ns of setup (nextpnr), a Z80A 35 ns. Holding
//     CLK stops the CPU, as on a CMOS Z80.
//
// RESET: power-on + the user button (pin 10, active low), stretched ~5 ms
// on the 12 MHz clock, asynchronous assertion, release synchronized to the
// Z80 clock so the first T1 always starts on a clean rising edge. (In the
// chip build that release needs CLK edges; the red LED shows a Z80 that is
// held in reset or halted.)
//
// sbc memory map (NASCOM BASIC 4.7 for the RC2014: 8 KiB ROM at 0x0000,
// RAM at 0x8000-0xFFFF):
//   0x0000-0x1FFF  ROM: 16 EBR (SB_RAM40_4K), preloaded from ROM_FILE at
//                  configuration. Writes at or above ROM_PROTECT_TOP land.
//                  The Makefile sets it to the end of the ROM image rounded
//                  up to 1 KiB: 0x2000 for NASCOM BASIC (none, a true ROM),
//                  0x0400 for the 1K Tiny BASIC (its variables and stack
//                  live at 0x0400-0x07FF).
//   0x2000-0x7FFF  unmapped: reads 0xFF (open bus), writes ignored
//   0x8000-0xFFFF  RAM: one SB_SPRAM256KA (16K x 16), byte writes through
//                  MASKWREN. SPRAM cannot be preloaded and powers up with
//                  undefined contents (BASIC's cold start does not care).
// Both memories read synchronously, one T-state of latency, which the Z80
// bus timing absorbs without wait states (details at the ROM below).
//
// sbc I/O map and interrupt semantics are identical to the IcePi port
// (branch fpga-icepi-zero-basic): ACIA status/control at 0x80, data at 0x81
// (Tiny BASIC's 0x00/0x01 decoded too), /INT = (RIE & RDRF) | (TIE & TDRE),
// IM 1 (INTA reads 0xFF = RST 38h). A TX write that finds the transmitter
// busy holds the CPU with /WAIT until the previous byte is out (Tiny BASIC
// writes without polling TDRE), so no byte is dropped.
//
// Board: 12 MHz oscillator on pin 35; FT2232H channel B UART: FPGA RX
// pin 6, TX pin 9; user button pin 10 (active low); red LED pin 11 and
// green LED pin 37 (active low).
//   green: heartbeat (~1.4 Hz, the 12 MHz domain is alive)
//   red:   sbc: serial activity (TX or RX); chip: Z80 in reset or halted

module top #(
    parameter CORE_HZ  = 4_000_000,             // sbc Z80 clock: 4_000_000 or 3_500_000 (PLL 48 / 42 MHz, /12)
    parameter BAUD     = 115_200,
    parameter ROM_FILE = "rom.mem",             // 8 KiB image for 0x0000-0x1FFF (hex2mem.py ... 8192)
    parameter ROM_PROTECT_TOP = 16'h2000,       // NASCOM 4.7 ROM is 0x0000-0x1FD9; writes below are dropped
    parameter EXTERNAL_BUS = 1'b1               // 1: chip build (PMODs are the live CPU bus); 0: BASIC sbc build
) (
    input  wire        clk,         // 12 MHz oscillator (pin 35)
    input  wire        uart_rx,     // FT2232H channel B -> FPGA serial (pin 6)
    output wire        uart_tx,     // FPGA -> FT2232H channel B serial (pin 9)
    input  wire        btn_n,       // user button, active low (pin 10)
    output wire        ledr_n,      // red LED, active low (pin 11)
    output wire        ledg_n,      // green LED, active low (pin 37)

    // Z80 bus on the PMODs (pinout above and in icebreaker.pcf). chip
    // build: CLK in, D bidirectional, the rest out. sbc build: all out.
    inout  wire        z80_clk,     // P1A10: chip: CLK input; sbc: the Z80 clock out
    inout  wire [7:0]  z80_d,       // P1B
    output wire        z80_mreq_n,  // P1A7
    output wire        z80_rd_n,    // P1A8
    output wire        z80_wr_n,    // P1A9
    output wire [11:0] z80_a        // A0-A7 on P2, A8-A11 on P1A1-P1A4
);
    // ---- clocks ----
    // Each build has exactly one Z80 clock source and no clock mux in the
    // netlist. (Inout pins are never assigned a constant 1'bz: yosys folds
    // that into an undriven net and then optimizes everything clocked by
    // it away. The chip build's CLK pin is an SB_GB_IO input instead.)
    wire clk12;         // 12 MHz domain: reset stretch, heartbeat
    wire clk_z80;       // the Z80 clock, on a global net
    wire pll_lock;

    generate if (!EXTERNAL_BUS) begin : g_clk_sbc
        localparam [6:0] PLL_DIVF = (CORE_HZ == 3_500_000) ? 7'd55 : 7'd63;
        wire clk_pll;   // 12 x CORE_HZ
`ifdef SYNTHESIS
        // `icepll -i 12 -o 48`: DIVR 0, DIVF 63, DIVQ 4 -> VCO 768 MHz,
        // /16 = 48 MHz (`-o 42`: DIVF 55 -> VCO 672 MHz, 42 MHz). The
        // 2_PAD variant also hands the pad's own 12 MHz to the fabric
        // (port A): pin 35 is the PLL's dedicated input pad, so it cannot
        // be a plain clock input as well. Port B leaves through the input
        // path of pin 37 (the green LED), which therefore stays an output.
        SB_PLL40_2_PAD #(
            .FEEDBACK_PATH("SIMPLE"),
            .PLLOUT_SELECT_PORTB("GENCLK"),
            .DIVR(4'd0),
            .DIVF(PLL_DIVF),
            .DIVQ(3'd4),
            .FILTER_RANGE(3'd1)
        ) pll (
            .PACKAGEPIN     (clk),
            .PLLOUTGLOBALA  (clk12),        // 12 MHz pass-through
            .PLLOUTGLOBALB  (clk_pll),      // 48 MHz
            .PLLOUTCOREA    (),
            .PLLOUTCOREB    (),
            .EXTFEEDBACK    (1'b0),
            .DYNAMICDELAY   (8'd0),
            .LOCK           (pll_lock),
            .BYPASS         (1'b0),
            .RESETB         (1'b1),
            .LATCHINPUTVALUE(1'b0),
            .SDO            (),
            .SDI            (1'b0),
            .SCLK           (1'b0)
        );
`else
        // Simulation stand-in: the testbench clock is the PLL output.
        assign clk12    = clk;
        assign clk_pll  = clk;
        assign pll_lock = 1'b1;
`endif
        // /12 toggle divider: clk_div flips every 6th PLL rising edge, so
        // both halves of the Z80 clock are exactly 6 PLL periods (both
        // edges matter to z80_top). One global buffer carries it to the
        // core, the memories and the SoC; the fabric copy drives the CLK pin.
        reg [2:0] ckdiv   = 3'd0;
        reg       clk_div = 1'b0;
        always @(posedge clk_pll) begin
            if (ckdiv == 3'd5) begin ckdiv <= 3'd0; clk_div <= ~clk_div; end
            else ckdiv <= ckdiv + 3'd1;
        end
`ifdef SYNTHESIS
        SB_GB zclk_gb (
            .USER_SIGNAL_TO_GLOBAL_BUFFER(clk_div),
            .GLOBAL_BUFFER_OUTPUT        (clk_z80)
        );
`else
        assign clk_z80 = clk_div;
`endif
        assign z80_clk = clk_div;           // P1A10 mirrors the Z80 clock
    end else begin : g_clk_chip
        assign clk12    = clk;              // no PLL in the chip build
        assign pll_lock = 1'b1;
`ifdef SYNTHESIS
        // Pin 44 is a GBIN: the pad drives the global network directly.
        // The weak pull-up keeps an unconnected CLK pin quiet (CPU stopped).
        SB_GB_IO #(
            .PIN_TYPE(6'b0000_01),          // no output, plain input
            .PULLUP(1'b1)
        ) zclk_pad (
            .PACKAGE_PIN         (z80_clk),
            .GLOBAL_BUFFER_OUTPUT(clk_z80)
        );
`else
        assign clk_z80 = z80_clk;
`endif
    end endgenerate

    // ---- power-on + button reset ----
    // One up-counter on the always-running 12 MHz clock (the Z80 clock may
    // be external and stopped): it starts at 0 after configuration
    // (power-on reset) and is cleared while the button is held or the PLL
    // is unlocked, then must count out 2^16 cycles (~5.5 ms) after release
    // before board_reset_n deasserts. The stretch swallows switch bounce.
    // board_reset_n is a flop, so the async reset below never sees a
    // decoder glitch.
    reg [15:0] rst_cnt       = 16'd0;
    reg [1:0]  btn_sync      = 2'b00;
    reg        board_reset_n = 1'b0;
    always @(posedge clk12) begin
        btn_sync <= {btn_sync[0], btn_n && pll_lock};
        if (!btn_sync[1])              rst_cnt <= 16'd0;    // pressed / no PLL
        else if (rst_cnt != 16'hFFFF)  rst_cnt <= rst_cnt + 16'd1;
        board_reset_n <= btn_sync[1] && (rst_cnt == 16'hFFFF);
    end
    // Asynchronous assertion, release synchronized to the Z80 clock, so
    // z80_top's first T1 starts on a clean rising edge.
    reg [1:0] rst_sync = 2'b00;
    always @(posedge clk_z80 or negedge board_reset_n) begin
        if (!board_reset_n) rst_sync <= 2'b00;
        else                rst_sync <= {rst_sync[0], 1'b1};
    end
    wire cpu_reset_n = rst_sync[1];

    // ---- Z80 ----
    wire [15:0] addr;
    wire [7:0]  data_out;
    wire        data_drive;
    wire        m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    wire [7:0]  data_in;
    wire [7:0]  pad_d;          // chip: the D pins as read
    wire [7:0]  soc_dout;       // sbc: memory / ACIA / INTA byte for the CPU
    wire        soc_int_n;      // sbc: the ACIA's /INT
    wire        soc_wait_n;     // sbc: the ACIA's TX flow control
    wire        uart_rx_s;      // sbc: synchronized RX, for the activity LED

    // Inputs not brought out are tied inactive; in the chip build the D
    // pins go to z80_top unsynchronized, as on a real Z80 (its falling-
    // and rising-edge samplers are the contract).
    assign data_in = EXTERNAL_BUS ? pad_d : soc_dout;

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

    // ---- PMOD drivers ----
    // chip: D is driven only while the CPU drives it (SB_IO tristate, OE =
    // data_drive), with weak pull-ups so an undriven bus reads 0xFF.
    // sbc: D shows the byte on the internal bus (the CPU's write data
    // while it drives, otherwise what memory / the ACIA returned).
    // BUSREQ is tied off, so BUSACK never releases the bus.
    generate if (EXTERNAL_BUS) begin : g_pmod_chip
`ifdef SYNTHESIS
        genvar i;
        for (i = 0; i < 8; i = i + 1) begin : g_d
            SB_IO #(
                .PIN_TYPE(6'b1010_01),      // tristate output (OE), plain input
                .PULLUP(1'b1)
            ) d_pad (
                .PACKAGE_PIN  (z80_d[i]),
                .OUTPUT_ENABLE(data_drive),
                .D_OUT_0      (data_out[i]),
                .D_IN_0       (pad_d[i])
            );
        end
`else
        assign z80_d = data_drive ? data_out : 8'hzz;
        assign pad_d = z80_d;
`endif
    end else begin : g_pmod_sbc
        assign z80_d = data_drive ? data_out : data_in;
        assign pad_d = 8'hFF;
    end endgenerate
    assign z80_a      = addr[11:0];
    assign z80_mreq_n = mreq_n;
    assign z80_rd_n   = rd_n;
    assign z80_wr_n   = wr_n;

    // ---- the SoC: ROM, RAM, ACIA (sbc build only) ----
    generate if (!EXTERNAL_BUS) begin : g_soc
        // Writes: WR is low across the rising edge of T3 only (it asserts
        // at T2's falling edge and releases at T3's), so each write fires
        // once.
        wire mem_wr = !mreq_n && !wr_n && data_drive;

        // -- ROM: 8 KiB of block RAM, image loaded with the bitstream --
        // Synchronous read, one T-state of latency: the address is on the
        // pins from the rising edge of T1, the EBR registers it at T2, and
        // the core takes the opcode at the rising edge after T2 (M1) or
        // the falling edge of T3 (memory read). ROM_PROTECT_TOP < 0x2000
        // turns the top of the window into RAM (e.g. 0x0400 for Tiny BASIC).
        localparam ROM_WRITABLE = (ROM_PROTECT_TOP < 16'h2000);
        reg [7:0] rom [0:8191];
        initial $readmemh(ROM_FILE, rom);
        reg [7:0] rom_dout;
        always @(posedge clk_z80) begin
            if (ROM_WRITABLE && mem_wr && addr[15:13] == 3'd0 && addr >= ROM_PROTECT_TOP)
                rom[addr[12:0]] <= data_out;
            rom_dout <= rom[addr[12:0]];
        end

        // -- RAM: 32 KiB SPRAM at 0x8000, 16-bit words, byte lanes by A0 --
        wire        ram_we = mem_wr && addr[15];
        wire [15:0] ram_q;
`ifdef SYNTHESIS
        SB_SPRAM256KA spram (
            .ADDRESS   (addr[14:1]),
            .DATAIN    ({data_out, data_out}),
            .MASKWREN  (addr[0] ? 4'b1100 : 4'b0011),   // nibble write enables
            .WREN      (ram_we),
            .CHIPSELECT(1'b1),
            .CLOCK     (clk_z80),
            .STANDBY   (1'b0),
            .SLEEP     (1'b0),
            .POWEROFF  (1'b1),                          // active low: powered
            .DATAOUT   (ram_q)
        );
`else
        // Behavioural SPRAM: same read latency, byte-lane writes, output
        // held through a write (the real DATAOUT is undefined then; the
        // CPU never samples it in a write cycle). Zeroed for X-free sims.
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

        // -- read mux, registered alongside the memories --
        reg [1:0] rd_sel = 2'd0;    // 2: RAM, 1: ROM, 0: unmapped
        reg       rd_hi  = 1'b0;    // RAM byte lane
        always @(posedge clk_z80) begin
            rd_sel <= addr[15] ? 2'd2 : (addr[15:13] == 3'd0) ? 2'd1 : 2'd0;
            rd_hi  <= addr[0];
        end
        wire [7:0] mem_dout = rd_sel[1] ? (rd_hi ? ram_q[15:8] : ram_q[7:0])
                            : rd_sel[0] ? rom_dout : 8'hFF;

        // -- UART --
        wire       rx_valid;
        wire [7:0] rx_data;
        reg        tx_strobe;
        reg  [7:0] tx_data;
        wire       tx_busy;

        uart_rx #(.CLK_HZ(CORE_HZ), .BAUD(BAUD)) urx (
            .clk(clk_z80), .reset_n(cpu_reset_n), .rx(uart_rx), .rx_sync(uart_rx_s),
            .valid(rx_valid), .data(rx_data)
        );
        uart_tx #(.CLK_HZ(CORE_HZ), .BAUD(BAUD)) utx (
            .clk(clk_z80), .reset_n(cpu_reset_n), .strobe(tx_strobe), .data(tx_data),
            .tx(uart_tx), .busy(tx_busy)
        );

        // -- RX FIFO (32 deep) --
        reg [7:0] fifo [0:31];
        reg [4:0] f_rd = 0, f_wr = 0;
        reg [5:0] f_cnt = 0;
        wire rx_avail = (f_cnt != 0);
        wire [7:0] fifo_head = fifo[f_rd];

        // -- ACIA-style I/O decode (as in the IcePi Zero port) --
        // IORQ/RD (or WR) assert at the rising edge of T2 and hold through
        // Tw and T3, so each is seen on two or more rising edges; act once
        // per I/O cycle by edge-detecting each cycle's own "fully active"
        // condition. The core latches read data at T3's falling edge, one
        // T-state after io_dout settles. INTA (iorq+m1, rd high) never
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

        // TX: an OUT to the data port hands its byte to the transmitter on
        // the first rising edge of the cycle that finds it idle (tx_taken:
        // once per cycle). NASCOM BASIC checks TDRE first, so that is the
        // cycle's first edge. Tiny BASIC writes without looking, faster
        // than the line drains: while the previous byte is still shifting
        // out, /WAIT holds the cycle in Tw (sampled at the falling edge of
        // Tw; IORQ and WR are low from T2, 1.5 clocks earlier).
        wire tx_wr = iowr_active && (port == 8'h81 || port == 8'h01);
        reg  tx_taken = 1'b0;
        assign soc_wait_n = !(tx_wr && !tx_taken && tx_busy);

        // 68B50 control register: bit7 RIE (RX-interrupt enable), bits 6:5
        // = 01 -> TIE (TX interrupt on TDRE). The NASCOM/z88dk ACIA driver
        // uses interrupt-driven TX: when it sees TDRE=0 it queues the byte
        // and *waits for a TDRE interrupt*, so TIE must really work -- a
        // status-only shim deadlocks the ROM's TX ring buffer at real baud.
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
        assign soc_int_n = ((rie && rx_avail) || (tie && tx_ready)) ? 1'b0 : 1'b1;

        // Data-bus input mux. INTA in IM 1 ignores the bus; 0xFF (= RST 38h)
        // is also the correct IM 0 behaviour of an open bus.
        assign soc_dout = inta_active ? 8'hFF : iord_active ? io_dout : mem_dout;
    end else begin : g_nosoc
        assign soc_dout  = 8'hFF;
        assign soc_int_n = 1'b1;
        assign soc_wait_n = 1'b1;
        assign uart_rx_s = 1'b1;
        assign uart_tx   = 1'b1;    // idle
    end endgenerate

    // ---- LEDs ----
    reg [22:0] beat = 23'd0;
    always @(posedge clk12) beat <= beat + 1'b1;
    assign ledg_n = ~beat[22];                      // ~1.4 Hz heartbeat
    assign ledr_n = EXTERNAL_BUS ? (cpu_reset_n && halt_n)   // lit: Z80 in reset or halted
                                 : (uart_tx && uart_rx_s);   // lit: serial activity
endmodule
