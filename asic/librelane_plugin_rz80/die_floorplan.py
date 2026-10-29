# RZ80.DieFloorplan's script, run by OpenROAD's Python with LibreLane's
# odbpy helpers on PYTHONPATH: places z80_top's anchor cells as
# arrangement.py lays the Z80 die out and fixes them (FIRM),
# before global placement places everything else around them.
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import arrangement as A  # noqa: E402
from reader import click, click_odb  # noqa: E402


def natural(name):
    """a net name as (stem, index) for sorting buses bit by bit"""
    m = re.fullmatch(r"(.*?)\[(\d+)\]", name)
    return (m.group(1), int(m.group(2))) if m else (name, -1)


def escape(name):
    return name.replace("[", "\\[").replace("]", "\\]")


class Floor:
    """the core's rows and which sites are taken"""

    def __init__(self, block):
        self.block = block
        self.dbu = block.getDbUnitsPerMicron()
        rows = sorted(block.getRows(), key=lambda r: r.getOrigin()[1])
        site = rows[0].getSite()
        self.sw, self.rh = site.getWidth(), site.getHeight()
        self.rows = [{"x0": r.getOrigin()[0], "y": r.getOrigin()[1], "n": r.getSiteCount(),
                      "orient": r.getOrient(), "occ": bytearray(r.getSiteCount())} for r in rows]
        core = block.getCoreArea()
        self.x0, self.y0, self.x1, self.y1 = core.xMin(), core.yMin(), core.xMax(), core.yMax()
        self.w, self.h = self.x1 - self.x0, self.y1 - self.y0
        for inst in block.getInsts():
            if inst.getPlacementStatus() == "FIRM":
                inst.setPlacementStatus("PLACED")      # a previous run of this step's anchors (taps are LOCKED)
            elif inst.isFixed():
                self.mark(inst)
        self.anchors, self.missing, self.unplaced = [], [], []
        self.nets = {n.getName().replace("\\", ""): n for n in block.getNets()}

    def mark(self, inst):
        bb = inst.getBBox()
        for row in self.rows:
            if bb.yMin() <= row["y"] < bb.yMax():
                a = max(0, (bb.xMin() - row["x0"]) // self.sw)
                b = min(row["n"], -(-(bb.xMax() - row["x0"]) // self.sw))
                for s in range(a, b):
                    row["occ"][s] = 1

    def row_y(self, k):
        return self.rows[max(0, min(len(self.rows) - 1, k))]["y"]

    def x_at(self, f):
        return self.x0 + int(f * self.w)

    def driver(self, net_name):
        """the cell driving net_name (an RTL name, unescaped)"""
        net = self.nets.get(net_name)
        if net is None:
            return None
        for it in net.getITerms():
            if it.isOutputSignal():
                return it.getInst()
        return None

    def place(self, inst, x, y, role, max_rows=8, max_sites=400):
        """the free spot nearest (x, y): same row first, then rows above and below"""
        master = inst.getMaster()
        w = -(-master.getWidth() // self.sw)
        k0 = min(range(len(self.rows)), key=lambda k: abs(self.rows[k]["y"] - y))
        best = None
        for dk in sorted(range(-max_rows, max_rows + 1), key=abs):
            k = k0 + dk
            if not 0 <= k < len(self.rows):
                continue
            if best is not None and abs(dk) * self.rh * 2 >= best[0]:
                break
            row = self.rows[k]
            s0 = min(max(0, round((x - row["x0"]) / self.sw)), row["n"] - w)
            for ds in sorted(range(-max_sites, max_sites + 1), key=abs):
                s = s0 + ds
                if 0 <= s <= row["n"] - w and not any(row["occ"][s:s + w]):
                    cost = abs(ds) * self.sw + abs(dk) * self.rh * 2
                    if best is None or cost < best[0]:
                        best = (cost, k, s)
                    break
        if best is None:
            self.unplaced.append((inst.getName(), role))
            return False
        _, k, s = best
        row = self.rows[k]
        inst.setOrient(row["orient"])
        inst.setLocation(row["x0"] + s * self.sw, row["y"])
        inst.setPlacementStatus("FIRM")
        for i in range(s, s + w):
            row["occ"][i] = 1
        self.anchors.append({"inst": inst.getName(), "master": master.getName(), "role": role,
                             "x": row["x0"] + s * self.sw, "y": row["y"], "orient": str(row["orient"])})
        return True

    def slots(self, k, x_from, sites, count, west=False):
        """x of `count` free windows of `sites` sites in row k, packed from
        x_from eastwards (or westwards), stepping over the tap cells: the
        rows of one parity carry their taps at the same x, so the windows
        hold in all of them"""
        row = self.rows[k]
        occ, n = row["occ"], row["n"]
        s = round((x_from - row["x0"]) / self.sw) - (sites if west else 0)
        out = []
        while len(out) < count and 0 <= s <= n - sites:
            if not any(occ[s:s + sites]):
                out.append(row["x0"] + s * self.sw)
                s += -sites if west else sites
            else:
                s += -1 if west else 1
        return out

    def anchor(self, net_name, x, y, role):
        inst = self.driver(net_name)
        if inst is None or inst.isFixed():
            if inst is None:
                self.missing.append((net_name, role))
            return None
        self.place(inst, x, y, role)
        return inst


def pins(fl):
    """each pin's flip-flops at the pin, inward from its edge"""
    die = fl.block.getDieArea()
    for bterm in fl.block.getBTerms():
        flops = A.PIN_FLOPS.get(bterm.getName().replace("\\", ""))
        if not flops:
            continue
        bb = bterm.getBBox()
        px, py = (bb.xMin() + bb.xMax()) // 2, (bb.yMin() + bb.yMax()) // 2
        side = min((px - die.xMin(), "W"), (die.xMax() - px, "E"), (py - die.yMin(), "S"), (die.yMax() - py, "N"))[1]
        x, y = px, py
        for j, q in enumerate(flops):
            inst = fl.driver(q)
            if inst is None:
                fl.missing.append((q, f"pin {bterm.getName()}"))
                continue
            w = inst.getMaster().getWidth()
            if side == "S":
                fl.place(inst, px - w // 2, fl.row_y(A.PIN_INSET_ROWS + j), f"pin {side} {bterm.getName()}")
            elif side == "N":
                fl.place(inst, px - w // 2, fl.row_y(len(fl.rows) - 1 - A.PIN_INSET_ROWS - j), f"pin {side} {bterm.getName()}")
            elif side == "E":
                x = (fl.x1 - A.PIN_INSET_ROWS * fl.sw * 4 if j == 0 else x) - w
                fl.place(inst, x, py - fl.rh // 2, f"pin {side} {bterm.getName()}")
            else:
                x = fl.x0 + A.PIN_INSET_ROWS * fl.sw * 4 if j == 0 else x
                fl.place(inst, x, py - fl.rh // 2, f"pin {side} {bterm.getName()}")
                x += w


def datapath(fl):
    """the address latch, the register file's 14 columns, the ALU's latches,
    every bit in a row across. The tap cells come every 70 sites, offset by
    half that from row to row, and a set flip-flop is 36 sites: two columns
    share a gap between taps as the die pairs its registers -- the first in
    the bit's row, the second in the row above, in the gap that row's taps
    leave half a gap further east -- so the columns stand a uniform 35 sites
    apart"""
    k_bit = lambda b: A.DATAPATH_SOUTH_ROWS + (15 - b) * A.BIT_ROWS     # bit 0 on top, bit 15 at the bottom
    flop = 36                  # sites of the widest register flip-flop (dffsnq_1)
    x_from = fl.x_at(A.DATAPATH_WEST) + A.ADDR_PLANE_SITES * fl.sw     # the address unit's planes go west of it
    even = fl.slots(k_bit(0), x_from, flop, 40)          # one window per gap between taps
    odd = fl.slots(k_bit(0) + 1, x_from, flop, 40)
    columns = list(A.WEST_COLUMNS) + list(A.REGISTER_COLUMNS) + list(A.EAST_COLUMNS)
    at = {}
    g = 0
    i = 0
    while i < len(columns):
        if columns[i] is None:              # an empty gap: the die's gap after IR
            g += 1
            i += 1
            continue
        left = columns[i]
        right = columns[i + 1] if i + 1 < len(columns) else None
        xl = even[g]
        xr = next(x for x in odd if x > xl)
        for entry, x, dk in ((left, xl, 0), (right, xr, 1)):
            if entry is None:
                continue
            label, nets = entry
            at[label] = x
            for bit, q in enumerate(nets):
                role = f"regfile {label} bit {bit}" if entry in A.REGISTER_COLUMNS else f"datapath {label}[{bit}]"
                fl.anchor(q, x, fl.row_y(k_bit(bit) + dk), role)
        g += 1
        i += 2
    top = k_bit(0) + A.BIT_ROWS
    for q, over in A.BANK_FLOPS:
        fl.anchor(q, at[over], fl.row_y(top), f"bank {q}")
    return x_from, max(at.values()) + flop * fl.sw, k_bit


def cones(fl, prefix, out_stems, in_stems):
    """the cells under prefix, with each one's input cone (the indices of
    the in_stems nets it reads, through its block), its output reach
    (the out_stems nets, as (stem, index)) and its logic depth"""
    sys.setrecursionlimit(20000)
    cells = [i for i in fl.block.getInsts() if i.getName().replace("\\", "").startswith(prefix)
             and i.getMaster().getType() == "CORE" and not i.isFixed()]
    names = {i.getName() for i in cells}

    def index(net, stems):
        m = re.fullmatch(r"u_core\.(\w+)\[(\d+)\]", net.getName().replace("\\", ""))
        return (m.group(1), int(m.group(2))) if m and m.group(1) in stems else None

    fanin, reach, depth = {}, {}, {}

    def cone_in(inst):
        k = inst.getName()
        if k not in fanin:
            fanin[k], depth[k] = set(), 0
            bits, d = set(), 0
            for it in inst.getITerms():
                net = it.getNet()
                if not it.isInputSignal() or net is None:
                    continue
                x = index(net, in_stems)
                if x:
                    bits.add(x[1])
                    continue
                for dv in net.getITerms():
                    if dv.isOutputSignal() and dv.getInst().getName() in names:
                        bits |= cone_in(dv.getInst())
                        d = max(d, depth[dv.getInst().getName()] + 1)
            fanin[k], depth[k] = bits, d
        return fanin[k]

    def cone_out(inst):
        k = inst.getName()
        if k not in reach:
            reach[k] = set()
            out = set()
            for it in inst.getITerms():
                net = it.getNet()
                if not it.isOutputSignal() or net is None:
                    continue
                if x := index(net, out_stems):
                    out.add(x)
                for ld in net.getITerms():
                    if ld.isInputSignal() and ld.getInst().getName() in names:
                        out |= cone_out(ld.getInst())
            reach[k] = out
        return reach[k]

    return [(inst, cone_in(inst), cone_out(inst), depth[inst.getName()]) for inst in cells]


def pack(fl, insts, rows, x0, role, west=False, spread=A.PLANE_SPREAD):
    """cells (inst, depth) across the given rows in turn, by depth from x0
    eastwards (or westwards), spread out by `spread`"""
    insts = sorted(insts, key=lambda t: (t[1], t[0].getName()))
    xs = {k: x0 for k in rows}
    for inst, _ in insts:
        k = min(rows, key=lambda r: (abs(xs[r] - x0), r))
        w = inst.getMaster().getWidth()
        if fl.place(inst, xs[k] - w if west else xs[k], fl.row_y(k), role):
            x = fl.anchors[-1]["x"]
            xs[k] = min(xs[k], x) - int(w * (spread - 1)) if west else max(xs[k], x) + int(w * spread)


def alu(fl, x_from, k_bit):
    """the ALU as 8 bit planes east of the register file, as the die stacks
    its ALU's bit slices: bit i's cells in the placement rows of the
    register file's bit row i, so the bus lines line up, and the logic no
    bit owns -- the flags' collectors, overflow, carry out, the decimal
    adjust's detection, the operation decode -- in a strip north of them,
    where the die has its flag area.

    The cells go where the die distributes the logic (its ALU rows hold the
    shifter and rotate muxes, the carry column, the parity chain, the bit
    select; the zero detect has pieces in every row):
    1. a cell whose output reaches exactly one result bit (alu_res[i]) is
       bit i's -- a shift or rotate mux reading a[i +- 1], a logic op, the
       bit select, a decimal-adjust correction sum;
    2. a carry or borrow cell reaching several result bits is the lowest
       one's, where that carry is generated;
    3. a cell reaching no single result bit (a zero-detect or parity leaf,
       a flag-only cell) is the median bit of what it reads directly -- the
       planes of its drivers and the operand bits -- if those span at most
       4 adjacent bits; otherwise it is shared (a tree's collector, overflow,
       the decode).
    Within a plane the cells go west to east by logic depth from the
    operands."""
    sys.setrecursionlimit(20000)
    cells = [i for i in fl.block.getInsts() if i.getName().replace("\\", "").startswith("u_core.u_alu.")
             and i.getMaster().getType() == "CORE" and not i.isFixed()]
    names = {i.getName(): i for i in cells}

    def index(net, stems):
        m = re.fullmatch(r"u_core\.(\w+)\[(\d+)\]", net.getName().replace("\\", ""))
        return (m.group(1), int(m.group(2))) if m and m.group(1) in stems else None

    direct, fanin, depth, reach = {}, {}, {}, {}

    def cone_in(k):
        if k not in fanin:
            fanin[k], depth[k] = set(), 0
            ds, ops = [], set()
            for it in names[k].getITerms():
                net = it.getNet()
                if not it.isInputSignal() or net is None:
                    continue
                if x := index(net, ("alu_a", "alu_b")):
                    ops.add(x[1])
                    continue
                for dv in net.getITerms():
                    if dv.isOutputSignal() and dv.getInst().getName() in names:
                        ds.append(dv.getInst().getName())
            direct[k] = (ds, ops)
            bits, d = set(ops), 0
            for x in ds:
                bits |= cone_in(x)
                d = max(d, depth[x] + 1)
            fanin[k], depth[k] = bits, d
        return fanin[k]

    def cone_out(k):
        if k not in reach:
            reach[k] = set()
            out = set()
            for it in names[k].getITerms():
                net = it.getNet()
                if not it.isOutputSignal() or net is None:
                    continue
                if x := index(net, ("alu_res",)):
                    out.add(x[1])
                for ld in net.getITerms():
                    if ld.isInputSignal() and ld.getInst().getName() in names:
                        out |= cone_out(ld.getInst().getName())
            reach[k] = out
        return reach[k]

    for k in names:
        cone_in(k)
        cone_out(k)
    bit = {}
    order = sorted(names, key=lambda k: (depth[k], k))
    for k in order:
        if len(reach[k]) == 1:
            bit[k] = next(iter(reach[k]))                       # 1. one result bit
        elif len(reach[k]) > 1 and fanin[k]:
            bit[k] = min(reach[k])                              # 2. a carry, where it starts
    for k in order:                                             # 3. by what it reads
        if k in bit:
            continue
        ds, ops = direct[k]
        bits = sorted([bit[x] for x in ds if bit.get(x) is not None] + list(ops))
        bit[k] = bits[(len(bits) - 1) // 2] if bits and bits[-1] - bits[0] <= 3 else None
    planes = {b: [(names[k], depth[k]) for k in order if bit[k] == b] for b in range(8)}
    shared = [(names[k], depth[k]) for k in order if bit[k] is None]
    x0 = x_from + A.ALU_GAP_SITES * fl.sw
    for b in range(8):
        pack(fl, planes[b], [k_bit(b) + j for j in range(A.BIT_ROWS)], x0, f"alu bit {b}", spread=A.ALU_SPREAD)
    top = k_bit(0) + A.BIT_ROWS + 1                             # the flag area, north of the planes
    pack(fl, shared, [top + j for j in range(A.ALU_FLAG_ROWS)], x0, "alu shared", spread=A.ALU_SPREAD)
    return {b: len(planes[b]) for b in range(8)}, len(shared)


def addr(fl, x_east, k_bit):
    """the address unit (the incrementer/decrementer and the address
    muxes) as 16 bit planes west of the address latch, along the A pins as
    on the die: bit i in the rows of the register file's bit i. A cell
    belongs to bit i when the address outputs it reaches (abus, idu, ea,
    al_in) are all bit i, or else when bit i is the highest bit of the
    16-bit inputs in its cone (the incrementer's carry counts to the bit it
    ends in); the rest (the control decode) goes in a strip above."""
    out = ("abus", "idu", "ea", "al_in")
    planes = {b: [] for b in range(16)}
    shared = []
    for inst, fi, fo, d in cones(fl, "u_core.u_addr.", out, A.ADDR_INPUTS):
        bits = {i for _, i in fo}
        if len(bits) == 1:
            planes[next(iter(bits))].append((inst, d))
        elif fi and fo:
            planes[max(fi)].append((inst, d))
        else:
            shared.append((inst, d))
    x0 = x_east - A.ALU_GAP_SITES * fl.sw // 2
    for b in range(16):
        pack(fl, planes[b], [k_bit(b) + j for j in range(A.BIT_ROWS)], x0, f"addr bit {b}", west=True)
    top = k_bit(0) + A.BIT_ROWS + 1
    pack(fl, shared, [top + j for j in range(4)], x0, "addr shared", west=True)
    return {b: len(planes[b]) for b in range(16)}, len(shared)


def timing(fl):
    """z80_timing (the pin images of the T-states) in a band above the
    PLA, under the control pins, as the die keeps its state logic"""
    top = len(fl.rows) - 1 - A.TIMING_NORTH_ROWS
    cells = [(inst, d) for inst, _, _, d in cones(fl, "u_core.u_timing", (), ())]
    pack(fl, cells, [top - j for j in range(A.TIMING_ROWS)], fl.x_at(A.TIMING_WEST), "timing")
    return len(cells)


def pla(fl):
    """the decoder: the instruction register and the input lines' drivers at
    the east end, the AND plane's terms in a grid west of them, the OR
    plane's lines in rows below -- on windows between the tap cells, so the
    grid stays regular"""
    top = len(fl.rows) - 1 - A.PLA_NORTH_ROWS
    west = fl.x_at(0.08)

    def plane_nets(pattern):
        """{index: net name} of the decoder's nets named like pattern (the
        planes' modules flattened: the wire may carry either module's name)"""
        out = {}
        for name in fl.nets:
            m = re.fullmatch(pattern, name)
            if m:
                k = int(m.group(1))
                if k not in out or len(name) < len(out[k]):
                    out[k] = name
        return out

    # the instruction register (and prefix) with each input line's drivers
    lits = plane_nets(r"u_core\.u_pla\.(?:u_lines\.)?c\[(\d+)\]")
    trues = plane_nets(r"u_core\.u_pla\.(?:u_lines\.)?t\[(\d+)\]")
    x_ir = fl.slots(top, fl.x_at(A.PLA_EAST), 48, 1, west=True)[0]
    for i, q in enumerate(A.PLA_INPUTS):
        y = fl.row_y(top - i * A.TERM_ROWS)
        fl.anchor(q, x_ir, y, f"pla input {q}")
        if i in lits:
            fl.anchor(lits[i], x_ir + 34 * fl.sw, y, f"pla line c[{i}]")
        if i in trues and (d := fl.driver(trues[i])) is not None and d.getName().startswith("u_core.u_pla."):
            fl.anchor(trues[i], x_ir + 40 * fl.sw, y, f"pla line t[{i}]")
    # the OR plane's cells, and each output line's last gate (an OR-plane
    # cell no other OR-plane cell reads), in the order of the nets they drive
    plane = {i.getName(): i for i in fl.block.getInsts() if i.getName().replace("\\", "").startswith("u_core.u_pla.u_or.")}
    finals = {}
    for inst in plane.values():
        for it in inst.getITerms():
            if it.isOutputSignal() and it.getNet() is not None:
                loads = [l.getInst().getName() for l in it.getNet().getITerms() if l.isInputSignal()]
                if not any(l in plane for l in loads) and inst.getMaster().getType() == "CORE":
                    finals[inst.getName()] = natural(it.getNet().getName().replace("\\", ""))
    order = sorted(finals, key=lambda n: finals[n])

    def lines_of(inst):
        """the output lines an AND-plane term feeds: its loads through the OR plane"""
        out, todo, seen = set(), [inst], set()
        while todo:
            cur = todo.pop()
            for it in cur.getITerms():
                if it.isOutputSignal() and it.getNet() is not None:
                    for l in it.getNet().getITerms():
                        name = l.getInst().getName()
                        if l.isInputSignal() and name in plane and name not in seen:
                            seen.add(name)
                            if name in finals:
                                out.add(name)
                            todo.append(l.getInst())
        return out

    # the AND plane: each output line's terms side by side, column by column
    # from the input lines westwards, so its OR gate stands below them
    names = plane_nets(r"u_core\.u_pla\.(?:u_and\.)?term\[(\d+)\]")
    terms = []
    for k in sorted(names):
        inst = fl.driver(names[k])
        if inst is None or inst.isFixed() or not inst.getName().startswith("u_core.u_pla."):
            fl.missing.append((names[k], "pla term (no gate of its own: a single literal, or shared)"))
        else:
            terms.append((k, inst, lines_of(inst)))
    cols = [x for x in fl.slots(top, x_ir, A.TERM_SITES, 400, west=True) if x >= west]
    nrow = max(1, -(-len(terms) // len(cols)))
    placed, band = set(), {}
    j = 0
    for line in order + [None]:
        mine = [(k, inst) for k, inst, ls in terms if k not in placed and (line in ls if line else True)]
        first = j
        for k, inst in mine:
            c, r = j // nrow, j % nrow
            fl.place(inst, cols[min(c, len(cols) - 1)], fl.row_y(top - r * A.TERM_ROWS), f"pla term {k}")
            placed.add(k)
            j += 1
        if line and mine:
            band[line] = cols[min((first + j - 1) // 2 // nrow, len(cols) - 1)]
    # the OR plane: each line's gate below its terms
    or_top = top - nrow * A.TERM_ROWS - 1
    n = 0
    for line in order:
        inst = plane[line]
        if inst.isFixed():
            continue
        x = band.get(line, cols[0])
        fl.place(inst, x, fl.row_y(or_top), "pla or " + finals[line][0] + (f"[{finals[line][1]}]" if finals[line][1] >= 0 else ""))
        n += 1
    return len(terms), len(cols), nrow, n


def state(fl):
    """the state logic's flip-flops in a row near the top"""
    y = fl.row_y(len(fl.rows) - 1 - A.STATE_ROWS)
    x = fl.x_at(A.STATE_X[0])
    for q in A.STATE_FLOPS:
        inst = fl.anchor(q, x, y, f"state {q}")
        if inst is not None:
            x += inst.getMaster().getWidth() + 4 * fl.sw


@click.command()
@click.option("--anchors", "anchors_path", required=True, help="write the anchors here (JSON)")
@click_odb
def main(reader, anchors_path):
    fl = Floor(reader.block)
    west, east, k_bit = datapath(fl)  # the regular blocks first, then what fits around them
    alu_bits, alu_shared = alu(fl, east, k_bit)
    addr_bits, addr_shared = addr(fl, west, k_bit)
    ntiming = timing(fl)
    nterms, ncol, nrow, nlines = pla(fl)
    state(fl)
    pins(fl)
    roles = {}
    for a in fl.anchors:
        key = a["role"].split()[0]
        roles[key] = roles.get(key, 0) + 1
    print(f"[INFO RZ80] core {fl.w / fl.dbu:.1f} x {fl.h / fl.dbu:.1f} um, {len(fl.rows)} rows")
    print(f"[INFO RZ80] anchored {len(fl.anchors)} cells: " + ", ".join(f"{k} {v}" for k, v in sorted(roles.items())))
    print("[INFO RZ80] ALU bit planes 0..7: " + " ".join(str(alu_bits[b]) for b in range(8))
          + f" cells, {alu_shared} shared")
    print("[INFO RZ80] address unit bit planes 0..15: " + " ".join(str(addr_bits[b]) for b in range(16))
          + f" cells, {addr_shared} shared; z80_timing {ntiming} cells above the PLA")
    print(f"[INFO RZ80] decoder: {nterms} AND-plane terms in {nrow} rows of {ncol}, {nlines} OR-plane lines")
    for q, role in fl.missing:
        print(f"[WARNING RZ80] no cell drives {q} ({role})")
    for name, role in fl.unplaced:
        print(f"[ERROR RZ80] no free spot for {name} ({role})")
    json.dump({"dbu": fl.dbu, "anchors": fl.anchors, "missing": fl.missing, "unplaced": fl.unplaced},
              open(anchors_path, "w"), indent=1)
    if fl.unplaced:
        sys.exit(1)


if __name__ == "__main__":
    main()
