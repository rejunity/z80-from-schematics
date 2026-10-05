// WAIT must stretch one continuous write pulse, including each Tw high half.
`timescale 1ns/1ps
`include "z80_defs.vh"
module tb_wait_write;
    parameter USE_CEN = 1;
    reg clk = 0, cen = 1, reset_n = 0, wait_n = 1;
    reg [7:0] mem [0:65535];
    wire [15:0] addr;
    wire [7:0] data_out;
    wire data_drive, m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    z80_core #(.USE_CEN(USE_CEN)) dut (
        .clk(clk), .cen(cen), .reset_n(reset_n), .addr(addr),
        .data_in(mem[addr]), .data_out(data_out), .data_drive(data_drive),
        .m1_n(m1_n), .mreq_n(mreq_n), .iorq_n(iorq_n), .rd_n(rd_n),
        .wr_n(wr_n), .rfsh_n(rfsh_n), .halt_n(halt_n), .busack_n(busack_n),
        .wait_n(wait_n), .int_n(1'b1), .nmi_n(1'b1), .busreq_n(1'b1),
        .dbg_t(), .dbg_phi(), .dbg_m()
    );
    task step;
        begin #5 clk = 1; #5 clk = 0; end
    endtask
    task reach;
        input [2:0] op;
        input [3:0] t;
        integer n;
        begin : search
            for (n = 0; n < 300; n = n + 1) begin
                if (dut.bus_op == op && dut.t_state == t && !dut.phi)
                    disable search;
                step;
            end
            $fatal(1, "write phase not reached op=%0d T=%0d", op, t);
        end
    endtask
    task check_write;
        input [2:0] op;
        input [15:0] address;
        begin
            if (addr !== address || data_out !== 8'h55 || data_drive !== 1 ||
                wr_n !== 0 || rd_n !== 1 || m1_n !== 1 || rfsh_n !== 1 ||
                mreq_n !== (op != `BUSOP_MWR) || iorq_n !== (op != `BUSOP_IOWR))
                $fatal(1, "write changed during Tw: op=%0d T=%0d phi=%b addr=%h data=%h WR=%b",
                       op, dut.t_state, dut.phi, addr, data_out, wr_n);
        end
    endtask
    task held_write;
        input [2:0] op;
        input [3:0] sample_t;
        input [15:0] address;
        integer n;
        begin
            reach(op, sample_t);
            if (op == `BUSOP_MWR && wr_n !== 1)
                $fatal(1, "WR asserted before initial T2.N");
            wait_n = 0;
            step;
            check_write(op, address);
            for (n = 0; n < 20; n = n + 1) begin
                step; // Tw.P must retain the asserted write strobe
                check_write(op, address);
                step; // Tw.N samples WAIT again
                check_write(op, address);
                if (dut.t_state !== sample_t) $fatal(1, "write escaped WAIT");
            end
            if (USE_CEN) begin
                cen = 0; step; check_write(op, address); cen = 1;
            end
            wait_n = 1;
            step; check_write(op, address); // previous low sample still inserts Tw
            step; check_write(op, address); // released WAIT sampled at Tw.N
            step; check_write(op, address); // final T.P
            step; // final T.N releases write
            if (wr_n !== 1 || mreq_n !== 1 || iorq_n !== 1)
                $fatal(1, "write did not end after WAIT release");
        end
    endtask
    integer i;
    initial begin
        for (i = 0; i < 65536; i = i + 1) mem[i] = 0;
        // LD A,55; LD (4000),A; OUT (20),A; HALT
        {mem[0],mem[1],mem[2],mem[3],mem[4],mem[5],mem[6],mem[7]} =
            64'h3e55320040d32076;
        step; reset_n = 1;
        held_write(`BUSOP_MWR, 2, 16'h4000);
        held_write(`BUSOP_IOWR, 3, 16'h5520);

        // Reproduce the guard's late assertion: WR has already fallen by
        // the time WAIT goes low. This write must finish without a Tw.
        reset_n = 0; step; reset_n = 1;
        reach(`BUSOP_MWR, 2);
        step; check_write(`BUSOP_MWR, 16'h4000);
        wait_n = 0;
        step;
        if (dut.t_state !== 3 || dut.phi !== 0)
            $fatal(1, "late WAIT incorrectly stretched the current write");
        step;
        if (wr_n !== 1 || mreq_n !== 1)
            $fatal(1, "write did not finish with late WAIT");
        wait_n = 1;

        // A reset during a held write must release the strobes immediately,
        // including when the phase clock enable is stopped.
        reset_n = 0; step; reset_n = 1;
        reach(`BUSOP_MWR, 2);
        wait_n = 0; step; step;
        check_write(`BUSOP_MWR, 16'h4000);
        cen = 0; reset_n = 0; #1;
        if ({wr_n, mreq_n, iorq_n} !== 3'b111 || data_drive !== 0)
            $fatal(1, "reset did not release held write");
        $display("PASS continuous memory/IO write WAIT strobes USE_CEN=%0d", USE_CEN);
        $finish;
    end
endmodule
