// top.v - RZ80 for the Sipeed Tang Primer 25K (Gowin GW5A-LV25MG121)
//
// Two builds from this one file, selected by EXTERNAL_BUS (see Makefile:
// `make sbc`, `make chip`):
//
//   EXTERNAL_BUS=0  "sbc": a BASIC single-board computer. 64 KiB internal
//     RAM with NASCOM BASIC 4.7 (or Tiny BASIC) preloaded, 68B50-style
//     ACIA on the BL616 USB-UART at 115200 8N1, Z80 clock from the PLL
//     (CORE_HZ, 4 MHz). The dock's J3 header carries the Z80 bus as
//     OUTPUTS in the chip-build pinout below (CLK included), so a logic
//     analyser or an external bus display sees exactly what a Z80 would
//     drive; only /RESET and /NMI are inputs.
//
//   EXTERNAL_BUS=1  "chip": a Z80 chip replacement. J3 is the live CPU
//     bus; CLK comes IN on J3-22 and is the clock of z80_top, memory and
//     peripherals are whatever is wired to the header. The BL616 UART is
//     unused. The header is 3.3 V and not 5 V tolerant: use level
//     translators before connecting it to 5 V Z80 hardware.
//
// Clocking. The Z80 clock is a real clock, one T-state per period:
// z80_top keeps all core state on its rising edge and reproduces the
// chip's falling-edge pin behaviour (MREQ/RD/WR edges, WAIT / read-data /
// INT / NMI sampling) with native negative-edge flops (docs/clocking.md).
// No 2x clock, no clock enable, nothing resamples the external clock.
//
//   - sbc: 50 MHz crystal on E2 -> PLLA -> 2 x CORE_HZ -> hard CLKDIV /2
//     -> CORE_HZ. The PLLA alone cannot go below 6.25 MHz, and a
//     fabric-FF divider never reaches the BSRAM CLK pins skew-free on
//     GW5A (-2.9 ns => hold violations); PLLA + CLKDIV is a true global
//     clock. CORE_HZ = 4_000_000 (Z80A) or 3_500_000 (3.516 MHz, the
//     Spectrum / Z80 rate). In simulation the PLL is a black box, so a
//     /SIM_CLK_DIV toggle divider stands in and the sim builds override
//     CORE_HZ to match.
//   - chip: CLK on J3-22 (ball J1) drives z80_top directly. Every rising
//     edge is one T-state; WAIT must be valid around the falling edge of
//     T2, read data around the falling edge of T3, exactly the Z80
//     setup/hold contract. Holding CLK stops the CPU, as on a CMOS Z80.
//     Ball J1 is a Gowin clock pin (GCLKT_5), but the open flow
//     (apicula/nextpnr-himbaechel, 2026-10) has no dedicated path from it
//     to the clock network on GW5A-25 and would route CLK to every flop
//     through general fabric (skew, hold violations). The .cst marks it
//     CLOCK_LOC BUFG: one short fabric hop to a BUFG beside J1, then the
//     global clock network. Keep external CLK <= 10 MHz.
//
// RESET: power-on + key[0] (H10, active HIGH: board pull-down) + header
// /RESET, asynchronous assertion, release synchronized to the Z80 clock
// so the first T1 always starts on a clean rising edge.
//
// Board: 50 MHz crystal E2; UART to the BL616: FPGA RX = B3, TX = C3;
// two push-buttons H10/H11 with pull-DOWNs; no user LEDs on the dock.
//
// sbc memory map / I/O map / interrupt semantics are identical to the
// IcePi port (fpga/icepi-zero/top.v): ROM write-protected below
// ROM_PROTECT_TOP (the Makefile sets it to the end of the ROM image
// rounded up to 1 KiB: 0x2000 for NASCOM BASIC, 0x0400 for the 1K Tiny
// BASIC, whose variables and stack live at 0x0400-0x07FF), ACIA at ports
// 0x80/0x81 (Tiny BASIC's 0x00/0x01 too), /INT = (RIE & RDRF) |
// (TIE & TDRE). A TX write that finds the transmitter busy holds the CPU
// with /WAIT until the previous byte is out (Tiny BASIC writes without
// polling TDRE); the sbc build shows that /WAIT on J3.

module top #(
    parameter CORE_HZ  = 4_000_000,             // sbc Z80 clock: 4_000_000 or 3_500_000; sim overrides to 4_166_667
    parameter SIM_CLK_DIV = 12,                 // simulation stand-in for the PLL: 50 MHz / 12
    parameter BAUD     = 115_200,
    parameter ROM_FILE = "rom.mem",
    parameter ROM_PROTECT_TOP = 16'h2000,       // NASCOM 4.7 ROM is 0x0000-0x1FD9
    parameter EXTERNAL_BUS = 1'b1               // 1: chip build (J3 is the live CPU bus); 0: BASIC sbc build
) (
    input  wire       clk,       // 50 MHz crystal (E2)
    input  wire       uart_rx,   // BL616 -> FPGA serial (B3)
    output wire       uart_tx,   // FPGA -> BL616 serial (C3)
    input  wire [1:0] key,       // push-buttons, ACTIVE HIGH (H10, H11)

    // Z80 bus on J3 (pinout in tang_primer_25k.cst). chip build: CLK, WAIT,
    // INT, NMI, BUSREQ, RESET are inputs, D is bidirectional, and address /
    // control / data drivers release while the core's internal BUSACK is
    // asserted. sbc build: everything is an output except RESET and NMI.
    // BUSACK itself is not exposed: its former K8 header pin replaces the
    // faulty B2/D4 connection.
    inout  wire        z80_clk,     // chip: input; sbc: output
    output wire [15:0] z80_a,
    inout  wire [7:0]  z80_d,
    output wire        z80_m1_n,
    output wire        z80_mreq_n,
    output wire        z80_iorq_n,
    output wire        z80_rd_n,
    output wire        z80_wr_n,
    output wire        z80_rfsh_n,
    output wire        z80_halt_n,
    inout  wire        z80_wait_n,  // chip: input; sbc: output (idle high)
    inout  wire        z80_int_n,   // chip: input; sbc: output (the ACIA's /INT)
    input  wire        z80_nmi_n,
    inout  wire        z80_busreq_n,// chip: input; sbc: output (idle high)
    input  wire        z80_reset_n
);
    // ---- internal Z80 clock (sbc build only) ----
    wire clk_int, pll_lock;
    generate if (!EXTERNAL_BUS) begin : g_pll
`ifdef SYNTHESIS
    // PLLA: VCO = 50 MHz x MDIV; CLKOUT0 = VCO / ODIV0 = 2 x CORE_HZ, then
    // the hard CLKDIV halves it onto the global clock network.
    //   4 MHz:   MDIV 20 -> VCO 1000 MHz, ODIV0 125 -> 8 MHz     -> 4 MHz
    //   3.5 MHz: MDIV 18 -> VCO 900 MHz,  ODIV0 128 -> 7.03 MHz  -> 3.516 MHz
    // Instantiation and parameter boilerplate follow apicula's
    // examples/gw5a/pll7.v (YosysHQ/apicula, MIT).
    localparam PLL_MDIV = (CORE_HZ == 3_500_000) ? 18  : 20;
    localparam PLL_ODIV = (CORE_HZ == 3_500_000) ? 128 : 125;
    wire clk_2x;
    wire gw_gnd = 1'b0;

    PLLA PLLA_inst (
        .LOCK(pll_lock),
        .CLKOUT0(clk_2x),
        .CLKOUT1(), .CLKOUT2(), .CLKOUT3(), .CLKOUT4(), .CLKOUT5(), .CLKOUT6(),
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
    defparam PLLA_inst.MDIV_SEL = PLL_MDIV;
    defparam PLLA_inst.MDIV_FRAC_SEL = 0;
    defparam PLLA_inst.ODIV0_SEL = PLL_ODIV;    // 2 x CORE_HZ
    defparam PLLA_inst.ODIV0_FRAC_SEL = 0;
    // Unused outputs still need explicit ODIVs: apycula's defaults for
    // A_ODIV1..6_SEL are the string '8', which its own binary parser
    // rejects (gowin_pack ValueError). Values are don't-care (EN=FALSE).
    defparam PLLA_inst.ODIV1_SEL = 45;
    defparam PLLA_inst.ODIV2_SEL = 45;
    defparam PLLA_inst.ODIV3_SEL = 45;
    defparam PLLA_inst.ODIV4_SEL = 45;
    defparam PLLA_inst.ODIV5_SEL = 45;
    defparam PLLA_inst.ODIV6_SEL = 45;
    defparam PLLA_inst.CLKOUT0_EN = "TRUE";
    defparam PLLA_inst.CLKOUT1_EN = "FALSE";
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

    CLKDIV #(.DIV_MODE("2")) clkdiv_inst (
        .HCLKIN(clk_2x), .RESETN(1'b1), .CALIB(1'b0), .CLKOUT(clk_int)
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
    assign clk_int = clk_div;
    assign pll_lock = 1'b1;
`endif
    end else begin : g_nopll
        assign clk_int  = 1'b0;     // chip build: no internal clock
        assign pll_lock = 1'b1;
    end endgenerate

    // ---- the Z80 clock ----
    // Each build has exactly one clock source and no clock mux in the
    // netlist. chip: the CLK pin, left undriven here so it is a pure
    // input; sbc: the PLL clock, also driven out on the CLK pin. (Inout
    // pins are never assigned a constant 1'bz: yosys folds that into an
    // undriven net and then optimizes everything clocked by it away.)
    wire clk_z80;
    generate if (EXTERNAL_BUS) begin : g_clk_chip
        assign clk_z80 = z80_clk;
    end else begin : g_clk_sbc
        assign clk_z80 = clk_int;
        assign z80_clk = clk_int;
    end endgenerate

    // ---- power-on + key[0] reset ----
    // Same debounced stretch as the IcePi port, but counted on the
    // always-running 50 MHz crystal (the Z80 clock may be external and
    // stopped): any press clears the counter and board_reset_n stays low
    // until ~5 ms after the last bounce. Buttons are active high here
    // (board pull-downs).
    reg [17:0] rst_cnt  = 18'd0;
    reg [1:0]  btn_sync = 2'b00;
    always @(posedge clk) begin
        btn_sync <= {btn_sync[0], key[0]};
        if (btn_sync[1] || !pll_lock)   rst_cnt <= 18'd0;     // pressed / PLL not locked
        else if (rst_cnt != 18'h3FFFF)  rst_cnt <= rst_cnt + 1'b1;
    end
    wire board_reset_n = (rst_cnt == 18'h3FFFF);

    // Header /RESET ORed in (both builds; pulled up on the pin). Asynchronous
    // assertion, release synchronized to the Z80 clock: z80_top's first T1
    // then starts on the next rising edge, whatever phase RESET rose at.
    wire reset_req_n = board_reset_n && z80_reset_n;
    reg  [1:0] rst_sync = 2'b00;
    always @(posedge clk_z80 or negedge reset_req_n) begin
        if (!reset_req_n) rst_sync <= 2'b00;
        else              rst_sync <= {rst_sync[0], 1'b1};
    end
    wire cpu_reset_n = rst_sync[1];

    // z80_top's power-on reset (not a Z80 pin): low from configuration until
    // the first release of the reset above, so the registers come up 5555
    // like perfectz80's. After that the button is the RESET pin alone, which
    // keeps the registers, as on the die.
    reg cpu_por_n = 1'b0;
    always @(posedge clk_z80) if (cpu_reset_n) cpu_por_n <= 1'b1;

    // ---- Z80 core ----
    wire [15:0] addr;
    wire [7:0]  data_out;
    wire        data_drive;
    wire        m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    reg  [7:0]  data_in;
    wire        soc_int_n;
    wire        soc_wait_n;     // the ACIA's TX flow control

    // Header inputs go to z80_top unsynchronized, as on a real Z80: WAIT
    // and the data bus are sampled by its falling-edge flops, INT/NMI by
    // the same, BUSREQ on the rising edge. The header contract is the
    // Z80's own setup/hold around those edges. /NMI is an input in both
    // builds.
    wire core_wait_n   = EXTERNAL_BUS ? z80_wait_n   : soc_wait_n;
    wire core_int_n    = EXTERNAL_BUS ? z80_int_n    : soc_int_n;
    wire core_nmi_n    = z80_nmi_n;
    wire core_busreq_n = EXTERNAL_BUS ? z80_busreq_n : 1'b1;

    wire        addr_drive, ctl_drive;   // the die's pad enables
    z80_top cpu (
        .clk        (clk_z80),
        .por_n      (cpu_por_n),
        .reset_n    (reset_req_n),  // sampled by z80_top as by the die: T1 on the 3rd rising edge after release
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
        .addr_drive (addr_drive),
        .ctl_drive  (ctl_drive),
        .dbg_t      (),
        .dbg_m      ()
    );

    // ---- J3 header drivers ----
    // chip: the die's pad drivers (z80_top addr_drive / ctl_drive /
    // data_drive, docs/timing.md "Pad drivers"): the address floats in a bus
    // grant and in the RESET idle, MREQ IORQ RD WR in a bus grant, both to
    // the rising edge that starts the held M-cycle; D is driven only while
    // the CPU drives it; M1 RFSH (and HALT) are always driven. BUSACK is
    // not brought out: K8 carries D4 instead.
    // sbc: everything out. D shows the byte on the internal bus (the CPU's
    // write data while it drives, otherwise what memory / the ACIA
    // returned); WAIT and BUSREQ idle high, INT is the ACIA's.
    generate if (EXTERNAL_BUS) begin : g_hdr_chip
        assign z80_a        = addr_drive ? addr : 16'hzzzz;
        assign z80_d        = data_drive ? data_out : 8'hzz;
        assign z80_m1_n     = m1_n;
        assign z80_mreq_n   = ctl_drive ? mreq_n : 1'bz;
        assign z80_iorq_n   = ctl_drive ? iorq_n : 1'bz;
        assign z80_rd_n     = ctl_drive ? rd_n   : 1'bz;
        assign z80_wr_n     = ctl_drive ? wr_n   : 1'bz;
        assign z80_rfsh_n   = rfsh_n;
        // WAIT / INT / BUSREQ: inputs, not driven
    end else begin : g_hdr_sbc
        assign z80_a        = addr;
        assign z80_d        = data_drive ? data_out : data_in;
        assign z80_m1_n     = m1_n;
        assign z80_mreq_n   = mreq_n;
        assign z80_iorq_n   = iorq_n;
        assign z80_rd_n     = rd_n;
        assign z80_wr_n     = wr_n;
        assign z80_rfsh_n   = rfsh_n;
        assign z80_wait_n   = soc_wait_n;
        assign z80_int_n    = soc_int_n;
        assign z80_busreq_n = 1'b1;
    end endgenerate
    assign z80_halt_n = halt_n;

    // ---- 64 KiB block RAM, ROM image preloaded at configuration ----
    reg [7:0] ram [0:65535];
    initial $readmemh(ROM_FILE, ram);
    // One T-state of read latency, as in the IcePi port: address on the
    // pins from T1, registered by the BRAM at T2, consumed by the core at
    // the rising edge after T2 (M1) or the falling edge of T3 (read).
    reg [7:0] ram_dout;
    always @(posedge clk_z80) begin
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
                // The backspace key of macOS terminals sends DEL (7Fh). NASCOM
                // BASIC deletes with BS (08h) and takes DEL as a Teletype
                // rubout (it reprints the deleted character), so DEL reaches
                // the CPU as BS, as in scripts/basicrunner.c.
                fifo[f_wr] <= (rx_data == 8'h7F) ? 8'h08 : rx_data;
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
        if (EXTERNAL_BUS)     data_in = z80_d;
        else if (inta_active) data_in = 8'hFF;
        else if (iord_active) data_in = io_dout;
        else                  data_in = ram_dout;
    end
endmodule
