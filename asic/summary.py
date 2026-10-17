#!/usr/bin/env python3
"""Print the tapeout-readiness numbers of one LibreLane run.

usage: summary.py <run dir>   (e.g. asic/sky130/runs/RUN_2026-10-17_...)

Reads <run>/final/metrics.json and prints area, timing (per corner where
LibreLane reports it) and the signoff checks. Exit status 1 if any
signoff check is non-zero, so CI can gate on it.
"""
import json, os, sys

run = sys.argv[1] if len(sys.argv) > 1 else ""
path = os.path.join(run, "final", "metrics.json")
if not os.path.isfile(path):
    sys.exit(f"summary: {path} not found -- did the flow finish?")
m = json.load(open(path))

def get(k, default="-"):
    return m.get(k, default)

def num(v, fmt="{:.3f}"):
    return fmt.format(v) if isinstance(v, (int, float)) else str(v)

die = get("design__die__bbox")
core = get("design__core__area")
print(f"run            {os.path.basename(os.path.normpath(run))}")
print(f"die bbox (um)  {die}")
print(f"die area (um2) {num(get('design__die__area'), '{:.0f}')}")
print(f"core area      {num(core, '{:.0f}')}")
print(f"std cells      {get('design__instance__count__stdcell', get('design__instance__count'))}"
      f"   area {num(get('design__instance__area__stdcell', get('design__instance__area')), '{:.0f}')} um2")
print(f"utilisation    {num(get('design__instance__utilization'), '{:.3f}')}")
print(f"flops          {get('design__instance__count__class:sequential_cell', get('design__instance__count__class:multi_input_combinational_cell', '-'))}")
print(f"wire length    {get('route__wirelength')} um")
print(f"power (W)      {num(get('power__total'), '{:.3e}')}")
print()
print("timing (worst over corners)")
for k in ("timing__setup__ws", "timing__setup__tns", "timing__setup_vio__count",
          "timing__hold__ws", "timing__hold__tns", "timing__hold_vio__count",
          "design__max_slew_violation__count", "design__max_cap_violation__count",
          "design__max_fanout_violation__count"):
    print(f"  {k:40s} {num(get(k))}")
corners = sorted({k.split("corner:")[1] for k in m if "timing__setup__ws__corner:" in k})
for c in corners:
    print(f"  {c:24s} setup ws {num(get('timing__setup__ws__corner:' + c)):>9}"
          f"   hold ws {num(get('timing__hold__ws__corner:' + c)):>9}")
print()
checks = [
    ("route DRC",        "route__drc_errors"),
    ("Magic DRC",        "magic__drc_error__count"),
    ("KLayout DRC",      "klayout__drc_error__count"),
    ("LVS",              "design__lvs_error__count"),
    ("antenna nets",     "antenna__violating__nets"),
    ("antenna pins",     "antenna__violating__pins"),
    ("disconnected pins", "design__disconnected_pin__count"),
    ("XOR (Magic/KLayout GDS)", "design__xor_difference__count"),
    ("setup violations", "timing__setup_vio__count"),
    ("hold violations",  "timing__hold_vio__count"),
]
bad = 0
print("signoff")
for name, k in checks:
    v = get(k)
    flag = "" if v in (0, "-") else "   <-- FAIL"
    if flag:
        bad += 1
    print(f"  {name:28s} {v}{flag}")
sys.exit(1 if bad else 0)
