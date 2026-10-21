// tb_chip_ref.v - the RTL reference for chip.py's transistor-level run:
// the same program on rtl/z80_top, a 4 MHz clock (the run's), and a line
// per clock edge with the pins, the T state and the machine cycle.
//
// 0000  LD A,5Ah        opcode fetch, memory read
// 0002  LD (8000h),A    ... memory write
// 0005  OUT (42h),A     ... I/O write
// 0007  IN A,(43h)      ... I/O read (the port answers A5h)
// 0009  HALT
`timescale 1ns/1ps

module tb_chip_ref;
    reg         clk = 1'b0;
    reg         reset_n = 1'b0;
    wire [15:0] addr;
    wire [7:0]  data_out;
    wire        data_drive;
    wire        m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    wire [3:0]  dbg_t;
    wire [2:0]  dbg_m;
    reg  [7:0]  mem [0:65535];
    wire [7:0]  data_in = (!rd_n && !mreq_n) ? mem[addr] : (!rd_n && !iorq_n) ? 8'hA5 : 8'hFF;

    z80_top dut (
        .clk(clk), .por_n(reset_n), .reset_n(reset_n),   // power-on with the first RESET
        .addr(addr), .data_in(data_in), .data_out(data_out), .data_drive(data_drive),
        .m1_n(m1_n), .mreq_n(mreq_n), .iorq_n(iorq_n), .rd_n(rd_n), .wr_n(wr_n),
        .rfsh_n(rfsh_n), .halt_n(halt_n), .busack_n(busack_n),
        .wait_n(1'b1), .int_n(1'b1), .nmi_n(1'b1), .busreq_n(1'b1),
        .dbg_t(dbg_t), .dbg_m(dbg_m)
    );

    always #125 clk = ~clk;                      // 4 MHz

    integer i, n;
    initial begin
        for (i = 0; i < 65536; i = i + 1) mem[i] = 8'h00;
        mem[0] = 8'h3E; mem[1] = 8'h5A;                     // LD A,5Ah
        mem[2] = 8'h32; mem[3] = 8'h00; mem[4] = 8'h80;     // LD (8000h),A
        mem[5] = 8'hD3; mem[6] = 8'h42;                     // OUT (42h),A
        mem[7] = 8'hDB; mem[8] = 8'h43;                     // IN A,(43h)
        mem[9] = 8'h76;                                     // HALT
        repeat (4) @(negedge clk);
        reset_n = 1'b1;
    end

    // a line per clock edge, 1 ns after it
    always @(clk) begin
        #1;
        $display("EDGE %0d %b %b A=%04h DO=%02h DD=%b M1=%b MREQ=%b IORQ=%b RD=%b WR=%b RFSH=%b HALT=%b DI=%02h T=%0d M=%0d",
                 $time - 1, clk, reset_n, addr, data_out, data_drive, m1_n, mreq_n, iorq_n, rd_n, wr_n,
                 rfsh_n, halt_n, data_in, dbg_t, dbg_m);
    end

    initial begin
        n = 0;
        wait (reset_n);
        while (halt_n !== 1'b0 && n < 200) begin @(posedge clk); n = n + 1; end
        repeat (2) @(posedge clk);
        $display("DONE %0d clocks after RESET", n);
        $finish;
    end
endmodule
