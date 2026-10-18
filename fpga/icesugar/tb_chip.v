// tb_chip.v - bus test of the chip build: the Z80 on the PMOD pins, with
// an external 1 KiB RAM and an external CLK wired to them, the way the
// chip build is used on hardware.
//
// `make sim_chip` runs it twice: against the RTL (top.v's behavioural pad
// stand-ins) and against the synthesized chip netlist with yosys' own
// iCE40 cell models, which exercises the real pad configuration: CLK in
// through SB_GB_IO, D through SB_IO tristate (OE = data_drive) plus its
// input path, the rest plain outputs.
//
// The RAM is preloaded with a program that writes a table, reads it back,
// calls / returns through a stack at the top of the 1 KiB window, and does
// one IN and one OUT. I/O cycles are what the PMOD bus shows as /RD or /WR
// with /MREQ high (no /IORQ on this board): the RAM ignores them, the IN
// reads the pulled-up open bus (0xFF), the testbench logs the OUT. The red
// LED (lit while the Z80 is in reset or halted) marks reset release and
// the final HALT.

`timescale 1ns / 1ps

module tb_chip;
    reg clk = 1'b0;             // the 12 MHz board clock, sped up: it only
    always #1 clk = ~clk;       // times the ~2^16-cycle reset stretch
    reg zclk = 1'b0;            // external Z80 CLK, 4 MHz
    always #125 zclk = ~zclk;

    wire        z80_clk = zclk;
    wire [7:0]  d;
    wire [9:0]  a;
    wire        mreq_n, rd_n, wr_n;
    wire        uart_tx, ledr_n, ledg_n, ledb_n;
    pullup pu[7:0] (d);         // the board's open bus reads 0xFF

    // top.v's default EXTERNAL_BUS = 1 is the chip build (the netlist has
    // no parameters left to override)
    top dut (
        .clk        (clk),
        .uart_rx    (1'b1),
        .uart_tx    (uart_tx),
        .btn_n      (1'b1),
        .ledr_n     (ledr_n),
        .ledg_n     (ledg_n),
        .ledb_n     (ledb_n),
        .z80_clk    (z80_clk),
        .z80_d      (d),
        .z80_mreq_n (mreq_n),
        .z80_rd_n   (rd_n),
        .z80_wr_n   (wr_n),
        .z80_a      (a)
    );

    // ---- external asynchronous RAM: /CE = /MREQ, /OE = /RD, /WE = /WR ----
    // Writes are enabled once the CPU has left reset (red LED dark): yosys'
    // gate-level FF models only act on an async reset *edge*, so at time 0
    // the netlist's pins read 0 (active strobes) until the first CLK edge.
    // (On the iCE40 the flops are cleared at configuration and the level-
    // sensitive async reset holds the strobes idle from the start.)
    reg [7:0] mem [0:1023];
    reg       running = 1'b0;
    always @(posedge ledr_n) running = 1'b1;
    assign d = (!mreq_n && !rd_n) ? mem[a] : 8'hzz;
    always @(*) if (running && !mreq_n && !wr_n) mem[a] = d;

    // ---- I/O write log, bus contention / X check ----
    reg [7:0] io_port = 8'h00, io_data = 8'h00;
    integer   io_writes = 0, bad_bus = 0;
    always @(negedge wr_n) if (running && mreq_n) begin
        io_port = a[7:0]; io_data = d; io_writes = io_writes + 1;
    end
    always @(negedge zclk) if (running && !rd_n && (^d === 1'bx)) bad_bus = bad_bus + 1;

    integer i;
    initial begin
        for (i = 0; i < 1024; i = i + 1) mem[i] = 8'h00;
        // 0000  LD SP,0400h      ; stack grows down from the top of the window
        // 0003  LD HL,0200h
        // 0006  LD B,10h
        // 0008  LD A,01h
        // 000A  LD (HL),A        ; 0200..020F = 01..10
        // 000B  INC HL
        // 000C  INC A
        // 000D  DJNZ 000Ah
        // 000F  CALL 0020h       ; return address 0012 -> 03FE/03FF
        // 0012  HALT
        {mem[10'h000], mem[10'h001], mem[10'h002]} = {8'h31, 8'h00, 8'h04};
        {mem[10'h003], mem[10'h004], mem[10'h005]} = {8'h21, 8'h00, 8'h02};
        {mem[10'h006], mem[10'h007]}               = {8'h06, 8'h10};
        {mem[10'h008], mem[10'h009]}               = {8'h3E, 8'h01};
        mem[10'h00A] = 8'h77;
        mem[10'h00B] = 8'h23;
        mem[10'h00C] = 8'h3C;
        {mem[10'h00D], mem[10'h00E]}               = {8'h10, 8'hFB};
        {mem[10'h00F], mem[10'h010], mem[10'h011]} = {8'hCD, 8'h20, 8'h00};
        mem[10'h012] = 8'h76;
        // 0020  LD A,(0205h)     ; 06
        // 0023  LD (03F0h),A
        // 0026  LD HL,(0200h)    ; 0201h
        // 0029  LD (03F2h),HL
        // 002C  IN A,(55h)       ; open bus: FFh
        // 002E  LD (03F4h),A
        // 0031  LD A,5Ah
        // 0033  OUT (0AAh),A
        // 0035  RET
        {mem[10'h020], mem[10'h021], mem[10'h022]} = {8'h3A, 8'h05, 8'h02};
        {mem[10'h023], mem[10'h024], mem[10'h025]} = {8'h32, 8'hF0, 8'h03};
        {mem[10'h026], mem[10'h027], mem[10'h028]} = {8'h2A, 8'h00, 8'h02};
        {mem[10'h029], mem[10'h02A], mem[10'h02B]} = {8'h22, 8'hF2, 8'h03};
        {mem[10'h02C], mem[10'h02D]}               = {8'hDB, 8'h55};
        {mem[10'h02E], mem[10'h02F], mem[10'h030]} = {8'h32, 8'hF4, 8'h03};
        {mem[10'h031], mem[10'h032]}               = {8'h3E, 8'h5A};
        {mem[10'h033], mem[10'h034]}               = {8'hD3, 8'hAA};
        mem[10'h035] = 8'hC9;
    end

    // ---- run: reset release (red LED off), then HALT (red LED on) ----
    integer errors = 0;
    task expect8(input [9:0] addr, input [7:0] want);
        if (mem[addr] !== want) begin
            $display("[tb_chip] FAIL: mem[%03h] = %02h, want %02h", addr, mem[addr], want);
            errors = errors + 1;
        end
    endtask

    initial begin
        $timeformat(-6, 2, " us", 0);
        wait (ledr_n === 1'b1);
        $display("[tb_chip] reset released at %0t", $realtime);
        wait (ledr_n === 1'b0);
        $display("[tb_chip] HALT at %0t", $realtime);
        for (i = 0; i < 16; i = i + 1) expect8(10'h200 + i, i + 1);
        expect8(10'h3F0, 8'h06);
        expect8(10'h3F2, 8'h01);
        expect8(10'h3F3, 8'h02);
        expect8(10'h3F4, 8'hFF);
        expect8(10'h3FE, 8'h12);
        expect8(10'h3FF, 8'h00);
        if (io_writes != 1 || io_port != 8'hAA || io_data != 8'h5A) begin
            $display("[tb_chip] FAIL: %0d I/O writes, last OUT (%02h),%02h; want one OUT (AA),5A",
                     io_writes, io_port, io_data);
            errors = errors + 1;
        end
        if (bad_bus != 0) begin
            $display("[tb_chip] FAIL: %0d reads saw X on D (contention?)", bad_bus);
            errors = errors + 1;
        end
        if (errors != 0) $fatal(1, "[tb_chip] FAIL");
        $display("[tb_chip] PASS: table, read-back, CALL/RET stack, IN open bus = FF, OUT (AA),5A");
        $finish;
    end

    initial begin
        #5_000_000;
        $fatal(1, "[tb_chip] FAIL: timeout (red LED %b)", ledr_n);
    end
endmodule
