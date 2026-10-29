# RZ80.PowerTree's and RZ80.PowerTreeChip's script, run by OpenROAD's
# Python with LibreLane's odbpy helpers on PYTHONPATH: the "old-school"
# power network, two interdigitated combs instead of a grid, as the NMOS
# Z80 die feeds +5 V from its east edge and GND from its west.
#
#   core   z80_top after pdngen has drawn only the Metal1 followpin rails:
#          - a VSS trunk down the west edge of the core and a VDD trunk
#            down the east edge (Metal4), the block's power pins;
#          - horizontal fingers (Metal5) off each trunk, VDD and VSS in
#            turn, each stopping short of the other net's trunk;
#          - short vertical twigs (Metal4) off each finger, tying it to the
#            rails of its net nearest to it (Metal1-to-Metal4 via stacks),
#            each rail fed by one finger only, so fingers meet only at
#            their trunk: a tree, not a mesh;
#          - each rail's end also on its own trunk.
#   chip   z80_chip: before pdngen, Metal5 over the z80_top macro is
#          obstructed (the macro's fingers are there, so the chip's grid
#          stays off it); after pdngen, the obstruction goes and Metal5
#          straps over each trunk's ends, where the macro has no Metal5,
#          feed the VDD trunk from the core ring's east side and the VSS
#          trunk from its west side -- the VCC pad's side and the GND pad's.
import json
import os
import sys

from reader import click, click_odb  # noqa: E402

import odb  # noqa: E402

GRID = 10  # manufacturing grid, in dbu (5 nm at 2000 dbu/um)


def snap(v):
    return int(round(v / GRID)) * GRID


class Vias:
    """the via definitions, as pdngen generates them on this PDK (the same
    rules, cut sizes, spacings and enclosures as the grid's own vias)"""

    # name: (rule, bottom, cut, top, cut size, cut spacing, enclosure xb yb xt yt, rows, cols)
    DEFS = {
        # a rail (0.6 um) under a twig (1.6 um): Metal1 -> Metal4, 1 x 3 cuts
        "rz80_v1_twig": ("Via1_GEN_HH", "Metal1", "Via1", "Metal2", 520, 520, (120, 340, 20, 120), 1, 3),
        "rz80_v2_twig": ("Via2_GEN_HH", "Metal2", "Via2", "Metal3", 520, 520, (20, 120, 120, 20), 1, 3),
        "rz80_v3_twig": ("Via3_GEN_HH", "Metal3", "Via3", "Metal4", 520, 520, (120, 20, 300, 120), 1, 3),
        # a rail under a trunk: 5 um wide, 1 x 9 cuts
        "rz80_v1_trunk": ("Via1_GEN_HH", "Metal1", "Via1", "Metal2", 520, 520, (120, 340, 20, 120), 1, 9),
        "rz80_v2_trunk": ("Via2_GEN_HH", "Metal2", "Via2", "Metal3", 520, 520, (20, 120, 120, 20), 1, 9),
        "rz80_v3_trunk": ("Via3_GEN_HH", "Metal3", "Via3", "Metal4", 520, 520, (120, 20, 580, 120), 1, 9),
        # a twig (1.6 um) under a finger (5 um): Metal4 -> Metal5, 9 x 3 cuts
        "rz80_v4_twig": ("Via4_GEN_HH", "Metal4", "Via4", "Metal5", 520, 520, (300, 580, 120, 580), 9, 3),
        # a 5 um strap on a trunk: Metal4 -> Metal5, 4 x 4 cuts
        "rz80_v4_trunk": ("Via4_GEN_HH", "Metal4", "Via4", "Metal5", 520, 720, (400, 120, 120, 400), 4, 4),
    }

    def __init__(self, block):
        tech = block.getDataBase().getTech()
        self.via = {}
        for name, (rule, bot, cut, top, size, space, enc, rows, cols) in self.DEFS.items():
            v = block.findVia(name) or odb.dbVia_create(block, name)
            p = odb.dbViaParams()
            p.setBottomLayer(tech.findLayer(bot))
            p.setCutLayer(tech.findLayer(cut))
            p.setTopLayer(tech.findLayer(top))
            p.setXCutSize(size)
            p.setYCutSize(size)
            p.setXCutSpacing(space)
            p.setYCutSpacing(space)
            p.setXBottomEnclosure(enc[0])
            p.setYBottomEnclosure(enc[1])
            p.setXTopEnclosure(enc[2])
            p.setYTopEnclosure(enc[3])
            p.setNumCutRows(rows)
            p.setNumCutCols(cols)
            v.setViaGenerateRule(tech.findViaGenerateRule(rule))
            v.setViaParams(p)
            self.via[name] = v

    def __getitem__(self, name):
        return self.via[name]


def swire(net):
    for sw in net.getSWires():
        return sw
    return odb.dbSWire_create(net, "ROUTED")


def rect(sw, layer, x0, y0, x1, y1, kind="STRIPE"):
    odb.dbSBox_create(sw, layer, snap(min(x0, x1)), snap(min(y0, y1)), snap(max(x0, x1)), snap(max(y0, y1)), kind)


def via(sw, v, x, y):
    odb.dbSBox_create(sw, v, snap(x), snap(y), "STRIPE")


def core_tree(reader, cfg):
    block = reader.block
    tech = block.getDataBase().getTech()
    um = block.getDbUnitsPerMicron()
    m4, m5 = tech.findLayer("Metal4"), tech.findLayer("Metal5")
    vias = Vias(block)
    core = block.getCoreArea()
    cx0, cy0, cx1, cy1 = core.xMin(), core.yMin(), core.xMax(), core.yMax()
    tw = snap(cfg["RZ80_TREE_TRUNK_WIDTH"] * um)
    fw = snap(cfg["RZ80_TREE_FINGER_WIDTH"] * um)
    gap = snap(cfg["RZ80_TREE_FINGER_GAP"] * um)
    pitch = snap(cfg["RZ80_TREE_TWIG_PITCH"] * um)
    twig_w = 3200
    nets = {"VDD": block.findNet(cfg["VDD_PIN"]), "VSS": block.findNet(cfg["GND_PIN"])}
    sw = {k: swire(n) for k, n in nets.items()}

    # the rails pdngen drew: (net, y centre)
    rails = []
    for key, net in nets.items():
        for w in net.getSWires():
            for b in w.getWires():
                if not b.isVia() and b.getTechLayer().getName() == "Metal1" and b.getWireShapeType() == "FOLLOWPIN":
                    rails.append((key, (b.yMin() + b.yMax()) // 2))
    if not rails:
        sys.exit("[ERROR RZ80] no followpin rails: RZ80.PowerTree expects pdngen to have drawn only them")

    # trunks: VSS west, VDD east -- the power pins
    trunk = {"VSS": (cx0, cx0 + tw), "VDD": (cx1 - tw, cx1)}
    for key, (x0, x1) in trunk.items():
        rect(sw[key], m4, x0, cy0, x1, cy1)
        bterm = block.findBTerm(nets[key].getName())
        if bterm is None:
            bterm = odb.dbBTerm_create(nets[key], nets[key].getName())
            bterm.setIoType("INOUT")
            bterm.setSigType("POWER" if key == "VDD" else "GROUND")
            bterm.setSpecial()
        for bp in list(bterm.getBPins()):
            odb.dbBPin_destroy(bp)
        bp = odb.dbBPin_create(bterm)
        odb.dbBox_create(bp, m4, snap(x0), snap(cy0), snap(x1), snap(cy1))
        bp.setPlacementStatus("FIRM")
    # every rail's end on its own trunk
    for key, y in rails:
        x0, x1 = trunk[key]
        for v in ("rz80_v1_trunk", "rz80_v2_trunk", "rz80_v3_trunk"):
            via(sw[key], vias[v], (x0 + x1) // 2, y)

    # fingers: one per band of rows, VSS and VDD in turn from the bottom
    row_h = block.getRows()[0].getSite().getHeight()
    band = cfg["RZ80_TREE_BAND_ROWS"] * row_h
    nb = max(2, (cy1 - cy0) // band)
    fingers = []
    for k in range(nb):
        key = "VSS" if k % 2 == 0 else "VDD"
        y = snap(cy0 + (k + 0.5) * (cy1 - cy0) / nb)
        fingers.append((key, y))
        x0, x1 = (cx0, cx1 - tw - gap) if key == "VSS" else (cx0 + tw + gap, cx1)
        rect(sw[key], m5, x0, y - fw // 2, x1, y + fw // 2)
        tx0, tx1 = trunk[key]
        via(sw[key], vias["rz80_v4_trunk"], (tx0 + tx1) // 2, y)
    # each rail to the nearest finger of its net
    mine = {i: [] for i in range(len(fingers))}
    for key, y in rails:
        i = min((i for i, (k, _) in enumerate(fingers) if k == key), key=lambda i: abs(fingers[i][1] - y))
        mine[i].append(y)
    # twigs: along each finger at the twig pitch, VDD's half a pitch from VSS's
    ntwig = nvia = 0
    for i, (key, y) in enumerate(fingers):
        ys = mine[i]
        if not ys:
            continue
        lo = min(min(ys) - 600, y - fw // 2)
        hi = max(max(ys) + 600, y + fw // 2)
        start = cx0 + tw + gap + twig_w + (pitch // 2 if key == "VDD" else 0)
        x = start
        while x + twig_w < cx1 - tw - gap:
            rect(sw[key], m4, x - twig_w // 2, lo, x + twig_w // 2, hi)
            via(sw[key], vias["rz80_v4_twig"], x, y)
            for ry in ys:
                for v in ("rz80_v1_twig", "rz80_v2_twig", "rz80_v3_twig"):
                    via(sw[key], vias[v], x, ry)
                nvia += 1
            ntwig += 1
            x += pitch
    # pdngen's step checked the grid before the trunks, the power pins, existed
    grid_check(reader, nets.values(), cfg["_OUT"])
    print(f"[INFO RZ80] power tree: trunks {tw / um:.1f} um (VSS west, VDD east), {len(fingers)} fingers "
          f"{fw / um:.1f} um on Metal5, {ntwig} twigs on Metal4, {nvia} rail taps, {len(rails)} rails")


def macro(block, name):
    inst = block.findInst(name)
    if inst is None:
        sys.exit(f"[ERROR RZ80] no macro instance {name}")
    return inst


def chip_obstruct(reader, cfg):
    """Metal5 over the macro: the chip's grid stays off its fingers"""
    block = reader.block
    m5 = block.getDataBase().getTech().findLayer("Metal5")
    inst = macro(block, cfg["RZ80_TREE_MACRO"])
    bb = inst.getBBox()
    halo = snap(cfg["RZ80_TREE_MACRO_HALO"] * block.getDbUnitsPerMicron())
    odb.dbObstruction_create(block, m5, bb.xMin() - halo, bb.yMin() - halo, bb.xMax() + halo, bb.yMax() + halo)
    print(f"[INFO RZ80] Metal5 obstructed over {inst.getName()} for the chip's power grid")


def grid_check(reader, nets, out):
    """PSM's connectivity check of the nets, its count written to `out` for
    the flow's own checker (pdngen's step checked before this one's shapes)"""
    bad = 0
    for net in nets:
        err = os.path.join(os.path.dirname(out), f"pg-{net.getName()}.rpt")
        reader.design.evalTclString(f"check_power_grid -net {net.getName()} -error_file {err}")
        if os.path.isfile(err):
            bad += sum(1 for line in open(err) if line.strip().startswith("violation type"))
    json.dump({"design__power_grid_violation__count": bad}, open(out, "w"))
    print(f"[INFO RZ80] power grid check: {bad} violations")


def chip_connect(reader, cfg):
    """the obstruction off, and each of the macro's trunks fed from its own
    side: Metal5 straps from the core ring's VDD side east of the macro onto
    the VDD trunk, from its VSS side west of it onto the VSS trunk. The
    straps cross the macro's top and bottom margins, where its abstract has
    no Metal5 (its fingers lie between), and land on the trunk's ends."""
    block = reader.block
    tech = block.getDataBase().getTech()
    um = block.getDbUnitsPerMicron()
    m4, m5 = tech.findLayer("Metal4"), tech.findLayer("Metal5")
    vias = Vias(block)
    inst = macro(block, cfg["RZ80_TREE_MACRO"])
    bb = inst.getBBox()
    halo = snap(cfg["RZ80_TREE_MACRO_HALO"] * um)
    for ob in list(block.getObstructions()):
        b = ob.getBBox()
        if b.getTechLayer().getName() == "Metal5" and (b.xMin(), b.yMin(), b.xMax(), b.yMax()) == \
                (bb.xMin() - halo, bb.yMin() - halo, bb.xMax() + halo, bb.yMax() + halo):
            odb.dbObstruction_destroy(ob)
    if inst.getOrient() != "R0":
        sys.exit(f"[ERROR RZ80] {inst.getName()} is placed {inst.getOrient()}: RZ80.PowerTreeChip expects R0")
    ox, oy = inst.getOrigin()
    trunks, m5_obs = {}, []
    for mterm in inst.getMaster().getMTerms():
        if mterm.getSigType() in ("POWER", "GROUND"):
            for mpin in mterm.getMPins():
                for g in mpin.getGeometry():
                    if g.getTechLayer().getName() == "Metal4":
                        trunks[mterm.getSigType()] = (g.xMin() + ox, g.yMin() + oy, g.xMax() + ox, g.yMax() + oy)
    for g in inst.getMaster().getObstructions():
        if g.getTechLayer() is not None and g.getTechLayer().getName() == "Metal5":
            m5_obs.append((g.yMin() + oy, g.yMax() + oy))
    # the macro's Metal5-free margins over each trunk's ends
    obs_lo = min(y0 for y0, _ in m5_obs) if m5_obs else bb.yMax()
    obs_hi = max(y1 for _, y1 in m5_obs) if m5_obs else bb.yMin()
    w = snap(cfg["RZ80_TREE_STRAP_WIDTH"] * um)
    clear = snap(cfg["RZ80_TREE_STRAP_CLEARANCE"] * um)
    made = 0
    for key, sig, east in (("VDD", "POWER", True), ("VSS", "GROUND", False)):
        net = block.findNet(cfg["VDD_PIN"] if key == "VDD" else cfg["GND_PIN"])
        tx0, ty0, tx1, ty1 = trunks[sig]
        tx = (tx0 + tx1) // 2
        # the ring's vertical side of this net on the trunk's side
        sides = []
        for wire in net.getSWires():
            for sb in wire.getWires():
                if not sb.isVia() and sb.getTechLayer().getName() == "Metal4" and sb.getWireShapeType() == "RING" \
                        and sb.getDY() > sb.getDX():
                    sides.append(sb)
        side = (min if east else max)([s_ for s_ in sides if (s_.xMin() > bb.xMax()) == east],
                                      key=lambda s_: (s_.xMin() + s_.xMax()) // 2)
        rx = (side.xMin() + side.xMax()) // 2
        sw = swire(net)

        # the chip's other Metal5 shapes the strap must keep clear of: wires,
        # and the Metal5 of the vias on them
        def on_m5(sb):
            if sb.isVia():
                v = sb.getBlockVia() or sb.getTechVia()
                return v is not None and "Metal5" in (v.getTopLayer().getName(), v.getBottomLayer().getName())
            return sb.getTechLayer().getName() == "Metal5"

        others = [sb for n in block.getNets() if n.isSpecial() for wire in n.getSWires() for sb in wire.getWires()
                  if on_m5(sb)]
        xa, xb = min(tx, rx) - w // 2, max(tx, rx) + w // 2

        def free(yc):
            return not any(o.xMin() < xb + clear and xa - clear < o.xMax() and
                           o.yMin() < yc + w // 2 + clear and yc - w // 2 - clear < o.yMax() for o in others)

        for lo, hi in ((ty0, obs_lo), (obs_hi, ty1)):      # the trunk's bottom end, its top end
            # the strap within the trunk's end, its via too, clear of the macro's fingers
            fits = [yc for yc in range(snap(max(lo, ty0) + w // 2 + clear), snap(min(hi, ty1) - w // 2 - clear) + 1, GRID)
                    if free(yc)]
            if not fits:
                continue
            yc = fits[len(fits) // 2]
            rect(sw, m5, xa, yc - w // 2, xb, yc + w // 2)
            via(sw, vias["rz80_v4_trunk"], tx, yc)
            via(sw, vias["rz80_v4_trunk"], rx, yc)
            made += 1
    if made < 2:
        sys.exit("[ERROR RZ80] the macro's trunks have no Metal5-free ends to feed")
    print(f"[INFO RZ80] macro trunks fed: VDD from the core ring's east side, VSS from its west side, {made} straps")
    grid_check(reader, [block.findNet(cfg["VDD_PIN"]), block.findNet(cfg["GND_PIN"])], cfg["_OUT"])


@click.command()
@click.option("--mode", type=click.Choice(["core", "chip-obstruct", "chip-connect"]), required=True)
@click.option("--metrics-out", default=None, help="core: write the power grid check's count here (JSON)")
@click_odb
def main(reader, mode, metrics_out):
    cfg = dict(reader.config)
    cfg["_OUT"] = metrics_out
    {"core": core_tree, "chip-obstruct": chip_obstruct, "chip-connect": chip_connect}[mode](reader, cfg)


if __name__ == "__main__":
    main()
