#!/usr/bin/env python3
"""test_chip.py - chip.py's simulate step, without the chip: a stand-in for
ngspice that writes what the real one wrote, and the deck chip.py hands it.

ws3's Transistor run 40864340281 (the chip of ws3 90bfb9d) went wrong this
way at tt and ff: ngspice gave up 2.6 ns into the power-up,

  doAnalyses: TRAN:  Timestep too small; time = 2.62724e-09, timestep =
  1.25e-20: trouble with diode_pd2nw_06v0-instance d.xchip.xgf180mcu_fd_io__
  bi_t_5.xgf_ni_bi_t_base_0.xcomp018green_esd_cdm_0.d1
  tran simulation(s) aborted

but an aborted transient still writes the points it reached (17 rows) and
wrnodev's state, so chip.py took the window for done and the next window
started from the state at 2.6 ns. Checked here:

  aborted    the recorded abort fails the window, naming ngspice's reason
  truncated  data that stops before the window's end fails it, message or not
  complete   a window that reaches its end passes, and leaves its end state
  ramp       the supply's 20 ns ramp has breakpoints at most 0.25 ns apart,
             as the clock's edges have (the abort came after ngspice had
             doubled its steps from 1 ps to 1.3 ns there)
  span       --span FROM:TO simulates that stretch (the nightly slice,
             0:40, power-up) as a window: its own deck, pins and end state
  xyce       the xyce step runs Xyce (engines.run_xyce, stood in for) on
             the window's deck, from the state ngspice started it from,
             and fails on what engines.window_failures finds against
             ngspice's pins, or on Xyce's own failure; passes otherwise,
             and leaves its result beside the window's pins

Needs iverilog (chip.py's reference: the RTL's pins at every clock edge);
no ngspice, Magic or PDK.

usage: test_chip.py
"""
import os, re, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import chip  # noqa: E402

# the stand-in: reads the deck's tran / wrdata / wrnodev, writes the window's
# pins to its end (complete), to 2.62724 ns with the recorded messages
# (aborted), or to half of it with none (truncated)
FAKE = r'''#!/usr/bin/env python3
import os, re, sys
deck = open(sys.argv[-1]).read()
tstop = float(re.search(r"^tran \S+ ([0-9.]+)n", deck, re.M).group(1)) * 1e-9
dat, *save = re.search(r"^wrdata (\S+) (.*)$", deck, re.M).group(1, 2)
state = re.search(r"^wrnodev (\S+)$", deck, re.M)
mode = os.environ["FAKE_NGSPICE"]
end = {"complete": tstop, "aborted": 2.62724e-9, "truncated": tstop / 2}[mode]
ts = [1e-12 * 2 ** k for k in range(12) if 1e-12 * 2 ** k < end] + [end]
cols = save[0].split()
with open(dat, "w") as f:
    f.write(" time " + " ".join(cols) + "\n")
    for t in ts:
        f.write(f" {t:.7e} " + " ".join("0.0000000e+00" for _ in cols) + "\n")
print(f"No. of Data Rows : {len(ts)}")
if state:
    open(state.group(1), "w").write(".ic v(p_clk)=0\n")
print("ngspice-45 done")
if mode == "aborted":
    sys.stderr.write("doAnalyses: TRAN:  Timestep too small; time = 2.62724e-09, timestep = 1.25e-20: "
                     "trouble with diode_pd2nw_06v0-instance d.xchip.xgf180mcu_fd_io__bi_t_5."
                     "xgf_ni_bi_t_base_0.xcomp018green_esd_cdm_0.d1\n\ntran simulation(s) aborted\n")
'''


def window(work, edges, mode):
    """simulate window 1 of 12 at tt: None, or chip.py's message"""
    os.environ["FAKE_NGSPICE"] = mode
    for f in os.listdir(os.path.join(work, "tt", "seg")) if os.path.isdir(os.path.join(work, "tt", "seg")) else []:
        os.remove(os.path.join(work, "tt", "seg", f))
    try:
        chip.simulate(work, "tt", edges, 1, (1, 12))
    except SystemExit as e:
        return str(e.code)
    return None


def main():
    fails = []
    with tempfile.TemporaryDirectory() as work:
        fake = os.path.join(work, "ngspice")
        open(fake, "w").write(FAKE)
        os.chmod(fake, 0o755)
        chip.tool = lambda name: fake
        # the extracted chip, as a subcircuit with z80_chip's ports and nothing in it
        open(os.path.join(work, "z80_chip.spice"), "w").write(
            ".subckt z80_chip " + " ".join(chip.PINS) + "\n.ends\n")
        edges = chip.edges_of(chip.reference(work))
        t1 = chip.windows(edges, 12)[0][1]

        msg = window(work, edges, "aborted")
        ok = msg is not None and "Timestep too small" in msg
        print(f"aborted:   {'ok' if ok else 'FAIL'}  ({msg or 'window 1 passed'})")
        ok or fails.append("aborted")

        msg = window(work, edges, "truncated")
        print(f"truncated: {'ok' if msg else 'FAIL'}  ({msg or 'window 1 passed'})")
        msg or fails.append("truncated")

        msg = window(work, edges, "complete")
        ok = msg is None and os.path.exists(chip.state_file(work, "tt", t1))
        print(f"complete:  {'ok' if ok else 'FAIL'}  ({msg or 'window 1 passed, end state written'})")
        ok or fails.append("complete")

        # xyce: engines.run_xyce and engines.compare stood in for
        import engines
        calls = []
        good = {"edges": 8, "edges_missing": 0, "max_edge_shift_ns": 0.009, "max_settled_dV": 0.004,
                "mismatches_ref": 0, "mismatches_other": 0}

        def xyce(run_rc, cmp):
            def run_xyce(sp, out_dir, *a, **kw):
                calls.append(sp)
                os.makedirs(out_dir, exist_ok=True)
                dat = os.path.join(out_dir, "chip_0.dat")
                open(dat, "w").write(open(chip.segment_file(work, "tt", 0.0)).read())
                return {"engine": "Xyce MPI x4", "rc": run_rc, "wall_s": 1.0, "dat": dat}
            engines.run_xyce = run_xyce
            engines.compare = lambda ref, dat, ref_txt, t_from=0.0: dict(cmp)
            try:
                chip.xyce_window(work, "tt", edges, 0.0, t1, 1)
            except SystemExit as e:
                return str(e.code)
            return None

        msg = xyce(0, good)
        res = os.path.join(work, "tt", "seg", "xyce_0", "result.json")
        ok = msg is None and calls and calls[-1].endswith("chip_0.sp") and os.path.exists(res)
        print(f"xyce:      {'ok' if ok else 'FAIL'}  ({msg or 'as ngspice: passed, ' + os.path.basename(res)})")
        ok or fails.append("xyce")
        for what, rc, cmp in (("an edge missing", 0, dict(good, edges_missing=1)),
                              ("an edge 1 ns off", 0, dict(good, max_edge_shift_ns=1.0)),
                              ("an RTL mismatch", 0, dict(good, mismatches_other=2)),
                              ("Xyce failed", 1, good)):
            msg = xyce(rc, cmp)
            print(f"xyce:      {'ok' if msg else 'FAIL'}  ({what}: {msg or 'passed'})")
            msg or fails.append(f"xyce ({what})")

        # span: the nightly's slice, 0-40 ns, as a window of its own
        os.environ["FAKE_NGSPICE"] = "complete"
        try:
            chip.simulate(work, "tt", edges, 1, span=(0.0, 40.0))
            msg = None
        except SystemExit as e:
            msg = str(e.code)
        sp0 = open(os.path.join(work, "tt", "seg", "chip_0.sp")).read()
        ok = msg is None and re.search(r"^tran \S+ 40\.000n", sp0, re.M) and os.path.exists(chip.state_file(work, "tt", 40.0))
        print(f"span:      {'ok' if ok else 'FAIL'}  ({msg or '0-40 ns: its deck, pins and end state'})")
        ok or fails.append("span")
        sp = open(chip.deck(work, "tt", edges, 1, t1)).read()
        brk = [float(b) for b in re.findall(r"([0-9.]+)n 0", sp[sp.index("V_brk"):sp.index("R_brk")])]
        ramp = [0.0] + [b for b in brk if b <= 20.0]
        gap = max(b - a for a, b in zip(ramp, ramp[1:]))
        ok = gap <= 0.25 + 1e-9 and ramp[-1] >= 20.0 - 1e-9
        print(f"ramp:      {'ok' if ok else 'FAIL'}  ({len(ramp) - 1} breakpoints in the supply's 20 ns ramp, "
              f"the widest gap {gap:.3f} ns)")
        ok or fails.append("ramp")
    print("test_chip: " + ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
