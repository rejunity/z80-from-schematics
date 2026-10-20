#!/usr/bin/env python3
# ===========================================================================
# phase_identity.py - prove the work tree's C model phase-identical to a
# reference revision: every pin of every phase (the 14 trace columns of
# compare_traces.py plus data_drive) and the register file at checkpoints.
#
#   phase_identity.py [--ref REV] [--long] [--jobs N] [--only SUBSTR]
#
# Builds cmodel/ of REV (default HEAD) into build/phase_ref/<rev>/ and runs
# scripts/pinhash.c against both models on each scenario below, diffing the
# checkpoint lines (a rolling hash of all pins, printed every 1M phases).
# The work tree is run through each bus front end it offers: z80_sys_phase
# (sys) or the harness' bus model around z80_phase_step (own), and the pin
# word API z80_tick (tick) when cmodel/z80.h has it.
#
# Scenarios: ZEXDOC, prelim, the three z80test variants, every trace program
# (tests/traces, with compare_traces.py's NMI pulse) and pin scenario (with
# its .events), random memory soups, a random byte stream on the data bus
# and ZEXDOC, all three under random INT / NMI / WAIT / BUSREQ / RESET
# activity with random interrupt-acknowledge bytes.
# --long runs ZEXDOC and z80test to completion and longer soups.
# ===========================================================================
import argparse, concurrent.futures, glob, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = os.environ.get("CC", "cc")
CFLAGS = os.environ.get("PHASE_CFLAGS", "-std=c99 -O2 -w").split()


def sh(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if p.returncode != 0:
        sys.stderr.write(p.stdout + p.stderr)
        raise SystemExit(f"command failed: {' '.join(cmd)}")
    return p.stdout


def build(src_dir, out_dir):
    """Compile src_dir/*.c (the model) + scripts/pinhash.c into out_dir/pinhash."""
    os.makedirs(out_dir, exist_ok=True)
    srcs = sorted(glob.glob(os.path.join(src_dir, "*.c")))
    srcs = [s for s in srcs if not s.endswith("z80_main.c")]
    exe = os.path.join(out_dir, "pinhash")
    sh([CC] + CFLAGS + ["-I" + src_dir, os.path.join(ROOT, "scripts", "pinhash.c")]
       + srcs + ["-o", exe])
    return exe


def scenarios(long_run):
    t = lambda *p: os.path.join(ROOT, "tests", *p)
    zex_ph = [] if long_run else ["-n", "300000000"]
    z80t_ph = [] if long_run else ["-n", "100000000"]
    soup_ph = "200000000" if long_run else "20000000"
    sc = [("zexdoc", [t("zex", "zexdoc.com")] + zex_ph, False),
          ("prelim", [t("zex", "prelim.com")], False)]
    for v in ("z80doc", "z80memptr", "z80full"):
        sc.append((v, [t("z80test", v + ".tap")] + z80t_ph, False))
    for p in sorted(glob.glob(t("traces", "*.hex"))):
        side = os.path.splitext(p)[0] + ".phases"
        ph = open(side).read().split()[0] if os.path.exists(side) else "400"
        sc.append((os.path.basename(p), [p, "-n", ph], False))
    for p in sorted(glob.glob(t("traces", "pin_scenarios", "*.hex"))):
        sc.append((os.path.basename(p), [p, "-n", "2000"], False))
    for seed in range(1, 5):
        # random memory soup, interrupts / WAIT / BUSREQ / RESET 4x the
        # default rates
        sc.append((f"soup{seed}", [f"rand:{seed}", "-r", f"{seed}:0.25", "-v",
                                   "-n", soup_ph], True))
    for seed in range(5, 9):
        # random bus: the decoder sees a random byte stream
        scale = "0.1" if seed <= 6 else "0.03"
        sc.append((f"randbus{seed}", [f"rand:{seed}", "-b", "-r", f"{seed}:{scale}",
                                      "-v", "-n", soup_ph], True))
    sc.append(("zexdoc+events", [t("zex", "zexdoc.com"), "-r", "77", "-v",
                                 "-n", "50000000"], True))
    sc.append(("zexdoc+wait", [t("zex", "zexdoc.com"), "-r", "78:0.1", "-v",
                               "-n", "50000000"], True))
    return sc


def run(exe, args, mode):
    out = sh([exe, "-q", "-m", mode] + args)
    return [ln for ln in out.splitlines() if not ln.startswith("#")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", default=os.environ.get("PHASE_REF", "HEAD"))
    ap.add_argument("--long", action="store_true")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--only", default="")
    ap.add_argument("--new-exe", default="",
                    help="the work tree's pinhash as built by make (default: build one)")
    a = ap.parse_args()

    rev = sh(["git", "-C", ROOT, "rev-parse", "--short", a.ref]).strip()
    ref_dir = os.path.join(ROOT, "build", "phase_ref", rev)
    src = os.path.join(ref_dir, "cmodel")
    if os.path.exists(src):
        shutil.rmtree(src)
    os.makedirs(ref_dir, exist_ok=True)
    arch = subprocess.run(["git", "-C", ROOT, "archive", rev, "cmodel"],
                          capture_output=True, check=True).stdout
    subprocess.run(["tar", "-x", "-C", ref_dir], input=arch, check=True)
    ref_exe = build(src, ref_dir)
    new_exe = a.new_exe or build(os.path.join(ROOT, "cmodel"),
                                 os.path.join(ROOT, "build", "phase_new"))
    has_tick = "Z80_PIN_M1" in open(os.path.join(ROOT, "cmodel", "z80.h")).read()
    print(f"phase identity: work tree vs {a.ref} ({rev}); front ends: "
          f"{'sys/own, tick' if has_tick else 'sys/own'}")

    jobs = []
    for name, args, own in scenarios(a.long):
        if a.only and a.only not in name:
            continue
        base = "own" if own else "sys"
        jobs.append((name, args, [("ref", ref_exe, base), ("new", new_exe, base)]
                     + ([("tick", new_exe, "tick")] if has_tick else [])))

    ok = True
    total = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = {}
        for name, args, runs in jobs:
            for tag, exe, mode in runs:
                futs[(name, tag)] = ex.submit(run, exe, args, mode)
        for name, args, runs in jobs:
            ref = futs[(name, "ref")].result()
            phases = int(ref[-1].split()[1])
            same = True
            for tag, _, mode in runs[1:]:
                got = futs[(name, tag)].result()
                if got == ref:
                    total += phases
                    continue
                ok = same = False
                bad = next((i for i, (x, y) in enumerate(zip(ref, got)) if x != y),
                           min(len(ref), len(got)))
                print(f"  {name} [{mode}]: FAIL at checkpoint {bad}")
                print(f"    ref: {ref[bad] if bad < len(ref) else '(end)'}")
                print(f"    new: {got[bad] if bad < len(got) else '(end)'}")
            print(f"  {name}: {'PASS' if same else 'FAIL'} ({phases} phases, "
                  f"{len(ref)} checkpoints)", flush=True)
    print(f"{'PASS' if ok else 'FAIL'}: {total} phases compared")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
