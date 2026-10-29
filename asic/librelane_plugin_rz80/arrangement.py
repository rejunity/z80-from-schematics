"""z80_top's floorplan after the Z80 die, written by hand from Ken
Shirriff's published die analysis (righto.com). No die data is read.

- "The Z-80's 16-bit increment/decrement circuit reverse engineered"
  (2013), its labelled die photo: the register file in the lower middle,
  the incrementer/decrementer at its left, the ALU at its right, the
  instruction PLA in the upper half with the instruction logic between it
  and the registers, the state logic on top.
- "Down to the silicon: how the Z80's registers are implemented" (2014):
  14 columns of 16-bit pairs, each register stored vertically, PC and IR
  set apart from the rest, AF at the right end by the ALU, the two banks
  side by side.
- "Why the Z-80's data pins are scrambled" (2014): the data pins down the
  right edge, the address pins along the left and bottom edges.

RZ80 keeps those pins on the same sides (asic/<pdk>/pin_order.cfg), so the
die's arrangement faces the right pins. Positions are fractions of
z80_top's core: x from west (0) to east (1), y from south (0) to north (1).
Rows and columns are in placement rows and sites, from the cell library.
"""


def bits(net, lo, n):
    return [f"{net}[{i}]" for i in range(lo, lo + n)]


# ---- the register file ----------------------------------------------------
# Columns west to east; each column's 16 rows top to bottom are the pair's
# bits 0..15 -- the low byte above the high byte, as the die stacks them.
# None is a column-wide gap: PC and IR stand apart from the others.
# The pairs come in twos (WZ SP, IY IX, DE DE', HL HL', BC BC', AF' AF) with
# a little more room between twos. p_*0 / p_*1 are RZ80's physical banks:
# EXX, EX AF,AF' and EX DE,HL swap which one a name reaches, as on the die.
REGISTER_COLUMNS = [
    ("PC", bits("u_core.r_pc", 0, 16)),
    ("IR", bits("u_core.reg_r", 0, 8) + bits("u_core.reg_i", 0, 8)),
    None,
    ("WZ", bits("u_core.r_wz", 0, 16)),
    ("SP", bits("u_core.r_sp", 0, 16)),
    ("IY", bits("u_core.r_iy", 0, 16)),
    ("IX", bits("u_core.r_ix", 0, 16)),
    ("DE", bits("u_core.u_regs.p_de0", 0, 16)),
    ("DE'", bits("u_core.u_regs.p_de1", 0, 16)),
    ("HL", bits("u_core.u_regs.p_hl0", 0, 16)),
    ("HL'", bits("u_core.u_regs.p_hl1", 0, 16)),
    ("BC", bits("u_core.u_regs.p_bc0", 0, 16)),
    ("BC'", bits("u_core.u_regs.p_bc1", 0, 16)),
    ("AF'", bits("u_core.u_regs.p_af1", 0, 16)),
    ("AF", bits("u_core.u_regs.p_af0", 0, 16)),
]
PAIR_GAP_AFTER = {"PC", "SP", "IX", "DE'", "HL'", "BC'"}   # a little more room after these
# the bank flip-flops in a row above the array, over the columns they swap
BANK_FLOPS = [("u_core.u_regs.xbank", "PC"), ("u_core.u_regs.xdh[0]", "DE"), ("u_core.u_regs.xdh[1]", "HL"),
              ("u_core.u_regs.xaf", "AF'")]

# ---- beside the register file, bit for bit ----------------------------------
# west of PC (the incrementer/decrementer's side): the address latch
WEST_COLUMNS = [("addr_hold", bits("u_core.addr_hold", 0, 16)),
                ("m_addr", bits("u_core.m_addr", 0, 16))]
# east of AF (the ALU's side): its operand latch
# (the write-data latch m_wdata is at the data pins, with their latches)
EAST_COLUMNS = [("tmp", bits("u_core.tmp8", 0, 8) + bits("u_core.tmph", 0, 8))]

# ---- the ALU: 8 bit planes east of the operand latches ---------------------
# bit i level with the register file's bit row i (its four placement rows),
# the shared logic (the flags' collectors, overflow, carry out, DAA's
# detection, the decode) north of them, in the die's flag area. The cells
# are spread by this factor: packed tighter, the ALU's many short wires do
# not route (the router's shorts pile up there).
ALU_GAP_SITES = 6
ALU_SPREAD = 2.0
PLANE_SPREAD = 1.5         # the address unit's planes and z80_timing
ALU_FLAG_ROWS = 8          # the ALU's shared logic: rows of the flag area north of its planes

# ---- the address unit: 16 bit planes west of the address latch ------------
# (the die's incrementer/decrementer strip by the A pins), in this many
# sites; its inputs with a bit index (the 16-bit ones, as their nets are
# named in the flattened netlist)
ADDR_PLANE_SITES = 110
ADDR_INPUTS = ("r_pc", "r_sp", "r_wz", "r_hl", "r_de", "r_bc", "rf_hlp", "rf_rp", "a16")

# ---- z80_timing: a band above the PLA, under the control pins ----------------
TIMING_NORTH_ROWS = 6      # its top row, counted down from the core's north edge
TIMING_ROWS = 6
TIMING_WEST = 0.08

# ---- grid ------------------------------------------------------------------
COLUMN_SITES = 38          # a register column: the 34-site flip-flop and room for its neighbours
PAIR_GAP_SITES = 6
BIT_ROWS = 4               # each bit row: the flip-flop row, the pair's second row, two free rows for its write logic (and horizontal wires); the ALU and address planes use all four
DATAPATH_WEST = 0.06       # the datapath band starts this far east...
DATAPATH_SOUTH_ROWS = 6    # ... and this many rows above the core's south edge (the pin flip-flops below)

# ---- the decoder: AND plane, OR plane, input lines, instruction register --
PLA_EAST = 0.90            # the instruction register's column (its east edge)
PLA_NORTH_ROWS = 13        # the PLA's top row, counted down from the core's north edge
TERM_SITES = 13          # an AND-plane slot (a product term's gate): five to a gap between tap cells
OR_SITES = 17            # an OR-plane slot (an output line's last gate)
TERM_ROWS = 2              # the AND plane's row pitch: a term row and a free row
PLA_INPUTS = bits("u_core.ir", 0, 8) + bits("u_core.prefix", 0, 3)   # literal i: in[i] (0..7 op, 8..10 prefix)

# ---- the state logic on top, the pins' flip-flops at their pins -------------
STATE_FLOPS = (bits("u_core.m_len", 0, 4) + bits("u_core.bus_op", 0, 3) + bits("u_core.irq_seq", 0, 2)
               + ["u_core.suppress_decode", "u_core.halted", "u_core.iff1", "u_core.iff2"])
STATE_ROWS = 5             # the state row, counted down from the core's north edge
STATE_X = (0.30, 0.60)

# each pin's flip-flops (Q nets), placed at the pin, inward from its edge:
# z80_top's dual-edge pin former (pin_a rising half, pin_b falling half;
# pins = {addr_drive, ctl_drive, addr[15:0], data_out[7:0], data_drive, m1,
# mreq, iorq, rd, wr, rfsh, halt, busack}) and its input samplers
def _pin_former(bit):
    return [f"pin_b[{bit}]", f"pin_a[{bit}]"]


PIN_FLOPS = {f"addr[{k}]": _pin_former(17 + k) for k in range(16)}
PIN_FLOPS.update({f"data_out[{k}]": _pin_former(9 + k) + [f"u_core.m_wdata[{k}]"] for k in range(8)})
PIN_FLOPS.update({f"data_in[{k}]": [f"data_in_neg[{k}]"] for k in range(8)})
PIN_FLOPS.update({
    "addr_drive": _pin_former(34), "ctl_drive": _pin_former(33), "data_drive": _pin_former(8),
    "m1_n": _pin_former(7), "mreq_n": _pin_former(6), "iorq_n": _pin_former(5), "rd_n": _pin_former(4),
    "wr_n": _pin_former(3), "rfsh_n": _pin_former(2), "halt_n": _pin_former(1), "busack_n": _pin_former(0),
    "nmi_n": ["nmi_n_neg", "nmi_hold", "nmi_hold_clr"], "int_n": ["int_n_rise"], "busreq_n": ["busreq_rise"],
    "reset_n": ["res_rise"], "wait_n": ["u_core.wait_n_neg"],
})
PIN_FLOPS.update({f"dbg_t[{k}]": [f"u_core.dbg_t[{k}]"] for k in range(4)})
PIN_FLOPS.update({f"dbg_m[{k}]": [f"u_core.dbg_m[{k}]"] for k in range(3)})
PIN_INSET_ROWS = 1         # rows / flip-flop widths between the core edge and a pin's first flip-flop
