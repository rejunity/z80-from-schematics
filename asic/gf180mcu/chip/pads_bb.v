// pads_bb.v - blackbox views of the gf180mcu_fd_io pad cells z80_chip.v
// uses, for synthesis and lint (PAD_VERILOG_MODELS): the ports and
// directions of the PDK's gf180mcu_fd_io.v, without its behavioural body
// (rnmos primitives, which Verilator rejects).
// Simulations use the PDK model itself.

(* blackbox *)
module gf180mcu_fd_io__bi_24t (CS, SL, IE, OE, PU, PD, A, PAD, Y, DVDD, DVSS, VDD, VSS);
    input CS;
    input SL;
    input IE;
    input OE;
    input PU;
    input PD;
    input A;
    inout PAD;
    output Y;
    inout DVDD;
    inout DVSS;
    inout VDD;
    inout VSS;
endmodule

(* blackbox *)
module gf180mcu_fd_io__in_c (PU, PD, PAD, Y, DVDD, DVSS, VDD, VSS);
    input PU;
    input PD;
    input PAD;
    output Y;
    inout DVDD;
    inout DVSS;
    inout VDD;
    inout VSS;
endmodule

(* blackbox *)
module gf180mcu_fd_io__in_s (PU, PD, PAD, Y, DVDD, DVSS, VDD, VSS);
    input PU;
    input PD;
    input PAD;
    output Y;
    inout DVDD;
    inout DVSS;
    inout VDD;
    inout VSS;
endmodule

(* blackbox *)
module gf180mcu_fd_io__dvdd (DVDD, DVSS, VSS);
    inout DVDD;
    inout DVSS;
    inout VSS;
endmodule

(* blackbox *)
module gf180mcu_fd_io__dvss (DVDD, DVSS, VDD);
    inout DVDD;
    inout DVSS;
    inout VDD;
endmodule
