// ===========================================================================
// z80_timing.v - external pin drive as a pure function of timing state
// (mirrors cmodel/z80_timing.c). t.P = (phi==0), t.N = (phi==1).
// ===========================================================================
`include "z80_defs.vh"

module z80_timing (
    input  wire [2:0]  bus_op,
    input  wire [2:0]  t_state,
    input  wire        phi,
    input  wire [3:0]  m_len,           // total T-states this M-cycle (incl. extras)
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
            // refresh address on T3..T4 only -- silicon deasserts RFSH
            // at T4 end even if the M1 extends into T5+ (NMI/INTA acks).
            // Mirrors cmodel/z80_timing.c. Closes prog10 phase 52/53 and
            // prog17 phase 50/51 rfsh-edge diffs vs perfectz80.
            if ((t == 3'd3) || (t == 3'd4)) addr = {reg_i, reg_r};
            m1_n   = (t <= 3'd2) ? 1'b0 : 1'b1;
            rfsh_n = ((t == 3'd3) || (t == 3'd4)) ? 1'b0 : 1'b1;
            // opcode fetch MREQ/RD: T1.N .. end of T2
            // refresh MREQ: T3.N .. T4.P
            mreq_n = (((t == 3'd1) && (phi == 1'b1)) || (t == 3'd2)
                   || ((t == 3'd3) && (phi == 1'b1)) || ((t == 3'd4) && (phi == 1'b0)))
                   ? 1'b0 : 1'b1;
            rd_n   = (((t == 3'd1) && (phi == 1'b1)) || (t == 3'd2)) ? 1'b0 : 1'b1;
        end
        `BUSOP_MRD: begin
            // MREQ/RD active T1.N .. T3.P (deassert at T3.N — falling-edge
            // transition matches gate-level / perfectz80 convention). Extra
            // T-states beyond T3 (e.g. CB (HL) reads) are internal compute
            // padding; MREQ stays high.
            if (!((t == 3'd1) && (phi == 1'b0)) && !((t == 3'd3) && (phi == 1'b1)) && (t <= 3'd3))
                begin mreq_n = 1'b0; rd_n = 1'b0; end
        end
        `BUSOP_MWR: begin
            if (!((t == 3'd1) && (phi == 1'b0)) && !((t == 3'd3) && (phi == 1'b1)) && (t <= 3'd3))
                begin mreq_n = 1'b0; data_drive = 1'b1; end
            if (((t == 3'd2) && (phi == 1'b1)) || ((t == 3'd3) && (phi == 1'b0))) wr_n = 1'b0;
        end
        `BUSOP_IORD: begin
            if ((t >= 3'd2) && !((t == 3'd4) && (phi == 1'b1)) && (t <= 3'd4))
                begin iorq_n = 1'b0; rd_n = 1'b0; end
        end
        `BUSOP_IOWR: begin
            if ((t >= 3'd2) && !((t == 3'd4) && (phi == 1'b1)) && (t <= 3'd4))
                begin iorq_n = 1'b0; data_drive = 1'b1; end
            if (((t == 3'd2) && (phi == 1'b1)) || (t == 3'd3) || ((t == 3'd4) && (phi == 1'b0))) wr_n = 1'b0;
        end
        `BUSOP_INTA: begin
            m1_n   = (t <= 3'd2) ? 1'b0 : 1'b1;
            iorq_n = (t >= 3'd3) ? 1'b0 : 1'b1;
        end
        default: ; // INTERNAL / NONE: hold address, no strobes
        endcase
    end
endmodule
