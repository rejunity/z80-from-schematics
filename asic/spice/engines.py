#!/usr/bin/env python3
"""engines.py - chip.py's transistor-level decks in other simulators than
ngspice, and their results against ngspice's.

chip.py writes an ngspice deck for each window of the program (its
simulate step); this script takes that deck as it is and

  xyce      translates it, the PDK's model library and the extracted chip,
            for Xyce (Sandia), runs it on its fast path -- UMFPACK with
            METIS ordering, one thread per process, the MPI build over 4
            ranks (asic/spice/nix/xyce-threaded.nix; README.md, "Profile")
            -- and writes the pins in chip.py's own .dat format beside
            ngspice's, so chip.py's checks read either
  compare   two runs of one window: every pin's largest difference where
            the reference has settled, every output edge's 1.5 V crossing
            against the reference's, and the pins against the RTL at every
            clock edge (chip.py's check_functional)

  cells     a few of chip_top's own standard cells (chains of inv_1 /
            nand2_1, a dffrnq_1 shift register) in ngspice, Xyce and VAJAX
            (BSIM4 as Verilog-A): wall time, steps, Newton iterations, and
            each against ngspice

usage: engines.py xyce DECK.sp [--out DIR] [--xyce PATH] [--ranks N] [--solver umfpack|klu]
       engines.py compare REF.dat OTHER.dat --ref ref.txt [--from NS]
       engines.py cells inv|nand|dff N --net chip.net [--engines ngspice,xyce] [--check]
                 [--va bsim4.va --python VENV/bin/python]   (VAJAX)

The translation changes no physics at the PDK's nominal settings, with one
exception, the diodes (below); every rule is in XYCE_* and in
asic/spice/README.md ("Other simulators").
"""
import argparse, json, os, re, shutil, subprocess, sys, time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import chip  # noqa: E402

NIXPKGS = "github:NixOS/nixpkgs/494ce7fd23ff6a5dff39e1fb11e9b6f2ac74bf25"   # pads.py's: Xyce 7.10.0

# Xyce's fast path (README.md, "Profile"): UMFPACK with METIS ordering
# (Amesos' UMFPACK, patched in xyce-threaded.nix to take METIS), one
# OpenMP and one BLAS thread in every process (Epetra's OpenMP threads
# otherwise spin in barriers: 5 %), and with the MPI build the devices'
# loads over XYCE_RANKS ranks, UMFPACK on rank 0 (slice B 808 s with KLU
# -> 259 s serial -> 176 s over 4 ranks, the same outputs). KLU stays the
# reference (solver="klu").
XYCE_NIX = os.path.join(HERE, "nix", "xyce-threaded.nix")
XYCE_SOLVERS = {"umfpack": ".options linsol type=umfpack", "klu": ".options linsol type=klu"}
XYCE_ENV = {"AMESOS_UMFPACK_ORDERING": "metis", "OMP_NUM_THREADS": "1", "OPENBLAS_NUM_THREADS": "1"}
XYCE_MPI_ENV = {"OMPI_MCA_rmaps_base_oversubscribe": "1",          # OpenMPI 5: as many ranks as asked
                "PRTE_MCA_rmaps_default_mapping_policy": ":oversubscribe"}
XYCE_RANKS = 4

# Xyce against ngspice, what fails (chip.py's windows; tt measured, README.md
# "Other simulators"): an output edge missing or more than 0.2 ns off (82 ps
# the largest, slice C), a settled output more than 0.25 V off (0.15 V the
# largest: D[5] while the bench's memory switch turns on, ngspice's sw with
# hysteresis against Xyce's smooth vswitch), any pin against the RTL in either
# engine, or no edge to compare at all
WINDOW_TOL = {"max_edge_shift_ns": 0.2, "max_settled_dV": 0.25}
# the cells (engines.py cells; the PDK's own cell netlists or chip_top's
# extracted ones): an edge more than 50 ps off (25 ps the largest, 10 inv_1),
# a settled node more than 2 mV off (0.34 mV), a missing edge, no edge at all
CELLS_TOL = {"max_edge_ps": 50.0, "max_settled_mV": 2.0}


# --------------------------------------------------------------- libraries
def resolve(path, section=None, seen=None):
    """the lines of an ngspice library: a whole file (.include) or one
    section of it (.lib file section), every .include / .lib inside
    resolved in turn. The PDK's sections name their own file
    'sm141064.spice' (it is sm141064.ngspice): a file that does not exist
    beside the including one is the including file itself."""
    seen = seen if seen is not None else []
    text = open(path).read().replace("\r", "")
    # as ngspice: comment and blank lines go first, so a '+' line after
    # them continues the line before them (test_engines.py)
    text = "\n".join(l for l in text.splitlines() if l.strip() and not l.lstrip().startswith("*"))
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


def xyce_deck(sp, out_dir, options=(), uic=True, solver="umfpack"):
    """chip.py's ngspice deck -> (Xyce deck, the .prn it writes, ngspice's
    .dat name for it, notes); solver: XYCE_SOLVERS, unless options name one"""
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
            # the linear solver (XYCE_SOLVERS); rshunt (1 Tohm on every node) ->
            # 1 Tohm on the nodes without a DC path to ground (the rest have one)
            body += ([] if any(o.lower().startswith(".options linsol") for o in options)
                     else [XYCE_SOLVERS[solver]]) + [
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
    deck = [open(sp).readline().rstrip(), f".include {libf}"] + body + list(options) + [
        f".tran {tran[1]} {tran[2]} {tran[3]} {tran[4]}" + (" UIC" if uic else ""),
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


def xyce_bin(mpi):
    """Xyce's fast build (xyce-threaded.nix; the MPI one with mpi): $XYCE_MPI
    or $XYCE when given, else built with nix (once; CI: from its cache)"""
    given = os.environ.get("XYCE_MPI" if mpi else "XYCE")
    if given:
        return given
    nix = shutil.which("nix") or os.path.expanduser("~/.nix-profile/bin/nix")
    p = subprocess.run([nix, "build", "-f", XYCE_NIX, "--print-out-paths", "--no-link"]
                       + (["--arg", "mpi", "true"] if mpi else []), capture_output=True, text=True)
    out = p.stdout.strip().splitlines()
    if p.returncode != 0 or not out:
        sys.exit(f"engines: no Xyce from {XYCE_NIX}{' (mpi)' if mpi else ''}:\n{p.stderr[-2000:]}")
    return os.path.join(out[-1], "bin", "Xyce")


def xyce_mpirun(xyce):
    """the mpirun of the OpenMPI an MPI build of Xyce links, or None (serial)"""
    m = re.search(r"(/nix/store/[^/]*openmpi[^/]*)/lib/libmpi",
                  subprocess.run(["ldd", xyce], capture_output=True, text=True).stdout)
    return os.path.join(m.group(1), "bin", "mpirun") if m else None


def xyce_ranks(mpirun, cpus=None):
    """XYCE_RANKS with an MPI build, never more than the CPUs; 1 serial"""
    return min(XYCE_RANKS, cpus or os.cpu_count() or 1) if mpirun else 1


def xyce_command(xyce, ranks, mpirun=None):
    """(the command line, the environment it adds) for Xyce on its fast path"""
    env = dict(XYCE_ENV)
    if ranks <= 1:
        return [xyce], env
    env.update(XYCE_MPI_ENV)
    return [mpirun, "--allow-run-as-root", "-np", str(ranks), xyce], env


def run_xyce(sp, out_dir, xyce=None, ranks=None, options=(), uic=True, solver="umfpack"):
    """Xyce on chip.py's deck, its fast path: the MPI build over XYCE_RANKS
    ranks (ranks=1: the serial build), UMFPACK with METIS; xyce: a build"""
    xsp, prn, dat, notes = xyce_deck(sp, out_dir, options, uic, solver)
    xyce = xyce or xyce_bin(mpi=ranks != 1)
    mpirun = xyce_mpirun(xyce)
    ranks = ranks or xyce_ranks(mpirun)
    if ranks > 1 and not mpirun:
        sys.exit(f"engines: {xyce} is not an MPI build, and {ranks} ranks were asked for")
    cmd, env = xyce_command(xyce, ranks, mpirun)
    log = xsp.replace(".sp", ".log")
    t0 = time.time()
    p = subprocess.run(cmd + ["-l", log, xsp], cwd=out_dir, capture_output=True, text=True,
                       env=dict(os.environ, **env))
    wall = time.time() - t0
    res = {"engine": "Xyce" + (f" MPI x{ranks}" if ranks > 1 else ""), "deck": xsp, "options": list(options),
           "solver": solver, "ranks": ranks, "wall_s": round(wall, 1), "rc": p.returncode, "notes": notes}
    if os.path.exists(prn):
        res["points"] = prn_to_dat(prn, os.path.join(out_dir, os.path.basename(dat)))
        res["dat"] = os.path.join(out_dir, os.path.basename(dat))
    txt = open(log).read() if os.path.exists(log) else p.stdout + p.stderr
    for k, pat in (("accepted", r"Number Successful Steps Taken:\s*(\d+)"),
                   ("rejected", r"Number Failed Steps Attempted:\s*(\d+)"),
                   ("newton", r"Number Nonlinear Iterations:\s*(\d+)"),
                   ("devices", r"Total Devices\s*(\d+)"), ("unknowns", r"Number of Unknowns = (\d+)")):
        m = re.findall(pat, txt)       # the last: the transient's, after the DC point's
        if m:
            res[k] = int(m[-1])
    for k, pat in (("residual_load_s", r"Total Residual Load Time:\s*([\d.]+)"),
                   ("jacobian_load_s", r"Total Jacobian Load Time:\s*([\d.]+)"),
                   ("linear_s", r"Total Linear Solution Time:\s*([\d.]+)")):
        m = re.findall(pat, txt)
        if m:
            res[k] = float(m[-1])
    print(json.dumps(res, indent=1))
    return res


# ------------------------------------------------------------------ VAJAX
# VAJAX (ChipFlow) has no BSIM4 of its own; `va` is a BSIM4 Verilog-A with
# module bsim4va, compiled by VAJAX's openvaf_jax -- not part of this
# repository (the Berkeley-derived VA-BSIM48 that OpenVAF's tests carry is
# CC BY-NC-SA 4.0). The PDK's cards go in as they are, every expression
# evaluated at the corner, with BSIM4_VA_FIXES for what that port gets
# wrong; VAJAX itself needs the local patches README.md lists
# ("VAJAX"), and VAJAX_ENV turns on the one that is a switch.
BSIM4_VA_FIXES = {
    # VA-BSIM48 never defaults wk2 when it is not given (lk2, pk2 it does):
    # k2 = k2 + wk2 / Weff with wk2 = its NOT_GIVEN mark, -12345789, so
    # k2ox ~ -1e7 and Abulk clamps at 0.05; ngspice's C code takes 0
    "wk2": 0.0,
}
VAJAX_ENV = {"VAJAX_AD_JAC": "bsim4va", "JAX_PLATFORMS": "cpu"}
BSIM4_MODELS = {"nfet_05v0": "nfet_06v0", "nfet_06v0": "nfet_06v0",
                "pfet_05v0": "pfet_06v0", "pfet_06v0": "pfet_06v0"}
SPICE_SCALE = {"t": 1e12, "g": 1e9, "meg": 1e6, "k": 1e3, "m": 1e-3, "u": 1e-6, "n": 1e-9, "p": 1e-12,
               "f": 1e-15, "a": 1e-18}


def spice_num(v):
    m = re.match(r"([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)(meg|[tgkmunpfa])?", v.strip(), re.I)
    return float(m.group(1)) * SPICE_SCALE.get((m.group(2) or "").lower(), 1.0)


def va_params(va):
    """the parameter names a Verilog-A module declares"""
    return {n.lower() for n in re.findall(r"parameter\s+(?:real|integer)\s+(\w+)", open(va).read())}


def bsim4_bins(pdk_ngspice, va, corner="typical"):
    """every bin of the PDK's 6 V MOSFETs (nfet_06v0.N / pfet_06v0.N) as
    the VA's parameters: {model: [(lmin, lmax, wmin, wmax, params)]}, the
    card's expressions evaluated with the PDK's .params at `corner`
    (mismatch and global variation off, as design.ngspice sets them), and
    the card parameters the VA has not"""
    lines = resolve(os.path.join(pdk_ngspice, "design.ngspice"))
    lines += resolve(os.path.join(pdk_ngspice, "sm141064.ngspice"), corner)
    lines += resolve(os.path.join(pdk_ngspice, "sm141064.ngspice"), "noise_corner")
    env, insub, last = {}, False, None
    for l in lines:
        t = l.split()
        if not t:
            continue
        if t[0].lower() == ".subckt":
            insub = True
        elif t[0].lower() == ".ends":
            insub = False
        # .param, and its ' +name=...' lines after blank ones (noise_corner)
        elif (t[0].lower() == ".param" or (t[0].startswith("+") and last == ".param")) and not insub:
            tail = l.split(None, 1)[1] if t[0].lower() == ".param" and len(t) > 1 else \
                l.strip()[1:] if t[0].startswith("+") else ""
            for k, v in param_tokens(quoted(tail)):
                if k:
                    env[k.lower()] = v
        if not t[0].startswith("+"):
            last = t[0].lower()

    def num(v):
        v = v.strip("{}")
        try:
            return spice_num(v) if NUMBER.match(v) else float(v)
        except (ValueError, AttributeError):
            pass
        expr = re.sub(r"[A-Za-z_]\w*", lambda m: f"({num(env[m.group(0).lower()])})"
                      if m.group(0).lower() in env else m.group(0), v)
        return float(eval(expr, {"__builtins__": {}}, {}))
    known = va_params(va)
    bins, dropped = {}, set()
    for l in lines:
        m = re.match(r"\s*\.model\s+(nfet_06v0|pfet_06v0)\.(\d+)\s+(nmos|pmos)\s+(.*)", l, re.I)
        if not m:
            continue
        p = {"type": 1.0 if m.group(3).lower() == "nmos" else -1.0}
        for k, v in param_tokens(quoted(m.group(4))):
            if not k or k.lower() in ("level", "version"):
                continue
            if k.lower() not in known:
                dropped.add(k.lower())
                continue
            p[k.lower()] = num(v)
        for k, v in BSIM4_VA_FIXES.items():
            p.setdefault(k, v)
        bins.setdefault(m.group(1).lower(), []).append((p["lmin"], p["lmax"], p["wmin"], p["wmax"], p))
    return bins, sorted(dropped)


def pick_bin(bins, l, w):
    """ngspice's choice: lmin <= L < lmax, wmin <= W < wmax"""
    for k, (lmin, lmax, wmin, wmax, _) in enumerate(bins):
        if lmin <= l < lmax and wmin <= w < wmax:
            return k
    sys.exit(f"engines: no bin for L={l:g} W={w:g}")


def net_subckts(net):
    """chip.py's chip.net: {cell: (ports, body lines)}"""
    cells, cur = {}, None
    for l in open(net):
        t = l.split()
        if not t or t[0].startswith("*"):
            continue
        if t[0].lower() == ".subckt":
            cur = t[1]
            cells[cur] = (t[2:], [])
        elif t[0].lower() == ".ends":
            cur = None
        elif cur:
            cells[cur][1].append(l.strip())
    return cells


CELL = "gf180mcu_fd_sc_mcu7t5v0__"
VDD = 5.0


def cell_circuit(kind, n):
    """a few of chip_top's own standard cells, each a list of (instance,
    cell, {port: node}); the stimuli; the nodes to compare; the time span"""
    inst = []
    p = lambda **kw: dict(kw, VDD="vdd", VSS="0", VNW="vdd", VPW="0")
    if kind == "inv":                   # a chain of n inv_1
        for k in range(n):
            inst.append((f"i{k}", CELL + "inv_1", p(I=f"n{k}", ZN=f"n{k + 1}")))
        src = {"n0": [(0, 0), (2, 0), (2.5, VDD), (12, VDD), (12.5, 0)]}
        probe = [f"n{k}" for k in sorted({1, max(1, n // 2), n})]
        return inst, src, probe, 25.0
    if kind == "nand":                  # n nand2_1 in a chain, A2 tied high
        for k in range(n):
            inst.append((f"g{k}", CELL + "nand2_1", p(A1=f"n{k}", A2="vdd", ZN=f"n{k + 1}")))
        src = {"n0": [(0, 0), (2, 0), (2.5, VDD), (12, VDD), (12.5, 0)]}
        return inst, src, [f"n{k}" for k in sorted({1, n})], 25.0
    if kind == "dff":                   # an n-bit shift register of dffrnq_1, 100 MHz clock
        for k in range(n):
            inst.append((f"f{k}", CELL + "dffrnq_1", p(D=f"q{k}", Q=f"q{k + 1}", CLK="clk", RN="rn")))
        clk = [(0, 0)]
        for c in range(6):
            t0 = 5 + 10 * c
            clk += [(t0, 0), (t0 + 0.5, VDD), (t0 + 5, VDD), (t0 + 5.5, 0)]
        src = {"clk": clk, "rn": [(0, 0), (1, 0), (1.5, VDD)], "q0": [(0, 0), (8, 0), (8.5, VDD), (18, VDD), (18.5, 0)]}
        return inst, src, [f"q{k}" for k in range(1, n + 1)], 60.0
    sys.exit(f"engines: no circuit {kind}")


def cells_decks(kind, n, net, pdk_ngspice, va, out_dir, corner="typical", temp=25):
    """one circuit of chip_top's cells as an ngspice deck (the PDK's own
    BSIM4 4.5 / 4.6), a Xyce deck from it (xyce_deck) and a VAJAX .sim
    (the VA, one model per bin), flattened, every transistor's L, W, AD,
    AS, PD, PS as extracted; returns the paths and the probes"""
    os.makedirs(out_dir, exist_ok=True)
    cells = net_subckts(net)
    inst, src, probe, tstop = cell_circuit(kind, n)
    # VAJAX's transient finds its DC point by plain Newton from mid-rail
    # (its op analysis's homotopy is not used there): on a long chain it
    # diverges, so circuits past 150 transistors start from 0 V (UIC) in
    # every engine, their stimuli 5 ns later
    uic = sum(1 for _, c, _ in inst for l in cells[c][1] if l[0] in "Xx") > 150
    if uic:
        src = {s: [(0, pts[0][1])] + [(a + 5, b) for a, b in pts[1:]] for s, pts in src.items()}
        tstop += 5
    used = sorted({c for _, c, _ in inst})
    tag = f"{kind}{n}"
    # ngspice: the cells' own subcircuits from chip.net (named *chip.net so
    # xyce_deck translates them as chip.py's netlist)
    cnet = os.path.join(out_dir, "cells.chip.net")
    with open(cnet, "w") as f:
        for c in used:
            f.write(f".subckt {c} {' '.join(cells[c][0])}\n" + "\n".join(cells[c][1]) + "\n.ends\n")

    def pwl(pts, unit="n"):
        return " ".join(f"{a:g}{unit} {b:g}" for a, b in pts)
    t = pdk_ngspice
    dat = os.path.join(out_dir, f"{tag}_ng.dat")
    sp = [f"* {tag}: chip_top's cells, GF180 {corner}, engines.py cells",
          f".include {t}/design.ngspice", f".lib {t}/sm141064.ngspice {corner}", f".include {cnet}",
          f".temp {temp}", ".options klu", f"V_vdd vdd 0 {VDD}"]
    sp += [f"V_{s} {s} 0 PWL({pwl(pts)})" for s, pts in src.items()]
    for name, c, conn in inst:
        sp.append(f"X{name} " + " ".join(conn[port] for port in cells[c][0]) + f" {c}")
    sp += [".control", "set wr_singlescale", "set wr_vecnames", "option numdgt=7",
           f"tran 0.01n {tstop:g}n 0 0.1n" + (" uic" if uic else ""),
           f"wrdata {dat} " + " ".join(f"v({x})" for x in probe),
           "rusage all", "quit", ".endc", ".end"]
    spf = os.path.join(out_dir, f"{tag}.sp")
    open(spf, "w").write("\n".join(sp) + "\n")
    if not va:                          # ngspice and Xyce only
        return {"sp": spf, "sim": None, "dat": dat, "probe": probe, "tstop_ns": tstop, "uic": uic,
                "transistors": sum(1 for _, c, _ in inst for l in cells[c][1] if l[0] in "XxMm"),
                "caps": sum(1 for _, c, _ in inst for l in cells[c][1] if l[0] in "Cc"),
                "bins": [], "card_params_dropped": []}
    # VAJAX: flat, the VA's models one per bin
    bins, dropped = bsim4_bins(pdk_ngspice, va, corner)
    shutil.copy(va, os.path.join(out_dir, "bsim4va.va"))
    used_bins, body, ncap, nmos = set(), [], 0, 0
    for name, c, conn in inst:
        ports = dict(zip(cells[c][0], [conn[p] for p in cells[c][0]]))
        node = lambda x: "0" if x == "0" else ports.get(x, f"{name}_{x}")
        for l in cells[c][1]:
            tk = l.split()
            if tk[0][0] in "Xx" and tk[5] in BSIM4_MODELS:
                kv = {k.lower(): spice_num(v) for k, v in (x.split("=") for x in tk[6:])}
                model = BSIM4_MODELS[tk[5]]
                b = pick_bin(bins[model], kv["l"], kv["w"])
                used_bins.add((model, b))
                geo = " ".join(f"{k}={kv[k]:.6g}" for k in ("w", "l", "ad", "as", "pd", "ps") if k in kv)
                body.append(f"m{name}_{tk[0]} ({' '.join(node(x) for x in tk[1:5])}) {model}_{b} {geo}")
                nmos += 1
            elif tk[0][0] in "Cc":
                body.append(f"c{name}_{tk[0]} ({node(tk[1])} {node(tk[2])}) cap c={spice_num(tk[3]):.6g}")
                ncap += 1
            else:
                sys.exit(f"engines: no VAJAX rule for {l}")
    sim = [f"{tag} chip_top cells GF180 {corner} engines.py cells", "ground 0", 'load "bsim4va.va"',
           'load "spice/capacitor.osdi"', "model cap sp_capacitor"]
    for model, b in sorted(used_bins):
        p = bins[model][b][4]
        sim.append(f"model {model}_{b} bsim4va ( " + " ".join(f"{k}={v:.10g}" for k, v in p.items()) + " )")
    sim.append(f"vvdd (vdd 0) vsource dc={VDD}")
    for s, pts in src.items():
        wave = ", ".join(f"{a * 1e-9:g}, {b:g}" for a, b in pts)
        sim.append(f'v{s} ({s} 0) vsource type="pwl" wave=[{wave}]')
    sim += body
    simf = os.path.join(out_dir, f"{tag}.sim")
    open(simf, "w").write("\n".join(sim) + "\n")
    return {"sp": spf, "sim": simf, "dat": dat, "probe": probe, "tstop_ns": tstop, "transistors": nmos, "uic": uic,
            "caps": ncap, "bins": sorted(f"{m}.{b}" for m, b in used_bins), "card_params_dropped": dropped}


VAJAX_RUN = r"""
import json, sys, time, numpy as np
from vajax import CircuitEngine
from vajax.analysis.transient.adaptive import AdaptiveConfig
sim, tstop, dat, probe, repeat = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4].split(","), int(sys.argv[5])
max_dt, sparse, uic = float(sys.argv[6]), sys.argv[7] == "1", sys.argv[8] == "1"
t0 = time.time(); e = CircuitEngine(sim); e.parse(); t_parse = time.time() - t0
if uic:
    e.options.icmode = "uic"
    e.analysis_params["icmode"] = "uic"      # what the transient reads
t0 = time.time()
e.prepare(t_stop=tstop, dt=1e-11, temperature=298.15, adaptive_config=AdaptiveConfig(max_dt=max_dt),
          use_sparse=sparse, backend="cpu")
t_prep = time.time() - t0
runs = []
for k in range(repeat):          # the first run JIT-compiles the transient loop
    t0 = time.time(); r = e.run_transient(); runs.append(time.time() - t0)
st = r.stats
t = np.asarray(r.times)
with open(dat, "w") as f:
    f.write(" time " + " ".join(f"v({p})" for p in probe) + "\n")
    cols = [np.asarray(r.voltages[p]) for p in probe]
    for k in range(len(t)):
        f.write(" " + " ".join(f"{x:.7e}" for x in [t[k]] + [c[k] for c in cols]) + "\n")
print(json.dumps({"parse_s": round(t_parse, 2), "prepare_s": round(t_prep, 2), "runs_s": [round(x, 3) for x in runs],
                  "accepted": int(st.get("accepted_steps", 0)), "rejected": int(st.get("rejected_steps", 0)),
                  "newton": int(st.get("total_nr_iterations", 0)), "solver": st.get("solver"),
                  "loop_wall_s": float(st.get("wall_time", 0.0))}))
"""


def run_vajax(d, python, repeat=2, threads=None, max_dt=1e-10, sparse=None):
    """VAJAX on a cells_decks() .sim: parse + model compile, prepare, then
    the transient `repeat` times (the first JIT-compiles it)"""
    env = dict(os.environ, **VAJAX_ENV)
    if threads:
        env["XLA_FLAGS"] = f"--xla_cpu_multi_thread_eigen={'true' if threads > 1 else 'false'} " \
                           f"intra_op_parallelism_threads={threads}"
        env["OMP_NUM_THREADS"] = str(threads)
    sparse = d.get("transistors", 0) > 400 if sparse is None else sparse      # VAJAX: sparse past ~1000 nodes
    dat = d["dat"].replace("_ng.dat", "_vajax.dat")
    t0 = time.time()
    p = subprocess.run([python, "-I", "-c", VAJAX_RUN, d["sim"], str(d["tstop_ns"] * 1e-9), dat,
                        ",".join(d["probe"]), str(repeat), str(max_dt), "1" if sparse else "0",
                        "1" if d.get("uic") else "0"],
                       cwd=os.path.dirname(d["sim"]),
                       capture_output=True, text=True, env=env)
    if p.returncode:
        return {"engine": "VAJAX", "error": (p.stdout + p.stderr)[-3000:]}
    res = json.loads(p.stdout.strip().splitlines()[-1])
    res.update(engine="VAJAX", wall_s=round(time.time() - t0, 1), dat=dat, threads=threads, max_dt=max_dt,
               sparse=sparse)
    return res


def run_ngspice_cells(d, threads=1):
    s = open(d["sp"]).read().replace(".control\n", f".control\nset num_threads={threads}\n", 1)
    f = d["sp"].replace(".sp", f"_ng{threads}.sp")
    open(f, "w").write(s)
    t0 = time.time()
    p = subprocess.run([chip.tool("ngspice"), "-b", f], cwd=os.path.dirname(f), capture_output=True, text=True)
    txt = p.stdout + p.stderr
    g = lambda pat: (re.findall(pat, txt) or [None])[-1]
    return {"engine": f"ngspice x{threads}", "wall_s": round(time.time() - t0, 2),
            "accepted": int(g(r"Accepted timepoints\s*=\s*(\d+)") or 0),
            "rejected": int(g(r"Rejected timepoints\s*=\s*(\d+)") or 0),
            "newton": int(g(r"Transient iterations\s*=\s*(\d+)") or 0),
            "analysis_s": float(g(r"Total analysis time \(seconds\)\s*=\s*([\d.]+)") or 0),
            "tran_s": float(g(r"Transient time\s*=\s*([\d.]+)") or 0),
            "aborted": "aborted" in txt, "dat": d["dat"]}


def load_dat(dat):
    rows = [l.split() for l in open(dat) if l.strip()]
    head = rows[0]
    data = np.array([[float(x) for x in r] for r in rows[1:]]) if rows[1:] else np.zeros((0, len(head)))
    return head, data


def cells_compare(ref, other, level=VDD / 2):
    """every probe's mid-supply crossings against the reference's, and
    the largest difference where the reference has settled"""
    hr, r = load_dat(ref)
    ho, o = load_dat(other)
    res = {"edges": 0, "missing": 0, "max_edge_ps": 0.0, "max_settled_mV": 0.0}
    for k, name in enumerate(hr[1:], 1):
        j = ho.index(name)
        tr, vr, to, vo = r[:, 0], r[:, k], o[:, 0], o[:, j]
        for i in range(1, len(tr)):
            if (vr[i - 1] - level) * (vr[i] - level) < 0:
                tx = tr[i - 1] + (level - vr[i - 1]) * (tr[i] - tr[i - 1]) / (vr[i] - vr[i - 1])
                res["edges"] += 1
                c = [to[m - 1] + (level - vo[m - 1]) * (to[m] - to[m - 1]) / (vo[m] - vo[m - 1])
                     for m in range(1, len(to)) if (vo[m - 1] - level) * (vo[m] - level) < 0
                     and (vo[m] > vo[m - 1]) == (vr[i] > vr[i - 1])]
                c = [x for x in c if abs(x - tx) < 1e-9]
                if not c:
                    res["missing"] += 1
                    continue
                res["max_edge_ps"] = max(res["max_edge_ps"], min(abs(x - tx) for x in c) * 1e12)
        # settled: the reference moves less than 1 mV within 0.5 ns either side
        lo, hi = np.searchsorted(tr, tr - 0.5e-9), np.searchsorted(tr, tr + 0.5e-9, side="right")
        for i in range(len(tr)):
            w = vr[lo[i]:hi[i]]
            if tr[i] > 0.5e-9 and tr[i] < min(tr[-1], to[-1]) - 0.5e-9 and w.max() - w.min() < 1e-3:
                res["max_settled_mV"] = max(res["max_settled_mV"], abs(np.interp(tr[i], to, vo) - vr[i]) * 1e3)
    res["max_edge_ps"] = round(res["max_edge_ps"], 2)
    res["max_settled_mV"] = round(res["max_settled_mV"], 3)
    return res


def cells(kind, n, net, pdk_ngspice, va, python, out_dir, engines_=("ngspice", "xyce", "vajax"),
          repeat=2, threads=1, xyce=None, solver="umfpack"):
    """one circuit in every engine: wall times, steps, Newton iterations,
    and each against ngspice"""
    d = cells_decks(kind, n, net, pdk_ngspice, va, out_dir)
    out = {"circuit": f"{kind}{n}", "transistors": d["transistors"], "caps": d["caps"], "bins": d["bins"], "uic": d["uic"],
           "card_params_dropped": d["card_params_dropped"], "runs": []}
    if "ngspice" in engines_:
        out["runs"].append(run_ngspice_cells(d, threads))
    if "xyce" in engines_:
        r = run_xyce(d["sp"], os.path.join(out_dir, "xyce"), xyce, ranks=1, uic=d["uic"],   # as ngspice
                     solver=solver)                                                         # and VAJAX
        log = r["deck"].replace(".sp", ".log")
        m = re.findall(r"Total Simulation Solvers Run Time:\s*([\d.]+)", open(log).read()) if os.path.exists(log) else []
        r["solvers_s"] = float(m[-1]) if m else None
        out["runs"].append(r)
    if "vajax" in engines_:
        out["runs"].append(run_vajax(d, python, repeat, threads))
    for r in out["runs"]:
        if r.get("dat") and os.path.exists(r["dat"]) and r["dat"] != d["dat"] and os.path.exists(d["dat"]):
            r["vs_ngspice"] = cells_compare(d["dat"], r["dat"])
    json.dump(out, open(os.path.join(out_dir, "result.json"), "w"), indent=1)
    print(json.dumps(out, indent=1))
    return out


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
    # settled: the reference moves less than 1 mV/ns there; outputs only (the
    # inputs are the same sources, sampled at other points)
    worst = {}
    for p in wr:
        if p in chip.INPUTS:
            continue
        best = (0.0, None)
        for k in range(1, len(tr) - 1, max(1, len(tr) // 4000)):
            if tr[k] < tr[0] + 25 or tr[k] > to[-1]:
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
            if (a - chip.OUT_LEVEL) * (b - chip.OUT_LEVEL) < 0 and tr[k] <= to[-1]:
                tx = tr[k - 1] + (chip.OUT_LEVEL - a) * (tr[k] - tr[k - 1]) / (b - a)
                near = [to[j - 1] + (chip.OUT_LEVEL - wo[p][j - 1]) * (to[j] - to[j - 1]) / (wo[p][j] - wo[p][j - 1])
                        for j in range(1, len(to)) if abs(to[j] - tx) < 30
                        and (wo[p][j - 1] - chip.OUT_LEVEL) * (wo[p][j] - chip.OUT_LEVEL) < 0
                        and (wo[p][j] > wo[p][j - 1]) == (b > a)]
                c = min(near, key=lambda x: abs(x - tx)) if near else None
                shifts.append((p, tx, None if c is None else c - tx))
    common = [e for e in edges if max(tr[0], to[0]) <= e[0] and e[0] + 110 <= min(tr[-1], to[-1])]
    bad_r = chip.check_functional(tr, wr, common)
    bad_o = chip.check_functional(to, wo, common)
    found = [s for s in shifts if s[2] is not None]
    res = {"window_ns": [round(t_from, 1), round(t_end, 1)],
           "max_settled_dV": max((v[0] for v in worst.values()), default=0.0),
           "worst_pin": max(worst, key=lambda p: worst[p][0]) if worst else None,
           "edges": len(shifts), "edges_missing": len(shifts) - len(found),
           "max_edge_shift_ns": max((abs(s[2]) for s in found), default=0.0),
           "mismatches_ref": len(bad_r), "mismatches_other": len(bad_o)}
    print(json.dumps(res, indent=1))
    return res


def window_failures(res, tol=WINDOW_TOL):
    """what fails in compare()'s result (WINDOW_TOL): [] when it passes"""
    bad = []
    if res["edges"] == 0:
        bad.append("no output edge to compare")
    if res["edges_missing"]:
        bad.append(f"{res['edges_missing']} of {res['edges']} output edges missing")
    for k, v in tol.items():
        if res[k] > v:
            bad.append(f"{k} {res[k]:g} > {v:g}")
    for k, who in (("mismatches_ref", "ngspice"), ("mismatches_other", "Xyce")):
        if res[k]:
            bad.append(f"{res[k]} pins against the RTL ({who})")
    return bad


def cells_failures(res, tol=CELLS_TOL):
    """what fails in cells_compare()'s result (CELLS_TOL): [] when it passes"""
    bad = []
    if res["edges"] == 0:
        bad.append("no edge to compare")
    if res["missing"]:
        bad.append(f"{res['missing']} of {res['edges']} edges missing")
    for k, v in tol.items():
        if res[k] > v:
            bad.append(f"{k} {res[k]:g} > {v:g}")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("xyce")
    a.add_argument("deck")
    a.add_argument("--out")
    a.add_argument("--xyce")
    a.add_argument("--translate-only", action="store_true")
    a.add_argument("--ranks", type=int, help=f"MPI processes (default: {XYCE_RANKS} with the MPI build)")
    a.add_argument("--solver", default="umfpack", choices=sorted(XYCE_SOLVERS))
    a.add_argument("--option", action="append", default=[], help="an extra deck line, e.g. '.options timeint erroption=1'")
    v = sub.add_parser("cells", help="chip_top's standard cells in ngspice, Xyce and VAJAX (BSIM4 Verilog-A)")
    v.add_argument("kind", choices=("inv", "nand", "dff"))
    v.add_argument("n", type=int, help="cells in the chain / bits of the shift register")
    v.add_argument("--net", required=True, help="chip.py's chip.net (the extracted cells)")
    v.add_argument("--pdk-ngspice", default=os.path.join(chip.PDK_ROOT, "libs.tech", "ngspice"))
    v.add_argument("--va", help="a BSIM4 Verilog-A file (module bsim4va; VAJAX only)")
    v.add_argument("--python", help="the Python that has vajax installed (patched, README.md; VAJAX only)")
    v.add_argument("--xyce", help="a Xyce build (default: xyce-threaded.nix's serial one)")
    v.add_argument("--solver", default="umfpack", choices=sorted(XYCE_SOLVERS))
    v.add_argument("--check", action="store_true", help=f"fail past CELLS_TOL ({CELLS_TOL}) against ngspice")
    v.add_argument("--out", default="cells")
    v.add_argument("--engines", default="ngspice,xyce,vajax")
    v.add_argument("--repeat", type=int, default=2, help="VAJAX transient runs (the first compiles)")
    v.add_argument("--threads", type=int, default=1)
    c = sub.add_parser("compare")
    c.add_argument("ref")
    c.add_argument("other")
    c.add_argument("--ref-txt", required=True)
    c.add_argument("--from", dest="t_from", type=float, default=0.0)
    a = ap.parse_args()
    if a.cmd == "xyce":
        out = a.out or os.path.join(os.path.dirname(os.path.abspath(a.deck)), "xyce")
        if a.translate_only:
            print(xyce_deck(a.deck, out, a.option))
            return 0
        return 0 if run_xyce(a.deck, out, a.xyce, a.ranks, a.option, solver=a.solver)["rc"] == 0 else 1
    if a.cmd == "cells":
        engs = a.engines.split(",")
        if "vajax" in engs and not (a.va and a.python):
            sys.exit("engines: VAJAX needs --va and --python")
        r = cells(a.kind, a.n, a.net, a.pdk_ngspice, a.va, a.python, os.path.join(a.out, f"{a.kind}{a.n}"),
                  engs, a.repeat, a.threads, a.xyce, a.solver)
        bad = [f"{x['engine']}: {x['error']}" for x in r["runs"] if "error" in x]
        bad += [f"{x['engine']}: rc {x['rc']}" for x in r["runs"] if x.get("rc")]
        if a.check:
            others = [x for x in r["runs"] if not x["engine"].startswith("ngspice")]
            bad += [f"{x['engine']}: no output" for x in others if "vs_ngspice" not in x]
            bad += [f"{x['engine']}: {f}" for x in others if "vs_ngspice" in x for f in cells_failures(x["vs_ngspice"])]
        for b in bad:
            print(f"engines: {r['circuit']}: FAIL {b}")
        if not bad and a.check:
            print(f"engines: {r['circuit']}: PASS " + ", ".join(
                f"{x['engine']} {x['vs_ngspice']['max_edge_ps']} ps {x['vs_ngspice']['max_settled_mV']} mV "
                f"({x['vs_ngspice']['edges']} edges)" for x in r["runs"] if "vs_ngspice" in x))
        return 1 if bad else 0
    compare(a.ref, a.other, a.ref_txt, a.t_from)
    return 0


if __name__ == "__main__":
    sys.exit(main())
