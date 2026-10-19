// pads_bb.v - blackbox views of the sg13g2_io pad cells z80_chip.v uses,
// for synthesis and lint (PAD_VERILOG_MODELS): the ports and directions of
// the PDK's sg13g2_io.v. Simulations use the PDK model itself.

(* blackbox *)
module sg13g2_IOPadIn (pad, p2c, vdd, vss, iovdd, iovss);
    input pad;
    output p2c;
    inout vdd;
    inout vss;
    inout iovdd;
    inout iovss;
endmodule

(* blackbox *)
module sg13g2_IOPadOut16mA (pad, c2p, vdd, vss, iovdd, iovss);
    output pad;
    input c2p;
    inout vdd;
    inout vss;
    inout iovdd;
    inout iovss;
endmodule

(* blackbox *)
module sg13g2_IOPadTriOut16mA (pad, c2p, c2p_en, vdd, vss, iovdd, iovss);
    output pad;
    input c2p;
    input c2p_en;
    inout vdd;
    inout vss;
    inout iovdd;
    inout iovss;
endmodule

(* blackbox *)
module sg13g2_IOPadInOut16mA (pad, c2p, p2c, c2p_en, vdd, vss, iovdd, iovss);
    inout pad;
    input c2p;
    output p2c;
    input c2p_en;
    inout vdd;
    inout vss;
    inout iovdd;
    inout iovss;
endmodule

(* blackbox *)
module sg13g2_IOPadIOVdd (vdd, vss, iovdd, iovss);
    inout vdd;
    inout vss;
    inout iovdd;
    inout iovss;
endmodule

(* blackbox *)
module sg13g2_IOPadIOVss (vdd, vss, iovdd, iovss);
    inout vdd;
    inout vss;
    inout iovdd;
    inout iovss;
endmodule

(* blackbox *)
module sg13g2_IOPadVdd (vdd, vss, iovdd, iovss);
    inout vdd;
    inout vss;
    inout iovdd;
    inout iovss;
endmodule

(* blackbox *)
module sg13g2_IOPadVss (vdd, vss, iovdd, iovss);
    inout vdd;
    inout vss;
    inout iovdd;
    inout iovss;
endmodule
