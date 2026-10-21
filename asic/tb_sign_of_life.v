// ===========================================================================
// tb_sign_of_life.v - minimal "is the chip alive" test for z80_top, for the
// RTL or for a synthesized / placed-and-routed gate-level netlist (compile
// the netlist plus the PDK's cell models instead of rtl/*.v; same module
// name and ports).
//
// A 37-byte program: write 01 02 04 .. 80 to 8000h-8007h, read them back
// and add them up, OUT the sum (FFh) to port 42h, CALL a subroutine that
// OUTs 5Ah to port 43h and returns, HALT. It exercises opcode fetch,
// immediate and memory reads, memory writes, the ALU, a DJNZ loop, I/O
// writes, the stack (CALL / RET) and HALT -- about 570 T-states.
//
// Checks: the memory pattern, both I/O writes in order (port, data), the
// return address on the stack, /HALT asserted, and on every clock that
// /RD and /WR, or /MREQ and /IORQ, are never low together. Prints
// "SIGN OF LIFE: PASS" or "SIGN OF LIFE: FAIL ..." and finishes.
// ===========================================================================
`timescale 1ns/1ps

module tb_sign_of_life;
    reg         clk = 1'b0;
    reg         por_n = 1'b0;           // z80_top's power-on reset (the chip has none)
    reg         reset_n = 1'b0;
    wire [15:0] addr;
    wire [7:0]  data_out;
    wire        data_drive;
    wire        m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busack_n;
    wire [3:0]  dbg_t;
    wire [2:0]  dbg_m;
    reg  [7:0]  mem [0:65535];
    wire [7:0]  data_in = (!mreq_n && !rd_n) ? mem[addr] : 8'hFF;

`ifndef DUT
`define DUT z80_top   // the chip: z80_chip_shim (chip_shim.v)
`endif
    `DUT dut (
        .clk(clk), .por_n(por_n), .reset_n(reset_n),
        .addr(addr), .data_in(data_in), .data_out(data_out), .data_drive(data_drive),
        .m1_n(m1_n), .mreq_n(mreq_n), .iorq_n(iorq_n), .rd_n(rd_n), .wr_n(wr_n),
        .rfsh_n(rfsh_n), .halt_n(halt_n), .busack_n(busack_n),
        .wait_n(1'b1), .int_n(1'b1), .nmi_n(1'b1), .busreq_n(1'b1),
        .dbg_t(dbg_t), .dbg_m(dbg_m)
    );

    always #50 clk = ~clk;                       // 10 MHz; zero-delay cell models

    integer i, cycles, errors, nio;
    reg [7:0]  io_port [0:3];
    reg [7:0]  io_data [0:3];
    reg        io_prev;
    initial begin
        for (i = 0; i < 65536; i = i + 1) mem[i] = 8'h00;
        // 0000  LD SP,FF00     LD HL,8000   LD A,1    LD B,8
        mem[16'h0000] = 8'h31; mem[16'h0001] = 8'h00; mem[16'h0002] = 8'hFF;
        mem[16'h0003] = 8'h21; mem[16'h0004] = 8'h00; mem[16'h0005] = 8'h80;
        mem[16'h0006] = 8'h3E; mem[16'h0007] = 8'h01;
        mem[16'h0008] = 8'h06; mem[16'h0009] = 8'h08;
        // 000A  loop1: LD (HL),A / ADD A,A / INC HL / DJNZ loop1
        mem[16'h000A] = 8'h77; mem[16'h000B] = 8'h87; mem[16'h000C] = 8'h23;
        mem[16'h000D] = 8'h10; mem[16'h000E] = 8'hFB;
        // 000F  LD HL,8000 / LD B,8 / XOR A
        mem[16'h000F] = 8'h21; mem[16'h0010] = 8'h00; mem[16'h0011] = 8'h80;
        mem[16'h0012] = 8'h06; mem[16'h0013] = 8'h08; mem[16'h0014] = 8'hAF;
        // 0015  loop2: ADD A,(HL) / INC HL / DJNZ loop2
        mem[16'h0015] = 8'h86; mem[16'h0016] = 8'h23;
        mem[16'h0017] = 8'h10; mem[16'h0018] = 8'hFC;
        // 0019  OUT (42h),A / CALL 0020h / HALT
        mem[16'h0019] = 8'hD3; mem[16'h001A] = 8'h42;
        mem[16'h001B] = 8'hCD; mem[16'h001C] = 8'h20; mem[16'h001D] = 8'h00;
        mem[16'h001E] = 8'h76;
        // 0020  LD A,5Ah / OUT (43h),A / RET
        mem[16'h0020] = 8'h3E; mem[16'h0021] = 8'h5A;
        mem[16'h0022] = 8'hD3; mem[16'h0023] = 8'h43;
        mem[16'h0024] = 8'hC9;
        errors = 0; nio = 0; io_prev = 1'b0;
        @(negedge clk); #1 por_n = 1'b1;
        repeat (5) @(negedge clk);
        reset_n = 1'b1;
    end

    // memory and I/O writes, bus sanity -- sampled on every falling edge
    always @(negedge clk) if (reset_n) begin
        if (!mreq_n && !wr_n) mem[addr] <= data_out;
        if (!iorq_n && !wr_n && !io_prev && nio < 4) begin
            io_port[nio] = addr[7:0]; io_data[nio] = data_out; nio = nio + 1;
        end
        io_prev = !iorq_n && !wr_n;
        if (!rd_n && !wr_n)     begin $display("FAIL: /RD and /WR low together at %0t", $time); errors = errors + 1; end
        if (!mreq_n && !iorq_n) begin $display("FAIL: /MREQ and /IORQ low together at %0t", $time); errors = errors + 1; end
    end

    initial begin
        cycles = 0;
        wait (reset_n);
        while (halt_n !== 1'b0 && cycles < 2000) begin @(posedge clk); cycles = cycles + 1; end
        repeat (4) @(posedge clk);
        if (halt_n !== 1'b0) begin $display("FAIL: no HALT within 2000 clocks"); errors = errors + 1; end
        for (i = 0; i < 8; i = i + 1)
            if (mem[16'h8000 + i] !== (8'h01 << i)) begin
                $display("FAIL: mem[%04h] = %02h, expected %02h", 16'h8000 + i, mem[16'h8000 + i], 8'h01 << i);
                errors = errors + 1;
            end
        if (nio != 2 || io_port[0] !== 8'h42 || io_data[0] !== 8'hFF || io_port[1] !== 8'h43 || io_data[1] !== 8'h5A) begin
            $display("FAIL: I/O writes: %0d seen, expected (42h,FFh) then (43h,5Ah)", nio);
            errors = errors + 1;
        end
        if (mem[16'hFEFE] !== 8'h1E || mem[16'hFEFF] !== 8'h00) begin
            $display("FAIL: return address on the stack = %02h%02h, expected 001E", mem[16'hFEFF], mem[16'hFEFE]);
            errors = errors + 1;
        end
        if (errors == 0) $display("SIGN OF LIFE: PASS (HALT after %0d clocks)", cycles);
        else             $display("SIGN OF LIFE: FAIL (%0d errors)", errors);
        $finish;
    end
endmodule
