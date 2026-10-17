#!/usr/bin/env python3
"""Summarise `make luts`: one row per FPGA family from build/luts/<fam>/stat.json.

LUT counts are logic LUTs only: ECP5 CCU2C carry cells (2 LUT4 + carry each)
and Gowin ALU cells are listed under "carry", the wide-function muxes
(PFUMX/L6MUX21, MUX2_LUT5..8) under "mux". The usual headline number for
an iCE40 is SB_LUT4; the iCE40UP5K (iCEBreaker, pico-ice) has 5,280.
"""
import json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAMILIES = sys.argv[1:] or ["ice40", "ecp5", "gowin"]

def classify(fam, cell):
    if fam == "ice40":
        if cell == "SB_LUT4": return "lut"
        if cell == "SB_CARRY": return "carry"
        if cell.startswith("SB_DFF"): return "ff"
    elif fam == "ecp5":
        if cell == "LUT4": return "lut"
        if cell == "CCU2C": return "carry"
        if cell in ("PFUMX", "L6MUX21"): return "mux"
        if cell == "TRELLIS_FF": return "ff"
    elif fam == "gowin":
        if cell in ("LUT1", "LUT2", "LUT3", "LUT4"): return "lut"
        if cell == "ALU": return "carry"
        if cell.startswith("MUX2_LUT"): return "mux"
        if cell.startswith("DFF"): return "ff"
    return "other"

print(f"{'family':8s} {'LUTs':>7s} {'carry':>7s} {'mux':>7s} {'FFs':>6s} {'other':>6s}")
for fam in FAMILIES:
    path = os.path.join(ROOT, "build", "luts", fam, "stat.json")
    if not os.path.isfile(path):
        print(f"{fam:8s} (no {os.path.relpath(path, ROOT)})")
        continue
    cells = json.load(open(path))["design"]["num_cells_by_type"]
    tot = {"lut": 0, "carry": 0, "mux": 0, "ff": 0, "other": 0}
    for cell, n in cells.items():
        if cell.startswith("$"):        # $scopeinfo etc.
            continue
        tot[classify(fam, cell)] += n
    print(f"{fam:8s} {tot['lut']:7,d} {tot['carry']:7,d} {tot['mux']:7,d} {tot['ff']:6,d} {tot['other']:6,d}")
