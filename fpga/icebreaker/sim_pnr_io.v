// sim_pnr_io.v - yosys techmap for `make sim_pnr`: the routed netlist's
// SB_IO pads become plain wires, so the testbench drives and reads the top
// level's ports directly. Only the two pad modes the sbc build uses; the
// Makefile asserts that no SB_IO is left over.
module SB_IO (inout PACKAGE_PIN, input LATCH_INPUT_VALUE, CLOCK_ENABLE, INPUT_CLK,
              OUTPUT_CLK, OUTPUT_ENABLE, D_OUT_0, D_OUT_1, output D_IN_0, D_IN_1);
    parameter PIN_TYPE = 0;
    parameter PULLUP = 0;
    parameter NEG_TRIGGER = 0;
    parameter IO_STANDARD = "SB_LVCMOS";
    generate
        if (PIN_TYPE == 6'b000001) begin            // PIN_INPUT
            assign D_IN_0 = PACKAGE_PIN;
        end else if (PIN_TYPE == 6'b011001) begin   // PIN_OUTPUT (+ input path)
            assign PACKAGE_PIN = D_OUT_0;
            assign D_IN_0 = D_OUT_0;
        end else begin
            wire _TECHMAP_FAIL_ = 1'b1;
        end
    endgenerate
endmodule
