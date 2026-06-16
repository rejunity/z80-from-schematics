// ===========================================================================
// tb_z80_netlist.v — gate-level Icarus Verilog testbench for the
// LibreLane-synthesized sky130 netlist.
//
// Mirror of tb_z80.v: same 64K memory + program loader + 14-column trace
// dump, same +prog= / +phases= / +nmi= plusargs. Differences are isolated
// to:
//   - the cell-model `include`s for sky130_fd_sc_hd (added at compile
//     time by the Makefile via -y / -I flags rather than literal
//     `include directives, so the same testbench works with whatever
//     PDK_ROOT resolves to)
//   - z80_core comes from build/synth/z80_core.nl.v (gates) rather than
//     rtl/z80_core.v (RTL)
//
// Compile (from the Makefile):
//   iverilog -g2012 -DFUNCTIONAL \
//     -o build/tb_z80_netlist.vvp \
//     tests/iverilog/tb_z80_netlist.v \
//     build/synth/z80_core.nl.v \
//     $PDK_ROOT/sky130A/libs.ref/sky130_fd_sc_hd/verilog/primitives.v \
//     $PDK_ROOT/sky130A/libs.ref/sky130_fd_sc_hd/verilog/sky130_fd_sc_hd.v
//
// `USE_POWER_PINS` is intentionally NOT defined — that picks the cell
// model variant that auto-tieoffs VPWR/VGND/VPB/VNB. Saves boilerplate
// for a sim-only flow.
//
// `FUNCTIONAL` IS defined (via the Makefile -DFUNCTIONAL flag) — that
// picks the zero-delay functional cell model variant. Much faster than
// the spec-block timing model and sufficient for logical correctness
// (which is what we're checking against the perfectz80 gate-level
// netlist, also unit-delay).
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

    // Synthesized netlist — same module name `z80_core`, same port list as
    // rtl/z80_core.v (synthesis preserved the port interface; dbg_* are
    // top-level OUTPUTs, see rtl/z80_core.v:23-25 and docs/librelane-flow.md
    // "RTL pre-flight").
    z80_core dut (
        .clk(clk), .reset_n(reset_n),
        .addr(addr), .data_in(data_in), .data_out(data_out), .data_drive(data_drive),
        .m1_n(m1_n), .mreq_n(mreq_n), .iorq_n(iorq_n), .rd_n(rd_n), .wr_n(wr_n),
        .rfsh_n(rfsh_n), .halt_n(halt_n), .busack_n(busack_n),
        .wait_n(wait_n), .int_n(int_n), .nmi_n(nmi_n), .busreq_n(busreq_n),
        .dbg_t(dbg_t), .dbg_phi(dbg_phi), .dbg_m(dbg_m)
    );

    // Combinational memory/I-O read — matches cmodel/z80_sim.c and tb_z80.v
    // exactly. The gate-level netlist drives the same control pins at the
    // same phases, so the same memory-response logic works unchanged.
    always @* begin
        if (!mreq_n && !rd_n)       data_in = mem[addr];
        else if (!iorq_n && !rd_n)  data_in = mem[addr];
        else                        data_in = 8'h00;
    end

    // Memory write
    always @(posedge clk) begin
        if (!mreq_n && !wr_n) mem[addr] <= data_out;
    end

    always #1 clk = ~clk;

    integer i, phases, nmiph;
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
        if (!$value$plusargs("nmi=%d", nmiph)) nmiph = -1;
        // Per-pin lo/hi event phases — see tb_z80.v for the encoding.
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

        nmi_n    = (0 == nmiph || 0 == nmi_lo) ? 1'b0 : 1'b1;
        int_n    = (0 == int_lo)    ? 1'b0 : 1'b1;
        wait_n   = (0 == wait_lo)   ? 1'b0 : 1'b1;
        busreq_n = (0 == busreq_lo) ? 1'b0 : 1'b1;
        if (0 == reset_lo) reset_n = 1'b0;
        dump;                                 // line 1 = reset state (T1.P)
        for (i = 1; i < phases; i = i + 1) begin
            @(negedge clk);
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
            if (nmiph >= 0 && i != nmiph && nmi_lo < 0 && nmi_hi < 0) nmi_n = 1'b1;
            dump;
        end
        $finish;
    end
endmodule
