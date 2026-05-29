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
    reg [1023:0] progf;

    task dump;
        $display("%0d %0d %0d %04X %02X %02X %0d %0d %0d %0d %0d %0d %0d %0d",
                 dbg_t, dbg_phi, dbg_m, addr, data_out, data_in,
                 mreq_n, iorq_n, rd_n, wr_n, m1_n, rfsh_n, halt_n, busack_n);
    endtask

    initial begin
        if (!$value$plusargs("prog=%s", progf)) progf = "prog.hex";
        if (!$value$plusargs("phases=%d", phases)) phases = 200;
        if (!$value$plusargs("nmi=%d", nmiph)) nmiph = -1;  // pulse nmi_n low at this phase
        for (i = 0; i < 65536; i = i + 1) mem[i] = 8'h00;
        $readmemh(progf, mem);

        reset_n = 1'b0;
        repeat (4) @(negedge clk);
        reset_n = 1'b1;

        nmi_n = (0 == nmiph) ? 1'b0 : 1'b1;
        dump;                                 // line 1 = reset state (T1.P)
        for (i = 1; i < phases; i = i + 1) begin
            @(negedge clk);
            nmi_n = (i == nmiph) ? 1'b0 : 1'b1;
            dump;
        end
        $finish;
    end
endmodule
