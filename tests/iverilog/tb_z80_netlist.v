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
//   - z80_top comes from build/synth/z80_top.nl.v (gates) rather than
//     rtl/z80_top.v + rtl/z80_core.v (RTL)
//
// Compile (from the Makefile):
//   iverilog -g2012 -DFUNCTIONAL \
//     -o build/tb_z80_netlist.vvp \
//     tests/iverilog/tb_z80_netlist.v \
//     build/synth/z80_top.nl.v \
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
    reg         por_n = 1'b0;           // power-on reset
    reg         reset_n = 1'b0;         // the RESET pin
    reg         wait_n = 1'b1, int_n = 1'b1, nmi_n = 1'b1, busreq_n = 1'b1;
    reg  [7:0]  data_in;

    wire [15:0] addr;
    wire [7:0]  data_out;
    wire        data_drive, m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    wire [3:0]  dbg_t;
    reg         dbg_phi;            // 0 = clock-high half, 1 = clock-low half
    wire [2:0]  dbg_m;

    reg  [7:0]  mem [0:65535];

    // The machine around the CPU (+machine=1 the ZX Spectrum 48K's ULA
    // holding CLK high, 2 the ZX80 / ZX81; +mt0 the ULA's T-state count at
    // trace line 0, +mnmi the ZX81's NMI generator): the model of
    // scripts/perfectz80_runner.c (MACHINE there), from <prog>.machine via
    // scripts/compare_signal_timing.py. The DUT's clock is the free-running
    // one held high while the ULA holds it.
    integer     machine, mt0, mnmi, m_hold;
    reg         m_hold_clk = 1'b0, m_decoy_on = 1'b0, m_held;
    reg  [7:0]  m_decoy = 8'h00;
    reg         m_int = 1'b1, m_wait = 1'b1, m_nmi = 1'b1;
    wire        clk_dut = clk | m_hold_clk;
    // the ZX80 / ZX81 forces an opcode fetch from 8000-FFFF whose byte has
    // bit 6 clear to 00 (NOP), while HALT is high
    wire        m_nop = machine == 2 && !m1_n && !mreq_n && !rd_n && addr[15] && halt_n && !mem[addr][6];

    // Synthesized netlist — same module name `z80_top`, same port list as
    // rtl/z80_top.v (synthesis preserved the port interface; dbg_* are
    // top-level OUTPUTs, see docs/librelane-flow.md "RTL pre-flight").
    z80_top dut (
        .clk(clk_dut), .por_n(por_n), .reset_n(reset_n),
        .addr(addr), .data_in(data_in), .data_out(data_out), .data_drive(data_drive),
        .m1_n(m1_n), .mreq_n(mreq_n), .iorq_n(iorq_n), .rd_n(rd_n), .wr_n(wr_n),
        .rfsh_n(rfsh_n), .halt_n(halt_n), .busack_n(busack_n),
        .wait_n(wait_n), .int_n(int_n), .nmi_n(nmi_n), .busreq_n(busreq_n),
        .dbg_t(dbg_t), .dbg_m(dbg_m)
    );

    // Combinational memory/I-O read — matches cmodel/z80_sim.c and tb_z80.v
    // exactly. The gate-level netlist drives the same control pins at the
    // same phases, so the same memory-response logic works unchanged.
    always @* begin
        if (m_decoy_on)             data_in = m_decoy;     // the ULA's byte, CLK held
        else if (m_nop)             data_in = 8'h00;
        else if (!mreq_n && !rd_n)  data_in = mem[addr];
        else if (!iorq_n && !rd_n)  data_in = mem[addr];
        else                        data_in = 8'h00;
    end

    // Memory write: captured from the settled pins at every phase (see
    // `service`), never in an edge-triggered block -- sampling dual-edge
    // flop outputs on the edge would race the cell models.

    always #2 clk = ~clk;           // period 4 = one T-state

    integer i, phases, nmiph;
    integer nmi_lo, nmi_hi, int_lo, int_hi, wait_lo, wait_hi;
    integer busreq_lo, busreq_hi, reset_lo, reset_hi;
    reg [8*512-1:0] progf;          // paths up to 512 characters

    task dump;
        $display("%0d %0d %0d %04X %02X %02X %0d %0d %0d %0d %0d %0d %0d %0d",
                 dbg_t, dbg_phi, dbg_m, addr, data_out, data_in,
                 mreq_n, iorq_n, rd_n, wr_n, m1_n, rfsh_n, halt_n, busack_n);
    endtask

    // the machine, before the edge into line k: the ZX81's NMI generator
    // (with the clock's rise), the inputs it set after line k - 1, and
    // whether that edge happens
    function automatic integer ula_delay(input integer t);
        integer lp;
        begin
            lp = ((t % 224) + 224) % 224;
            ula_delay = lp >= 128 ? 0 : (lp % 8) >= 6 ? 0 : 6 - (lp % 8);
        end
    endfunction
    function automatic zx81_nmi_low(input integer t);
        integer p;
        begin
            p = ((t % 207) + 207) % 207;
            zx81_nmi_low = p >= 16 && p < 32;
        end
    endfunction
    task machine_pre(input integer k);
        begin
            if (machine == 2 && mnmi != 0 && !k[0]) m_nmi = !zx81_nmi_low(k / 2 + mt0);
            if (machine == 2) begin int_n = m_int; wait_n = m_wait; nmi_n = m_nmi; end
            m_held = m_hold > 0;
            if (m_held) begin
                m_hold = m_hold - 1;
                if (k[0]) m_hold_clk = 1'b1;     // before the fall it holds off
            end
        end
    endtask
    // in line k, just past its edge (or where it would have been)
    task machine_in(input integer k);
        begin
            m_decoy_on = m_held;
            m_decoy = (8'hA5 + 8'h3B * k[7:0]);
            if (!k[0] && m_hold == 0) m_hold_clk = 1'b0;   // the clock is high: no edge
        end
    endtask
    // after line k, from its pins: the inputs for line k + 1, and the hold
    // (the ULA: a contended address with MREQ high, or T2 of an I/O
    // cycle to its own, even, port)
    reg m_iorq_was = 1'b1, m_port;
    task machine_post(input integer k);
        begin
            if (machine == 1 && !k[0]) begin
                m_port = !iorq_n && m1_n && !addr[0];
                if (m_hold == 0 && ((addr[15:14] == 2'b01 && mreq_n && !m_port) || (m_port && m_iorq_was)))
                    m_hold = 2 * ula_delay(k / 2 + mt0);
                m_iorq_was = iorq_n;
            end
            if (machine == 2) begin
                m_int = addr[6];
                m_wait = !(!m_nmi && halt_n);
            end
        end
    endtask

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
        if (!$value$plusargs("machine=%d",   machine))   machine   = 0;
        if (!$value$plusargs("mt0=%d",       mt0))       mt0       = 0;
        if (!$value$plusargs("mnmi=%d",      mnmi))      mnmi      = 0;
        m_hold = 0; m_held = 1'b0;
        for (i = 0; i < 65536; i = i + 1) mem[i] = 8'h00;
        $readmemh(progf, mem);

        // Same reset / phase choreography as tb_z80.v: power-on, RESET
        // released in the clock-low half, T1.P of the first M1 on the third
        // rising edge = trace line 0.
        por_n = 1'b0; reset_n = 1'b0;
        @(negedge clk); #1 por_n = 1'b1;
        repeat (2) @(negedge clk);
        #1 reset_n = 1'b1;
        @(posedge clk); @(posedge clk);
        events(0);
        @(posedge clk); #1;
        if (m1_n !== 1'b0) $display("tb_z80_netlist: no M1 at trace line 0");
        dbg_phi = 1'b0;
        service;
        dump;                                 // line 0 = T1.P
        machine_post(0);
        for (i = 1; i < phases; i = i + 1) begin
            machine_pre(i);
            events(i);
            // odd phase: falling edge; even phase: rising edge. Settle #1
            // past the edge before sampling outputs.
            if (i[0]) @(negedge clk); else @(posedge clk);
            machine_in(i);
            #1;
            dbg_phi = i[0];
            service;
            dump;
            machine_post(i);
        end
        $finish;
    end
endmodule
