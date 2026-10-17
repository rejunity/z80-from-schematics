// sim_pll.v - SB_PLL40_2_PAD stand-in for the gate-level simulation
// (`make sim_gl`). yosys' iCE40 cell library has no behavioural PLL, so,
// as in the RTL simulations, the testbench clock plays the PLL output:
// port A (the 12 MHz pass-through) and port B (48 MHz) both follow the
// pad, the PLL is always locked. Parameters as in cells_sim.v.
module SB_PLL40_2_PAD (
    input        PACKAGEPIN,
    output       PLLOUTCOREA,
    output       PLLOUTGLOBALA,
    output       PLLOUTCOREB,
    output       PLLOUTGLOBALB,
    input        EXTFEEDBACK,
    input  [7:0] DYNAMICDELAY,
    output       LOCK,
    input        BYPASS,
    input        RESETB,
    input        LATCHINPUTVALUE,
    output       SDO,
    input        SDI,
    input        SCLK
);
    parameter FEEDBACK_PATH = "SIMPLE";
    parameter DELAY_ADJUSTMENT_MODE_FEEDBACK = "FIXED";
    parameter DELAY_ADJUSTMENT_MODE_RELATIVE = "FIXED";
    parameter SHIFTREG_DIV_MODE = 1'b0;
    parameter FDA_FEEDBACK = 4'b0000;
    parameter FDA_RELATIVE = 4'b0000;
    parameter PLLOUT_SELECT_PORTB = "GENCLK";
    parameter DIVR = 4'b0000;
    parameter DIVF = 7'b0000000;
    parameter DIVQ = 3'b000;
    parameter FILTER_RANGE = 3'b000;
    parameter ENABLE_ICEGATE_PORTA = 1'b0;
    parameter ENABLE_ICEGATE_PORTB = 1'b0;
    parameter TEST_MODE = 1'b0;
    parameter EXTERNAL_DIVIDE_FACTOR = 1;

    assign PLLOUTCOREA = PACKAGEPIN, PLLOUTGLOBALA = PACKAGEPIN;
    assign PLLOUTCOREB = PACKAGEPIN, PLLOUTGLOBALB = PACKAGEPIN;
    assign LOCK = 1'b1;
    assign SDO  = 1'b0;
endmodule
