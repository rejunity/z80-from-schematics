// Interrupt devices drive data only during M1+IORQ, never during RD.
`timescale 1ns/1ps
`include "z80_defs.vh"
module tb_int_ack;
    parameter USE_CEN = 1;
    reg clk=0, cen=1, reset_n=0, wait_n=1, int_n=1;
    reg [7:0] mem [0:65535];
    reg [7:0] vector;
    wire [15:0] addr;
    wire [7:0] dout;
    wire m1, mr, io, rd, wr, refresh, halt;
    wire [7:0] din = !m1 && !io ? vector : (!mr && !rd ? mem[addr] : 8'h00);
    z80_core #(.USE_CEN(USE_CEN)) dut (
        .clk(clk), .cen(cen), .reset_n(reset_n), .wait_n(wait_n),
        .int_n(int_n), .nmi_n(1'b1), .busreq_n(1'b1),
        .addr(addr), .data_in(din), .data_out(dout), .data_drive(),
        .m1_n(m1), .mreq_n(mr), .iorq_n(io), .rd_n(rd), .wr_n(wr),
        .rfsh_n(refresh), .halt_n(halt), .busack_n(), .dbg_t(), .dbg_phi(), .dbg_m()
    );
    task step;
        begin
            #5 clk=1; #1;
            if (!mr && !wr) mem[addr]=dout;
            #4 clk=0;
        end
    endtask
    integer i, mode, n, waits, overlaps, refreshes;
    reg seen_ack, reached;
    reg [7:0] ack_r;
    reg [15:0] return_pc, target, expected_sp;
    task run_case;
        input integer imode;
        input [7:0] opcode;
        input integer insert_wait;
        input integer from_halt;
        begin
            reset_n=0; int_n=1; wait_n=1; step;
            for (i=0;i<65536;i=i+1) mem[i]=0;
            // LD SP,9000; IM n; LD A,80; LD I,A; EI; NOP; NOPs
            mem[0]=8'h31; mem[1]=0; mem[2]=8'h90;
            mem[3]=8'hed; mem[4]=(imode==0)?8'h46:(imode==1?8'h56:8'h5e);
            mem[5]=8'h3e; mem[6]=8'h80; mem[7]=8'hed; mem[8]=8'h47;
            mem[9]=8'hfb;
            if(from_halt) mem[10]=8'h76;
            mem[16'h8034]=0; mem[16'h8035]=8'h02;
            target=(imode==1 || (imode==0 && opcode==8'hff))?16'h0038:16'h0200;
            mem[target]=8'h76;
            vector=opcode; waits=0; overlaps=0; refreshes=0; seen_ack=0; reached=0;
            reset_n=1;
            for(n=0;n<600 && !reached;n=n+1) begin
                if(dut.iff1 && (!from_halt || dut.halted)) int_n=0;
                if(dut.bus_op==`BUSOP_INTA) begin
                    if(!seen_ack) begin
                        seen_ack=1; return_pc=addr; ack_r=dut.reg_r;
                        if(imode==0 && (opcode==8'hcd || opcode==8'hc3)) begin
                            mem[return_pc]=0; mem[return_pc+1]=8'h02;
                        end
                        if(imode==0 && opcode==0) begin target=return_pc; mem[target]=8'h76; end
                    end
                    if(dut.t_state<=4 && addr!==return_pc) $fatal(1,"ack address changed");
                    if(io !== !((dut.t_state==3 && dut.phi) || dut.t_state==4))
                        $fatal(1,"wrong IORQ acknowledge phase");
                    if(!rd || !wr) $fatal(1,"INTA asserted RD/WR");
                    if(!io && m1) $fatal(1,"IORQ without M1 during INTA");
                    if(dut.t_state<=4 && !mr) $fatal(1,"INTA performed memory read");
                    if(!io) overlaps=overlaps+1;
                    if(!refresh) refreshes=refreshes+1;
                    if(dut.t_state==4 && dut.phi==0 && waits<insert_wait) begin
                        wait_n=0; waits=waits+1;
                        vector=8'h00; // vector not ready while WAIT is held
                    end else begin wait_n=1; vector=opcode; end
                    if(dut.t_state==4 && dut.phi==1 && dut.wait_sampled) begin
                        // A short falling-edge sample must survive release.
                        wait_n=1;
                        if(USE_CEN) begin cen=0; step; cen=1; end
                    end
                end
                if(seen_ack && dut.bus_op==`BUSOP_M1 && !rd && addr==target) reached=1;
                step;
            end
            if(!seen_ack || !reached || overlaps!=3+2*insert_wait || refreshes!=4 || waits!=insert_wait)
                $fatal(1,"INT case mode=%0d op=%h reached=%b overlap=%0d refresh=%0d waits=%0d",imode,opcode,reached,overlaps,refreshes,waits);
            if(dut.reg_r !== ack_r+8'd1) $fatal(1,"ack must increment R once");
            expected_sp=(imode==0 && (opcode==8'hc3 || opcode==0))?16'h9000:16'h8ffe;
            if(dut.rf[`RFP_SP]!==expected_sp) $fatal(1,"wrong interrupt stack depth");
            if(expected_sp==16'h8ffe) begin
                if(imode==0 && opcode==8'hcd) return_pc=return_pc+2;
                if({mem[16'h8fff],mem[16'h8ffe]}!==return_pc) $fatal(1,"wrong return PC");
            end
        end
    endtask
    initial begin
        for(mode=0;mode<3;mode=mode+1) begin
            run_case(mode, mode==2?8'h34:8'hff,0,0);
            run_case(mode, mode==2?8'h34:8'hff,3,0);
            run_case(mode, mode==2?8'h34:8'hff,3,1);
        end
        run_case(0,8'hcd,3,0); run_case(0,8'hc3,3,0); run_case(0,0,3,0);
        $display("PASS interrupt acknowledge, IM0/1/2, WAIT USE_CEN=%0d",USE_CEN);
        $finish;
    end
endmodule
