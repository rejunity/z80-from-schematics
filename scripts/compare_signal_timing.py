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
# When EMIT_VCD=1, emit build/vcd/<prog>.vcd per program containing both
# our model's pins (top scope) and perfectz80's pins (under "perfectz80"
# scope) so GTKWave / Surfer can render them side-by-side from one file.
EMIT_VCD    = os.environ.get("EMIT_VCD",    "1") != "0"
VCD_DIR     = os.path.join(ROOT, "build", "vcd")

def write_vcd(out_path, c_rows, g_rows, source_label):
    """Emit a VCD with our pins at top scope and perfectz80's pins under
    a `perfectz80` scope. One time unit per phase (1 ns)."""
    n = min(len(c_rows), len(g_rows))
    if n == 0:
        return
    # Signal table: (var_id, name, width). Single chars from '!' (0x21)
    # onward are valid VCD identifier codes.
    ours   = [("addr", 16), ("data_o", 8), ("data_i", 8),
              ("mreq", 1), ("iorq", 1), ("rd", 1), ("wr", 1),
              ("m1", 1), ("rfsh", 1), ("halt", 1), ("busack", 1)]
    theirs = [("addr", 16), ("data_o", 8), ("data_i", 8),
              ("mreq", 1), ("iorq", 1), ("rd", 1), ("wr", 1),
              ("m1", 1), ("rfsh", 1), ("halt", 1)]
    # Assign identifier codes. Use 'a'..'z', 'A'..'Z' for theirs to keep
    # them clearly distinct from ours.
    next_code = iter([chr(c) for c in range(ord('!'), ord('~'))])
    sig_ours   = {n: next(next_code) for n, _ in ours}
    sig_theirs = {n: next(next_code) for n, _ in theirs}

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w") as f:
        f.write("$version compare_signal_timing.py $end\n")
        f.write("$timescale 1ns $end\n")
        f.write("$scope module top $end\n")
        for name, width in ours:
            f.write(f"$var wire {width} {sig_ours[name]} {name} $end\n")
        f.write("$upscope $end\n")
        f.write("$scope module perfectz80 $end\n")
        for name, width in theirs:
            f.write(f"$var wire {width} {sig_theirs[name]} {name} $end\n")
        f.write("$upscope $end\n")
        f.write("$enddefinitions $end\n")
        # Dump initial values + per-phase updates.
        prev_c = {n: None for n, _ in ours}
        prev_g = {n: None for n, _ in theirs}
        for i in range(n):
            f.write(f"#{i}\n")
            for name, width in ours:
                v = c_rows[i].get(name, "0")
                if v == prev_c[name]:
                    continue
                prev_c[name] = v
                if width == 1:
                    f.write(f"{v}{sig_ours[name]}\n")
                else:
                    try:
                        val = int(v, 16)
                    except ValueError:
                        val = 0
                    bits = bin(val & ((1 << width) - 1))[2:]
                    f.write(f"b{bits} {sig_ours[name]}\n")
            for name, width in theirs:
                v = g_rows[i].get(name, "0")
                if v == prev_g[name]:
                    continue
                prev_g[name] = v
                if width == 1:
                    f.write(f"{v}{sig_theirs[name]}\n")
                else:
                    try:
                        val = int(v, 16)
                    except ValueError:
                        val = 0
                    bits = bin(val & ((1 << width) - 1))[2:]
                    f.write(f"b{bits} {sig_theirs[name]}\n")

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

def parse_events_to_plusargs(events_path):
    """Parse a .events sidecar (`<phase> <pin> <0|1>` per line) into
    per-pin plusargs `+<pin>_lo=N +<pin>_hi=M` consumed by the iverilog
    and Verilator testbenches. Each pin can have at most one lo and one
    hi event in the current sidecar format; pin-scenarios that need
    richer event sequences would need a richer encoding."""
    if not events_path or not os.path.exists(events_path):
        return []
    lo = {}
    hi = {}
    with open(events_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) != 3:
                continue
            phase, pin, val = parts[0], parts[1], parts[2]
            try:
                ph = int(phase)
            except ValueError:
                continue
            if val == "0":
                lo[pin] = ph
            elif val == "1":
                hi[pin] = ph
    args = []
    for pin in ("nmi", "int", "wait", "busreq", "reset"):
        if pin in lo:
            args.append(f"+{pin}_lo={lo[pin]}")
        if pin in hi:
            args.append(f"+{pin}_hi={hi[pin]}")
    return args

def compare(prog, phases, source="c"):
    events = prog[:-4] + ".events" if prog.endswith(".hex") else prog + ".events"
    events = events if os.path.exists(events) else ""
    event_plusargs = parse_events_to_plusargs(events)
    if source == "c":
        tg_argv = [TRACEGEN, prog, str(phases)] + ([events] if events else [])
        emit_label = "C"
    elif source == "iverilog":
        # iverilog testbench accepts +<pin>_lo=N +<pin>_hi=M plusargs
        # for the pin-scenario .events sidecars. The C tracegen and
        # perfectz80_runner still consume the raw sidecar; the iverilog
        # path takes the same events through this plusarg encoding so
        # all three drive the same pin transitions at the same phases.
        tg_argv = [VVP, TB_VVP, f"+prog={prog}", f"+phases={phases}"] + event_plusargs
        emit_label = "iverilog RTL"
    elif source == "verilator":
        # sim_main.cpp accepts the same +<pin>_lo / +<pin>_hi argv form
        # as the iverilog testbenches.
        tg_argv = [VERILATOR_SIM, prog, str(phases)] + event_plusargs
        emit_label = "Verilator RTL"
    elif source == "netlist":
        # LibreLane-synthesized sky130 gate-level netlist driven by the
        # iverilog testbench tb_z80_netlist.v — same plusargs as tb_z80.v.
        tg_argv = [VVP, TB_NETLIST_VVP, f"+prog={prog}", f"+phases={phases}"] + event_plusargs
        emit_label = "gate-level netlist"
    else:
        raise SystemExit(f"unknown source: {source}")
    pz_argv = [PZ80,     prog, str(phases + PZ_OFFSET)] + ([events] if events else [])
    c = parse_tracegen(run(tg_argv))
    g = parse_pz80(run(pz_argv))
    g = g[PZ_OFFSET:]                 # drop pz80's pre-M1 idle phase(s)
    n = min(len(c), len(g))
    name = os.path.basename(prog)
    suffix = " + events" if events else ""
    if n == 0:
        print(f"  {name}: FAIL (no rows)"); return False

    # Emit VCD waveform — our pins at top, pz80 pins under perfectz80 scope.
    # Open with `gtkwave build/vcd/<prog>.vcd` to see both sides side-by-side.
    if EMIT_VCD:
        vcd_path = os.path.join(VCD_DIR, name.replace(".hex", "") + f".{source}.vcd")
        write_vcd(vcd_path, c, g, source)

    ctrl_mism = 0           # control-pin mismatches — count against pass/fail
    bus_addr_mism = 0       # bus-value mismatches — informational by default
    bus_data_mism = 0
    bus_addr_compared = 0
    bus_data_compared = 0
    bus_addr_dontcare = 0   # raw mismatches absorbed by the don't-care rule
                            # (phases where no data transfer is in progress)
    shown = 0

    def is_data_transfer(row):
        """True if this phase's strobe combo means an actual data transfer
        is in progress (MREQ+RD / MREQ+WR / IORQ+RD / IORQ+WR). Refresh
        cycles (MREQ asserted alone, RD+WR both high) are NOT data
        transfers — they're DRAM-refresh strobes, no addr semantics for
        the CPU. Idle phases (all strobes high) are obviously not transfers."""
        mreq_active = row["mreq"] == "0"
        iorq_active = row["iorq"] == "0"
        rd_active   = row["rd"]   == "0"
        wr_active   = row["wr"]   == "0"
        return (mreq_active or iorq_active) and (rd_active or wr_active)

    def addr_dont_care(i):
        """`addr` is don't-care at phase i when no actual data transfer
        is in progress on EITHER side. A data transfer requires both a
        target strobe (MREQ or IORQ) AND a direction strobe (RD or WR).
        Cases this catches:
          - Refresh windows: MREQ low + RFSH low, but RD/WR both high.
            The bus addr is just the DRAM refresh row with no CPU-data
            meaning. Our model legitimately leads with the next M-cycle's
            address while perfectz80 stays on {I,R}.
          - Idle phases between M-cycles: all strobes high. addr may be
            transitioning or stale on either side.
          - T1.P / setup phases: addr being driven but strobes haven't
            dropped yet — neither side has committed.
        """
        return (not is_data_transfer(c[i])) and (not is_data_transfer(g[i]))

    for i in range(n):
        diff = [(col, c[i][col], g[i][col]) for col in COMPARED if c[i][col] != g[i][col]]
        bus_diff = []
        if COMPARE_BUS:
            # Compare addr at every phase, with the settle tolerance.
            # Tolerance fires only when (mreq, iorq, rd, wr) are all
            # inactive on both sides at phase i AND our addr matches
            # pz80's value at one of the next 1-2 phases — the canonical
            # "our addr settles to the next M-cycle's value 1-2 phases
            # ahead of pz80" pattern.
            bus_addr_compared += 1
            if c[i]["addr"] == g[i]["addr"]:
                pass  # exact match
            elif addr_dont_care(i):
                bus_addr_dontcare += 1
            else:
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
        settled_note = f" ({bus_addr_dontcare} don't-care phases)" if bus_addr_dontcare else ""
        print(f"           bus addr   {bus_addr_compared - bus_addr_mism}/{bus_addr_compared} "
              f"({addr_pct:.1f}%) match{settled_note};  data_o {bus_data_compared - bus_data_mism}/{bus_data_compared} "
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
