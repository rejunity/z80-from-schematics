#!/usr/bin/env python3
"""engines.py - chip.py's transistor-level decks in other simulators than
ngspice, and their results against ngspice's.

chip.py writes an ngspice deck for each window of the program (its
simulate step); this script takes that deck as it is and

  xyce      translates it, the PDK's model library and the extracted chip,
            for Xyce (Sandia; KLU2 through Amesos2), runs it, and writes
            the pins in chip.py's own .dat format beside ngspice's, so
            chip.py's checks read either
  compare   two runs of one window: every pin's largest difference where
            the reference has settled, every output edge's 1.5 V crossing
            against the reference's, and the pins against the RTL at every
            clock edge (chip.py's check_functional)

usage: engines.py xyce DECK.sp [--out DIR] [--xyce PATH]
       engines.py compare REF.dat OTHER.dat --ref ref.txt [--from NS]

The translation changes no physics at the PDK's nominal settings, with one
exception, the diodes (below); every rule is in XYCE_* and in
asic/spice/README.md ("Other simulators").
"""
import argparse, json, os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import chip  # noqa: E402

NIXPKGS = "github:NixOS/nixpkgs/494ce7fd23ff6a5dff39e1fb11e9b6f2ac74bf25"   # pads.py's: Xyce 7.10.0


# --------------------------------------------------------------- libraries
def resolve(path, section=None, seen=None):
    """the lines of an ngspice library: a whole file (.include) or one
    section of it (.lib file section), every .include / .lib inside
    resolved in turn. The PDK's sections name their own file
    'sm141064.spice' (it is sm141064.ngspice): a file that does not exist
    beside the including one is the including file itself."""
    seen = seen if seen is not None else []
    text = open(path).read().replace("\r", "")
    lines = []
    for l in re.sub(r"\n\+", " ", text).splitlines():
        lines.append(l)
    if section is not None:
        out, inside = [], False
        for l in lines:
            t = l.split()
            if t and t[0].lower() == ".lib" and len(t) == 2 and t[1].lower() == section.lower():
                inside = True
                continue
            if t and t[0].lower() == ".endl" and inside:
                break
            if inside:
                out.append(l)
        if not inside:
            sys.exit(f"engines: no section {section} in {path}")
        lines = out
    key = (os.path.abspath(path), section)
    if key in seen:
        return []
    seen.append(key)
    res = []
    for l in lines:
        t = l.split()
        kw = t[0].lower() if t else ""
        if kw in (".include", ".inc") or (kw == ".lib" and len(t) >= 3):
            f = t[1].strip("'\"")
            f = f if os.path.isabs(f) else os.path.join(os.path.dirname(path), f)
            if not os.path.exists(f):
                f = path
            res += resolve(f, t[2] if kw == ".lib" else None, seen)
        elif kw in (".lib", ".endl"):
            continue                      # a whole file's section marks
        else:
            res.append(l)
    return res


# ---------------------------------------------------------------- for Xyce
# The PDK's diodes are ngspice level 3 (area / perimeter scaling, its
# temperature models). Xyce's level 2 is the same junction (IS, JSW, CJO,
# CJP, MJ, MJSW, PB, PHP, N, RS, BV, IBV, IKF) without ngspice's temperature
# coefficients of the capacitances and potentials and its tunnelling
# current: XYCE_DIODE_DROP. At 25 C (tt, tref = 25) the coefficients do
# nothing; at ss (125 C) and ff (-40 C) the pads' ESD diodes and the
# n+ resistors' junctions keep their 25 C capacitances.
XYCE_DIODE_RENAME = {"ik": "ikf", "tref": "tnom", "cj": "cjo", "cjsw": "cjp", "pb": "vj", "mj": "m"}
XYCE_DIODE_DROP = {"cta", "ctp", "tpb", "tphp", "tlev", "tlevc", "jtun", "jtunsw", "ntun", "xtitun",
                   "tcv", "trs2",
                   "ikr"}     # ngspice disables it for these models: "IKR too small - model effect disabled"
# resistors' and capacitors' noise and model entries Xyce's R / C have not
XYCE_RC_DROP = {"af", "kf", "noise", "lf", "wf", "ef"}
# BSIM4 instances: mulu0 (1 - mis_k * sw_stat_mismatch) is exactly 1 with
# the PDK's mismatch off (design.ngspice: sw_stat_mismatch = 0); Xyce's
# BSIM4 has no such instance parameter
XYCE_MOS_DROP = {"mulu0"}


def quoted(s):
    """'expr' -> {expr}"""
    return re.sub(r"'([^']*)'", r"{\1}", s)


def params_split(tokens):
    i = next((k for k, x in enumerate(tokens) if "=" in x), len(tokens))
    return tokens[:i], tokens[i:]


def param_tokens(s):
    """name=value pairs of a line's tail, values in {} or bare"""
    s = re.sub(r"\s*=\s*", "=", s)
    out, k = [], 0
    while k < len(s):
        m = re.match(r"\s*([A-Za-z_][\w.]*)=", s[k:])
        if not m:
            rest = s[k:].strip()
            if rest:
                out.append((None, rest))
            break
        k += m.end()
        if k < len(s) and s[k] == "{":
            d, j = 0, k
            while j < len(s):
                d += {"{": 1, "}": -1}.get(s[j], 0)
                j += 1
                if d == 0:
                    break
            out.append((m.group(1), s[k:j]))
            k = j
        else:
            m2 = re.match(r"[^\s]*", s[k:])
            out.append((m.group(1), m2.group(0)))
            k += m2.end()
    return out


NUMBER = re.compile(r"[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?(meg|[tgkmunpfa])?[a-z]*$", re.I)


def value(k, v, default=False):
    """a parameter's value for Xyce: numbers as they are, names and
    expressions in {} (Xyce reads a bare name as a string); a .subckt's
    self-referring default (lr=lr: none) as 0, every instance sets it"""
    if v.startswith("{") or NUMBER.match(v):
        return v
    if default and k and v.lower() == k.lower():
        return "0"
    return "{" + v + "}"


def pairs_str(pairs, default=False):
    return " ".join(f"{k}={value(k, v, default)}" if k else v for k, v in pairs)


def xyce_lib(lines, notes):
    """the resolved library for Xyce: {expr}, PARAMS: on .subckt lines,
    temper -> TEMP, agauss(mean, ...) -> its mean (its every use is
    multiplied by sw_stat_mismatch or sw_stat_global, 0), the diodes'
    level and parameters, the instance parameters Xyce's devices have not"""
    out, model = [], None
    for l in lines:
        l = quoted(l)
        l = re.sub(r"\btemper\b", "TEMP", l, flags=re.I)
        l = re.sub(r"agauss\(\s*([^,()]+)\s*,[^()]*\)", r"(\1)", l, flags=re.I)
        t = l.split()
        if not t or t[0].startswith("*"):
            continue
        kw = t[0].lower()
        if kw == ".subckt":
            head, _ = params_split(t)
            tail = l.split(None, len(head))[len(head)] if len(t) > len(head) else ""
            l = " ".join(head) + (" PARAMS: " + pairs_str(param_tokens(tail), True) if tail else "")
        elif kw == ".model":
            name, typ = t[1], t[2].lower()
            pairs = param_tokens(l.split(None, 3)[3] if len(t) > 3 else "")
            if typ == "d":
                lv = {k.lower(): v for k, v in pairs if k}
                if lv.get("level") == "3":
                    keep = []
                    for k, v in pairs:
                        kl = (k or "").lower()
                        if kl in XYCE_DIODE_DROP:
                            notes.setdefault("diode parameters dropped", set()).add(kl)
                            continue
                        if kl == "level":
                            v = "2"
                        keep.append((XYCE_DIODE_RENAME.get(kl, k), v))
                    pairs = keep
            elif typ in ("r", "c"):
                pairs = [(k, v) for k, v in pairs if (k or "").lower() not in XYCE_RC_DROP]
            l = f".model {name} {typ} " + pairs_str(pairs)
        elif kw[0] == "m":
            nodes, _ = params_split(t)
            tail = l.split(None, len(nodes))[len(nodes)] if len(t) > len(nodes) else ""
            pairs = [(k, v) for k, v in param_tokens(tail) if (k or "").lower() not in XYCE_MOS_DROP]
            l = " ".join(nodes) + " " + pairs_str(pairs)
        out.append(l)
    return out


SAFE = re.compile(r"[A-Za-z0-9_]+$")
CHARS = {"/": "_s_", "[": "_l_", "]": "_r_", "<": "_lt_", ">": "_gt_", ":": "_c_", "#": "_h_", ".": "_d_",
         "!": "_x_", "$": "_dl_", "%": "_pc_", "-": "_m_", "+": "_p_"}


def safe(n):
    """a node or instance name Xyce reads as one name (':' is its
    hierarchy's separator; brackets, '/', '<>' out too)"""
    return n if SAFE.match(n) else "".join(CHARS.get(c, c) for c in n)


def xyce_netlist(src, dst):
    """chip.py's prepared netlist (chip.net) with every name safe(); the
    renaming is one to one (checked)"""
    names = {}
    out = []
    for l in open(src):
        t = l.split()
        if not t or t[0].startswith("*"):
            continue
        kw = t[0].lower()
        if kw == ".subckt":
            nodes, prm = params_split(t[2:])
            t = [t[0], t[1]] + [safe(x) for x in nodes] + prm
        elif kw == ".ends":
            pass
        elif kw[0] == "x":
            nodes, prm = params_split(t[1:])
            t = [safe(t[0])] + [safe(x) for x in nodes[:-1]] + [nodes[-1]] + prm
        elif kw[0] in "cdr":
            t = [safe(t[0]), safe(t[1]), safe(t[2])] + t[3:]
        else:
            sys.exit(f"engines: no rule for {t[0]} in {src}")
        for x in t:
            s = safe(x)
            if names.setdefault(s.upper(), x.upper()) != x.upper() and s != x:
                sys.exit(f"engines: {x} and {names[s.upper()]} would both be {s}")
        out.append(" ".join(t))
    open(dst, "w").write("\n".join(out) + "\n")


def xyce_ic(src, dst):
    """ngspice's wrnodev state (.ic v(xchip.xcell.node) = V) for Xyce:
    ':' between the levels, every name safe(); the PDK's moscaps' and
    other subcircuits' own nodes inside the chip's cells as well"""
    out = []
    for l in open(src):
        m = re.match(r"\s*\.ic\s+v\((.*)\)\s*=\s*(\S+)", l)
        if m:
            out.append(f".ic v({':'.join(safe(x) for x in m.group(1).split('.'))})={m.group(2)}")
    open(dst, "w").write("\n".join(out) + "\n")
    return len(out)


def xyce_deck(sp, out_dir):
    """chip.py's ngspice deck -> (Xyce deck, the .prn it writes, ngspice's
    .dat name for it, notes)"""
    os.makedirs(out_dir, exist_ok=True)
    notes = {}
    lib, body, ctl, in_ctl = [], [], [], False
    raw = re.sub(r"\n\+", " ", open(sp).read()).splitlines()
    for l in raw:
        t = l.split()
        if not t:
            continue
        kw = t[0].lower()
        if kw == ".control":
            in_ctl = True
            continue
        if kw == ".endc":
            in_ctl = False
            continue
        if in_ctl:
            ctl.append(l)
        elif kw == ".include" and t[1].endswith("chip.net"):
            net = os.path.join(out_dir, "chip.xyce.net")
            xyce_netlist(t[1], net)
            body.append(f".include {net}")
        elif kw == ".include" and t[1].endswith(".ic"):
            ic = os.path.join(out_dir, os.path.basename(t[1]).replace(".ic", ".xyce.ic"))
            notes["initial node voltages"] = xyce_ic(t[1], ic)
            body.append(f".include {ic}")
        elif kw in (".include", ".lib"):
            lib += resolve(t[1], t[2] if kw == ".lib" and len(t) > 2 else None)
        elif kw == ".temp":
            body.append(f".options device temp={t[1]}")
        elif kw == ".options":
            # klu: Xyce's serial default; rshunt (1 Tohm on every node) -> 1 Tohm
            # on the nodes without a DC path to ground (the rest have one)
            body += [".options linsol type=klu",
                     ".preprocess addresistors nodcpath 1e12", ".preprocess addresistors oneterminal 1e12"]
        elif kw == ".model" and len(t) > 2 and t[2].lower() == "sw":
            p = dict(x.split("=") for x in t[3:])
            vt, vh = float(p.get("vt", 0)), float(p.get("vh", 0))
            body.append(f".model {t[1]} vswitch von={vt + vh:g} voff={vt - vh:g} ron={p['ron']} roff={p['roff']}")
        elif kw == ".end":
            pass
        else:
            body.append(l)
    tran = next(l.split() for l in ctl if l.split()[0] == "tran")
    wr = next(l.split() for l in ctl if l.split()[0] == "wrdata")
    dat, cols = wr[1], wr[2:]
    prn = os.path.join(out_dir, os.path.basename(dat).replace(".dat", ".prn"))
    libf = os.path.join(out_dir, "models.xyce.lib")
    open(libf, "w").write("\n".join(xyce_lib(lib, notes)) + "\n")
    deck = [open(sp).readline().rstrip(), f".include {libf}"] + body + [
        f".tran {tran[1]} {tran[2]} {tran[3]} {tran[4]} UIC",
        f".print tran format=noindex file={prn} " + " ".join(cols),
        ".end"]
    xsp = os.path.join(out_dir, os.path.basename(sp).replace(".sp", ".xyce.sp"))
    open(xsp, "w").write("\n".join(deck) + "\n")
    notes = {k: sorted(v) if isinstance(v, set) else v for k, v in notes.items()}
    return xsp, prn, dat, notes


def prn_to_dat(prn, dat, t_from=0.0):
    """Xyce's .print (TIME V(...)) as chip.py's .dat ( time v(...)), in the
    window's own time as ngspice's wrdata writes it"""
    lines = [l.split() for l in open(prn) if l.strip() and not l.startswith("End of")]
    head = ["time"] + [h.lower() for h in lines[0][1:]]
    rows = [" " + " ".join(f"{float(x):.7e}" for x in r) for r in lines[1:]]
    open(dat, "w").write(" " + " ".join(head) + "\n" + "\n".join(rows) + "\n")
    return len(rows)


def run_xyce(sp, out_dir, xyce=None):
    xsp, prn, dat, notes = xyce_deck(sp, out_dir)
    if not xyce:
        p = subprocess.run([os.path.expanduser("~/.nix-profile/bin/nix"), "build", f"{NIXPKGS}#xyce",
                            "--print-out-paths", "--no-link"], capture_output=True, text=True)
        xyce = os.path.join(p.stdout.strip().splitlines()[-1], "bin", "Xyce")
    log = xsp.replace(".sp", ".log")
    t0 = time.time()
    p = subprocess.run([xyce, "-l", log, xsp], cwd=out_dir, capture_output=True, text=True)
    wall = time.time() - t0
    res = {"engine": "Xyce", "deck": xsp, "wall_s": round(wall, 1), "rc": p.returncode, "notes": notes}
    if os.path.exists(prn):
        res["points"] = prn_to_dat(prn, os.path.join(out_dir, os.path.basename(dat)))
        res["dat"] = os.path.join(out_dir, os.path.basename(dat))
    txt = open(log).read() if os.path.exists(log) else p.stdout + p.stderr
    for k, pat in (("accepted", r"Number Successful Steps Taken:\s*(\d+)"),
                   ("rejected", r"Number Failed Steps Attempted:\s*(\d+)"),
                   ("newton", r"Number Nonlinear Iterations:\s*(\d+)"),
                   ("unknowns", r"Total Devices\s*(\d+)")):
        m = re.search(pat, txt)
        if m:
            res[k] = int(m.group(1))
    print(json.dumps(res, indent=1))
    return res


# ----------------------------------------------------------------- compare
def compare(ref_dat, dat, ref_txt, t_from=0.0):
    """two runs of one window against each other and against the RTL"""
    edges = chip.edges_of(open(ref_txt).read())
    tr, wr, _ = chip.load(ref_dat)
    to, wo, _ = chip.load(dat)
    t_end = min(tr[-1], to[-1]) + t_from
    tr = [x + t_from for x in tr] if tr[0] < t_from - 1 else tr
    to = [x + t_from for x in to] if to[0] < t_from - 1 else to
    vdd = max(wr["M1_n"])
    # settled: the reference moves less than 1 mV/ns there
    worst = {}
    for p in wr:
        best = (0.0, None)
        for k in range(1, len(tr) - 1, max(1, len(tr) // 4000)):
            if tr[k] < 25:
                continue
            if abs(wr[p][k + 1] - wr[p][k - 1]) / max(tr[k + 1] - tr[k - 1], 1e-6) > 1e-3:
                continue
            d = abs(chip.value_at(to, wo[p], tr[k]) - wr[p][k])
            if d > best[0]:
                best = (d, tr[k])
        worst[p] = best
    # every output edge's 1.5 V crossing
    shifts = []
    for p in wr:
        if p in chip.INPUTS:
            continue
        for k in range(1, len(tr)):
            a, b = wr[p][k - 1], wr[p][k]
            if (a - chip.OUT_LEVEL) * (b - chip.OUT_LEVEL) < 0 and tr[k] > 25:
                tx = tr[k - 1] + (chip.OUT_LEVEL - a) * (tr[k] - tr[k - 1]) / (b - a)
                c = chip.crossing(to, wo[p], chip.OUT_LEVEL, b > a, tx - 30, within=60)
                shifts.append((p, tx, None if c is None else c - tx))
    bad_r = chip.check_functional(tr, wr, edges)
    bad_o = chip.check_functional(to, wo, edges)
    found = [s for s in shifts if s[2] is not None]
    res = {"window_ns": [round(t_from, 1), round(t_end, 1)],
           "max_settled_dV": max((v[0] for v in worst.values()), default=0.0),
           "worst_pin": max(worst, key=lambda p: worst[p][0]) if worst else None,
           "edges": len(shifts), "edges_missing": len(shifts) - len(found),
           "max_edge_shift_ns": max((abs(s[2]) for s in found), default=0.0),
           "mismatches_ref": len(bad_r), "mismatches_other": len(bad_o)}
    print(json.dumps(res, indent=1))
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("xyce")
    a.add_argument("deck")
    a.add_argument("--out")
    a.add_argument("--xyce")
    a.add_argument("--translate-only", action="store_true")
    c = sub.add_parser("compare")
    c.add_argument("ref")
    c.add_argument("other")
    c.add_argument("--ref-txt", required=True)
    c.add_argument("--from", dest="t_from", type=float, default=0.0)
    a = ap.parse_args()
    if a.cmd == "xyce":
        out = a.out or os.path.join(os.path.dirname(os.path.abspath(a.deck)), "xyce")
        if a.translate_only:
            print(xyce_deck(a.deck, out))
            return 0
        return 0 if run_xyce(a.deck, out, a.xyce)["rc"] == 0 else 1
    compare(a.ref, a.other, a.ref_txt, a.t_from)
    return 0


if __name__ == "__main__":
    sys.exit(main())
