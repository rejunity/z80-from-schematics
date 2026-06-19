// ===========================================================================
// tb_z80.v - Icarus Verilog testbench. Wires z80_core to a 64K memory and
// emits the shared bus-cycle trace (one line per phase) to stdout, matching
// the C-model format produced by scripts/tracegen.c.
//
//   vvp tb_z80.vvp +prog=PROG.hex +phases=N
//
// Simulation-only constructs (initial, $readmemh, $display) live here, never
// in the synthesizable RTL.
// ===========================================================================
`timescale 1ns/1ns

module tb_z80;
    reg         clk = 1'b0;
    reg         reset_n = 1'b0;
    reg         wait_n = 1'b1, int_n = 1'b1, nmi_n = 1'b1, busreq_n = 1'b1;
    reg  [7:0]  data_in;

    wire [15:0] addr;
    wire [7:0]  data_out;
    wire        data_drive, m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    wire [3:0]  dbg_t;
    wire        dbg_phi;
    wire [2:0]  dbg_m;

    reg  [7:0]  mem [0:65535];

    z80_core dut (
        .clk(clk), .reset_n(reset_n),
        .addr(addr), .data_in(data_in), .data_out(data_out), .data_drive(data_drive),
        .m1_n(m1_n), .mreq_n(mreq_n), .iorq_n(iorq_n), .rd_n(rd_n), .wr_n(wr_n),
        .rfsh_n(rfsh_n), .halt_n(halt_n), .busack_n(busack_n),
        .wait_n(wait_n), .int_n(int_n), .nmi_n(nmi_n), .busreq_n(busreq_n),
        .dbg_t(dbg_t), .dbg_phi(dbg_phi), .dbg_m(dbg_m)
    );

    // combinational memory/I-O read (matches cmodel/z80_sim.c)
    always @* begin
        if (!mreq_n && !rd_n)       data_in = mem[addr];
        else if (!iorq_n && !rd_n)  data_in = mem[addr];
        else                        data_in = 8'h00;
    end

    // memory write
    always @(posedge clk) begin
        if (!mreq_n && !wr_n) mem[addr] <= data_out;
    end

    always #1 clk = ~clk;

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
    reg [1023:0] progf;

    task dump;
        $display("%0d %0d %0d %04X %02X %02X %0d %0d %0d %0d %0d %0d %0d %0d",
                 dbg_t, dbg_phi, dbg_m, addr, data_out, data_in,
                 mreq_n, iorq_n, rd_n, wr_n, m1_n, rfsh_n, halt_n, busack_n);
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

        reset_n = 1'b0;
        repeat (4) @(negedge clk);
        reset_n = 1'b1;

        // Drive pins for phase 0 (legacy nmi shorthand kept side-by-side
        // with the new per-pin lo/hi plusargs).
        nmi_n    = (0 == nmiph || 0 == nmi_lo) ? 1'b0 : 1'b1;
        int_n    = (0 == int_lo)    ? 1'b0 : 1'b1;
        wait_n   = (0 == wait_lo)   ? 1'b0 : 1'b1;
        busreq_n = (0 == busreq_lo) ? 1'b0 : 1'b1;
        // reset_n stays asserted (1) here — the reset block above already
        // released it. A `reset_lo` event would re-assert it (drive 0).
        if (0 == reset_lo) reset_n = 1'b0;
        #0;                                   // iverilog delta-cycle settle so
                                              // the in_reset_hold combinational
                                              // mux sees reset_n=1 before dump
        dump;                                 // line 1 = reset state (T1.P)
        for (i = 1; i < phases; i = i + 1) begin
            @(negedge clk);
            // Each pin holds its previous value unless an event fires
            // at this phase (lo overrides hi if both happen at same phase).
            if (i == nmi_hi)    nmi_n    = 1'b1;
            if (i == nmiph || i == nmi_lo) nmi_n = 1'b0;
            if (i == int_hi)    int_n    = 1'b1;
            if (i == int_lo)    int_n    = 1'b0;
            if (i == wait_hi)   wait_n   = 1'b1;
            if (i == wait_lo)   wait_n   = 1'b0;
            if (i == busreq_hi) busreq_n = 1'b1;
            if (i == busreq_lo) busreq_n = 1'b0;
            if (i == reset_hi)  reset_n  = 1'b1;
            if (i == reset_lo)  reset_n  = 1'b0;
            // Pulse-on-same-phase: if neither lo nor hi event fired but
            // the legacy nmi shorthand says "single-phase nmi at i", keep
            // the old behaviour of high outside that one phase. Otherwise
            // honor the new lo/hi events.
            if (nmiph >= 0 && i != nmiph && nmi_lo < 0 && nmi_hi < 0) begin
                nmi_n = 1'b1;
            end
            dump;
        end
        $finish;
    end
endmodule
