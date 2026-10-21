// ===========================================================================
// tb_z80.v - Icarus Verilog testbench. Wires z80_top to a 64K memory and
// emits the shared bus-cycle trace (one line per phase) to stdout, matching
// the C-model format produced by scripts/tracegen.c.
//
//   vvp tb_z80.vvp +prog=PROG.hex +phases=N
//
// Clocking: `clk` IS the Z80 clock (one period = one T-state). A "phase"
// is one half-period: even phases are the clock-high half (after a rising
// edge), odd phases the clock-low half (after a falling edge). Pin events
// scheduled for phase i are applied just before the edge that starts phase
// i, so the DUT samples them on that edge -- the same "value present
// during phase i" semantics the C tracegen uses, and perfectz80_runner's
// (inputs set before each half-cycle step). Memory writes are captured
// from the settled pins at every phase, exactly like cmodel/z80_sim.c.
//
// Simulation-only constructs (initial, $readmemh, $display) live here, never
// in the synthesizable RTL.
// ===========================================================================
`timescale 1ns/1ns

module tb_z80;
    reg         clk = 1'b0;
    reg         por_n = 1'b0;           // power-on reset
    reg         reset_n = 1'b0;         // the RESET pin
    reg         wait_n = 1'b1, int_n = 1'b1, nmi_n = 1'b1, busreq_n = 1'b1;
    wire [7:0]  data_in;

    wire [15:0] addr;
    wire [7:0]  data_out;
    wire        data_drive, m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    wire [3:0]  dbg_t;
    reg         dbg_phi;            // 0 = clock-high half, 1 = clock-low half
    wire [2:0]  dbg_m;

    reg  [7:0]  mem [0:65535];

    z80_top dut (
        .clk(clk), .por_n(por_n), .reset_n(reset_n),
        .addr(addr), .data_in(data_in), .data_out(data_out), .data_drive(data_drive),
        .m1_n(m1_n), .mreq_n(mreq_n), .iorq_n(iorq_n), .rd_n(rd_n), .wr_n(wr_n),
        .rfsh_n(rfsh_n), .halt_n(halt_n), .busack_n(busack_n),
        .wait_n(wait_n), .int_n(int_n), .nmi_n(nmi_n), .busreq_n(busreq_n),
        .dbg_t(dbg_t), .dbg_m(dbg_m)
    );

    // Combinational memory/I-O read (matches cmodel/z80_sim.c). Use an
    // indexed continuous read: always @* expands all 64K words into its
    // sensitivity list, making Icarus elaboration/startup very expensive.
    assign data_in = ((!mreq_n || !iorq_n) && !rd_n) ? mem[addr] : 8'h00;

    // memory write: captured from the settled pins at every phase (see
    // `service` below), never on a clock edge -- the pins themselves are
    // driven by flops on both edges, and sampling them in an edge-triggered
    // block would race the DUT (RTL and, worse, gate-level cell models).

    always #2 clk = ~clk;           // period 4 = one T-state

    integer i, phases, nmiph;
    // Per-pin lo/hi event phases. -1 means "never" (no event).
    // Encodes the .events sidecar format as a flat set of plusargs:
    //   +nmi_lo=N +nmi_hi=M    drive nmi_n low at phase N, high at phase M
    //   +int_lo=N +int_hi=M
    //   +wait_lo=N +wait_hi=M
    //   +busreq_lo=N +busreq_hi=M
    //   +reset_lo=N +reset_hi=M
    // scripts/compare_signal_timing.py parses each <prog>.events sidecar
    // into these plusargs so the iverilog RTL path can run the same pin-
    // scenario programs that the C tracegen + perfectz80_runner consume.
    integer nmi_lo, nmi_hi, int_lo, int_hi, wait_lo, wait_hi;
    integer busreq_lo, busreq_hi, reset_lo, reset_hi;
    reg [8*512-1:0] progf;          // paths up to 512 characters

    task dump;
        $display("%0d %0d %0d %04X %02X %02X %0d %0d %0d %0d %0d %0d %0d %0d",
                 dbg_t, dbg_phi, dbg_m, addr, data_out, data_in,
                 mreq_n, iorq_n, rd_n, wr_n, m1_n, rfsh_n, halt_n, busack_n);
    endtask

    // one phase of system service: capture a write from the pins the DUT
    // is driving this half-cycle (matches cmodel/z80_sim.c z80_sys_phase)
    task service;
        if (!mreq_n && !wr_n) mem[addr] = data_out;
    endtask

    // pin events of phase k, applied just before the edge that enters
    // phase k -- the way perfectz80_runner and the C tracegen apply them:
    // the DUT samples them on that edge, as a pin meeting its setup time,
    // and they hold until a later event
    task events(input integer k);
        begin
            if (k == nmi_hi)    nmi_n    = 1'b1;
            if (k == nmiph || k == nmi_lo) nmi_n = 1'b0;
            if (k == int_hi)    int_n    = 1'b1;
            if (k == int_lo)    int_n    = 1'b0;
            if (k == wait_hi)   wait_n   = 1'b1;
            if (k == wait_lo)   wait_n   = 1'b0;
            if (k == busreq_hi) busreq_n = 1'b1;
            if (k == busreq_lo) busreq_n = 1'b0;
            if (k == reset_hi)  reset_n  = 1'b1;
            if (k == reset_lo)  reset_n  = 1'b0;
            // legacy single-phase NMI shorthand: high outside that phase
            if (nmiph >= 0 && k != nmiph && nmi_lo < 0 && nmi_hi < 0) nmi_n = 1'b1;
        end
    endtask

    initial begin
        if (!$value$plusargs("prog=%s", progf)) progf = "prog.hex";
        if (!$value$plusargs("phases=%d", phases)) phases = 200;
        if (!$value$plusargs("nmi=%d", nmiph)) nmiph = -1;  // legacy single-phase shorthand
        if (!$value$plusargs("nmi_lo=%d",    nmi_lo))    nmi_lo    = -1;
        if (!$value$plusargs("nmi_hi=%d",    nmi_hi))    nmi_hi    = -1;
        if (!$value$plusargs("int_lo=%d",    int_lo))    int_lo    = -1;
        if (!$value$plusargs("int_hi=%d",    int_hi))    int_hi    = -1;
        if (!$value$plusargs("wait_lo=%d",   wait_lo))   wait_lo   = -1;
        if (!$value$plusargs("wait_hi=%d",   wait_hi))   wait_hi   = -1;
        if (!$value$plusargs("busreq_lo=%d", busreq_lo)) busreq_lo = -1;
        if (!$value$plusargs("busreq_hi=%d", busreq_hi)) busreq_hi = -1;
        if (!$value$plusargs("reset_lo=%d",  reset_lo))  reset_lo  = -1;
        if (!$value$plusargs("reset_hi=%d",  reset_hi))  reset_hi  = -1;
        for (i = 0; i < 65536; i = i + 1) mem[i] = 8'h00;
        $readmemh(progf, mem);

        // Power-on: por_n, then RESET held across a couple of T-states and
        // released in a clock-low half. The core samples RESET on rising
        // edges and enters T1.P of the M1 fetch at PC=0 on the third one --
        // that is trace line 0, matching the C model's z80_init() state.
        por_n = 1'b0; reset_n = 1'b0;
        @(negedge clk); #1 por_n = 1'b1;
        repeat (2) @(negedge clk);
        #1 reset_n = 1'b1;
        @(posedge clk); @(posedge clk);
        events(0);
        @(posedge clk); #1;
        if (m1_n !== 1'b0) $display("tb_z80: no M1 at trace line 0");
        dbg_phi = 1'b0;
        service;
        dump;                                 // line 0 = T1.P
        for (i = 1; i < phases; i = i + 1) begin
            events(i);
            // odd phase: falling edge; even phase: rising edge. Settle #1
            // past the edge before sampling outputs.
            if (i[0]) @(negedge clk); else @(posedge clk);
            #1;
            dbg_phi = i[0];
            service;
            dump;
        end
        $finish;
    end
endmodule
