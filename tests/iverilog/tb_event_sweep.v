// ===========================================================================
// tb_event_sweep.v - RESET / INT / NMI timing sweep testbench (one case per
// run). Drives pin events placed at 1/16-T resolution relative to a marker
// opcode fetch and dumps one line of pins per half-cycle, in the format
// scripts/event_sweep_pz80.c produces for the perfectz80 netlist, so
// scripts/event_sweep.py can diff the two.
//
//   vvp tb_event_sweep.vvp +prog=P.hex [+events=E.txt] +run=T
//       [+marker=0200] [+inta_release=1] [+int_at=0066] [+hold=8000 +hold_t=N]
//       [+inta_vec=FF] [+inta_n=N +inta_ops=HEX] [+busreq_walk=1] [+wait_walk=1]
//       [+dwin=r0,r1,w,vv,dd]
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
//   +busreq_walk   with BUSREQ low from the events file: each grant is
//                  released for one T-state -- BUSREQ high when BUSACK is
//                  seen low, low again once BUSACK is high
//   +wait_walk     WAIT low for the three half-cycles from each fall of
//                  MREQ (not a refresh) or IORQ
// The interrupting device: +inta_vec=V is the byte it puts on the bus in
// INTA (M1 + IORQ), FF if not given; +inta_n=N +inta_ops=HEX are N more
// bytes (HEX little-endian, the first one lowest) it hands out after it,
// one per INTA cycle or memory read, in place of memory: the rest of an
// IM0 instruction (the die fetches the opcode after a prefix with another
// INTA and reads the operands at the unchanged PC). An opcode fetch from
// memory ends the device's instruction. An IN reads what an OUT wrote to its
// address, FF before (as perfectz80's harness and the C system do).
// +dwin is a data window: in half-cycles r0 .. r1 from the marker's T1 the
// data bus carries dd (hex), in half-cycle w vv, from just after the edge
// that starts the half-cycle to the edge that ends it (as the events).
// Memory writes are captured from the pins at the end of every half-cycle.
// Output: `h clk addr data mreq iorq rd wr m1 rfsh halt busack aen den cen f wz q`
// for halves 0 .. 2*run-1 from the marker's T1, pins sampled at the end of
// each half; aen den cen are the pads driven (hex masks: A0-A15, D0-D7,
// MREQ IORQ RD WR M1 RFSH HALT BUSACK bit 0 first): addr_drive, data_drive,
// ctl_drive (M1 RFSH HALT BUSACK always); then the core's F, WZ and Q.
// ===========================================================================
`timescale 1ns/1ns

module tb_event_sweep;
    reg         clk = 1'b0;
    reg         por_n = 1'b0;           // power-on reset, first half only
    reg         reset_n = 1'b0;         // the RESET pin
    reg         wait_n = 1'b1, int_n = 1'b1, nmi_n = 1'b1, busreq_n = 1'b1;
    wire [7:0]  data_in;
    wire [15:0] addr;
    wire [7:0]  data_out;
    wire        data_drive, m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    wire        addr_drive, ctl_drive;
    wire [3:0]  dbg_t;
    wire [2:0]  dbg_m;
    reg  [7:0]  mem [0:65535];

    z80_top dut (
        .clk(clk), .por_n(por_n), .reset_n(reset_n),
        .addr(addr), .data_in(data_in), .data_out(data_out), .data_drive(data_drive),
        .m1_n(m1_n), .mreq_n(mreq_n), .iorq_n(iorq_n), .rd_n(rd_n), .wr_n(wr_n),
        .rfsh_n(rfsh_n), .halt_n(halt_n), .busack_n(busack_n),
        .addr_drive(addr_drive), .ctl_drive(ctl_drive),
        .wait_n(wait_n), .int_n(int_n), .nmi_n(nmi_n), .busreq_n(busreq_n),
        .dbg_t(dbg_t), .dbg_m(dbg_m)
    );

    // Memory reads as in tb_z80.v (or the device's byte); INTA (M1 + IORQ)
    // sees the device's byte, I/O reads 0xFF.
    reg  [7:0]  dev_byte;
    reg  [71:0] dev_q;          // the device's bytes, the first one lowest
    reg         dev_read;
    reg  [7:0]  io [0:65535];   // I/O space: OUT writes, IN reads back (FF until written)
    reg         dwin_on;        // +dwin: this half-cycle is in the window
    reg  [7:0]  dwin_byte;
    assign data_in = dwin_on ? dwin_byte :
                     (!mreq_n && !rd_n) ? (dev_read ? dev_byte : mem[addr]) :
                     (!iorq_n && !m1_n) ? dev_byte :
                     (!iorq_n && !rd_n) ? io[addr] : 8'h00;

    integer ev_pin [0:7], ev_lo [0:7], ev_hi [0:7];
    integer n_ev, fd, r, k, e, run, marker, hold, hold_t, int_at, inta_release, vec;
    integer t, half, mark_half, hold_until, in_read;
    integer inta_n, dev_pos, was_read, was_inta;
    integer busreq_walk, wait_walk, walk_up, walk_wait_until, strobe, prev_strobe;
    integer hold_low;           // +hold's WAIT is low (RESET releases it; WAIT pulses it doesn't)
    integer dw_r0, dw_r1, dw_w, dw_v, dw_d;
    reg [8*64-1:0] dwin_arg;
    reg [63:0] inta_ops;
    reg [8*512-1:0] progf, evf;     // paths up to 512 characters
    reg found;

    task drive(input integer pin, input integer level);
        case (pin)
            0: nmi_n   = level;
            1: int_n   = level;
            2: wait_n  = level;
            3: busreq_n = level;
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
        if (!$value$plusargs("inta_vec=%h", vec)) vec = 'hff;
        if (!$value$plusargs("inta_n=%d", inta_n)) inta_n = 0;
        if (!$value$plusargs("inta_ops=%h", inta_ops)) inta_ops = 64'h0;
        if (!$value$plusargs("busreq_walk=%d", busreq_walk)) busreq_walk = 0;
        if (!$value$plusargs("wait_walk=%d", wait_walk)) wait_walk = 0;
        walk_up = 0; walk_wait_until = -1; prev_strobe = 0; hold_low = 0;
        dw_r0 = -1; dw_r1 = -1; dw_w = -1; dw_v = 0; dw_d = 0; dwin_on = 1'b0; dwin_byte = 8'h00;
        if ($value$plusargs("dwin=%s", dwin_arg))
            if ($sscanf(dwin_arg, "%d,%d,%d,%h,%h", dw_r0, dw_r1, dw_w, dw_v, dw_d) != 5) begin
                $display("bad +dwin"); $finish(0);
            end
        dev_q = {inta_ops, vec[7:0]};
        dev_read = 1'b0; dev_byte = 8'h00; dev_pos = 0; was_read = 0; was_inta = 0;
        n_ev = 0;
        if ($value$plusargs("events=%s", evf)) begin
            fd = $fopen(evf, "r");
            if (fd == 0) begin $display("cannot open %0s", evf); $finish(0); end
            while (n_ev < 8 && $fscanf(fd, "%d %d %d\n", ev_pin[n_ev], ev_lo[n_ev], ev_hi[n_ev]) == 3)
                n_ev = n_ev + 1;
            $fclose(fd);
        end
        for (k = 0; k < 65536; k = k + 1) begin mem[k] = 8'h00; io[k] = 8'hff; end
        $readmemh(progf, mem);

        // Power-on: por_n for the first half-cycle, RESET 4 T low, released
        // in a clock-low half.
        found = 0; mark_half = 0; hold_until = -1; in_read = 0;
        half = 0;
        for (t = 0; t >= 0; t = t + 1) begin
            // t counts 1/32 T: edges at multiples of 16, rising at even halves.
            if (t % 16 == 0) begin
                clk = (t % 32 == 0);
                half = t / 16;
            end
            if (t == 8) por_n = 1'b1;
            if (!found && t == 4 * 32 + 16 + 8) reset_n = 1'b1;
            if (t % 16 == 1) begin
                // the device: its next byte at each INTA, and at each memory
                // read after one until it has none left
                if (!m1_n && !iorq_n) begin
                    if (!was_inta) begin
                        dev_byte = (dev_pos <= inta_n) ? dev_q[8 * dev_pos +: 8] : 8'hff;
                        dev_pos = dev_pos + 1;
                    end
                    was_inta = 1;
                end else was_inta = 0;
                if (!mreq_n && !rd_n) begin
                    if (!m1_n) dev_pos = 0;                 // an opcode from memory
                    else if (!was_read && dev_pos > 0 && dev_pos <= inta_n) begin
                        dev_read = 1'b1; dev_byte = dev_q[8 * dev_pos +: 8];
                        dev_pos = dev_pos + 1;
                    end
                    was_read = 1;
                end else begin
                    dev_read = 1'b0; was_read = 0;
                end
                if (!found && !m1_n && addr == marker) begin
                    found = 1;
                    mark_half = half - (half % 2);   // the T1 rising edge
                end
                if (found) begin
                    dwin_on = dw_r0 >= 0 && half - mark_half >= dw_r0 && half - mark_half <= dw_r1;
                    dwin_byte = (half - mark_half == dw_w) ? dw_v : dw_d;
                    if (inta_release && !m1_n && !iorq_n) int_n = 1'b1;
                    if (int_at >= 0 && !m1_n && !mreq_n && addr == int_at) int_n = 1'b0;
                    if (int_at >= 0 && !m1_n && !iorq_n) int_n = 1'b1;
                    if (hold >= 0 && m1_n && !mreq_n && !rd_n && addr == hold) begin
                        if (!in_read) begin
                            wait_n = 1'b0; in_read = 1; hold_low = 1;
                            hold_until = half + 2 * hold_t;
                        end
                    end else in_read = 0;
                    if (hold_low && (half >= hold_until || !reset_n)) begin wait_n = 1'b1; hold_low = 0; end
                    if (busreq_walk) begin
                        if (!busack_n && !walk_up) begin busreq_n = 1'b1; walk_up = 1; end
                        else if (walk_up && busack_n) begin busreq_n = 1'b0; walk_up = 0; end
                    end
                    if (wait_walk) begin
                        strobe = ((!mreq_n && rfsh_n) || !iorq_n) ? 1 : 0;
                        if (strobe && !prev_strobe) begin
                            wait_n = 1'b0; walk_wait_until = half - mark_half + 3;
                        end else if (walk_wait_until >= 0 && half - mark_half >= walk_wait_until) begin
                            wait_n = 1'b1; walk_wait_until = -1;
                        end
                        prev_strobe = strobe;
                    end
                end
            end
            // Pin events at sixteenth s = (t - mark_t - 1) / 2, mark_t = mark_half*16.
            if (found && (t - mark_half * 16) % 2 == 1) begin
                for (e = 0; e < n_ev; e = e + 1) begin
                    if ((t - mark_half * 16 - 1) / 2 == ev_lo[e]) drive(ev_pin[e], 0);
                    if (ev_hi[e] >= 0 && (t - mark_half * 16 - 1) / 2 == ev_hi[e]) drive(ev_pin[e], 1);
                end
                if (hold_low && !reset_n) begin wait_n = 1'b1; hold_low = 0; end
            end
            #1;
            if (t % 16 == 15 && !mreq_n && !wr_n) mem[addr] = data_out;
            if (t % 16 == 15 && !iorq_n && !wr_n) io[addr] = data_out;
            if (found && t % 16 == 15) begin
                $display("%0d %0d %04x %02x %0d %0d %0d %0d %0d %0d %0d %0d %04x %02x %02x %02x %04x %02x",
                         half - mark_half, clk, addr, data_out,
                         mreq_n, iorq_n, rd_n, wr_n, m1_n, rfsh_n, halt_n, busack_n,
                         addr_drive ? 16'hffff : 16'h0000, data_drive ? 8'hff : 8'h00,
                         {4'hf, {4{ctl_drive}}},
                         dut.u_core.r_af[7:0], dut.u_core.r_wz, dut.u_core.reg_q);
                if (half - mark_half >= 2 * run - 1) $finish(0);
            end
            if (!found && t > 32 * 4000) begin $display("marker not reached"); $finish(0); end
        end
    end
endmodule
