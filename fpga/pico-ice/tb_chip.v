// tb_chip.v - smoke test of the pico-ice *chip* build (EXTERNAL_BUS=1).
//
// The testbench is the external Z80 system wired to the headers, using
// only the signals this board brings out (CLK, D0-D7, /MREQ, /RD, /WR,
// A0-A12; /RESET is SW2):
//   - CLK: an external 4 MHz clock on ICE44 (asynchronous to the board's
//     12 MHz, which only runs the reset stretch)
//   - an 8 KiB asynchronous SRAM on A0-A12, enabled by /MREQ, read with
//     /RD, written while /WR is low
//   - an output port: /WR low while /MREQ is high is an I/O write (there
//     is no /IORQ pin); port 0x81 prints the data byte
//
// Program (hand-assembled): copy a string from "ROM" at 0x0040 to RAM at
// 0x1000 with LDIR, then CALL a routine that prints it from RAM via OUT
// (0x81),A, and HALT. That exercises opcode fetches, memory reads and
// writes, the stack (CALL/RET) and I/O writes through the header pins.
// The test also fails on bus contention: the CPU must never drive D while
// the memory does.

`timescale 1ns / 1ps

module tb_chip;
    reg clk = 0;
    always #41.667 clk = ~clk;          // board clock: 12 MHz from the RP2040
    reg zclk = 0;
    always #125 zclk = ~zclk;           // external Z80 CLK: 4 MHz, 50 % duty

    wire uart_tx, led_r, led_g, led_b;
    wire        z80_clk = zclk;
    wire [7:0]  z80_d;
    wire [12:0] z80_a;
    wire z80_mreq_n, z80_rd_n, z80_wr_n;

    top #(.EXTERNAL_BUS(1'b1)) dut (
        .clk        (clk),
        .uart_rx    (1'b1),
        .uart_tx    (uart_tx),
        .btn_n      (1'b1),
        .led_r      (led_r),
        .led_g      (led_g),
        .led_b      (led_b),
        .z80_clk    (z80_clk),
        .z80_d      (z80_d),
        .z80_a      (z80_a),
        .z80_mreq_n (z80_mreq_n),
        .z80_rd_n   (z80_rd_n),
        .z80_wr_n   (z80_wr_n)
    );

    // ---- 8 KiB SRAM on A0-A12 ----
    reg [7:0] mem [0:8191];
    integer i;
    reg [8*14-1:0] msg = {"Z80 chip OK", 8'h0D, 8'h0A, 8'h00};
    initial begin
        for (i = 0; i < 8192; i = i + 1) mem[i] = 8'h00;    // NOP
        // 0000  LD SP,2000h / LD HL,0040h / LD DE,1000h / LD BC,14 / LDIR
        mem[16'h00] = 8'h31; mem[16'h01] = 8'h00; mem[16'h02] = 8'h20;
        mem[16'h03] = 8'h21; mem[16'h04] = 8'h40; mem[16'h05] = 8'h00;
        mem[16'h06] = 8'h11; mem[16'h07] = 8'h00; mem[16'h08] = 8'h10;
        mem[16'h09] = 8'h01; mem[16'h0A] = 8'h0E; mem[16'h0B] = 8'h00;
        mem[16'h0C] = 8'hED; mem[16'h0D] = 8'hB0;
        // 000E  LD HL,1000h / CALL 0030h / HALT
        mem[16'h0E] = 8'h21; mem[16'h0F] = 8'h00; mem[16'h10] = 8'h10;
        mem[16'h11] = 8'hCD; mem[16'h12] = 8'h30; mem[16'h13] = 8'h00;
        mem[16'h14] = 8'h76;
        // 0030  prt: LD A,(HL) / OR A / RET Z / OUT (81h),A / INC HL / JR prt
        mem[16'h30] = 8'h7E; mem[16'h31] = 8'hB7; mem[16'h32] = 8'hC8;
        mem[16'h33] = 8'hD3; mem[16'h34] = 8'h81; mem[16'h35] = 8'h23;
        mem[16'h36] = 8'h18; mem[16'h37] = 8'hF8;
        // 0040  "Z80 chip OK\r\n", 0
        for (i = 0; i < 14; i = i + 1) mem[16'h40 + i] = msg[8*(13-i) +: 8];
    end

    wire mem_rd = !z80_mreq_n && !z80_rd_n;
    assign z80_d = mem_rd ? mem[z80_a] : 8'hzz;
    // asynchronous SRAM write: follows the bus while /MREQ and /WR are low
    always @* if (!z80_mreq_n && !z80_wr_n) mem[z80_a] = z80_d;

    // ---- output port: I/O write = /WR low with /MREQ high ----
    reg [8*32-1:0] out = 0;
    integer n_out = 0;
    always @(negedge z80_wr_n) begin
        #20;
        if (z80_mreq_n && z80_a[7:0] == 8'h81) begin
            $write("%c", z80_d);
            out = {out[8*31-1:0], z80_d};
            n_out = n_out + 1;
            if (n_out == 13) begin
                if (out[8*13-1:0] == {"Z80 chip OK", 8'h0D, 8'h0A} &&
                    mem[16'h1000] == "Z" && mem[16'h100A] == "K" && mem[16'h100D] == 8'h00)
                    $display("[tb_chip] PASS: external-bus Z80 copied to RAM, called, printed via OUT");
                else
                    $display("[tb_chip] FAIL: wrong output or RAM copy");
                $finish;
            end
        end
    end

    // ---- bus checks ----
    always @(posedge zclk or negedge zclk) begin
        if (mem_rd && dut.data_drive) begin
            $display("[tb_chip] FAIL: bus contention at A=%h", z80_a);
            $fatal;
        end
        if (mem_rd && ^z80_d === 1'bx) begin
            $display("[tb_chip] FAIL: X on D during a read at A=%h", z80_a);
            $fatal;
        end
    end

    initial begin
        #20_000_000;                    // 20 ms: ~5.5 ms reset stretch + the program
        $display("\n[tb_chip] FAIL: timeout (%0d chars out)", n_out);
        $fatal;
    end
endmodule
