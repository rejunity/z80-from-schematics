// ===========================================================================
// z80_timing.v - external pin drive as a pure function of timing state
// (mirrors cmodel/z80_timing.c). t.P = (phi==0), t.N = (phi==1).
// ===========================================================================
`include "z80_defs.vh"

module z80_timing (
    input  wire [2:0]  bus_op,
    input  wire [2:0]  t_state,
    input  wire        phi,
    input  wire [15:0] m_addr,
    input  wire [7:0]  m_wdata,
    input  wire [7:0]  reg_i,
    input  wire [7:0]  reg_r,
    output reg  [15:0] addr,
    output reg  [7:0]  data_out,
    output reg         data_drive,
    output reg         m1_n,
    output reg         mreq_n,
    output reg         iorq_n,
    output reg         rd_n,
    output reg         wr_n,
    output reg         rfsh_n
);
    wire [2:0] t = t_state;

    always @* begin
        // defaults: inactive
        m1_n = 1'b1; mreq_n = 1'b1; iorq_n = 1'b1;
        rd_n = 1'b1; wr_n = 1'b1; rfsh_n = 1'b1;
        data_drive = 1'b0; data_out = m_wdata;
        addr = m_addr;

        case (bus_op)
        `BUSOP_M1: begin
            // refresh address on T3..T4
            if (t >= 3'd3) addr = {reg_i, reg_r};
            m1_n   = (t <= 3'd2) ? 1'b0 : 1'b1;
            rfsh_n = (t >= 3'd3) ? 1'b0 : 1'b1;
            // opcode fetch MREQ/RD: T1.N .. end of T2
            // refresh MREQ: T3.N .. T4.P
            mreq_n = (((t == 3'd1) && (phi == 1'b1)) || (t == 3'd2)
                   || ((t == 3'd3) && (phi == 1'b1)) || ((t == 3'd4) && (phi == 1'b0)))
                   ? 1'b0 : 1'b1;
            rd_n   = (((t == 3'd1) && (phi == 1'b1)) || (t == 3'd2)) ? 1'b0 : 1'b1;
        end
        `BUSOP_MRD: begin
            // active from T1.N onward
            if (!((t == 3'd1) && (phi == 1'b0))) begin mreq_n = 1'b0; rd_n = 1'b0; end
        end
        `BUSOP_MWR: begin
            if (!((t == 3'd1) && (phi == 1'b0))) begin mreq_n = 1'b0; data_drive = 1'b1; end
            // WR low: T2.N .. end of T3
            if (((t == 3'd2) && (phi == 1'b1)) || (t == 3'd3)) wr_n = 1'b0;
        end
        `BUSOP_IORD: begin
            if (t >= 3'd2) begin iorq_n = 1'b0; rd_n = 1'b0; end
        end
        `BUSOP_IOWR: begin
            if (t >= 3'd2) begin iorq_n = 1'b0; data_drive = 1'b1; end
            if (((t == 3'd2) && (phi == 1'b1)) || (t >= 3'd3)) wr_n = 1'b0;
        end
        `BUSOP_INTA: begin
            m1_n   = (t <= 3'd2) ? 1'b0 : 1'b1;
            iorq_n = (t >= 3'd3) ? 1'b0 : 1'b1;
        end
        default: ; // INTERNAL / NONE: hold address, no strobes
        endcase
    end
endmodule
