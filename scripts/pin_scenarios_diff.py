#!/usr/bin/env python3
# pin_scenarios_diff.py - print a per-phase side-by-side diff of our
# pin trace vs perfectz80's gate-level pin trace for a single program.
# Shows a configurable context window around each diff so the
# direction-of-diff is visible (we lead by 1 phase, we have opposite
# logic, etc).
#
#   pin_scenarios_diff.py <prog.hex> [phases=200] [context=2]
#
# Reads <prog.hex>.events sidecar if present (passes through to both
# runners via the existing tracegen / perfectz80_runner CLI).
#
# Designed for investigating residual pin_scenarios divergences vs
# perfectz80 -- complements compare_signal_timing.py's pass/fail mode.
import os, sys, subprocess

ROOT     = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACEGEN = os.path.join(ROOT, "build", "bin", "tracegen")
PZ80     = os.path.join(ROOT, "build", "bin", "perfectz80_runner")

# Our trace columns (tracegen).
C_COLS = ["t", "phi", "m", "addr", "data_o", "data_i",
          "mreq", "iorq", "rd", "wr", "m1", "rfsh", "halt", "busack"]
# perfectz80 columns (one row per phase).
P_COLS = ["phase", "addr", "data_o", "data_i",
          "mreq", "iorq", "rd", "wr", "m1", "rfsh", "halt"]
# Columns that gate compare_signal_timing.py's PASS/FAIL.
CTRL_COLS = ["mreq", "iorq", "rd", "wr", "m1", "rfsh", "halt"]
# Bus columns -- informational only; addr is don't-care during refresh
# windows where MREQ asserts alone (no RD/WR) and during pure-idle phases.
BUS_COLS  = ["addr", "data_o"]
CMP_COLS  = BUS_COLS + CTRL_COLS

def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.stdout.splitlines()

def parse_trace(lines, header_cols):
    rows = []
    for ln in lines:
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        parts = ln.split()
        if len(parts) != len(header_cols):
            continue
        try:
            int(parts[0])
        except ValueError:
            continue
        rows.append(dict(zip(header_cols, parts)))
    return rows

def main():
    if len(sys.argv) < 2:
        print("usage: pin_scenarios_diff.py <prog.hex> [phases=200] [context=2]")
        sys.exit(2)
    prog    = sys.argv[1]
    phases  = sys.argv[2] if len(sys.argv) > 2 else "200"
    context = int(sys.argv[3]) if len(sys.argv) > 3 else 2

    events = prog + ".events".replace(".hex", "")  # noqa
    # Honest events file path: <prog>.events sits next to <prog.hex>.
    events_path = prog.rsplit(".hex", 1)[0] + ".events"
    extra = [events_path] if os.path.exists(events_path) else []

    # PZ_OFFSET=1: perfectz80 emits one pre-M1 idle phase before its
    # first M1, our tracegen starts at T1.P of the first M1 directly.
    # compare_signal_timing.py drops perfectz80's first row to align;
    # mirror that here so the harness shows the SAME residual diffs the
    # main compare script gates on.
    PZ_OFFSET = 1
    c_rows = parse_trace(run([TRACEGEN, prog, phases] + extra), C_COLS)
    p_rows = parse_trace(run([PZ80, prog, str(int(phases) + PZ_OFFSET)] + extra), P_COLS)
    p_rows = p_rows[PZ_OFFSET:]
    n = min(len(c_rows), len(p_rows))

    # Bucket the diff phases:
    #   - ctrl_diff: ctrl-pin diff (gates compare_signal_timing.py)
    #   - bus_only: addr/data diff with all ctrl pins matching
    ctrl_diff_phases = []
    bus_only_phases  = []
    for i in range(n):
        c, p = c_rows[i], p_rows[i]
        any_ctrl = any(c.get(col) != p.get(col) for col in CTRL_COLS)
        any_bus  = any(c.get(col) != p.get(col) for col in BUS_COLS)
        if any_ctrl:
            ctrl_diff_phases.append(i)
        elif any_bus:
            bus_only_phases.append(i)

    diff_phases = ctrl_diff_phases + bus_only_phases
    if not diff_phases:
        print(f"  {os.path.basename(prog)}: no diffs across {n} phases")
        return 0

    print(f"  {os.path.basename(prog)}: ctrl-pin diff phases {len(ctrl_diff_phases)} "
          f"+ bus-only {len(bus_only_phases)} out of {n} (context = +/-{context})\n")

    # Print a compact header + per-phase rows. Highlight diffs with `!`.
    cols_display = ["addr", "m1", "mreq", "rd", "wr", "iorq",
                    "rfsh", "halt"]
    head = "  ph | C: " + " ".join(f"{c:>4}" for c in cols_display)
    head += "   pz80: " + " ".join(f"{c:>4}" for c in cols_display)
    print(head)
    print("  " + "-" * (len(head) - 2))

    printed = set()
    for dp in diff_phases:
        lo = max(0, dp - context)
        hi = min(n, dp + context + 1)
        for i in range(lo, hi):
            if i in printed:
                continue
            printed.add(i)
            c, p = c_rows[i], p_rows[i]
            marker = "!" if i in diff_phases else " "
            our  = " ".join(f"{c.get(col, ''):>4}" for col in cols_display)
            theirs = " ".join(f"{p.get(col, ''):>4}" for col in cols_display)
            print(f"  {i:3d}{marker}|     {our}            {theirs}")
        # Spacer between diff windows.
        if hi < n and (hi - 1) in printed:
            next_diff = next((d for d in diff_phases if d > dp), None)
            if next_diff is not None and next_diff > hi + 1:
                print("  ...")
    return 1

if __name__ == "__main__":
    sys.exit(main())
