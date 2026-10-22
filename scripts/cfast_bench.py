#!/usr/bin/env python3
"""cfast_bench.py - cfast, the reference C model and four other C Z80
emulators on the same ZEXDOC instructions: the best of several interleaved
rounds of each (the process's CPU time), as ZEXDOC instructions a second
and the Z80 clock each would keep. Writes bench.json, bench.md (a Markdown
table) and bench.svg (the same as bars).

  scripts/cfast_bench.py [--bin build/bin] [--prog tests/zex/zexdoc.com]
                         [--instr 50000000] [--rounds 5] [--cpu N]
                         [--out build/bench]

make cfast_bench builds every emulator and runs this (cfast/BENCHMARK.md).
Every emulator must run all the instructions, and their T-states must
agree within 0.5 % (floooh/chips and redcode count a prefix's end their
own way, which moves them by about 0.1 %): else it exits 1."""
import argparse
import datetime
import html
import json
import os
import platform
import re
import subprocess
import sys

# name, binary, what the caller steps through, about it (Markdown)
EMULATORS = [
    ("rz80 cfast, z80f_run", "zexfast", "half clock",
     "the phase engine generated from the C model's timing tables, steps back to back with the bus inlined (`cfast/`)"),
    ("rz80 cfast, z80f_tick", "zexrunner_fast", "half clock",
     "the same steps, one half clock per call, the pins in a word"),
    ("rz80 C model", "zexrunner", "half clock",
     "the reference model the RTL is checked against (`cmodel/`)"),
    ("floooh/chips", "bench_chips", "clock",
     "[floooh/chips](https://github.com/floooh/chips) `z80.h`: `z80_tick`, the pins in a word"),
    ("suzukiplan", "bench_suzukiplan", "instruction",
     "[suzukiplan/z80](https://github.com/suzukiplan/z80): `execute(1)`"),
    ("superzazu", "bench_superzazu", "instruction",
     "[superzazu/z80](https://github.com/superzazu/z80): `z80_step`"),
    ("redcode", "bench_redcode", "instruction",
     "[redcode/Z80](https://github.com/redcode/Z80): `z80_execute(1)`"),
]
REFERENCE = "zexrunner"       # its T-states are the stream every other must match
BASE = "zexfast"              # the column "against cfast"


def run(exe, prog, n, cpu):
    """(CPU seconds, instructions, T-states, final PC) of one run"""
    cmd = (["taskset", "-c", str(cpu)] if cpu is not None else []) + [exe, prog, str(n)]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = p.stdout.read().decode(errors="replace")
    _, status, ru = os.wait4(p.pid, 0)      # taskset execs the emulator: the same process
    p.returncode = os.waitstatus_to_exitcode(status)
    instr = re.search(r"instructions executed: (\d+)", out)
    pc = re.search(r"final PC=([0-9A-F]{4})", out)
    ts = re.search(r"T-states: (\d+)", out)
    ph = re.search(r"(\d+) phases", out)
    if p.returncode not in (0, 3) or not instr or not pc or not (ts or ph) or "LIMIT REACHED" not in out:
        sys.exit(f"cfast_bench: {' '.join(cmd)} failed (exit {p.returncode}):\n{out[-1500:]}")
    t = int(ts.group(1)) if ts else int(ph.group(1)) / 2
    return ru.ru_utime + ru.ru_stime, int(instr.group(1)), t, pc.group(1)


def machine():
    cpu = next((l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")),
               platform.processor() or platform.machine())
    cc = subprocess.run([os.environ.get("CC", "cc"), "--version"], capture_output=True, text=True).stdout.split("\n")[0]
    where = (f"GitHub Actions ({os.environ.get('ImageOS', os.environ.get('RUNNER_OS', ''))})"
             if os.environ.get("GITHUB_ACTIONS") else platform.node())
    git = lambda *a: subprocess.run(["git", *a], capture_output=True, text=True).stdout.strip()
    return {"cpu": cpu, "cores": os.cpu_count(), "compiler": cc, "where": where,
            "commit": git("rev-parse", "--short", "HEAD"),
            "ref": os.environ.get("GITHUB_REF_NAME") or git("rev-parse", "--abbrev-ref", "HEAD"),
            "run": os.environ.get("GITHUB_RUN_ID", ""),
            "date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")}


def markdown(res, m, a):
    base = next(r for r in res if r["binary"] == BASE)
    rows = ["| Emulator | Sees every | ZEXDOC Minstr/s | Z80 clock | Against cfast |",
            "|---|---|---:|---:|---:|"]
    for r in res:
        name = f"**{r['name']}**" if r["binary"] == BASE else r["name"]
        rows.append(f"| {name} | {r['steps']} | {r['minstr']:.1f} | {r['mhz']:.0f} MHz | "
                    f"{r['minstr'] / base['minstr']:.2f}× |")
    rows += ["", f"The first {a.instr:,} instructions of ZEXDOC, the best of {a.rounds} rounds of each "
                 f"emulator's CPU time{'' if a.cpu is None else f' (on CPU {a.cpu})'}; Z80 clock: the T-states "
                 f"it emulates a second. {m['cpu']}, {m['compiler']}, -O2; {m['where']}, "
                 f"commit {m['commit']}, {m['date']}.", ""]
    rows += [f"- **{r['name']}**: {r['about']}" for r in res]
    return "\n".join(rows) + "\n"


def svg(res, m, a):
    """the table as bars: Minstr/s, coloured by what the caller steps"""
    colour = {"half clock": "#1f55c0", "clock": "#2a9d8f", "instruction": "#8a8f98"}
    W, left, top, row = 780, 176, 54, 30
    H = top + row * len(res) + 46
    top_v = max(r["minstr"] for r in res)
    scale = (W - left - 150) / top_v
    e = html.escape
    o = [f"<svg xmlns='http://www.w3.org/2000/svg' width='{W}' height='{H}' viewBox='0 0 {W} {H}' "
         f"font-family='Helvetica, Arial, sans-serif'>",
         f"<rect width='{W}' height='{H}' fill='white'/>",
         f"<text x='16' y='24' font-size='15' font-weight='bold' fill='#111'>C Z80 emulators on ZEXDOC: "
         f"million instructions a second</text>",
         f"<text x='16' y='42' font-size='11.5' fill='#555'>{e(m['cpu'])}, {e(m['compiler'].split(' (')[0])} -O2; "
         f"best of {a.rounds} x {a.instr / 1e6:g}M instructions; Z80 clock in grey; commit {e(m['commit'])}, "
         f"{e(m['date'])}</text>"]
    for k, r in enumerate(res):
        y = top + row * k
        w = max(r["minstr"] * scale, 1)
        bold = " font-weight='bold'" if r["binary"] == BASE else ""
        o += [f"<text x='{left - 8}' y='{y + 19}' font-size='13' text-anchor='end' fill='#111'{bold}>"
              f"{e(r['name'])}</text>",
              f"<rect x='{left}' y='{y + 6}' width='{w:.1f}' height='18' rx='2' fill='{colour[r['steps']]}'/>",
              f"<text x='{left + w + 6:.1f}' y='{y + 19}' font-size='12.5' fill='#111'>{r['minstr']:.1f}"
              f"<tspan dx='9' fill='#666'>{r['mhz']:.0f} MHz</tspan></text>"]
    y = top + row * len(res) + 22
    x = left
    for steps, c in colour.items():
        o += [f"<rect x='{x}' y='{y - 10}' width='12' height='12' rx='2' fill='{c}'/>",
              f"<text x='{x + 17}' y='{y}' font-size='12' fill='#333'>sees every {steps}</text>"]
        x += 170
    o.append("</svg>")
    return "\n".join(o) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--bin", default="build/bin")
    ap.add_argument("--prog", default="tests/zex/zexdoc.com")
    ap.add_argument("--instr", type=int, default=50_000_000)
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--cpu", type=int, help="pin every run to this CPU (taskset)")
    ap.add_argument("--out", default="build/bench")
    a = ap.parse_args()
    times = {b: [] for _, b, _, _ in EMULATORS}
    seen = {}
    for k in range(a.rounds):
        for name, b, _, _ in EMULATORS:     # interleaved: a slow moment hits every emulator alike
            cpu_s, n, t, pc = run(os.path.join(a.bin, b), a.prog, a.instr, a.cpu)
            times[b].append(cpu_s)
            seen[b] = (n, t, pc)
        print(f"cfast_bench: round {k + 1} of {a.rounds}: "
              + ", ".join(f"{b} {times[b][-1]:.2f} s" for _, b, _, _ in EMULATORS), flush=True)
    t_ref = seen[REFERENCE][1]
    res = []
    for name, b, steps, about in EMULATORS:
        n, t, pc = seen[b]
        if abs(t - t_ref) > 0.005 * t_ref:
            sys.exit(f"cfast_bench: {b} ran {t:.0f} T-states, {REFERENCE} {t_ref:.0f}: not the same instructions")
        best = min(times[b])
        res.append({"name": name, "binary": b, "steps": steps, "about": about, "cpu_s": times[b],
                    "best_s": best, "instructions": n, "tstates": t, "final_pc": pc,
                    "minstr": n / best / 1e6, "mhz": t / best / 1e6})
    m = machine()
    os.makedirs(a.out, exist_ok=True)
    json.dump({"machine": m, "program": os.path.basename(a.prog), "instructions": a.instr, "rounds": a.rounds,
               "cpu_pinned": a.cpu, "results": res}, open(os.path.join(a.out, "bench.json"), "w"), indent=1)
    md = markdown(res, m, a)
    open(os.path.join(a.out, "bench.md"), "w").write(md)
    open(os.path.join(a.out, "bench.svg"), "w").write(svg(res, m, a))
    print(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
