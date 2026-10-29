#!/usr/bin/env python3
# compare_signal_timing.py - diff per-half-cycle pin signals between our
# emitter of choice (C tracegen, iverilog RTL, or Verilator RTL) and the
# perfectz80 gate-level netlist simulator (build/bin/perfectz80_runner).
#
# Compares, in every phase, the 7 CPU-driven control pins (mreq/iorq/rd/wr/
# m1/rfsh/halt) and the address -- transfers, refreshes (I:R), and the
# internal and idle T-states, where the bus keeps its last address -- and
# the data the CPU drives wherever WR is low on both sides. Any difference
# fails the program.
#
# If a `<prog>.events` sidecar exists alongside the .hex, every harness
# applies its per-phase pin events: the C tracegen and perfectz80_runner
# read the file, the RTL testbenches (iverilog, Verilator, the netlist)
# take it as per-pin +<pin>_lo=N +<pin>_hi=M plusargs. A `<prog>.machine`
# sidecar puts the logic of a machine around the CPU -- the ZX Spectrum
# 48K's ULA holding CLK high through contended accesses, the ZX80 /
# ZX81's display tricks and NMI / WAIT generators -- into every harness
# but Verilator's (scripts/perfectz80_runner.c describes the model; the
# iverilog testbenches take it as +machine / +mt0 / +mnmi).
#
# perfectz80's side is recorded: tests/traces/pz80/<prog>.txt holds
# perfectz80_runner's output for the program, its .events and the phase
# count, under a header naming them (sha1s), the harness (perfectz80, its
# simulator and the runner, hashed) and the command that wrote it. A run
# takes the recording when the header matches and runs perfectz80 live
# otherwise; --recorded never runs it and fails on a missing or stale
# recording (CI, and repositories without perfectz80 or a C compiler).
#
# Usage:
#   scripts/compare_signal_timing.py [mode] [--rtl=SRC] [phases] [prog...]
#     --rtl=c (default) | iverilog | verilator | netlist
#     mode: (none)  the recording if it matches, else perfectz80 live
#           --recorded         recordings only (fails on a missing or stale one)
#           --live             perfectz80 live, recordings ignored
#           --record           perfectz80 live, written to tests/traces/pz80/
#                              (no model runs)
#           --verify-recorded  perfectz80 live, which must print every
#                              recording byte for byte (no model runs)
#
# --rtl=netlist is the LibreLane gate-level path: feeds the synthesized
# sky130 netlist (build/tb_z80_netlist.vvp) through iverilog and diffs
# against perfectz80. See docs/librelane-flow.md.
#
# PZ80_TRACES (the recordings' directory) and TB_VVP / TB_NETLIST_VVP (the
# testbench builds) in the environment point elsewhere, for a repository
# that carries copies of them.
import hashlib, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACEGEN = os.path.join(ROOT, "build", "bin", "tracegen")
PZ80     = os.path.join(ROOT, "build", "bin", "perfectz80_runner")
VVP      = "vvp"
TB_VVP   = os.environ.get("TB_VVP", os.path.join(ROOT, "build", "tb_z80.vvp"))
# TB_NETLIST_VVP in the environment selects another netlist build (the
# FPGA-mapped ones from `make luts_netlist`).
TB_NETLIST_VVP = os.environ.get("TB_NETLIST_VVP",
                                os.path.join(ROOT, "build", "tb_z80_netlist.vvp"))
VERILATOR_SIM = os.path.join(ROOT, "build", "obj_dir", "sim_z80")
# perfectz80's recorded traces (--record writes them)
RECORDED = os.environ.get("PZ80_TRACES", os.path.join(ROOT, "tests", "traces", "pz80"))
# Bumped when a change to how this script runs perfectz80_runner changes
# what it prints for a program it ran before (the runner's own source, the
# netlist and the simulator are hashed into the harness id anyway).
TRACE_HARNESS_VERSION = 1

# tracegen cols: t phi m addr data_o data_i mreq iorq rd wr m1 rfsh halt busack
# pz80 cols:    phase addr data_o data_i mreq iorq rd wr m1 rfsh halt
#
# perfectz80 starts one phase before our tracegen "T1.P" (its phase 0 = pre-M1
# idle); align with PZ_OFFSET=1.
PZ_OFFSET = 1

# The control pins compared in every phase, with the address and the data
# the CPU drives under WR.
COMPARED  = ["mreq","iorq","rd","wr","m1","rfsh","halt"]
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

MACHINES = {"spectrum48": 1, "zx81": 2}

def machine_of(prog):
    m = os.path.splitext(prog)[0] + ".machine"
    return m if os.path.exists(m) else ""

def parse_machine_to_plusargs(machine):
    """<prog>.machine -> the iverilog testbenches' plusargs"""
    if not machine:
        return []
    for line in open(machine):
        f = line.split()
        if not f or f[0].startswith("#"):
            continue
        if f[0] == "spectrum48" and len(f) >= 2:
            return ["+machine=1", f"+mt0={int(f[1])}"]
        if f[0] == "zx81" and len(f) >= 3:
            return ["+machine=2", f"+mnmi={int(f[1])}", f"+mt0={int(f[2])}"]
        break
    raise SystemExit(f"{machine}: not a machine line (spectrum48 <t> | zx81 <nmi> <t>)")

def side_args(events, machine):
    """the C harnesses' optional arguments: [events|-] [machine]"""
    if machine:
        return [events or "-", machine]
    return [events] if events else []

def hex_for_readmemh(prog):
    """$readmemh (iverilog) accepts only // and /* */ comments, but the
    trace programs use '#' / ';' comment lines (which tracegen.c,
    perfectz80_runner.c and sim_main.cpp all skip). Hand the iverilog
    testbenches a comment-stripped copy under build/ so the pin-scenario
    programs load instead of silently running an empty memory."""
    out_dir = os.path.join(ROOT, "build", "hex_readmemh")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, os.path.basename(prog))
    with open(prog) as f, open(out, "w") as g:
        for line in f:
            # whole-line AND trailing comments ("ED 56   # IM 1"): cut at
            # the first comment introducer, exactly like tracegen.c's
            # token loader (which stops reading the line at a #, ; or /)
            cut = len(line)
            for intro in ("#", ";", "//"):
                i = line.find(intro)
                if i >= 0:
                    cut = min(cut, i)
            body = line[:cut].strip()
            if body:
                g.write(body + "\n")
    return out

def harness_id():
    """perfectz80 -- its netlist, its simulator -- and perfectz80_runner,
    hashed: a recorded trace holds while they are unchanged. None in a
    repository that has the recordings but not perfectz80 (the header is
    then taken as it is: the repository it was copied from checks it)."""
    runner = os.path.join(ROOT, "scripts", "perfectz80_runner.c")
    refs = os.path.join(ROOT, "scripts", "refs", "perfectz80")
    if not (os.path.exists(runner) and os.path.isdir(refs)):
        return None
    h = hashlib.sha1(f"perfectz80_runner v{TRACE_HARNESS_VERSION}\n".encode())
    for f in [runner] + sorted(os.path.join(refs, f) for f in os.listdir(refs)
                               if f.endswith((".c", ".h"))):
        h.update(open(f, "rb").read())
    return h.hexdigest()[:16]

def sha1_of(path):
    return hashlib.sha1(open(path, "rb").read()).hexdigest() if path else "none"

def recording_path(prog):
    return os.path.join(RECORDED, os.path.splitext(os.path.basename(prog))[0] + ".txt")

def recording_wants(prog, events, pz_phases, hid):
    """the header a recording of this run must carry"""
    want = {"program-sha1": sha1_of(prog), "events-sha1": sha1_of(events),
            "phases": str(pz_phases)}
    if machine_of(prog):
        want["machine-sha1"] = sha1_of(machine_of(prog))
    if hid is not None:
        want["harness"] = hid
    return want

def read_recording(prog, events, pz_phases, hid):
    """(perfectz80's recorded output, None), or (None, why there is none)"""
    path = recording_path(prog)
    rel = os.path.relpath(path, ROOT)
    if not os.path.exists(path):
        return None, f"no recording ({rel})"
    head, body = {}, []
    for ln in open(path):
        if ln.startswith("#"):
            m = re.match(r"#\s*([a-z0-9-]+):\s*(.*)", ln)
            if m:
                head[m.group(1)] = m.group(2).strip()
        else:
            body.append(ln)
    for k, v in recording_wants(prog, events, pz_phases, hid).items():
        if head.get(k) != v:
            return None, (f"{rel} is stale: its {k} is {head.get(k)}, this run's {v} "
                          f"(make record_pz80_traces)")
    return "".join(body), None

def git_head():
    try:
        return subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True).stdout.strip() or "unknown"
    except OSError:
        return "unknown"

def write_recording(prog, events, pz_phases, hid, text, command):
    path = recording_path(prog)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    head = [f"# perfectz80's trace of {os.path.relpath(prog, ROOT)}"
            + (f" with {os.path.relpath(events, ROOT)}" if events else "")
            + (f" in {os.path.relpath(machine_of(prog), ROOT)}" if machine_of(prog) else "")
            + ": perfectz80_runner's output (phase addr data_o data_i mreq iorq rd wr m1 rfsh halt)",
            f"# generator: {command}",
            f"# rz80: {git_head()}"]
    head += [f"# {k}: {v}" for k, v in recording_wants(prog, events, pz_phases, hid).items()]
    with open(path, "w") as f:
        f.write("\n".join(head) + "\n" + text)
    return path

def events_of(prog):
    events = prog[:-4] + ".events" if prog.endswith(".hex") else prog + ".events"
    return events if os.path.exists(events) else ""

def pz80_live(prog, phases, events):
    return run([PZ80, prog, str(phases + PZ_OFFSET)] + side_args(events, machine_of(prog)))

def compare(prog, phases, source="c", mode="", hid=None):
    events = events_of(prog)
    machine = machine_of(prog)
    event_plusargs = parse_events_to_plusargs(events) + parse_machine_to_plusargs(machine)
    if source == "c":
        tg_argv = [TRACEGEN, prog, str(phases)] + side_args(events, machine)
        emit_label = "C"
    elif source == "iverilog":
        # iverilog testbench accepts +<pin>_lo=N +<pin>_hi=M plusargs
        # for the pin-scenario .events sidecars. The C tracegen and
        # perfectz80_runner still consume the raw sidecar; the iverilog
        # path takes the same events through this plusarg encoding so
        # all three drive the same pin transitions at the same phases.
        tg_argv = [VVP, TB_VVP, f"+prog={hex_for_readmemh(prog)}", f"+phases={phases}"] + event_plusargs
        emit_label = "iverilog RTL"
    elif source == "verilator":
        # sim_main.cpp accepts the same +<pin>_lo / +<pin>_hi argv form
        # as the iverilog testbenches; it has no machine models.
        if machine:
            print(f"  {os.path.basename(prog)}: FAIL: the Verilator harness has no machine ({os.path.basename(machine)})")
            return False
        tg_argv = [VERILATOR_SIM, prog, str(phases)] + event_plusargs
        emit_label = "Verilator RTL"
    elif source == "netlist":
        # LibreLane-synthesized sky130 gate-level netlist driven by the
        # iverilog testbench tb_z80_netlist.v — same plusargs as tb_z80.v.
        tg_argv = [VVP, TB_NETLIST_VVP, f"+prog={hex_for_readmemh(prog)}", f"+phases={phases}"] + event_plusargs
        emit_label = "gate-level netlist"
    else:
        raise SystemExit(f"unknown source: {source}")
    name = os.path.basename(prog)
    text, why = (None, "--live") if mode == "live" else read_recording(prog, events, phases + PZ_OFFSET, hid)
    if text is None:
        if mode == "recorded":
            print(f"  {name}: FAIL: {why}"); return False
        if not os.path.exists(PZ80):
            print(f"  {name}: FAIL: {why}, and no perfectz80_runner to run live"); return False
        if mode != "live":
            print(f"  {name}: perfectz80 live: {why}")
        text = pz80_live(prog, phases, events)
    c = parse_tracegen(run(tg_argv))
    g = parse_pz80(text)
    g = g[PZ_OFFSET:]                 # drop pz80's pre-M1 idle phase(s)
    n = min(len(c), len(g))
    suffix = (" + events" if events else "") + (" + machine" if machine else "")
    if n == 0 or len(c) != len(g):
        print(f"  {name}: FAIL ({len(c)} phases from {source}, {len(g)} from perfectz80)"); return False

    # Emit VCD waveform — our pins at top, pz80 pins under perfectz80 scope.
    # Open with `gtkwave build/vcd/<prog>.vcd` to see both sides side-by-side.
    if EMIT_VCD:
        vcd_path = os.path.join(VCD_DIR, name.replace(".hex", "") + f".{source}.vcd")
        write_vcd(vcd_path, c, g, source)

    mism = 0                # phases with any difference
    data_compared = 0       # phases where both sides drive WR low
    shown = 0
    for i in range(n):
        diff = [(col, c[i][col], g[i][col]) for col in COMPARED if c[i][col] != g[i][col]]
        if c[i]["addr"] != g[i]["addr"]:
            diff.append(("addr", c[i]["addr"], g[i]["addr"]))
        if c[i]["wr"] == "0" and g[i]["wr"] == "0":
            data_compared += 1
            if c[i]["data_o"] != g[i]["data_o"]:
                diff.append(("data_o", c[i]["data_o"], g[i]["data_o"]))
        if diff:
            mism += 1
            if shown < 5:
                shown += 1
                d = " ".join(f"{c1}: C={v1} pz80={v2}" for c1, v1, v2 in diff)
                print(f"  {name} phase {i} differ:  {d}")
    is_ok = mism == 0
    head  = "PASS" if is_ok else f"{mism}/{n} phases differ"
    print(f"  {name}: {head}{suffix} ({n} phases, data under WR in {data_compared})")
    return is_ok

def record_or_verify(mode, phases, progs):
    """--record / --verify-recorded: perfectz80 alone, live"""
    if not os.path.exists(PZ80):
        raise SystemExit("missing perfectz80_runner (make build/bin/perfectz80_runner)")
    hid = harness_id()
    command = "make record_pz80_traces (scripts/compare_signal_timing.py --record)"
    bad = 0
    for p in progs:
        ph = program_phases(p, phases)
        events = events_of(p)
        text = pz80_live(p, ph, events)
        name = os.path.basename(p)
        if mode == "record":
            path = write_recording(p, events, ph + PZ_OFFSET, hid, text, command)
            print(f"  {name}: recorded {os.path.relpath(path, ROOT)} ({text.count(chr(10))} lines)")
            continue
        rec, why = read_recording(p, events, ph + PZ_OFFSET, hid)
        if rec is None:
            bad += 1; print(f"::error::{name}: {why}")
        elif rec != text:
            bad += 1
            print(f"::error::{name}: perfectz80 now prints something else than "
                  f"{os.path.relpath(recording_path(p), ROOT)} (make record_pz80_traces)")
        else:
            print(f"  {name}: perfectz80 live = its recording")
    if mode == "verify":
        print(f"{len(progs) - bad} of {len(progs)} recordings identical to perfectz80 live")
    return 1 if bad else 0

def program_phases(p, phases):
    # a program may ask for a longer run in <prog>.phases (one number)
    side = os.path.splitext(p)[0] + ".phases"
    if os.path.exists(side):
        return max(phases, int(open(side).read().split()[0]))
    return phases

def main():
    # Parse the mode and --rtl=<source> before positional args (keeps
    # existing callers — they pass no flag — backward-compatible).
    argv = sys.argv[1:]
    source, mode = "c", ""
    modes = {"--recorded": "recorded", "--live": "live", "--record": "record",
             "--verify-recorded": "verify"}
    while argv and (argv[0] in modes or argv[0].startswith("--rtl")):
        a = argv.pop(0)
        if a in modes:
            mode = modes[a]
        elif a.startswith("--rtl="):
            source = a.split("=", 1)[1]
        else:
            source = "iverilog" if not argv else argv.pop(0)
    if source not in ("c", "iverilog", "verilator", "netlist"):
        raise SystemExit(f"unknown --rtl source: {source}")
    phases = int(argv[0]) if argv else 200
    rest = argv[1:]
    if rest:
        progs = rest
    else:
        # every trace program: the hand-written ones and the random ones
        # from scripts/gen_random_trace_progs.py
        import glob
        progs = sorted(glob.glob(os.path.join(ROOT, "tests/traces/*.hex")))
    if mode in ("record", "verify"):
        return record_or_verify(mode, phases, progs)
    if source == "c"        and not os.path.exists(TRACEGEN):
        raise SystemExit("missing tracegen (make tracegen)")
    if source == "iverilog" and not os.path.exists(TB_VVP):
        raise SystemExit("missing tb_z80.vvp (make iverilog)")
    if source == "verilator" and not os.path.exists(VERILATOR_SIM):
        raise SystemExit("missing sim_z80 (make verilator)")
    if source == "netlist" and not os.path.exists(TB_NETLIST_VVP):
        raise SystemExit("missing tb_z80_netlist.vvp (make iverilog_netlist)")
    if mode == "live" and not os.path.exists(PZ80):
        raise SystemExit("missing perfectz80_runner")
    src_label = {
        "c":         "C model",
        "iverilog":  "iverilog RTL",
        "verilator": "Verilator RTL",
        "netlist":   "gate-level netlist (" + os.environ.get("NETLIST_LABEL", "sky130 / LibreLane") + ")",
    }[source]
    pz_label = {"recorded": "perfectz80's recorded traces", "live": "perfectz80 live"}.get(
        mode, "perfectz80 (recorded, live where a recording is missing)")
    print(f"{src_label} vs {pz_label}: gate-level signal-timing comparison "
          f"({phases} phases per program: {len(COMPARED)} control pins and the address "
          f"in every phase, the data under WR)")
    hid = harness_id()
    ok = True
    for p in progs:
        if not compare(p, program_phases(p, phases), source=source, mode=mode, hid=hid): ok = False
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
