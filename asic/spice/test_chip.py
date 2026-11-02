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
