#!/usr/bin/env python3
"""Print the tapeout-readiness numbers of one LibreLane run.

usage: summary.py <run dir>   (e.g. asic/sky130/runs/RUN_2026-10-17_...)

Reads <run>/final/metrics.json and <run>/resolved.json and prints area,
per-corner timing and the sign-off checks. Exit status 1 if a check
fails, so CI can gate on it:
  - route DRC, Magic / KLayout DRC, LVS, antenna, critical disconnected
    pins (LibreLane's own rule: an unconnected output, such as the chip's
    unused debug pins of z80_top, is not one), the Magic-vs-KLayout GDS
    XOR: must be 0;
  - setup: no violations at the corners LibreLane itself signs off
    (TIMING_VIOLATION_CORNERS, typical by default -- the same rule the
    flow's own checkers and the TinyTapeout / wafer.space flows apply);
    the other corners are printed for information;
  - hold: no violations at any corner (a hold failure breaks the chip
    at every clock frequency).
"""
import fnmatch, json, os, sys

run = sys.argv[1] if len(sys.argv) > 1 else ""
path = os.path.join(run, "final", "metrics.json")
if not os.path.isfile(path):
    sys.exit(f"summary: {path} not found -- did the flow finish?")
m = json.load(open(path))
cfg = json.load(open(os.path.join(run, "resolved.json")))
gate = cfg.get("TIMING_VIOLATION_CORNERS") or ["*tt*"]

def get(k, default="-"):
    return m.get(k, default)

def num(v, fmt="{:.3f}"):
    return fmt.format(v) if isinstance(v, (int, float)) else str(v)

print(f"run            {os.path.basename(os.path.normpath(run))}")
print(f"design         {cfg.get('DESIGN_NAME')} on {cfg.get('PDK')} / {cfg.get('STD_CELL_LIBRARY')}, "
      f"clock {cfg.get('CLOCK_PERIOD')} ns ({1000.0 / float(cfg.get('CLOCK_PERIOD')):.1f} MHz)")
print(f"die bbox (um)  {get('design__die__bbox')}")
print(f"die area (um2) {num(get('design__die__area'), '{:.0f}')}")
print(f"core area      {num(get('design__core__area'), '{:.0f}')}")
print(f"std cells      {get('design__instance__count__stdcell', get('design__instance__count'))}"
      f"   area {num(get('design__instance__area__stdcell', get('design__instance__area')), '{:.0f}')} um2")
print(f"utilisation    {num(get('design__instance__utilization'), '{:.3f}')}")
print(f"wire length    {get('route__wirelength')} um")
print(f"power (W)      {num(get('power__total'), '{:.3e}')}")
print(f"max slew / cap / fanout violations: {get('design__max_slew_violation__count')} / "
      f"{get('design__max_cap_violation__count')} / {get('design__max_fanout_violation__count')}")
print()

corners = sorted({k.split("corner:")[1] for k in m if "timing__setup__ws__corner:" in k})
gated = [c for c in corners if any(fnmatch.fnmatch(c, g) for g in gate)]
print(f"timing per corner (* = signed off: {', '.join(gate)})")
setup_vio = hold_vio = 0
for c in corners:
    sv = get("timing__setup_vio__count__corner:" + c, 0)
    hv = get("timing__hold_vio__count__corner:" + c, 0)
    mark = "*" if c in gated else " "
    print(f"  {mark} {c:24s} setup ws {num(get('timing__setup__ws__corner:' + c)):>9} ({sv} vio)"
          f"   hold ws {num(get('timing__hold__ws__corner:' + c)):>7} ({hv} vio)")
    if c in gated and isinstance(sv, (int, float)):
        setup_vio += sv
    if isinstance(hv, (int, float)):
        hold_vio += hv
print()

print(f"disconnected pins, all: {get('design__disconnected_pin__count')}")
checks = [
    ("route DRC",               get("route__drc_errors")),
    ("Magic DRC",               get("magic__drc_error__count")),
    ("KLayout DRC",             get("klayout__drc_error__count")),
    ("LVS",                     get("design__lvs_error__count")),
    ("antenna nets",            get("antenna__violating__nets")),
    ("antenna pins",            get("antenna__violating__pins")),
    ("disconnected pins (crit.)", get("design__critical_disconnected_pin__count")),
    ("XOR (Magic/KLayout GDS)", get("design__xor_difference__count")),
    ("setup violations (*)",    setup_vio),
    ("hold violations (all)",   hold_vio),
]
bad = 0
print("signoff")
for name, v in checks:
    flag = "" if v in (0, "-") else "   <-- FAIL"
    bad += bool(flag)
    print(f"  {name:28s} {v}{flag}")
sys.exit(1 if bad else 0)
