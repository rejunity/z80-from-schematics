#!/usr/bin/env python3
"""z80_top's die floorplan, checked on a finished run: every anchor
RZ80.DieFloorplan fixed (its anchors.json) is in the final layout within a
few sites of where the step put it (RZ80.ReleaseAnchors frees them after
global placement, so the repairs may size them and detailed placement
shift them to fit), and the final layout keeps the die's arrangement --
the register file's 14 columns in the die's order west to east, each
column's 16 bits in order top to bottom.

    floorplan_check.py RUN_DIR
"""
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "librelane_plugin_rz80"))
import arrangement as A  # noqa: E402

MAX_SHIFT_UM = 10.0       # how far detailed placement may shift a released anchor


def components(def_path):
    txt = open(def_path).read()
    sec = txt[txt.index("\nCOMPONENTS"):txt.index("\nEND COMPONENTS")]
    out = {}
    for m in re.finditer(r"^\s*-\s+(\S+)\s+(\S+)(.*?);", sec, re.M | re.S):
        name, master, rest = m.group(1), m.group(2), m.group(3)
        p = re.search(r"\+\s+(PLACED|FIXED|FIRM|COVER)\s+\(\s*(-?\d+)\s+(-?\d+)\s*\)\s+(\w+)", rest)
        if p:
            out[name.replace("\\", "")] = (p.group(1), int(p.group(2)), int(p.group(3)), p.group(4), master)
    return out


def main():
    run = sys.argv[1].rstrip("/")
    steps = glob.glob(os.path.join(run, "*-rz80-diefloorplan", "anchors.json"))
    if not steps:
        sys.exit(f"floorplan_check: no RZ80.DieFloorplan step in {run}")
    anchors = json.load(open(steps[0]))
    dbu = anchors["dbu"]
    comps = components(os.path.join(run, "final", "def", "z80_top.def"))
    bad = []
    moved = []
    final = {}
    for a in anchors["anchors"]:
        c = comps.get(a["inst"].replace("\\", ""))
        if c is None:
            bad.append(f"{a['inst']} ({a['role']}): not in the final layout")
            continue
        final[a["inst"]] = (c[1], c[2])
        dx, dy = abs(c[1] - a["x"]) / dbu, abs(c[2] - a["y"]) / dbu
        if dx or dy:
            moved.append(max(dx, dy))
        if dx > MAX_SHIFT_UM or dy > MAX_SHIFT_UM:
            bad.append(f"{a['inst']} ({a['role']}): moved {dx:.1f} / {dy:.1f} um from where the step put it")
    # the register file: columns west to east in the die's order, bits top to bottom
    reg = {}
    for a in anchors["anchors"]:
        m = re.fullmatch(r"regfile (\S+) bit (\d+)", a["role"])
        if m:
            reg.setdefault(m.group(1), {})[int(m.group(2))] = a
    order = [e[0] for e in A.REGISTER_COLUMNS if e is not None]
    xs = []
    for label in order:
        col = reg.get(label, {})
        if len(col) != 16:
            bad.append(f"register file column {label}: {len(col)} of 16 bits anchored")
            continue
        ys = [final.get(col[b]["inst"], (0, col[b]["y"]))[1] for b in range(16)]
        if ys != sorted(ys, reverse=True):
            bad.append(f"register file column {label}: bits not in order top to bottom")
        xs.append(sum(final.get(col[b]["inst"], (col[b]["x"], 0))[0] for b in range(16)) / 16)
    if xs != sorted(xs):
        bad.append("register file: columns not in the die's order west to east")
    # the ALU's bit planes: bit 0 on top, in order down, as the register rows
    plane_y = []
    for b in range(8):
        ys = [final[a["inst"]][1] for a in anchors["anchors"] if a["role"] == f"alu bit {b}" and a["inst"] in final]
        if ys:
            plane_y.append(sum(ys) / len(ys))
    if plane_y and plane_y != sorted(plane_y, reverse=True):
        bad.append("ALU: bit planes not in bit order top to bottom")
    terms = [a for a in anchors["anchors"] if a["role"].startswith("pla term")]
    if anchors["missing"]:
        for q, role in anchors["missing"]:
            print(f"floorplan_check: note: no cell of its own drives {q} ({role})")
    if bad:
        for b in bad[:20]:
            print("floorplan_check:", b)
        sys.exit(f"floorplan_check: {len(bad)} problems")
    roles = {}
    for a in anchors["anchors"]:
        roles[a["role"].split()[0]] = roles.get(a["role"].split()[0], 0) + 1
    print(f"floorplan_check: {len(anchors['anchors'])} anchors, {len(moved)} moved by detailed placement "
          f"(at most {max(moved, default=0):.1f} um) ("
          + ", ".join(f"{k} {v}" for k, v in sorted(roles.items())) + ")")
    print(f"floorplan_check: register file {len(order)} columns x 16 bits in the die's order; "
          f"{len(terms)} AND-plane terms in a grid; {len(plane_y)} ALU bit planes in bit order")


if __name__ == "__main__":
    main()
