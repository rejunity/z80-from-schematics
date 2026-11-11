#!/usr/bin/env python3
"""test_engines.py - engines.py's reading of an ngspice library, which both
the Xyce and the VAJAX decks are made from.

ngspice drops comment and blank lines before it joins a '+' line to the line
it continues, so a .param whose '+' lines follow a comment keeps them. The
PDK's design.ngspice is written that way (mc_skew, fnoicor); resolve() used
to join them onto the comment, and the Xyce deck lost the parameters
without a word. Checked here, on a library made up in the same shape:

  plain     a '+' line right after its .param continues it
  comment   a '+' line after a comment line continues the .param before it
  include   ... and after a blank line, in a file reached by .include
  section   ... inside a .lib section

and Xyce's fast path (README.md, "Profile"), on a deck of one resistor:

  fast      the deck solves with UMFPACK (.options linsol type=umfpack),
            KLU only when asked for (solver="klu", the reference)
  env       every Xyce process orders with METIS and runs one OpenMP and
            one BLAS thread (seven idle threads spun in barriers: 5 %)
  mpi       the MPI build runs over mpirun -np 4, oversubscribe allowed;
            the serial build alone; never more ranks than CPUs
  checks    a window or a cells run against ngspice fails on a missing
            edge, an edge or a settled level past its tolerance, and an
            RTL mismatch in either engine
  corners   Xyce runs at tt, ss and ff (XYCE_CORNERS): ss since each
            diode's area is folded into its model (area, below) -- before,
            at 125 C its residual went NaN and it stopped ("Maximum number
            of failures"); slices B and C at ss pass against ngspice
  abort     a Xyce that aborts, prints a NaN, or stops before the end
            fails the run, with Xyce's own message; never a quiet pass
            (a stand-in for Xyce)
  area      every diode of the chip's netlist gets a model of its own with
            its area folded in -- IS, IBV (BV given: 1e-3 if not), CJO and
            IKF times area * m, RS divided by it, pj times m, no area or m
            left on the instance -- as ngspice scales them (diotemp.c):
            Xyce's diode takes its critical voltage from IS per unit area,
            below 0 V at 125 C for the PDK's per-area cards, and its pnjlim
            then takes the log of a negative voltage (README.md, "Xyce's
            NaN at ss"); IKF too, which Xyce's own code leaves unscaled
  indent    the library's lines start in column 1: Xyce takes ' .param x'
            for no parameter at all, and every model using x then aborts
            ("contains unrecognized symbols") -- the PDK as ciel fetches it
            (54435919) indents its typical section's .params, the copy the
            slices were run with does not

No ngspice, Xyce or PDK.

usage: test_engines.py
"""
import os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import engines  # noqa: E402

DESIGN = """* design parameters
.param
+ sw_stat_global = 0
* the skew, as in the PDK's design.ngspice
+ mc_skew = 1
+ fnoicor = 0

.param a = 1
+ b = 2
.include lib.ngspice
"""
LIB = """* sections
.lib typical
.param c = 3
* noise
+ d = 4

+ e = 5
.endl typical
"""


def params(lines):
    """name -> value of every .param in the resolved lines"""
    out = {}
    for l in lines:
        t = l.replace("=", " = ").split()
        if not t or t[0].lower() != ".param":
            continue
        t = t[1:]
        for k in range(len(t) - 2):
            if t[k + 1] == "=":
                out[t[k]] = t[k + 2]
    return out


DECK = """* one resistor
V1 a 0 1
R1 a 0 1k
.options klu rshunt=1e12
.control
tran 0.1n 1n 0 0.1n
wrdata {dat} v(a)
.endc
.end
"""


FAKE_XYCE = r"""#!/usr/bin/env python3
import os, re, sys
log, deck = sys.argv[sys.argv.index("-l") + 1], sys.argv[-1]
text = open(deck).read()
prn = re.search(r"file=(\S+)", text).group(1)
tstop = float(re.search(r"^\.tran \S+ ([0-9.e+-]+)n", text, re.M).group(1)) * 1e-9
mode = os.environ["FAKE_XYCE"]
end = {"complete": tstop, "nan": 2.8e-11, "short": tstop / 2, "abort": 5e-12}[mode]
with open(prn, "w") as f:
    f.write("TIME V(A)\n")
    for k in range(11):
        f.write(f"{end * k / 10:.8e} 1.0\n")
    f.write("End of Xyce(TM) Simulation\n")
msg = {"complete": "", "short": "",
       "nan": "Transient failure history: residual is NaN\nNonlinear solver: Time step too small near step number: 2\n",
       "abort": "Parameter TOXE for model NFET_06V0.1 contains unrecognized symbols\n*** Xyce Abort ***\n"}[mode]
open(log, "w").write("***** Welcome to the Xyce(TM) Parallel Electronic Simulator\n" + msg)
sys.exit(1 if mode == "abort" else 0)
"""


DIODE_LIB = """* a level-3 diode with per-area cards, as GF180's
.param jsa = 0.85
.model dpa d level=3 is={6.88e-7*jsa} ikf=229000 ibv=0.001 bv=11 cj=0.00095 rs=2e-10 jsw=4.88e-13 cjsw=1.33e-10 n=1.05
.model dpb d level=3 is=2e-7 bv=10.5 cj=0.0009 rs=2e-10
"""
DIODE_NET = """.subckt cell VSS I VDD
D0 VSS I dpa pj=1.86u area=0.2052p
D1 I VDD dpb pj=4u area=1p
D2 VSS VDD dpa pj=2u area=0.5p m=2
.ends
"""
DIODE_DECK = """* three diodes
.include {lib}
.include {net}
Xc 0 a b cell
V1 b 0 5
V2 a 0 1
.options klu rshunt=1e12
.control
tran 0.1n 1n 0 0.1n
wrdata {dat} v(a)
.endc
.end
"""


def area_cases(d, fails):
    """each diode its own model, the area folded in (as ngspice scales it)"""
    lib, net = os.path.join(d, "diodes.ngspice"), os.path.join(d, "chip.net")
    open(lib, "w").write(DIODE_LIB)
    open(net, "w").write(DIODE_NET)
    sp = os.path.join(d, "dio.sp")
    open(sp, "w").write(DIODE_DECK.format(lib=lib, net=net, dat=os.path.join(d, "dio.dat")))
    out = os.path.join(d, "area")
    engines.xyce_deck(sp, out)
    models = {}
    for l in open(os.path.join(out, "models.xyce.lib")):
        t = l.split()
        if len(t) > 2 and t[0].lower() == ".model" and t[2].lower() == "d":
            models[t[1].lower()] = {k.lower(): v for k, v in engines.param_tokens(l.split(None, 3)[3])}
    num = lambda v: eval(v.strip("{}").replace("jsa", "0.85"), {"__builtins__": {}})
    want = {"D0": ("dpa", 0.2052e-12, 1.86e-6, {"is": 6.88e-7 * 0.85, "ikf": 229000, "ibv": 0.001, "cjo": 0.00095},
                   {"rs": 2e-10}),
            "D1": ("dpb", 1e-12, 4e-6, {"is": 2e-7, "ibv": 0.001, "cjo": 0.0009}, {"rs": 2e-10}),
            "D2": ("dpa", 2 * 0.5e-12, 2 * 2e-6, {"is": 6.88e-7 * 0.85, "ikf": 229000, "ibv": 0.001, "cjo": 0.00095},
                   {"rs": 2e-10})}
    seen = set()
    for l in open(os.path.join(out, "chip.xyce.net")):
        t = l.split()
        if not t or t[0] not in want:
            continue
        base, a, pj, times, per = want[t[0]]
        kv = {k.lower(): v for k, v in (x.split("=") for x in t[4:])}
        m = models.get(t[3].lower())
        seen.add(t[0])
        if t[3].lower() == base or m is None:
            fails.append(f"area: {t[0]} uses {t[3]}, not a model of its own")
            continue
        if engines.spice_num(kv.get("area", "1")) != 1.0 or "m" in kv:
            fails.append(f"area: {t[0]} keeps area / m on the instance: {kv}")
        if abs(engines.spice_num(kv.get("pj", "0")) / pj - 1) > 1e-9:
            fails.append(f"area: {t[0]} pj {kv.get('pj')}, not {pj:g}")
        for k, v in times.items():
            if k not in m or abs(num(m[k]) / (v * a) - 1) > 1e-9:
                fails.append(f"area: {t[0]} {k} = {m.get(k)}, not {v:g} * {a:g}")
        for k, v in per.items():
            if k not in m or abs(num(m[k]) / (v / a) - 1) > 1e-9:
                fails.append(f"area: {t[0]} {k} = {m.get(k)}, not {v:g} / {a:g}")
        if num(m.get("jsw", "0")) not in (0, 4.88e-13):
            fails.append(f"area: {t[0]} jsw scaled: {m.get('jsw')}")
    if seen != set(want):
        fails.append(f"area: diodes in the Xyce netlist: {sorted(seen)}")


def xyce_cases(d, fails):
    sp = os.path.join(d, "r.sp")
    with open(sp, "w") as f:
        f.write(DECK.format(dat=os.path.join(d, "r.dat")))
    lines = lambda x: [l.strip().lower() for l in open(x)]
    xsp = engines.xyce_deck(sp, os.path.join(d, "fast"))[0]
    if ".options linsol type=umfpack" not in lines(xsp) or any("type=klu" in l for l in lines(xsp)):
        fails.append("fast: the deck does not solve with UMFPACK alone")
    xsp = engines.xyce_deck(sp, os.path.join(d, "klu"), solver="klu")[0]
    if ".options linsol type=klu" not in lines(xsp) or any("type=umfpack" in l for l in lines(xsp)):
        fails.append("fast: solver='klu' does not give KLU alone")
    cmd, env = engines.xyce_command("/x/bin/Xyce", 1)
    want = {"AMESOS_UMFPACK_ORDERING": "metis", "OMP_NUM_THREADS": "1", "OPENBLAS_NUM_THREADS": "1"}
    if cmd != ["/x/bin/Xyce"] or any(env.get(k) != v for k, v in want.items()):
        fails.append(f"env: serial {cmd} {env}")
    cmd, env = engines.xyce_command("/x/bin/Xyce", 4, mpirun="/m/bin/mpirun")
    if cmd[0] != "/m/bin/mpirun" or cmd[cmd.index("-np") + 1] != "4" or cmd[-1] != "/x/bin/Xyce" \
            or env.get("OMPI_MCA_rmaps_base_oversubscribe") != "1" \
            or env.get("PRTE_MCA_rmaps_default_mapping_policy") != ":oversubscribe" \
            or any(env.get(k) != v for k, v in want.items()):
        fails.append(f"mpi: {cmd} {env}")
    for mpirun, cpus, ranks in ((None, 8, 1), ("/m/bin/mpirun", 8, 4), ("/m/bin/mpirun", 4, 4), ("/m/bin/mpirun", 2, 2)):
        if engines.xyce_ranks(mpirun, cpus) != ranks:
            fails.append(f"mpi: {engines.xyce_ranks(mpirun, cpus)} ranks for {mpirun} on {cpus} CPUs, not {ranks}")
    good = {"edges": 7, "edges_missing": 0, "max_edge_shift_ns": 0.009, "max_settled_dV": 0.03,
            "mismatches_ref": 0, "mismatches_other": 0}
    if engines.window_failures(good):
        fails.append(f"checks: a window as measured fails: {engines.window_failures(good)}")
    for k, v in (("edges_missing", 1), ("max_edge_shift_ns", 1.0), ("max_settled_dV", 1.0),
                 ("mismatches_ref", 1), ("mismatches_other", 1), ("edges", 0)):
        if not engines.window_failures(dict(good, **{k: v})):
            fails.append(f"checks: a window with {k} = {v} passes")
    if tuple(engines.XYCE_CORNERS) != ("tt", "ss", "ff"):
        fails.append(f"corners: Xyce's corners are {engines.XYCE_CORNERS}, not tt, ss and ff")
    fake = os.path.join(d, "Xyce")
    with open(fake, "w") as f:
        f.write(FAKE_XYCE)
    os.chmod(fake, 0o755)
    for mode, want in (("complete", None), ("nan", "NaN"), ("abort", "Xyce Abort"), ("short", "stopped at")):
        os.environ["FAKE_XYCE"] = mode
        r = engines.run_xyce(sp, os.path.join(d, "x_" + mode), fake, ranks=1)
        err = r.get("error")
        if want is None and (err or r["rc"]):
            fails.append(f"abort: a complete run fails: {err} (rc {r['rc']})")
        if want and (not err or want not in err):
            fails.append(f"abort: Xyce {mode}: {err!r} does not say {want!r}")
    lib = engines.xyce_lib([" .param nfet_x = 0", "\t.param nfet_y = 1",
                            ".model m.0 nmos level=54 toxe={1.52e-008+nfet_x}"], {})
    if any(l[:1].isspace() for l in lib):
        fails.append(f"indent: {[l for l in lib if l[:1].isspace()]}")
    cell = {"edges": 6, "missing": 0, "max_edge_ps": 25.0, "max_settled_mV": 0.4}
    if engines.cells_failures(cell):
        fails.append(f"checks: cells as measured fail: {engines.cells_failures(cell)}")
    for k, v in (("missing", 1), ("max_edge_ps", 500.0), ("max_settled_mV", 50.0), ("edges", 0)):
        if not engines.cells_failures(dict(cell, **{k: v})):
            fails.append(f"checks: cells with {k} = {v} pass")


def main():
    d = tempfile.mkdtemp(prefix="test_engines.")
    with open(os.path.join(d, "design.ngspice"), "w") as f:
        f.write(DESIGN)
    with open(os.path.join(d, "lib.ngspice"), "w") as f:
        f.write(LIB.replace(".lib typical\n", "").replace(".endl typical\n", ""))
    with open(os.path.join(d, "sm.ngspice"), "w") as f:
        f.write(LIB)
    fails = []
    p = params(engines.resolve(os.path.join(d, "design.ngspice")))
    for case, names in (("comment", {"sw_stat_global": "0", "mc_skew": "1", "fnoicor": "0"}),
                        ("plain", {"a": "1", "b": "2"}),
                        ("include", {"c": "3", "d": "4", "e": "5"})):
        for n, v in names.items():
            if p.get(n) != v:
                fails.append(f"{case}: .param {n} = {v} read as {p.get(n)}")
    p = params(engines.resolve(os.path.join(d, "sm.ngspice"), "typical"))
    for n, v in {"c": "3", "d": "4", "e": "5"}.items():
        if p.get(n) != v:
            fails.append(f"section: .param {n} = {v} read as {p.get(n)}")
    xyce_cases(d, fails)
    area_cases(d, fails)
    for f in fails:
        print("engines: FAIL", f)
    if fails:
        sys.exit(1)
    print("engines: PASS (plain, comment, include, section, fast, env, mpi, checks, indent, corners, abort, area)")


if __name__ == "__main__":
    main()
