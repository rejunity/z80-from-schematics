#!/usr/bin/env python3
"""xy_study.py - how far the reference emulators agree on F's X and Y (bits 5
and 3), and which of them agrees with the die (perfectz80).

Every case is a FUSE tests.in record. scripts/xy_oracles.cpp runs it through
our C model (rz80), floooh/chips, superzazu, suzukiplan and redcode/Z80;
scripts/fuse_pz80.c runs it on the die. Three kinds of case:

  single  one instruction: every opcode (CB, ED, DD, FD, DDCB, FDCB and the
          undocumented ones) from random registers, WZ, memory and PC, the
          instructions whose X / Y have rules of their own (SCF, CCF, BIT
          n,(HL), the block instructions, ...) from many more
  pair    an instruction, then SCF or CCF where it continues: the Q the
          instruction leaves
  triple  a flag-writing instruction, an instruction, then SCF or CCF:
          whether the middle one keeps or clears that Q
  wpair   a flag-writing instruction, then SCF or CCF behind prefixes (DD 37,
          FD DD 3F, ...): whether a prefix keeps the Q it left

Every case on which the five do not all agree about F runs on the die, and
so does a random sample (--sample) of the ones where they all agree. The
report (build/xy/report.md) counts, per emulator and class of instruction,
how often its X / Y and its F are the die's. --corpus DIR writes every case
where any emulator's F differs from the die's (at most --per-pattern of
each opcode, kind and set of emulators that differ) and the sampled ones the
die agreed with all of, as DIR/tests.in and DIR/tests.expected, with the
die's values.

  scripts/xy_study.py [--seed S] [--k K] [--k-hot K] [--states N] [--sample P]
                      [--jobs N] [--corpus DIR] [--per-pattern N]
"""
import argparse, collections, os, random, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.environ.get("XY_OUT") or os.path.join(ROOT, "build", "xy")
ORACLES = os.environ.get("XY_ORACLES") or os.path.join(ROOT, "build", "xy", "xy_oracles")
PZ80 = os.environ.get("XY_PZ80") or os.path.join(ROOT, "build", "bin", "fuse_pz80")
EMUS = ["rz80", "chips", "superzazu", "suzukiplan", "redcode"]

# ---- opcodes -------------------------------------------------------------------
N8 = {0x06,0x0E,0x16,0x1E,0x26,0x2E,0x36,0x3E,0xC6,0xCE,0xD6,0xDE,0xE6,0xEE,0xF6,0xFE,0xD3,0xDB}
E8 = {0x10,0x18,0x20,0x28,0x30,0x38}
NN = {0x01,0x11,0x21,0x31,0x22,0x2A,0x32,0x3A,0xC2,0xC3,0xC4,0xCA,0xCC,0xCD,0xD2,0xD4,0xDA,0xDC,
      0xE2,0xE4,0xEA,0xEC,0xF2,0xF4,0xFA,0xFC}
ED_NN = {0x43,0x4B,0x53,0x5B,0x63,0x6B,0x73,0x7B}
XD_D = {0x34,0x35,0x46,0x4E,0x56,0x5E,0x66,0x6E,0x7E,0x70,0x71,0x72,0x73,0x74,0x75,0x77,
        0x86,0x8E,0x96,0x9E,0xA6,0xAE,0xB6,0xBE}

def opcodes():
    """[(label, prefix bytes, opcode, operand kind)]"""
    ops = []
    for o in range(256):
        if o not in (0xCB, 0xDD, 0xED, 0xFD):
            ops.append((f"{o:02x}", [], o, "n" if o in N8 else "e" if o in E8 else "nn" if o in NN else ""))
    for o in range(256):
        ops.append((f"cb{o:02x}", [0xCB], o, ""))
    for o in range(256):
        ops.append((f"ed{o:02x}", [0xED], o, "nn" if o in ED_NN else ""))
    for p in (0xDD, 0xFD):
        for o in range(256):
            if o != 0xCB:
                k = "dn" if o == 0x36 else "d" if o in XD_D else \
                    "n" if o in N8 else "e" if o in E8 else "nn" if o in NN else ""
                ops.append((f"{p:02x}{o:02x}", [p], o, k))
        for o in range(256):
            ops.append((f"{p:02x}cb{o:02x}", [p, 0xCB], o, "cb"))
    return ops

BLOCK = {f"ed{o:02x}" for o in (0xA0,0xA1,0xA2,0xA3,0xA8,0xA9,0xAA,0xAB,
                                 0xB0,0xB1,0xB2,0xB3,0xB8,0xB9,0xBA,0xBB)}
IO_BLOCK = {f"ed{o:02x}" for o in (0xA2,0xA3,0xAA,0xAB,0xB2,0xB3,0xBA,0xBB)}
HOT = {"27", "2f", "37", "3f", "08", "f1"} | BLOCK
HOT |= {f"cb{o:02x}" for o in range(0x40, 0x80) if o & 7 == 6}
HOT |= {f"ed{o:02x}" for o in (0x57, 0x5F, 0x67, 0x6F, 0x44, 0x70, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68,
                               0x78, 0x42, 0x4A, 0x52, 0x5A, 0x62, 0x6A, 0x72, 0x7A)}
HOT |= {f"{p}cb{o:02x}" for p in ("dd", "fd") for o in range(0x40, 0x80, 8)}

def klass(label):
    """the class of instruction a label belongs to, for the tables"""
    if label in ("37", "3f"): return "SCF / CCF"
    if label in BLOCK: return ("I/O " if label in IO_BLOCK else "") + \
        ("block, repeating" if label[2] == "b" else "block, once")
    if label.startswith(("ddcb", "fdcb")):
        o = int(label[4:], 16)
        return "DDCB/FDCB BIT" if 0x40 <= o < 0x80 else "DDCB/FDCB"
    if label.startswith("cb"):
        o = int(label[2:], 16)
        return "BIT n,(HL)" if 0x40 <= o < 0x80 and o & 7 == 6 else "CB"
    if label.startswith("ed"): return "ED"
    if label.startswith(("dd", "fd")): return "DD/FD"
    return "unprefixed"

# ---- cases -----------------------------------------------------------------------
class Case:
    """a FUSE tests.in record"""
    def __init__(self, name, label, kind, regs, pc, i, r, iff, im, ts, mem, code):
        self.name, self.label, self.kind = name, label, kind
        self.regs, self.pc, self.i, self.r, self.iff, self.im, self.ts = regs, pc, i, r, iff, im, ts
        self.mem, self.code = mem, code            # mem: {addr: byte}
    def text(self):
        g = self.regs
        out = [self.name,
               " ".join(f"{g[k]:04x}" for k in ("af","bc","de","hl","af2","bc2","de2","hl2","ix","iy","sp"))
               + f" {self.pc:04x} {g['wz']:04x}",
               f"{self.i:02x} {self.r:02x} {self.iff} {self.iff} {self.im} 0 {self.ts}"]
        run_a, run = None, []
        for a in sorted(self.mem):
            if run_a is not None and a == run_a + len(run):
                run.append(self.mem[a]); continue
            if run: out.append(f"{run_a:04x} " + " ".join(f"{b:02x}" for b in run) + " -1")
            run_a, run = a, [self.mem[a]]
        if run: out.append(f"{run_a:04x} " + " ".join(f"{b:02x}" for b in run) + " -1")
        out.append("-1")
        return "\n".join(out) + "\n"
    def copy(self, name, kind):
        c = Case(name, self.label, kind, dict(self.regs), self.pc, self.i, self.r, self.iff, self.im,
                 self.ts, dict(self.mem), list(self.code))
        return c

def instr(rng, op):
    label, pre, o, kind = op
    if kind == "cb": return pre + [rng.randrange(256), o]
    b = pre + [o]
    if kind in ("n", "e", "d"): b.append(rng.randrange(256))
    elif kind in ("nn", "dn"): b += [rng.randrange(256), rng.randrange(256)]
    return b

def zone(pc, n):
    """addresses a case keeps clear: fuse_pz80's preamble before PC, the code"""
    return lambda a: (pc - 72) <= (a & 0xFFFF) < (pc + n + 8) or (a & 0xFFFF) < 0x10

def operand_addrs(code, regs):
    """the memory one instruction can read besides its code"""
    g = regs
    adds = [g["hl"], g["hl"] + 1, g["bc"], g["bc"] + 1, g["de"], g["de"] + 1,
            g["sp"] - 2, g["sp"] - 1, g["sp"], g["sp"] + 1, g["wz"] - 1]
    if code and code[0] in (0xDD, 0xFD) and len(code) >= 3:
        base = g["ix" if code[0] == 0xDD else "iy"]
        d = code[2]
        adds.append(base + (d - 256 if d >= 128 else d))
    if len(code) >= 3:
        nn = code[-2] | code[-1] << 8
        adds += [nn, nn + 1]
    return [a & 0xFFFF for a in adds]

def new_case(rng, name, label, kind, code, ts, regs_fn=None):
    for _ in range(2000):
        pc = rng.randrange(0x0200, 0xF000)
        g = {k: rng.randrange(0x10000) for k in ("af","bc","de","hl","af2","bc2","de2","hl2","ix","iy","sp","wz")}
        if regs_fn: regs_fn(rng, g)
        bad = zone(pc, len(code))
        adds = operand_addrs(code, g)
        if any(bad(a) for a in adds): continue
        mem = {a: rng.randrange(256) for a in adds}
        for k, b in enumerate(code): mem[(pc + k) & 0xFFFF] = b
        return Case(name, label, kind, g, pc, rng.randrange(256), rng.randrange(256),
                    rng.randrange(2), rng.randrange(3), ts, mem, code)
    return None

def block_regs(label):
    """a block instruction ends (BC or B = 1) in about a third of the cases"""
    if label not in BLOCK: return None
    def fn(rng, g):
        if rng.random() < 0.35:
            if label in IO_BLOCK: g["bc"] = 0x0100 | (g["bc"] & 0xFF)
            else: g["bc"] = 1
        elif rng.random() < 0.2:
            g["bc"] = 0 if label not in IO_BLOCK else (g["bc"] & 0xFF)
    return fn

WRITERS = [([0xFE], 7), ([0xC6], 7), ([0xD6], 7), ([0xE6], 7), ([0xEE], 7), ([0xF6], 7), ([0xCE], 7),
           ([0xDE], 7)]
WRITERS1 = [([0x37], 4), ([0x3F], 4), ([0x3C], 4), ([0x3D], 4), ([0x04], 4), ([0x0D], 4), ([0x27], 4),
            ([0x2F], 4), ([0x17], 4), ([0x1F], 4), ([0x07], 4), ([0x0F], 4), ([0xB7], 4), ([0xA7], 4),
            ([0xAF], 4), ([0xB8], 4), ([0x87], 4), ([0x9F], 4), ([0xED, 0x44], 8), ([0xCB, 0x47], 8),
            ([0xCB, 0x7F], 8), ([0xCB, 0x10], 8), ([0xED, 0x57], 9), ([0x09], 11), ([0xED, 0x52], 15)]

def writer(rng):
    if rng.random() < 0.5:
        b, t = rng.choice(WRITERS)
        return b + [rng.randrange(256)], t
    b, t = rng.choice(WRITERS1)
    return list(b), t

NO_NEXT = {"76", "dd76", "fd76"}          # HALT: the SCF after it never runs
# SCF / CCF behind prefixes, for wpairs (the threshold runs every prefix)
WPAIR_X = [(lbl, b) for s_ in (0x37, 0x3F) for lbl, b in (
    (f"{s_:02x}", [s_]), (f"dd{s_:02x}", [0xDD, s_]), (f"fd{s_:02x}", [0xFD, s_]),
    (f"dddd{s_:02x}", [0xDD, 0xDD, s_]), (f"ddfd{s_:02x}", [0xDD, 0xFD, s_]),
    (f"fddd{s_:02x}", [0xFD, 0xDD, s_]), (f"fdfd{s_:02x}", [0xFD, 0xFD, s_]))]

def free_for_next(c, nxt):
    """whether SCF / CCF can go where the instruction continues: not over the
    case's memory, its code (a repeat), the die's preamble before PC or the
    vectors"""
    return (c.label not in NO_NEXT and nxt not in c.mem and nxt >= 0x10
            and (nxt - (c.pc - 72)) & 0xFFFF >= 72)

# ---- runs ------------------------------------------------------------------------
def write(path, cases):
    with open(path, "w") as f:
        for c in cases: f.write(c.text() + "\n")

def run_oracles(path, only_ours=False):
    """{case: {emu: (af, bc, ..., wz, i, r, ts)}}"""
    args = [ORACLES, path] + (["--only", "rz80"] if only_ours else [])
    out = subprocess.run(args, capture_output=True, text=True, check=True).stdout
    res = collections.defaultdict(dict)
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 22:
            res[p[0]][p[1]] = tuple(int(x, 16) for x in p[2:17]) + tuple(int(x) for x in p[17:22])
    return res

REG = ["af","bc","de","hl","af2","bc2","de2","hl2","ix","iy","sp","pc","wz","i","r","ts"]

def comparable(res, name):
    """the emulators whose run of a case ended where ours did: an emulator that
    refuses an opcode (suzukiplan throws on undefined ED and DD / FD ones) or
    stops at another instruction boundary is left out of that case"""
    r = res.get(name, {})
    ours = r.get("rz80")
    return {e: v for e, v in r.items()
            if v[15] >= 0 and ours is not None and v[11] == ours[11] and v[15] == ours[15]}

def chunks(lst, n):
    for k in range(0, len(lst), n): yield lst[k:k + n]

def run_die(cases, ours, jobs, tag):
    """the die's values for cases: {name: (af, ..., r, ts)} and {name: status}"""
    todo = list(chunks(cases, 1500))
    def one(k_part):
        k, part = k_part
        pin = os.path.join(OUT, f"die_{tag}_{k}.in"); pex = os.path.join(OUT, f"die_{tag}_{k}.expected")
        write(pin, part)
        with open(pex, "w") as f:
            for c in part:
                o = ours[c.name]["rz80"]
                f.write(f"{c.name}\n" + " ".join(f"{v:04x}" for v in o[:13]) +
                        f"\n{o[13]:02x} {o[14]:02x} {o[16]} {o[17]} {o[18]} {o[19]} {o[15]}\n\n")
        r = subprocess.run([PZ80, pin, pex, "-v"], capture_output=True, text=True)
        return r.stdout
    die, status = {}, {}
    with ThreadPoolExecutor(jobs) as ex:
        for text in ex.map(one, enumerate(todo)):
            lines = text.splitlines()
            k = 0
            while k < len(lines):
                ln = lines[k]
                m = re.match(r"^(\S+)\s+(DIFFERS|not run|no expected)", ln)
                if m:
                    status[m.group(1)] = ln; k += 1; continue
                p = ln.split()
                if len(p) == 1 and k + 2 < len(lines) and len(lines[k + 1].split()) == 13:
                    regs = [int(x, 16) for x in lines[k + 1].split()]
                    st = lines[k + 2].split()
                    die[p[0]] = tuple(regs) + (int(st[0], 16), int(st[1], 16), int(st[6]), int(st[5]))
                    k += 3; continue
                k += 1
    return die, status

# ---- main ------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--k", type=int, default=10, help="states per opcode")
    ap.add_argument("--k-hot", type=int, default=120, help="states per opcode with X / Y rules of its own")
    ap.add_argument("--states", type=int, default=2, help="states per opcode for pairs and triples")
    ap.add_argument("--sample", type=float, default=0.04, help="share of all-agree cases run on the die")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--corpus", default=None)
    ap.add_argument("--per-pattern", type=int, default=3)
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    rng = random.Random(a.seed)
    ops = opcodes()

    # singles
    singles = []
    for op in ops:
        for j in range(a.k_hot if op[0] in HOT else a.k):
            c = new_case(rng, f"s{op[0]}_{j}", op[0], "single", instr(rng, op), 1, block_regs(op[0]))
            if c: singles.append(c)
    write(os.path.join(OUT, "singles.in"), singles)
    res = run_oracles(os.path.join(OUT, "singles.in"))
    print(f"singles: {len(singles)} cases", flush=True)

    # pairs: X, then SCF / CCF where our model continues
    pairs = []
    by_label = collections.defaultdict(list)
    for c in singles: by_label[c.label].append(c)
    for label, cs in by_label.items():
        used = 0
        for c in cs:
            if used >= a.states: break
            o = res[c.name]["rz80"]
            nxt = o[11]
            if not free_for_next(c, nxt): continue
            for s, sname in ((0x37, "scf"), (0x3F, "ccf")):
                p = c.copy(f"p{label}_{used}_{sname}", "pair")
                p.mem[nxt] = s; p.ts = o[15] + 1
                pairs.append(p)
            used += 1

    # triples: W (writes F), X, SCF / CCF
    pre = []
    for op in ops:
        for j in range(a.states):
            w, tw = writer(rng)
            x = instr(rng, op)
            c = new_case(rng, f"t{op[0]}_{j}", op[0], "triple", w + x, tw + 1, block_regs(op[0]))
            if c:
                c.tw = tw; pre.append(c)
    write(os.path.join(OUT, "triples_pre.in"), pre)
    rp = run_oracles(os.path.join(OUT, "triples_pre.in"), only_ours=True)
    triples = []
    for c in pre:
        o = rp[c.name]["rz80"]
        nxt = o[11]
        if not free_for_next(c, nxt) or o[15] <= c.tw:
            continue
        for s, sname in ((0x37, "scf"), (0x3F, "ccf")):
            t = c.copy(f"{c.name}_{sname}", "triple")
            t.mem[nxt] = s; t.ts = o[15] + 1
            triples.append(t)
    # wpairs: W, then SCF / CCF behind prefixes
    wpairs = []
    for label, x in WPAIR_X:
        for j in range(a.k_hot // 3):
            w, tw = writer(rng)
            c = new_case(rng, f"w{label}_{j}", label, "wpair", w + x, tw + 4 * len(x) - 3)
            if c: wpairs.append(c)
    write(os.path.join(OUT, "seq.in"), pairs + triples + wpairs)
    res.update(run_oracles(os.path.join(OUT, "seq.in")))
    cases = singles + pairs + triples + wpairs
    print(f"pairs: {len(pairs)}, triples: {len(triples)}, wpairs: {len(wpairs)}", flush=True)

    # who agrees with whom
    die_set, agree_pool = [], []
    for c in cases:
        fs = {e: v[0] & 0xFF for e, v in comparable(res, c.name).items()}
        if len(set(fs.values())) > 1: die_set.append(c)
        else: agree_pool.append(c)
    sample = [c for c in agree_pool if rng.random() < a.sample]
    print(f"F disagreements: {len(die_set)}; all-agree sample: {len(sample)} of {len(agree_pool)}", flush=True)
    die, status = run_die(die_set + sample, res, a.jobs, "run")
    print(f"die: {len(die)} cases read, {sum(1 for s in status.values() if 'not run' in s)} not run", flush=True)

    # tables
    import json
    with open(os.path.join(OUT, "results.json"), "w") as f:
        json.dump({"cases": {c.name: {"label": c.label, "kind": c.kind, "class": klass(c.label),
                                      "sample": c in sample} for c in die_set + sample},
                   "emus": {n: {e: res[n][e] for e in EMUS if e in res[n]} for n in [c.name for c in die_set + sample]},
                   "die": die, "status": status}, f)
    report(cases, die_set, sample, res, die, status, a)
    if a.corpus:
        corpus(a, die_set, sample, res, die, status)

def report(cases, die_set, sample, res, die, status, a):
    L = ["# X / Y (F bits 5, 3): the emulators against each other and the die\n",
         f"{len(cases)} cases (seed {a.seed}): "
         f"{sum(1 for c in cases if c.kind == 'single')} single instructions, "
         f"{sum(1 for c in cases if c.kind == 'pair')} pairs, {sum(1 for c in cases if c.kind == 'triple')} triples, "
         f"{sum(1 for c in cases if c.kind == 'wpair')} wpairs. "
         f"{len(die_set)} with the five not agreeing on F ran on the die, and {len(sample)} of those where they "
         f"all agree.\n"]
    def kclass(c):
        if c.kind == "wpair": return "SCF/CCF behind prefixes, after a flag write"
        return klass(c.label) if c.kind == "single" else f"SCF/CCF after ({c.kind}s)"
    # the emulators against each other: X / Y equal, every case both ran
    pair_n = collections.Counter(); pair_eq = collections.Counter()
    all5 = collections.Counter(); alln = collections.Counter()
    for c in cases:
        r = comparable(res, c.name)
        for i, e1 in enumerate(EMUS):
            for e2 in EMUS[i + 1:]:
                if e1 in r and e2 in r:
                    pair_n[e1, e2] += 1; pair_eq[e1, e2] += (r[e1][0] & 0x28) == (r[e2][0] & 0x28)
        if len(r) == len(EMUS):
            k = kclass(c); alln[k] += 1
            all5[k] += len({v[0] & 0x28 for v in r.values()}) == 1
    L.append("\n## The emulators against each other\n")
    L.append("X / Y equal, in % of the cases both ran to the same end (suzukiplan refuses "
             "undefined ED and DD / FD opcodes):\n")
    L.append("| | " + " | ".join(EMUS[1:]) + " |"); L.append("|---|" + "---|" * (len(EMUS) - 1))
    for i, e1 in enumerate(EMUS[:-1]):
        L.append(f"| {e1} | " + " | ".join(
            (f"{100*pair_eq[e1, e2]/pair_n[e1, e2]:.1f}" if j > i else "") for j, e2 in enumerate(EMUS[1:], 1)) + " |")
    L.append("\nAll five with the same X / Y, by class:\n")
    L.append("| class | cases | all five agree |"); L.append("|---|---|---|")
    for k in sorted(alln):
        L.append(f"| {k} | {alln[k]} | {100*all5[k]/alln[k]:.1f} % |")
    def table(title, subset):
        tot = collections.Counter(); xy = collections.defaultdict(collections.Counter)
        ff = collections.defaultdict(collections.Counter); ran = collections.defaultdict(collections.Counter)
        for c in subset:
            if c.name not in die: continue
            d = die[c.name][0] & 0xFF
            k = kclass(c)
            tot[k] += 1
            for e, o in comparable(res, c.name).items():
                ran[k][e] += 1
                xy[k][e] += (o[0] & 0x28) == (d & 0x28)
                ff[k][e] += (o[0] & 0xFF) == d
        L.append(f"\n## {title}\n")
        L.append("X / Y as the die's, then the whole F as the die's, in % of the cases the emulator ran:\n")
        L.append("| class | cases | " + " | ".join(EMUS) + " |")
        L.append("|---|---|" + "---|" * len(EMUS))
        pc = lambda a, b: f"{100*a/b:.0f}" if b else "-"
        for k in sorted(tot):
            L.append(f"| {k} | {tot[k]} | " + " | ".join(
                f"{pc(xy[k][e], ran[k][e])} / {pc(ff[k][e], ran[k][e])}" for e in EMUS) + " |")
        T = sum(tot.values())
        if T:
            L.append(f"| **all** | {T} | " + " | ".join(
                f"{100*sum(xy[k][e] for k in tot)/max(1,sum(ran[k][e] for k in tot)):.1f} / "
                f"{100*sum(ff[k][e] for k in tot)/max(1,sum(ran[k][e] for k in tot)):.1f}" for e in EMUS) + " |")
    table("Where the emulators disagree", die_set)
    table("Sampled where they all agree", sample)
    bad = [c for c in sample if c.name in die and (die[c.name][0] & 0xFF) != (res[c.name]["rz80"][0] & 0xFF)]
    ours_bad = [c for c in die_set + sample if c.name in die and res[c.name]["rz80"][:13] != die[c.name][:13]]
    L.append(f"\nOur C model's registers (F, WZ and the rest) differ from the die's in {len(ours_bad)} of the "
             f"{sum(1 for c in die_set + sample if c.name in die)} cases run there"
             + (": " + ", ".join(c.name for c in ours_bad[:40]) if ours_bad else "") + "\n")
    L.append(f"\nSampled all-agree cases where the die's F differs from all five: {len(bad)}"
             + (": " + ", ".join(c.name for c in bad[:20]) if bad else "") + "\n")
    nr = [s for s in status.values() if "not run" in s]
    if nr: L.append(f"Not run on the die (preamble collision): {len(nr)}\n")
    with open(os.path.join(OUT, "report.md"), "w") as f: f.write("\n".join(L) + "\n")
    print("\n".join(L))

def corpus(a, die_set, sample, res, die, status):
    keep, seen = [], collections.Counter()
    for c in die_set:
        if c.name not in die: continue
        d = die[c.name]
        differ = tuple(e for e, v in comparable(res, c.name).items() if (v[0] & 0xFF) != (d[0] & 0xFF))
        ours_bad = res[c.name]["rz80"][:13] != d[:13]
        key = (c.label, c.kind, differ, ours_bad)
        if seen[key] < a.per_pattern:
            seen[key] += 1; keep.append(c)
    keep += [c for c in sample if c.name in die]
    os.makedirs(a.corpus, exist_ok=True)
    with open(os.path.join(a.corpus, "tests.in"), "w") as f:
        for c in keep: f.write(c.text() + "\n")
    with open(os.path.join(a.corpus, "tests.expected"), "w") as f:
        for c in keep:
            d, o = die[c.name], res[c.name]["rz80"]
            # IFF1, IFF2 and IM have no reader in perfectz80: our model's
            f.write(f"{c.name}\n" + " ".join(f"{v:04x}" for v in d[:13]) +
                    f"\n{d[13]:02x} {d[14]:02x} {o[16]} {o[17]} {o[18]} {d[16]} {d[15]}\n\n")
    print(f"corpus: {len(keep)} cases in {a.corpus}")

if __name__ == "__main__":
    main()
