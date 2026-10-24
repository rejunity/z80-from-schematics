#!/usr/bin/env python3
"""redcode/Z80 through the event sweep, against perfectz80's recorded traces.

Every case of scripts/event_sweep.py (the default groups and opcodes_reset)
runs through scripts/event_sweep_redcode.c -- redcode/Z80, the reference
emulator in scripts/refs/redcode_z80, as Zilog NMOS, one instruction or
interrupt response at a time, with the pins presented as the die samples
them -- and is compared with the die's recorded trace
(tests/event_sweep/pz80/) at the level an instruction-stepped emulator
works at: the bus transactions (opcode fetches, INTA cycles, memory and I/O
reads and writes, each with its address and data, in order) and the T-state
each M1-type cycle starts in.

WAIT and BUSREQ only stretch the die's time, and redcode has no notion of
them: where a case has either, the harness takes the die's instruction
boundaries from the trace (-M) and only the order and content of the
transactions are compared. A RESET cuts the die's instruction where it is
seen; redcode can only finish the instruction or not run it, so a RESET
case that differs is run again with the cut instruction undone (-x drop).

Where the bus cycles agree, F, WZ and Q are compared too: perfectz80's in
T4 of each M1 that starts an instruction, redcode's at the boundary before
it (up to the first RESET). Each case that does not match is classified:
  cut         RESET inside an instruction: agrees with it finished or undone,
              apart from the die's partial cycle at the cut -- or only the
              registers stored after the restart differ: the die lost the
              result it writes back in the next M1 (known difference 19)
  latch       the latch group's data window: an instruction-stepped
              emulator has no edge it takes a byte on
  state       the bus cycles agree, F, WZ or Q does not
  differs     anything else -- redcode does not do what the die does
Usage: scripts/redcode_sweep.py [--groups all,opcodes_reset] [--match S] [--jobs N]
Writes build/redcode_sweep/report.md and build/redcode_sweep/cases.txt."""
import argparse, collections, hashlib, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import event_sweep as es

HARNESS = os.environ.get("REDCODE_HARNESS") or os.path.join(es.ROOT, "build", "bin", "event_sweep_redcode")
OUT = os.path.join(es.ROOT, "build", "redcode_sweep")
PREFIX = {0xdd, 0xfd, 0xed, 0xcb}
STRETCH = (es.WAIT, es.BUSREQ)


def die_transactions(rows):
    """[(t, kind, addr, data)] from perfectz80's rows; t: the T-state of the
    cycle's T1 for M1-type cycles (where M1 falls), else of its first half."""
    out, cur, m1_start, prev_m1 = [], None, 0, 1
    for r in rows:
        if not r[es.M1] and prev_m1:
            m1_start = r[es.H]
        prev_m1 = r[es.M1]
        if not r[es.M1] and not r[es.MREQ] and not r[es.RD]: k = "F"
        elif not r[es.M1] and not r[es.IORQ]: k = "A"
        elif not r[es.MREQ] and not r[es.RD] and r[es.RFSH]: k = "R"
        elif not r[es.MREQ] and not r[es.RFSH]: k = "Q"      # the refresh: I and R
        elif not r[es.MREQ] and not r[es.WR]: k = "W"
        elif not r[es.IORQ] and not r[es.RD]: k = "I"
        elif not r[es.IORQ] and not r[es.WR]: k = "O"
        else: k = None
        key = (k, r[es.ADDR]) if k else None
        if cur and key != (cur[1], cur[2]):
            out.append(tuple(cur)); cur = None
        if key:
            if cur is None:
                t = (m1_start if k in "FA" else r[es.H]) // 2
                cur = [t, k, r[es.ADDR], r[es.DATA]]
            cur[3] = r[es.DATA] if k != "Q" else 0
    return out          # a cycle still going at the trace's end is left out


def die_boundaries(rows, trs):
    """[(s, e, n)] per instruction from the marker on: s the T-state it
    starts in, e the one whose rising edge samples INT / NMI at its end (its
    last T-state outside a bus grant), n the one the next starts in."""
    starts, prev, pending = [], None, None
    for t, k, a, d in trs:
        if k in "FA":
            # a prefix's M1 leads to the opcode's: the next byte's fetch, or
            # the next INTA of an IM0 response (DD CB d op's next M1 is past
            # d and op; an NMI response's fetch or a HALT NOP that reads a
            # prefix byte leads elsewhere). The byte after ED or CB is the
            # opcode whatever it is; after DD / FD, another prefix counts.
            cont = pending is not None and prev is not None \
                and ((prev[1] == k == "F" and a == (prev[2] + 1) & 0xffff) or prev[1] == k == "A")
            if not cont:
                starts.append(t)
            if cont and pending in (0xed, 0xcb):
                pending = None
            elif cont and d == 0xcb:              # DD CB d op
                pending = None
            else:
                pending = d if d in PREFIX else None
            prev = (t, k, a, d)
    grant = {r[es.H] // 2 for r in rows if not r[es.BUSACK] and r[es.H] % 2 == 0}
    out = []
    for i in range(len(starts) - 1):
        e = starts[i + 1] - 1
        while e > starts[i] and e in grant:
            e -= 1
        out.append((starts[i], e, starts[i + 1]))
    return out


def run_redcode(c, work, drop=False, samples=None):
    ev = os.path.join(work, f"{c['name']}.rc.events")
    with open(ev, "w") as f:
        for p, lo, hi in c["pulses"]:
            f.write(f"{p} {lo} {hi}\n")
    cmd = [HARNESS, "-e", ev, "-r", str(c["run"])]
    if c["inta_release"]: cmd.append("-i")
    if c["int_at"] is not None: cmd += ["-a", f"{c['int_at']:04x}"]
    if c["vec"] != 0xff: cmd += ["-v", f"{c['vec']:02x}"]
    if c["ops"]: cmd += ["-o", "".join(f"{b:02x}" for b in c["ops"])]
    if drop: cmd += ["-x", "drop"]
    if samples:
        sp = os.path.join(work, f"{c['name']}.rc.samples")
        with open(sp, "w") as f:
            f.writelines(f"{s} {e} {n}\n" for s, e, n in samples)
        cmd += ["-M", sp]
    cmd.append(c["prog"])
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    trs, resets, state = [], [], {}
    for line in out.splitlines():
        v = line.split()
        if v[0] == "X": resets.append((int(v[1]), int(v[2]), int(v[3])))
        elif v[0] == "S": state[int(v[1])] = (int(v[2], 16), int(v[3], 16), int(v[4], 16))
        elif v[0] != "U" and len(v) == 4:
            trs.append((int(v[0]), v[1], int(v[2], 16), int(v[3], 16)))
    return trs, resets, state


def compare(die, rc, timed):
    """(index of the first difference or None, what)"""
    for i, d in enumerate(die):
        if i >= len(rc):
            return i, "redcode ends"
        r = rc[i]
        if d[1:] != r[1:]:
            return i, "transaction"
        if timed and d[1] in "FA" and d[0] != r[0]:
            return i, "M1 T-state"
    return None, ""


def fmt(trs, i, n=6):
    lo = max(0, i - 2)
    return " ".join(f"{t}:{k}{a:04x}={d:02x}" for t, k, a, d in trs[lo:lo + n])


def outside(trs, cut):
    """trs without the cut instruction's bus cycles: those from its start to
    the restart -- but a refresh that ends an M1 kept before the cut stays
    (WAIT can push the die's past the cut instruction's start)"""
    out, prev_kept = [], False
    for x in trs:
        inside = any(t0 <= x[0] < r for t0, r in cut)
        keep = not inside or (x[1] == "Q" and prev_kept and out and out[-1][1] in "FA")
        if keep:
            out.append(x)
        prev_kept = keep
    return out


def check(c, die_rows, work):
    die = die_transactions(die_rows)
    stretch = any(p in STRETCH for p, _, _ in c["pulses"]) or c["hold"] or c["busreq_walk"] or c["wait_walk"]
    samples = die_boundaries(die_rows, die) if stretch else None
    rc, resets, rstate = run_redcode(c, work, samples=samples)
    i, what = compare(die, rc, not stretch)
    if i is None:
        # F, WZ and Q: the die's in T4.P of each M1 (es.state_samples) that
        # starts an instruction, redcode's at the boundary before it; up to
        # the first RESET, which may leave results half-written
        dstate, m1s = es.state_samples(die_rows, True), [d for d in die if d[1] in "FA"]
        halves = sorted(dstate)
        limit = min((k for k, _, _ in resets), default=1 << 30)
        for t, st in sorted(rstate.items()):
            m = next((m for m in m1s if m[0] == t), None)
            if t >= limit or m is None:
                continue
            h = next((h for h in halves if h >= 2 * t), None)
            d = dstate.get(h)
            # at an INTA, the die has written back the instruction before with
            # P/V from IFF2 the INT just cleared (the LD A,I / LD A,R race);
            # redcode clears P/V as it takes the INT: compare the rest
            pv = 0xfb if m[1] == "A" else 0xff
            if d is not None and (d[0] & pv, d[1], d[2] & pv) != (st[0] & pv, st[1], st[2] & pv):
                return "state", {"at": t, "die": f"F={d[0]:02x} WZ={d[1]:04x} Q={d[2]:02x}",
                                 "redcode": f"F={st[0]:02x} WZ={st[1]:04x} Q={st[2]:02x}"}
        return "match", None
    detail = {"at": i, "what": what, "die": fmt(die, i), "redcode": fmt(rc, i), "resets": resets}
    if any(p == es.RESET for p, _, _ in c["pulses"]):
        # the die cuts the instruction it sees RESET in: compare everything
        # but that instruction's bus cycles (from its start to the restart),
        # with redcode's instruction finished, then undone
        rc2, resets2, _ = run_redcode(c, work, drop=True, samples=samples)
        for how, rr, res in (("finished", rc, resets), ("undone", rc2, resets2)):
            if not res:
                continue
            cut = [(t0, restart) for _, restart, t0 in res]
            if compare(outside(die, cut), outside(rr, cut), not stretch)[0] is None:
                return "cut", dict(detail, how=how)
        # the die loses (or keeps half of) a result it writes back in the next
        # M1 when RESET is seen at the end of the instruction or in that M1:
        # the same bus cycles, only the registers the dump stores differ
        for how, rr, res in (("result lost", rc, resets), ("result lost", rc2, resets2)):
            if not res:
                continue
            cut = [(t0, restart) for _, restart, t0 in res]
            first = min(r for _, r in cut)
            out = lambda trs: outside(trs, cut)
            # (around the stack, SP = 9000, after the restart: a PUSH or POP the
            # cut stopped half-way leaves SP moved by one, and what the dump
            # pushed is read back)
            st = lambda t, a: t >= first and 0x8f00 <= a <= 0x90ff
            loose = lambda trs: [(t, k, a if not (k == "W" and st(t, a)) else None,
                                  d if not ((k == "W" and t >= first) or (k == "R" and st(t, a))) else None)
                                 for t, k, a, d in out(trs)]
            if compare(loose(die), loose(rr), not stretch)[0] is None:
                return "cut", dict(detail, how=how)
    if c["dwin"]:
        return "latch", detail
    return "differs", detail


PZ80_RUNNER = os.path.join(es.ROOT, "build", "bin", "perfectz80_runner")


def runner_transactions(out):
    """perfectz80_runner's rows (phase addr data_o data_i mreq iorq rd wr m1
    rfsh halt; phase 1 is the first fetch's T1.P) as die_transactions does"""
    trs, cur, m1_start, prev_m1 = [], None, 0, 1
    for line in out.splitlines():
        v = line.split()
        if len(v) != 11 or not v[0].isdigit():
            continue
        h = int(v[0]) - 1
        addr, do, di = int(v[1], 16), int(v[2], 16), int(v[3], 16)
        mreq, iorq, rd, wr, m1, rfsh = (int(x) for x in v[4:10])
        if h < 0:
            continue
        if not m1 and prev_m1:
            m1_start = h
        prev_m1 = m1
        if not m1 and not mreq and not rd: k, d = "F", di
        elif not m1 and not iorq: k, d = "A", do
        elif not mreq and not rd and rfsh: k, d = "R", di
        elif not mreq and not rfsh: k, d = "Q", 0
        elif not mreq and not wr: k, d = "W", do
        elif not iorq and not rd: k, d = "I", 0
        elif not iorq and not wr: k, d = "O", do
        else: k = None
        key = (k, addr) if k else None
        if cur and key != (cur[1], cur[2]):
            trs.append(tuple(cur)); cur = None
        if key:
            if cur is None:
                cur = [(m1_start if k in "FA" else h) // 2, k, addr, d]
            cur[3] = d
    return trs


def traces(a):
    """The trace programs and pin scenarios (compare_signal_timing.py's set),
    perfectz80_runner live against redcode"""
    import glob
    progs = sorted(glob.glob(os.path.join(es.ROOT, "tests", "traces", "*.hex")))
    progs += sorted(glob.glob(os.path.join(es.ROOT, "tests", "traces", "pin_scenarios", "*.hex")))
    work = os.path.join(a.out, "traces")
    os.makedirs(work, exist_ok=True)
    names = {0: "nmi", 1: "int", 2: "wait", 3: "busreq", 4: "reset"}
    pins = {v: k for k, v in names.items()}
    rows = []

    def one(prog):
        base = prog[:-4]
        phases = int(open(base + ".phases").read().split()[0]) if os.path.exists(base + ".phases") else 200
        events = base + ".events" if os.path.exists(base + ".events") else None
        cmd = [PZ80_RUNNER, prog, str(phases + 1)] + ([events] if events else [])
        die = runner_transactions(subprocess.run(cmd, capture_output=True, text=True, check=True).stdout)
        pulses, level = [], {}
        if events:
            for line in open(events):
                v = line.split()
                if len(v) == 3 and v[0].isdigit() and v[1] in pins:
                    p, s16, low = pins[v[1]], 8 * (int(v[0]) - 1), v[2] == "0"
                    if low and p not in level:
                        level[p] = s16
                    elif not low and p in level:
                        pulses.append((p, level.pop(p), s16))
            pulses += [(p, lo, -1) for p, lo in level.items()]
        ev = os.path.join(work, os.path.basename(base) + ".rc.events")
        with open(ev, "w") as f:
            f.writelines(f"{p} {lo} {hi}\n" for p, lo, hi in pulses)
        stretch = any(p in STRETCH for p, _, _ in pulses)
        cmd = [HARNESS, "-e", ev, "-r", str(phases // 2), "-m", "0000", "-v", "00", "-I", prog]
        out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
        lines = [l.split() for l in out.splitlines()]
        rc = [(int(v[0]), v[1], int(v[2], 16), int(v[3], 16)) for v in lines if len(v) == 4 and v[0] != "X"]
        cut = [(int(v[3]), int(v[2])) for v in lines if v and v[0] == "X"]
        # the window ends inside redcode's run: compare the die's whole cycles
        i, what = compare(die, rc, not stretch)
        if i is not None and cut:     # a RESET: everything but the instruction it cut
            if compare(outside(die, cut), outside(rc, cut), not stretch)[0] is None:
                what = "cut"
        return prog, phases, pulses, stretch, len(die), i, what, fmt(die, i or 0), fmt(rc, i or 0)
    with ThreadPoolExecutor(a.jobs) as pool:
        for r in pool.map(one, progs):
            rows.append(r)
    bad = 0
    for prog, phases, pulses, stretch, n, i, what, d, r in rows:
        name = os.path.relpath(prog, es.ROOT)
        if i is None:
            print(f"  {name}: match ({n} transactions, {phases} phases{', untimed: WAIT/BUSREQ' if stretch else ''})")
        elif what == "cut":
            print(f"  {name}: match but for the instruction RESET cuts ({n} transactions)")
        else:
            bad += 1
            print(f"  {name}: {what} at {i}\n    die:     {d}\n    redcode: {r}")
    print(f"{len(rows) - bad} / {len(rows)} programs match")
    return 1 if bad else 0


def halt2int(a):
    """make halt2int's ten placements (scripts/halt2int_probe.c): IM 1; EI;
    HALT, INT low 2, 4 .. 20 half-cycles after the HALT pin falls (T4.N of
    HALT's M1, 7 halves after its T1.P), the T-states from there to IORQ
    falling in the INTA (5 halves after its T1.P) against the die's"""
    die = [6, 5, 4, 7, 6, 5, 4, 7, 6, 5]
    work = os.path.join(a.out, "halt2int")
    os.makedirs(work, exist_ok=True)
    prog = os.path.join(work, "halt2int.hex")
    with open(prog, "w") as f:
        f.write("@0000 ed 56 fb 76\n@0038 76\n")
    got = []
    for dp in range(2, 21, 2):
        halt_half = 2 * 12 + 7                  # HALT's M1 starts in T-state 12
        int_half = halt_half + dp
        ev = os.path.join(work, f"dp{dp}.events")
        with open(ev, "w") as f:
            f.write(f"1 {8 * (int_half + 1) - 1} -1\n")   # low from the edge after that half
        out = subprocess.run([HARNESS, "-e", ev, "-r", "60", "-m", "0000", "-i", prog],
                             capture_output=True, text=True, check=True).stdout
        inta = [int(v[0]) for v in (l.split() for l in out.splitlines()) if len(v) == 4 and v[1] == "A"]
        halt = [int(v[0]) for v in (l.split() for l in out.splitlines()) if len(v) == 4 and v[1] == "F" and v[2] == "0003"]
        assert halt and halt[0] == 12, out
        got.append((2 * inta[0] + 5 - int_half) // 2 if inta else None)
    print("redcode:", " ".join(map(str, got)), "\ndie:    ", " ".join(map(str, die)))
    ok = got == die
    print("PASS: every delay as on the die" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--groups", default="all,opcodes_reset")
    ap.add_argument("--match", default="")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--traces", action="store_true",
                    help="the trace programs and pin scenarios instead, perfectz80_runner live")
    ap.add_argument("--halt2int", action="store_true", help="make halt2int's ten placements instead")
    a = ap.parse_args()
    if a.traces:
        return traces(a)
    if a.halt2int:
        return halt2int(a)
    if not os.path.exists(HARNESS):
        sys.exit(f"missing {HARNESS}: make build/bin/event_sweep_redcode")
    work = os.path.join(a.out, "work")
    os.makedirs(work, exist_ok=True)
    todo = [c for c in es.cases(a.groups.split(","), 2) if a.match in c["name"]]
    progs = {}
    for c in todo:
        pid = es.prog_id(c)
        if pid not in progs:
            ram = es.image(c)
            sha = hashlib.sha1(ram).hexdigest()[:16]
            path = os.path.join(work, f"prog_{sha}.hex")
            if not os.path.exists(path):
                es.write_hex(ram, path)
            progs[pid] = (path, sha)
        c["prog"], c["prog_sha"] = progs[pid]
    cache, hid = es.load_cache(), es.harness_id()
    missing = [c["name"] for c in todo if es.cache_key(c, hid) not in cache]
    todo = [c for c in todo if es.cache_key(c, hid) in cache]
    print(f"{len(todo)} cases with a recorded perfectz80 trace ({len(missing)} without), {a.jobs} jobs",
          flush=True)

    def one(c):
        rows = es.parse_rows(cache[es.cache_key(c, hid)], c["name"])
        return c, check(c, rows, work)
    results = []
    with ThreadPoolExecutor(a.jobs) as pool:
        for i, r in enumerate(pool.map(one, todo), 1):
            results.append(r)
            if i % 5000 == 0:
                print(f"  {i}/{len(todo)}", flush=True)
    by = collections.defaultdict(collections.Counter)
    for c, (st, _) in results:
        by[c["group"]][st] += 1
    kinds = ["match", "cut", "latch", "state", "differs"]
    lines = ["| group | cases | " + " | ".join(kinds) + " |", "|---|---:|" + "---:|" * len(kinds)]
    tot = collections.Counter()
    for g in es.GROUPS:
        if g in by:
            n = sum(by[g].values()); tot.update(by[g])
            lines.append(f"| {g} | {n} | " + " | ".join(str(by[g][k]) for k in kinds) + " |")
    lines.append(f"| **all** | {sum(tot.values())} | " + " | ".join(f"**{tot[k]}**" for k in kinds) + " |")
    with open(os.path.join(a.out, "cases.txt"), "w") as f:
        for c, (st, d) in results:
            if st != "match":
                f.write(f"{c['name']} {st} {d}\n")
    with open(os.path.join(a.out, "report.md"), "w") as f:
        f.write("# redcode/Z80 against perfectz80's recorded traces\n\n" + "\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"cases: {os.path.relpath(os.path.join(a.out, 'cases.txt'), es.ROOT)}")
    return 1 if tot["differs"] or tot["state"] else 0


if __name__ == "__main__":
    sys.exit(main())
