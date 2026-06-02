// ===========================================================================
// z80_idu.v - 16-bit incrementer/decrementer + signed-displacement adder.
//
// One of the named blocks visible on the real Z80 die (the "incrementer
// /decrementer unit" on the address path). Handles all 16-bit address
// arithmetic that produces no flags: PC + 1 every fetch/operand-read,
// SP ± 1 (push/pop/CALL/RET/RST), rp ± 1 (INC rp / DEC rp / block-op
// pointer updates), R-register refresh increment (low 7 bits only),
// and PC + signed-displacement (JR / JR cc / DJNZ).
//
// Pure combinational. The output is driven onto the system bus (sb)
// when ctl_bus_inc_oe is asserted in the sequencer's (M, T) matrix
// (see docs/silicon-microarch.md).
//
// Signal names match Baltazar Studios' Z80 Explorer (exec_matrix.vh):
//   - ctl_inc_cy:      carry-in (1 = INC contribution, 0 = pure pass / DEC)
//   - ctl_inc_dec:     direction select (0 = add, 1 = subtract)
//   - ctl_inc_limit6:  limit increment to low 7 bits (refresh R register)
// In this module the ctl_* signals are folded into the `op` selector
// for clarity; the wrapper at the instantiation site decodes:
//   - op == IDU_INC      <=> ctl_inc_cy=1, ctl_inc_dec=0
//   - op == IDU_DEC      <=> ctl_inc_cy=0, ctl_inc_dec=1 (or cy=1,dec=1)
//   - op == IDU_ADD_DISP <=> 16-bit add of signed 8-bit disp
//   - op == IDU_NONE     <=> passthrough
// ===========================================================================
`include "z80_defs.vh"

module z80_idu (
    input  wire [15:0] in,
    input  wire [1:0]  op,
    input  wire [7:0]  disp,
    output reg  [15:0] out
);
    always @* begin
        case (op)
            `IDU_INC:      out = in + 16'd1;
            `IDU_DEC:      out = in - 16'd1;
            `IDU_ADD_DISP: out = in + {{8{disp[7]}}, disp};
            default:       out = in;
        endcase
    end
endmodule
