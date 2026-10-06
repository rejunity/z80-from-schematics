// top.v - RZ80 SoC for the Sipeed Tang Primer 25K (Gowin GW5A-LV25MG121)
//
// By default the rz80 core is presented as an external, Z80-like bus on the
// dock's 2x20 J3 header.  Set EXTERNAL_BUS=0 for the original NASCOM BASIC
// SoC (64 KiB internal RAM + BL616 USB-UART at 115200 8N1), as the Verilator
// smoke test does.  The header bus uses 3.3 V signalling and is not 5 V
// tolerant; use level translators before connecting it to 5 V Z80 hardware.
//
// Board details:
//
//   - 50 MHz crystal on E2. A PLLA generates a 30 MHz sampling clock for
//     external-bus mode and the 20 MHz core clock for internal-SoC mode.
//     The sampler detects both edges of external Z80 CLK and enables one
//     core phase per edge; it does not free-run the CPU. The internal-SoC
//     clock is 2x its Z80 rate, so 20 MHz gives a 10 MHz CPU. A PLL, not a
//     divider, is used because a
//     fabric-FF clock never reaches the BSRAM CLK pins skew-free on
//     GW5A (-2.9 ns => hold violations); PLLA output is a true global
//     clock. In simulation the PLLA is a black box, so a /4 divider
//     stands in and the Makefile overrides CORE_HZ to match.
//   - UART to the BL616: FPGA RX = B3, FPGA TX = C3
//   - two push-buttons on H10/H11 with pull-DOWNs -> ACTIVE-HIGH
//     (IcePi's are active-low); key[0] (H10) is the Z80 reset
//   - the dock has no user LEDs, so no led port
//
// z80_clk is an external INPUT, like CLK on a physical Z80. A 30 MHz internal
// sampling clock synchronizes it and advances one core phase on every detected
// rising or falling edge. Holding z80_clk stops CPU execution. External CLK
// should not exceed 10 MHz, ensuring each half-cycle is observed reliably.
//
// In EXTERNAL_BUS=0 mode, the memory map / I/O map / interrupt semantics
// are identical to the IcePi port: 64 KiB BRAM with the ROM preloaded and
// write-protected below 0x2000, 68B50 ACIA shim at ports 0x80/0x81 (Tiny
// BASIC's 0x00/0x01 too), /INT = (RIE & RDRF) | (TIE & TDRE).

module top #(
    parameter CORE_HZ  = 20_000_000,            // PLLA output; sim overrides to 12_500_000
    parameter BAUD     = 115_200,
    parameter ROM_FILE = "rom.mem",
    parameter ROM_PROTECT_TOP = 16'h2000,       // NASCOM 4.7 ROM is 0x0000-0x1FD9
    parameter EXTERNAL_BUS = 1'b1               // 1: J3 is the live CPU bus; 0: internal BASIC SoC
) (
    input  wire       clk,       // 50 MHz crystal (E2)
    input  wire       uart_rx,   // BL616 -> FPGA serial (B3)
    output wire       uart_tx,   // FPGA -> BL616 serial (C3)
    input  wire [1:0] key,       // push-buttons, ACTIVE HIGH (H10, H11)

    // Z80-compatible external bus on J3. Address/control/data drivers release
    // while the core's internal BUSACK is asserted, and z80_d is also released
    // on reads. BUSACK itself is not exposed: its former K8 header pin is used
    // as a replacement for the faulty B2/D4 connection.
    input  wire        z80_clk,
    output wire [15:0] z80_a,
    inout  wire [7:0]  z80_d,
    output wire        z80_m1_n,
    output wire        z80_mreq_n,
    output wire        z80_iorq_n,
    output wire        z80_rd_n,
    output wire        z80_wr_n,
    output wire        z80_rfsh_n,
    output wire        z80_halt_n,
    input  wire        z80_wait_n,
    input  wire        z80_int_n,
    input  wire        z80_nmi_n,
    input  wire        z80_busreq_n,
    input  wire        z80_reset_n
);
    // ---- core clock ----
`ifdef SYNTHESIS
    // PLLA: VCO = 50 MHz x MDIV 18 = 900 MHz; CLKOUT0 = VCO / ODIV0
    // = 900 / 45 = 20 MHz. Instantiation and parameter boilerplate
    // follow apicula's examples/gw5a/pll7.v (YosysHQ/apicula, MIT).
    wire clk_cpu;
    wire clk_sampler;
    wire pll_lock;
    wire gw_gnd = 1'b0;

    PLLA PLLA_inst (
        .LOCK(pll_lock),
        .CLKOUT0(clk_cpu),
        .CLKOUT1(clk_sampler), .CLKOUT2(), .CLKOUT3(), .CLKOUT4(), .CLKOUT5(), .CLKOUT6(),
        .CLKFBOUT(),
        .MDRDO(),
        .CLKIN(clk),
        .CLKFB(gw_gnd),
        .RESET(gw_gnd),
        .PLLPWD(gw_gnd),
        .RESET_I(gw_gnd),
        .RESET_O(gw_gnd),
        .PSSEL({gw_gnd,gw_gnd,gw_gnd}),
        .PSDIR(gw_gnd),
        .PSPULSE(gw_gnd),
        .SSCPOL(gw_gnd),
        .SSCON(gw_gnd),
        .SSCMDSEL({gw_gnd,gw_gnd,gw_gnd,gw_gnd,gw_gnd,gw_gnd,gw_gnd}),
        .SSCMDSEL_FRAC({gw_gnd,gw_gnd,gw_gnd}),
        .MDCLK(gw_gnd),
        .MDOPC({gw_gnd,gw_gnd}),
        .MDAINC(gw_gnd),
        .MDWDI({gw_gnd,gw_gnd,gw_gnd,gw_gnd,gw_gnd,gw_gnd,gw_gnd,gw_gnd})
    );

    defparam PLLA_inst.FCLKIN = "50";
    defparam PLLA_inst.IDIV_SEL = 1;
    defparam PLLA_inst.FBDIV_SEL = 1;
    defparam PLLA_inst.MDIV_SEL = 18;           // VCO = 900 MHz
    defparam PLLA_inst.MDIV_FRAC_SEL = 0;
    defparam PLLA_inst.ODIV0_SEL = 45;          // 900 / 45 = 20 MHz
    defparam PLLA_inst.ODIV0_FRAC_SEL = 0;
    // Unused outputs still need explicit ODIVs: apycula's defaults for
    // A_ODIV1..6_SEL are the string '8', which its own binary parser
    // rejects (gowin_pack ValueError). Values are don't-care (EN=FALSE).
    defparam PLLA_inst.ODIV1_SEL = 30;           // 900 / 30 = 30 MHz CLK sampler
    defparam PLLA_inst.ODIV2_SEL = 45;
    defparam PLLA_inst.ODIV3_SEL = 45;
    defparam PLLA_inst.ODIV4_SEL = 45;
    defparam PLLA_inst.ODIV5_SEL = 45;
    defparam PLLA_inst.ODIV6_SEL = 45;
    defparam PLLA_inst.CLKOUT0_EN = "TRUE";
    defparam PLLA_inst.CLKOUT1_EN = "TRUE";
    defparam PLLA_inst.CLKOUT2_EN = "FALSE";
    defparam PLLA_inst.CLKOUT3_EN = "FALSE";
    defparam PLLA_inst.CLKOUT4_EN = "FALSE";
    defparam PLLA_inst.CLKOUT5_EN = "FALSE";
    defparam PLLA_inst.CLKOUT6_EN = "FALSE";
    defparam PLLA_inst.CLKFB_SEL = "INTERNAL";
    defparam PLLA_inst.CLKOUT0_DT_DIR = 1'b1;
    defparam PLLA_inst.CLKOUT1_DT_DIR = 1'b1;
    defparam PLLA_inst.CLKOUT2_DT_DIR = 1'b1;
    defparam PLLA_inst.CLKOUT3_DT_DIR = 1'b1;
    defparam PLLA_inst.CLKOUT0_DT_STEP = 0;
    defparam PLLA_inst.CLKOUT1_DT_STEP = 0;
    defparam PLLA_inst.CLKOUT2_DT_STEP = 0;
    defparam PLLA_inst.CLKOUT3_DT_STEP = 0;
    defparam PLLA_inst.CLK0_IN_SEL = 1'b0;
    defparam PLLA_inst.CLK0_OUT_SEL = 1'b0;
    defparam PLLA_inst.CLK1_IN_SEL = 1'b0;
    defparam PLLA_inst.CLK1_OUT_SEL = 1'b0;
    defparam PLLA_inst.CLK2_IN_SEL = 1'b0;
    defparam PLLA_inst.CLK2_OUT_SEL = 1'b0;
    defparam PLLA_inst.CLK3_IN_SEL = 1'b0;
    defparam PLLA_inst.CLK3_OUT_SEL = 1'b0;
    defparam PLLA_inst.CLK4_IN_SEL = 2'b00;
    defparam PLLA_inst.CLK4_OUT_SEL = 1'b0;
    defparam PLLA_inst.CLK5_IN_SEL = 1'b0;
    defparam PLLA_inst.CLK5_OUT_SEL = 1'b0;
    defparam PLLA_inst.CLK6_IN_SEL = 1'b0;
    defparam PLLA_inst.CLK6_OUT_SEL = 1'b0;
    defparam PLLA_inst.DYN_DPA_EN = "FALSE";
    defparam PLLA_inst.CLKOUT0_PE_COARSE = 0;
    defparam PLLA_inst.CLKOUT0_PE_FINE = 0;
    defparam PLLA_inst.CLKOUT1_PE_COARSE = 0;
    defparam PLLA_inst.CLKOUT1_PE_FINE = 0;
    defparam PLLA_inst.CLKOUT2_PE_COARSE = 0;
    defparam PLLA_inst.CLKOUT2_PE_FINE = 0;
    defparam PLLA_inst.CLKOUT3_PE_COARSE = 0;
    defparam PLLA_inst.CLKOUT3_PE_FINE = 0;
    defparam PLLA_inst.CLKOUT4_PE_COARSE = 0;
    defparam PLLA_inst.CLKOUT4_PE_FINE = 0;
    defparam PLLA_inst.CLKOUT5_PE_COARSE = 0;
    defparam PLLA_inst.CLKOUT5_PE_FINE = 0;
    defparam PLLA_inst.CLKOUT6_PE_COARSE = 0;
    defparam PLLA_inst.CLKOUT6_PE_FINE = 0;
    defparam PLLA_inst.DYN_PE0_SEL = "FALSE";
    defparam PLLA_inst.DYN_PE1_SEL = "FALSE";
    defparam PLLA_inst.DYN_PE2_SEL = "FALSE";
    defparam PLLA_inst.DYN_PE3_SEL = "FALSE";
    defparam PLLA_inst.DYN_PE4_SEL = "FALSE";
    defparam PLLA_inst.DYN_PE5_SEL = "FALSE";
    defparam PLLA_inst.DYN_PE6_SEL = "FALSE";
    defparam PLLA_inst.DE0_EN = "FALSE";
    defparam PLLA_inst.DE1_EN = "FALSE";
    defparam PLLA_inst.DE2_EN = "FALSE";
    defparam PLLA_inst.DE3_EN = "FALSE";
    defparam PLLA_inst.DE4_EN = "FALSE";
    defparam PLLA_inst.DE5_EN = "FALSE";
    defparam PLLA_inst.DE6_EN = "FALSE";
    defparam PLLA_inst.RESET_I_EN = "FALSE";
    defparam PLLA_inst.RESET_O_EN = "FALSE";
    defparam PLLA_inst.ICP_SEL = 6'bXXXXXX;
    defparam PLLA_inst.LPF_RES = 3'bXXX;
    defparam PLLA_inst.LPF_CAP = 2'b00;
    defparam PLLA_inst.SSC_EN = "FALSE";
`else
    // Simulation stand-in: /4 divider (12.5 MHz). Build sims with
    // -GCORE_HZ=12500000 so the UART divisors match.
    reg [1:0] ckdiv = 0;
    always @(posedge clk) ckdiv <= ckdiv + 1'b1;
    wire clk_cpu = ckdiv[1];
    wire clk_sampler = clk;
    wire pll_lock = 1'b1;
`endif

    // ---- power-on + key[0] reset (core clk domain) ----
    // Same debounced stretch as the IcePi port: any press clears the
    // counter and reset_n stays low until ~5 ms after the last bounce.
    // Buttons are active high here (board pull-downs).
    reg [15:0] rst_cnt  = 16'd0;
    reg [1:0]  btn_sync = 2'b00;
    always @(posedge clk_cpu) begin
        btn_sync <= {btn_sync[0], key[0]};
        if (btn_sync[1] || !pll_lock)  rst_cnt <= 16'd0;      // pressed / PLL not locked
        else if (rst_cnt != 16'hFFFF)  rst_cnt <= rst_cnt + 1'b1;
    end
    wire board_reset_n = (rst_cnt == 16'hFFFF);

    // The rz80 engine consumes two phase steps per physical Z80 clock. Sample
    // the external CLK with the dedicated 30 MHz PLL output and create one clock
    // enable for each observed edge. CLK is deliberately treated as data here:
    // J3-21/J2 is not a global-clock pin. Waiting for a synchronized rising
    // edge before releasing the core aligns reset phi=0 with external CLK=1.
    reg [2:0] z80_clk_sync = 3'b000;
    // Delay WAIT alongside CLK. When core_ce observes a CLK transition,
    // bit 1 holds WAIT from the sampler tick that first saw that edge.
    reg [2:0] z80_wait_sync = 3'b111;
    reg       z80_clk_ready = 1'b0;
    reg [2:0] z80_reset_sync = 3'b000;
    wire external_reset_n = board_reset_n && z80_reset_n;
    // Asynchronous assertion, synchronized release. Discard CLK edges already
    // in the pipeline when RESET rises; wait for a fresh rising CLK.
    always @(posedge clk_sampler or negedge external_reset_n) begin
        if (!external_reset_n) z80_reset_sync <= 3'b000;
        else z80_reset_sync <= {z80_reset_sync[1:0], 1'b1};
    end
    always @(posedge clk_sampler) begin
        z80_clk_sync <= {z80_clk_sync[1:0], z80_clk};
        z80_wait_sync <= {z80_wait_sync[1:0], z80_wait_n};
        if (!external_reset_n || !z80_reset_sync[2])
            z80_clk_ready <= 1'b0;
        else if ((z80_clk_sync[2] ^ z80_clk_sync[1]) && z80_clk_sync[1])
            z80_clk_ready <= 1'b1;
    end
    wire z80_clk_edge = z80_clk_sync[2] ^ z80_clk_sync[1];
    wire core_host_clk = EXTERNAL_BUS ? clk_sampler : clk_cpu;
    wire core_ce = EXTERNAL_BUS ? z80_clk_edge : 1'b1;
    wire cpu_reset_n = board_reset_n &&
                       (!EXTERNAL_BUS || (z80_reset_n && z80_reset_sync[2] && z80_clk_ready));

    // ---- Z80 core ----
    wire [15:0] addr;
    wire [7:0]  data_out;
    wire        data_drive;
    wire        m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    reg  [7:0]  data_in;
    wire        soc_int_n;

    wire core_wait_n   = EXTERNAL_BUS ? z80_wait_sync[1] : 1'b1;
    wire core_int_n    = EXTERNAL_BUS ? z80_int_n    : soc_int_n;
    wire core_nmi_n    = EXTERNAL_BUS ? z80_nmi_n    : 1'b1;
    wire core_busreq_n = EXTERNAL_BUS ? z80_busreq_n : 1'b1;

    z80_core #(.USE_CEN(EXTERNAL_BUS)) cpu (
        .clk        (core_host_clk),
        .cen        (core_ce),
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
        .wait_n     (core_wait_n),
        .int_n      (core_int_n),
        .nmi_n      (core_nmi_n),
        .busreq_n   (core_busreq_n),
        .dbg_t      (),
        .dbg_phi    (),
        .dbg_m      ()
    );

    // ---- external J3 bus buffers ----
    // Only HALT remains driven during a DMA grant; address, data and the
    // cycle-control pins are released. The internal BUSACK still controls
    // bus release, but is not brought out because K8 carries D4 instead.
    wire header_bus_owned = EXTERNAL_BUS && busack_n;
    assign z80_a        = header_bus_owned ? addr       : 16'hzzzz;
    assign z80_d        = (header_bus_owned && data_drive) ? data_out : 8'hzz;
    assign z80_m1_n     = header_bus_owned ? m1_n       : 1'bz;
    assign z80_mreq_n   = header_bus_owned ? mreq_n     : 1'bz;
    assign z80_iorq_n   = header_bus_owned ? iorq_n     : 1'bz;
    assign z80_rd_n     = header_bus_owned ? rd_n       : 1'bz;
    assign z80_wr_n     = header_bus_owned ? wr_n       : 1'bz;
    assign z80_rfsh_n   = header_bus_owned ? rfsh_n     : 1'bz;
    assign z80_halt_n   = EXTERNAL_BUS ? halt_n         : 1'bz;

    // ---- 64 KiB block RAM, ROM image preloaded at configuration ----
    reg [7:0] ram [0:65535];
    initial $readmemh(ROM_FILE, ram);
    reg [7:0] ram_dout;
    always @(posedge clk_cpu) begin
        if (!EXTERNAL_BUS && !mreq_n && !wr_n && data_drive && (addr >= ROM_PROTECT_TOP))
            ram[addr] <= data_out;
        ram_dout <= ram[addr];
    end

    // ---- UART ----
    wire       rx_valid;
    wire [7:0] rx_data;
    reg        tx_strobe;
    reg  [7:0] tx_data;
    wire       tx_busy;
    wire       uart_rx_s;

    uart_rx #(.CLK_HZ(CORE_HZ), .BAUD(BAUD)) urx (
        .clk(clk_cpu), .reset_n(board_reset_n), .rx(uart_rx), .rx_sync(uart_rx_s),
        .valid(rx_valid), .data(rx_data)
    );
    uart_tx #(.CLK_HZ(CORE_HZ), .BAUD(BAUD)) utx (
        .clk(clk_cpu), .reset_n(board_reset_n), .strobe(tx_strobe), .data(tx_data),
        .tx(uart_tx), .busy(tx_busy)
    );

    // ---- RX FIFO (32 deep) ----
    reg [7:0] fifo [0:31];
    reg [4:0] f_rd = 0, f_wr = 0;
    reg [5:0] f_cnt = 0;
    wire rx_avail = (f_cnt != 0);
    wire [7:0] fifo_head = fifo[f_rd];

    // ---- ACIA-style I/O decode (see fpga/icepi-zero/top.v) ----
    wire iord_active = !iorq_n && !rd_n;
    wire iowr_active = !iorq_n && !wr_n;
    wire inta_active = !iorq_n && !m1_n;
    reg  prev_iord = 0, prev_iowr = 0;
    reg  [7:0] io_dout = 8'hFF;
    wire [7:0] port = addr[7:0];
    wire tx_ready = !tx_busy;

    wire fifo_pop  = iord_active && !prev_iord && (port == 8'h81 || port == 8'h01) && rx_avail;
    wire fifo_push = rx_valid && (f_cnt != 6'd32);

    // 68B50 control register: bit7 RIE, bits 6:5 = 01 -> TIE. The
    // NASCOM/z88dk ACIA driver's TX is interrupt-driven, so TIE must
    // really work (a status-only shim deadlocks TX at real baud).
    reg [7:0] acia_ctrl = 8'h00;
    wire rie = acia_ctrl[7];
    wire tie = (acia_ctrl[6:5] == 2'b01);

    always @(posedge clk_cpu) begin
        if (!board_reset_n) begin
            f_rd <= 0; f_wr <= 0; f_cnt <= 0;
            prev_iord <= 0; prev_iowr <= 0;
            tx_strobe <= 0;
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

            if (!EXTERNAL_BUS && iowr_active && !prev_iowr) begin
                if (port == 8'h81 || port == 8'h01) begin
                    tx_data   <= data_out;
                    tx_strobe <= 1'b1;
                end else if (port == 8'h80) begin
                    acia_ctrl <= data_out;   // divisor/format bits ignored
                end
            end
        end
    end

    // 68B50 IRQ: RX byte pending (if RIE) or TX empty (if TIE).
    assign soc_int_n = ((rie && rx_avail) || (tie && tx_ready)) ? 1'b0 : 1'b1;

    // Data-bus input mux. INTA in IM 1 ignores the bus; 0xFF = RST 38h.
    always @(*) begin
        if (EXTERNAL_BUS)     data_in = z80_d;
        else if (inta_active) data_in = 8'hFF;
        else if (iord_active) data_in = io_dout;
        else                  data_in = ram_dout;
    end
endmodule
