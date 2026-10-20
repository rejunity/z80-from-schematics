// ===========================================================================
// tb_event_sweep.v - RESET / INT / NMI timing sweep testbench (one case per
// run). Drives pin events placed at 1/16-T resolution relative to a marker
// opcode fetch and dumps one line of pins per half-cycle, in the format
// scripts/event_sweep_pz80.c produces for the perfectz80 netlist, so
// scripts/event_sweep.py can diff the two.
//
//   vvp tb_event_sweep.vvp +prog=P.hex [+events=E.txt] +run=T
//       [+marker=0200] [+inta_release=1] [+int_at=0066] [+hold=8000 +hold_t=N]
//
// Time: one T-state is 32 time units, so an event placed at sixteenth s
// lands at 2*s+1 units, never on a clock edge. Sixteenth 0 is the rising
// edge that starts the marker fetch's T1 (the edge before the first half
// in which M1 is low with addr == marker).
//
// Events file: one `<pin> <lo> <hi>` line per pulse, pin 0=NMI 1=INT 4=RESET,
// lo/hi in sixteenths (hi < 0: stays low).
// Reactive pins (evaluated once per half-cycle, just after its edge):
//   +inta_release  INT goes high during the first INTA half-cycle
//   +int_at=A      INT goes low during the M1 fetch of A, high again at INTA
//   +hold=A        WAIT low at the start of each read of A (not M1) for
//                  +hold_t T-states, or until RESET falls
// Memory writes are captured from the pins at the end of every half-cycle.
// Output: `h clk addr data mreq iorq rd wr m1 rfsh halt busack` for halves
// 0 .. 2*run-1 from the marker's T1, pins sampled at the end of each half.
// ===========================================================================
`timescale 1ns/1ns

module tb_event_sweep;
    reg         clk = 1'b0;
    reg         reset_n = 1'b0;
    reg         wait_n = 1'b1, int_n = 1'b1, nmi_n = 1'b1, busreq_n = 1'b1;
    wire [7:0]  data_in;
    wire [15:0] addr;
    wire [7:0]  data_out;
    wire        data_drive, m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    wire [3:0]  dbg_t;
    wire [2:0]  dbg_m;
    reg  [7:0]  mem [0:65535];

    z80_top dut (
        .clk(clk), .reset_n(reset_n),
        .addr(addr), .data_in(data_in), .data_out(data_out), .data_drive(data_drive),
        .m1_n(m1_n), .mreq_n(mreq_n), .iorq_n(iorq_n), .rd_n(rd_n), .wr_n(wr_n),
        .rfsh_n(rfsh_n), .halt_n(halt_n), .busack_n(busack_n),
        .wait_n(wait_n), .int_n(int_n), .nmi_n(nmi_n), .busreq_n(busreq_n),
        .dbg_t(dbg_t), .dbg_m(dbg_m)
    );

    // Memory reads as in tb_z80.v; INTA (M1 + IORQ) and I/O reads see 0xFF.
    assign data_in = (!mreq_n && !rd_n) ? mem[addr] :
                     (!iorq_n && (!rd_n || !m1_n)) ? 8'hff : 8'h00;

    integer ev_pin [0:7], ev_lo [0:7], ev_hi [0:7];
    integer n_ev, fd, r, k, e, run, marker, hold, hold_t, int_at, inta_release;
    integer t, half, mark_half, hold_until, in_read;
    reg [8*512-1:0] progf, evf;     // paths up to 512 characters
    reg found;

    task drive(input integer pin, input integer level);
        case (pin)
            0: nmi_n   = level;
            1: int_n   = level;
            4: reset_n = level;
        endcase
    endtask

    initial begin
        if (!$value$plusargs("prog=%s", progf)) progf = "prog.hex";
        if (!$value$plusargs("run=%d", run)) run = 100;
        if (!$value$plusargs("marker=%h", marker)) marker = 16'h0200;
        if (!$value$plusargs("inta_release=%d", inta_release)) inta_release = 0;
        if (!$value$plusargs("int_at=%h", int_at)) int_at = -1;
        if (!$value$plusargs("hold=%h", hold)) hold = -1;
        if (!$value$plusargs("hold_t=%d", hold_t)) hold_t = 0;
        n_ev = 0;
        if ($value$plusargs("events=%s", evf)) begin
            fd = $fopen(evf, "r");
            if (fd == 0) begin $display("cannot open %0s", evf); $finish(0); end
            while (n_ev < 8 && $fscanf(fd, "%d %d %d\n", ev_pin[n_ev], ev_lo[n_ev], ev_hi[n_ev]) == 3)
                n_ev = n_ev + 1;
            $fclose(fd);
        end
        for (k = 0; k < 65536; k = k + 1) mem[k] = 8'h00;
        $readmemh(progf, mem);

        // Power-on reset: 4 T low, released in a clock-low half.
        found = 0; mark_half = 0; hold_until = -1; in_read = 0;
        half = 0;
        for (t = 0; t >= 0; t = t + 1) begin
            // t counts 1/32 T: edges at multiples of 16, rising at even halves.
            if (t % 16 == 0) begin
                clk = (t % 32 == 0);
                half = t / 16;
            end
            if (!found && t == 4 * 32 + 16 + 8) reset_n = 1'b1;
            if (t % 16 == 1) begin
                if (!found && !m1_n && addr == marker) begin
                    found = 1;
                    mark_half = half - (half % 2);   // the T1 rising edge
                end
                if (found) begin
                    if (inta_release && !m1_n && !iorq_n) int_n = 1'b1;
                    if (int_at >= 0 && !m1_n && !mreq_n && addr == int_at) int_n = 1'b0;
                    if (int_at >= 0 && !m1_n && !iorq_n) int_n = 1'b1;
                    if (hold >= 0 && m1_n && !mreq_n && !rd_n && addr == hold) begin
                        if (!in_read) begin
                            wait_n = 1'b0; in_read = 1;
                            hold_until = half + 2 * hold_t;
                        end
                    end else in_read = 0;
                    if (!wait_n && (half >= hold_until || !reset_n)) wait_n = 1'b1;
                end
            end
            // Pin events at sixteenth s = (t - mark_t - 1) / 2, mark_t = mark_half*16.
            if (found && (t - mark_half * 16) % 2 == 1) begin
                for (e = 0; e < n_ev; e = e + 1) begin
                    if ((t - mark_half * 16 - 1) / 2 == ev_lo[e]) drive(ev_pin[e], 0);
                    if (ev_hi[e] >= 0 && (t - mark_half * 16 - 1) / 2 == ev_hi[e]) drive(ev_pin[e], 1);
                end
                if (!reset_n && !wait_n) wait_n = 1'b1;
            end
            #1;
            if (t % 16 == 15 && !mreq_n && !wr_n) mem[addr] = data_out;
            if (found && t % 16 == 15) begin
                $display("%0d %0d %04x %02x %0d %0d %0d %0d %0d %0d %0d %0d",
                         half - mark_half, clk, addr, data_out,
                         mreq_n, iorq_n, rd_n, wr_n, m1_n, rfsh_n, halt_n, busack_n);
                if (half - mark_half >= 2 * run - 1) $finish(0);
            end
            if (!found && t > 32 * 4000) begin $display("marker not reached"); $finish(0); end
        end
    end
endmodule
