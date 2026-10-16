// Control pins must match the timing contract after each phase, and must
// not follow intermediate decoder states between phase edges. Injected skew
// is a structural regression, not a model of a particular routed delay.
`timescale 1ns/1ps
`include "z80_defs.vh"

module tb_bus_controls;
    parameter USE_CEN = 1;
    reg clk = 0, cen = 1, reset_n = 0;
    reg wait_n = 1, int_n = 1, nmi_n = 1, busreq_n = 1;
    wire [15:0] addr;
    wire [7:0] data_out;
    reg [7:0] mem [0:65535];
    wire data_drive, m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    wire [7:0] data_in = !mreq_n && !rd_n ? mem[addr] : 8'hff;
    wire [8:0] pins = {data_drive, m1_n, mreq_n, iorq_n, rd_n,
                       wr_n, rfsh_n, halt_n, busack_n};
    z80_core #(.USE_CEN(USE_CEN)) dut (
        .clk(clk), .cen(cen), .reset_n(reset_n),
        .addr(addr), .data_in(data_in), .data_out(data_out), .data_drive(data_drive),
        .m1_n(m1_n), .mreq_n(mreq_n), .iorq_n(iorq_n), .rd_n(rd_n),
        .wr_n(wr_n), .rfsh_n(rfsh_n), .halt_n(halt_n), .busack_n(busack_n),
        .wait_n(wait_n), .int_n(int_n), .nmi_n(nmi_n), .busreq_n(busreq_n),
        .dbg_t(), .dbg_phi(), .dbg_m()
    );

    // Existing timing contract evaluated on the committed state. This catches
    // an accidental extra phase of latency, including refresh and WAIT phases.
    wire ref_drive, ref_m1, ref_mreq, ref_iorq, ref_rd, ref_wr, ref_rfsh;
    z80_timing reference_timing (
        .bus_op(dut.bus_op), .t_state(dut.t_state[2:0]), .phi(dut.phi),
        .m_len(dut.m_len), .m_addr(dut.m_addr), .m_wdata(dut.m_wdata),
        .reg_i(dut.reg_i), .reg_r(dut.reg_r), .addr(), .data_out(),
        .data_drive(ref_drive), .m1_n(ref_m1), .mreq_n(ref_mreq),
        .iorq_n(ref_iorq), .rd_n(ref_rd), .wr_n(ref_wr), .rfsh_n(ref_rfsh)
    );
    wire ref_halt = !(dut.halted || ((dut.exec_w == `EXEC_HALT) &&
                       (dut.bus_op == `BUSOP_M1) &&
                       (dut.t_state == 4) && dut.phi));
    // A memory-write Tw.P retains WR; the base timing decoder describes
    // only the initial T2.P before WR first falls.
    reg ref_wait_write = 0;
    always @(posedge clk or negedge reset_n)
        if (!reset_n) ref_wait_write <= 0;
        else if (!USE_CEN || cen)
            ref_wait_write <= dut.stall && dut.bus_op == `BUSOP_MWR;
    wire [8:0] expected = {dut.hold_pins ? 7'b0111111 :
                          {ref_drive, ref_m1, ref_mreq, ref_iorq, ref_rd,
                           ref_wait_write ? 1'b0 : ref_wr, ref_rfsh},
                          ref_halt, !dut.bus_granted};
    integer i, steps = 0, waits = 0, grants = 0, halts = 0;
    reg [7:0] bus_seen = 0;
    reg [8:0] before_pins;
    reg [3:0] saved_t;
    reg saved_phi;
    reg [2:0] saved_op;
    reg [7:0] saved_ir;

    initial begin
        #100000;
        $fatal(1, "bus control regression timed out");
    end

    task check_pins;
        begin
            if (pins !== expected)
                $fatal(1, "phase %0d op=%0d T%0d phi=%0d pins=%b expected=%b",
                       steps, dut.bus_op, dut.t_state, dut.phi, pins, expected);
        end
    endtask

    task step;
        reg [8:0] previous;
        begin
            #5;
            previous = pins;
            clk = 1;
            #2;
            steps = steps + 1;
            check_pins;
            if (reset_n && USE_CEN && !cen && pins !== previous)
                $fatal(1, "controls changed with clock enable disabled");
            if (reset_n && (!USE_CEN || cen)) begin
                bus_seen[dut.bus_op] = 1;
                if (dut.stall) waits = waits + 1;
                if (!busack_n) grants = grants + 1;
                if (!halt_n) halts = halts + 1;
            end
            #3; clk = 0;
        end
    endtask

    task unchanged;
        begin
            #1;
            if (pins !== before_pins)
                $fatal(1, "decoder skew leaked to pins: before=%b after=%b", before_pins, pins);
        end
    endtask

    always @(posedge clk)
        if (reset_n && (!USE_CEN || cen) && !mreq_n && !wr_n && data_drive)
            mem[addr] <= data_out;

    initial begin
        for (i = 0; i < 65536; i = i + 1) mem[i] = 0;
        // SP=8000; memory read/write; IO read/write; INC (HL); indexed read
        // with internal phases; IM 1; EI; HALT; loop. ISR/NMI return to HALT.
        {mem[0],mem[1],mem[2],mem[3],mem[4],mem[5],mem[6],mem[7],
         mem[8],mem[9],mem[10],mem[11],mem[12],mem[13],mem[14],mem[15],
         mem[16],mem[17],mem[18],mem[19],mem[20],mem[21],mem[22],mem[23],
         mem[24],mem[25],mem[26],mem[27],mem[28],mem[29],mem[30],mem[31],mem[32],mem[33]} =
         272'h3100803e553200403a0040d320db2021004034dd210040dd7e00ed56fb0076c30300;
        mem['h38] = 'hed; mem['h39] = 'h4d;
        mem['h66] = 'hed; mem['h67] = 'h45;
        step;
        reset_n = 1; #1; check_pins; // preserve reset-release T1.P
        for (i = 0; i < 1200; i = i + 1) begin
            cen = (i % 7) >= 2;
            wait_n = (i % 31) >= 6;
            busreq_n = !(i >= 350 && i < 390);
            nmi_n = !(i >= 500 && i < 505);
            int_n = !(i >= 750 && i < 790);
            step;
        end
        if ((bus_seen & 8'hfe) !== 8'hfe || waits == 0 || grants == 0 || halts == 0)
            $fatal(1, "missing coverage bus=%b waits=%0d grants=%0d halts=%0d",
                   bus_seen, waits, grants, halts);

        // Runtime reset with CEN low must immediately make transfers idle.
        cen = 0; reset_n = 0; #1; check_pins;
        step;
        mem[0] = 0;
        wait_n = 1; int_n = 1; nmi_n = 1; busreq_n = 1;
        reset_n = 1; #1; check_pins;
        cen = 1;
        while (!(dut.bus_op == `BUSOP_M1 && dut.t_state == 4 && dut.phi)) step;
        before_pins = pins;
        saved_t = dut.t_state; saved_phi = dut.phi; saved_op = dut.bus_op;
        saved_ir = dut.ir;

        // T4.N -> T1.P: new T arrives before phi. Old RTL asserts M1/RD/MREQ.
        force dut.t_state = 1;
        unchanged;
        force dut.phi = 0;
        unchanged;
        // Other intermediate decodes exercise WR/IORQ and data output enable.
        force dut.bus_op = `BUSOP_MWR;
        force dut.t_state = 3;
        unchanged;
        force dut.bus_op = `BUSOP_IOWR;
        unchanged;
        force dut.bus_op = `BUSOP_MRD;
        unchanged;
        release dut.t_state; release dut.phi; release dut.bus_op;
        dut.t_state = saved_t; dut.phi = saved_phi; dut.bus_op = saved_op;
        // HALT must also be isolated from transient instruction decoding.
        force dut.ir = 8'h76;
        unchanged;
        release dut.ir;
        dut.ir = saved_ir;
        #1; check_pins;
        step; // real T1.P must assert M1, leaving all transfer strobes idle
        if (m1_n !== 0 || {mreq_n, iorq_n, rd_n, wr_n} !== 4'b1111)
            $fatal(1, "invalid M1 entry");
        step; // T1.N must start the real fetch without an extra phase delay
        if ({mreq_n, rd_n} !== 2'b00) $fatal(1, "late opcode read");
        $display("PASS bus controls USE_CEN=%0d: %0d phases, bus=%b, WAIT/DMA/HALT/reset/skew",
                 USE_CEN, steps, bus_seen);
        $finish;
    end
endmodule
