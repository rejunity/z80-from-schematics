#!/usr/bin/env python3
# compare_signal_timing.py - diff per-half-cycle pin signals between our
# emitter of choice (C tracegen, iverilog RTL, or Verilator RTL) and the
# perfectz80 gate-level netlist simulator (build/bin/perfectz80_runner).
#
# Compares the 7 CPU-driven control pins (mreq/iorq/rd/wr/m1/rfsh/halt) for
# every phase. ALSO compares addr+data on the windows where they're known
# valid (addr during any mreq/iorq active, data_o during wr active).
#
# If a `<prog>.events` sidecar exists alongside the .hex, the C tracegen
# loads it and applies the per-phase pin-events. (The iverilog and
# Verilator RTL testbenches today only understand the `+nmi=<phase>`
# shorthand, not the full sidecar; programs that need INT/WAIT/BUSREQ/
# RESET events won't be exercised through the RTL paths yet.)
#
# Usage:
#   scripts/compare_signal_timing.py [phases] [prog...]
#   scripts/compare_signal_timing.py --rtl=iverilog [phases] [prog...]
#   scripts/compare_signal_timing.py --rtl=verilator [phases] [prog...]
#   scripts/compare_signal_timing.py --rtl=netlist  [phases] [prog...]
#
# --rtl=netlist is the LibreLane gate-level path: feeds the synthesized
# sky130 netlist (build/tb_z80_netlist.vvp) through iverilog and diffs
# against perfectz80. See docs/librelane-flow.md.
import os, sys, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACEGEN = os.path.join(ROOT, "build", "bin", "tracegen")
PZ80     = os.path.join(ROOT, "build", "bin", "perfectz80_runner")
VVP      = "vvp"
TB_VVP   = os.path.join(ROOT, "build", "tb_z80.vvp")
TB_NETLIST_VVP = os.path.join(ROOT, "build", "tb_z80_netlist.vvp")
VERILATOR_SIM = os.path.join(ROOT, "build", "obj_dir", "sim_z80")

# tracegen cols: t phi m addr data_o data_i mreq iorq rd wr m1 rfsh halt busack
# pz80 cols:    phase addr data_o data_i mreq iorq rd wr m1 rfsh halt
#
# perfectz80 starts one phase before our tracegen "T1.P" (its phase 0 = pre-M1
# idle); align with PZ_OFFSET=1.
PZ_OFFSET = 1

# Always-on control-pin parity (CPU-driven, no don't-cares) — this is the
# gate of record and affects exit code.
COMPARED  = ["mreq","iorq","rd","wr","m1","rfsh","halt"]
# Bus parity: addr is valid when (mreq||iorq) is low; data_o is valid
# when wr is low. Compared only on phases where BOTH the C model AND
# perfectz80 agree on the relevant strobe being low. Reported as
# informational findings (counted, summarised, but DO NOT affect exit
# code) because there's a well-known one-phase addr-settle delta where
# our model latches addr slightly earlier than pz80 — silicon-faithfulness
# polish item, tracked in docs/test-expansion-plan.md "bus-window
# comparison" and docs/audit-followups.md.
# Enable bus comparison: COMPARE_BUS=1 (default).
# Make bus diffs gate exit code too: BUS_STRICT=1 (default off).
COMPARE_BUS = os.environ.get("COMPARE_BUS", "1") != "0"
BUS_STRICT  = os.environ.get("BUS_STRICT",  "0") == "1"

def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write(p.stderr); raise SystemExit(f"failed: {' '.join(cmd)}")
    return p.stdout

def parse_tracegen(text):
    rows = []
    for ln in text.splitlines():
        p = ln.split()
        if len(p) < 14: continue
        if not p[0].lstrip('-').isdigit(): continue
        rows.append(dict(zip(["t","phi","m","addr","data_o","data_i","mreq","iorq","rd","wr","m1","rfsh","halt","busack"], p)))
    return rows

def parse_pz80(text):
    rows = []
    for ln in text.splitlines():
        p = ln.split()
        if len(p) < 11: continue
        if not p[0].isdigit(): continue
        rows.append(dict(zip(["phase","addr","data_o","data_i","mreq","iorq","rd","wr","m1","rfsh","halt"], p)))
    return rows

def compare(prog, phases, source="c"):
    events = prog[:-4] + ".events" if prog.endswith(".hex") else prog + ".events"
    events = events if os.path.exists(events) else ""
    if source == "c":
        tg_argv = [TRACEGEN, prog, str(phases)] + ([events] if events else [])
        emit_label = "C"
    elif source == "iverilog":
        # iverilog testbench only understands +nmi=<phase> shorthand, not
        # the full sidecar; skip events for now. Programs with .events
        # files will show ctrl-pin diffs vs perfectz80 (which DOES apply
        # the sidecar) and that's expected — pin-scenarios still belong
        # to the C-only path.
        tg_argv = [VVP, TB_VVP, f"+prog={prog}", f"+phases={phases}"]
        emit_label = "iverilog RTL"
    elif source == "verilator":
        tg_argv = [VERILATOR_SIM, prog, str(phases)]
        emit_label = "Verilator RTL"
    elif source == "netlist":
        # LibreLane-synthesized sky130 gate-level netlist driven by the
        # same iverilog testbench. Like iverilog mode, the testbench
        # doesn't consume the .events sidecar — pin-scenarios still go
        # through the C-only path.
        tg_argv = [VVP, TB_NETLIST_VVP, f"+prog={prog}", f"+phases={phases}"]
        emit_label = "gate-level netlist"
    else:
        raise SystemExit(f"unknown source: {source}")
    pz_argv = [PZ80,     prog, str(phases + PZ_OFFSET)] + ([events] if events else [])
    c = parse_tracegen(run(tg_argv))
    g = parse_pz80(run(pz_argv))
    g = g[PZ_OFFSET:]                 # drop pz80's pre-M1 idle phase(s)
    n = min(len(c), len(g))
    name = os.path.basename(prog)
    # C tracegen consumes events; RTL paths don't yet — note that in the
    # report so a curious reader knows why pin_scenarios diff through
    # iverilog when they pass through C.
    if events and source == "c":
        suffix = f" + events"
    elif events:
        suffix = f" (events skipped — RTL path)"
    else:
        suffix = ""
    if n == 0:
        print(f"  {name}: FAIL (no rows)"); return False
    ctrl_mism = 0           # control-pin mismatches — count against pass/fail
    bus_addr_mism = 0       # bus-value mismatches — informational by default
    bus_data_mism = 0
    bus_addr_compared = 0
    bus_data_compared = 0
    shown = 0
    for i in range(n):
        diff = [(col, c[i][col], g[i][col]) for col in COMPARED if c[i][col] != g[i][col]]
        bus_diff = []
        if COMPARE_BUS:
            # addr is valid when (mreq||iorq) is low on BOTH sides.
            c_addr_valid = (c[i]["mreq"] == "0" or c[i]["iorq"] == "0")
            g_addr_valid = (g[i]["mreq"] == "0" or g[i]["iorq"] == "0")
            if c_addr_valid and g_addr_valid:
                bus_addr_compared += 1
                if c[i]["addr"] != g[i]["addr"]:
                    bus_addr_mism += 1
                    bus_diff.append(("addr", c[i]["addr"], g[i]["addr"]))
            # data_o is valid when wr is low on BOTH sides.
            if c[i]["wr"] == "0" and g[i]["wr"] == "0":
                bus_data_compared += 1
                if c[i]["data_o"] != g[i]["data_o"]:
                    bus_data_mism += 1
                    bus_diff.append(("data_o", c[i]["data_o"], g[i]["data_o"]))
        if diff:
            ctrl_mism += 1
        if (diff or bus_diff) and shown < 5:
            shown += 1
            d = " ".join(f"{c1}: C={v1} pz80={v2}" for c1,v1,v2 in (diff + bus_diff))
            print(f"  {name} phase {i} differ:  {d}")
    # Headline status reflects control-pin parity only (the gate of record);
    # bus diffs are summarised in a second line as informational findings.
    is_ok = (ctrl_mism == 0) and (not BUS_STRICT or (bus_addr_mism + bus_data_mism) == 0)
    head  = "PASS" if ctrl_mism == 0 else f"{ctrl_mism}/{n} ctrl-pin phases differ"
    print(f"  {name}: {head}{suffix}")
    if COMPARE_BUS:
        addr_pct = (100.0 * (bus_addr_compared - bus_addr_mism) / bus_addr_compared) \
                   if bus_addr_compared else 100.0
        data_pct = (100.0 * (bus_data_compared - bus_data_mism) / bus_data_compared) \
                   if bus_data_compared else 100.0
        flag = "" if BUS_STRICT else "  [informational]"
        print(f"           bus addr   {bus_addr_compared - bus_addr_mism}/{bus_addr_compared} "
              f"({addr_pct:.1f}%) match;  data_o {bus_data_compared - bus_data_mism}/{bus_data_compared} "
              f"({data_pct:.1f}%) match{flag}")
    return is_ok

def main():
    # Parse --rtl=<source> before positional args (keeps existing
    # callers — they pass no flag — backward-compatible).
    argv = sys.argv[1:]
    source = "c"
    if argv and argv[0].startswith("--rtl="):
        source = argv.pop(0).split("=", 1)[1]
    elif argv and argv[0] == "--rtl":
        argv.pop(0)
        source = "iverilog" if not argv else argv.pop(0)
    if source not in ("c", "iverilog", "verilator", "netlist"):
        raise SystemExit(f"unknown --rtl source: {source}")
    if source == "c"        and not os.path.exists(TRACEGEN):
        raise SystemExit("missing tracegen (make tracegen)")
    if source == "iverilog" and not os.path.exists(TB_VVP):
        raise SystemExit("missing tb_z80.vvp (make iverilog)")
    if source == "verilator" and not os.path.exists(VERILATOR_SIM):
        raise SystemExit("missing sim_z80 (make verilator)")
    if source == "netlist" and not os.path.exists(TB_NETLIST_VVP):
        raise SystemExit("missing tb_z80_netlist.vvp (make iverilog_netlist)")
    if not os.path.exists(PZ80):     raise SystemExit("missing perfectz80_runner")
    phases = int(argv[0]) if argv else 200
    rest = argv[1:]
    if rest:
        progs = rest
    else:
        hand = [os.path.join(ROOT, f"tests/traces/{n}.hex") for n in (
                    "prog1","prog2","prog3_cb","prog4_ed",
                    "prog5_ddfd","prog6_block","prog7_ddcb","prog8_nmi")]
        # Random programs from scripts/gen_random_trace_progs.py. Sorted
        # so the seed order is deterministic in the report.
        import glob
        rnd = sorted(glob.glob(os.path.join(ROOT, "tests/traces/prog_rnd_*.hex")))
        progs = sorted([p for p in (hand + rnd) if os.path.exists(p)])
    bus_mode = "informational" if (COMPARE_BUS and not BUS_STRICT) else \
               ("strict"        if BUS_STRICT else "off")
    src_label = {
        "c":         "C model",
        "iverilog":  "iverilog RTL",
        "verilator": "Verilator RTL",
        "netlist":   "gate-level netlist (sky130 / LibreLane)",
    }[source]
    print(f"{src_label} vs perfectz80 gate-level signal-timing comparison "
          f"({phases} phases per program, {len(COMPARED)} ctrl pins; "
          f"bus addr+data {bus_mode})")
    ok = True
    for p in progs:
        if not compare(p, phases, source=source): ok = False
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
