#!/usr/bin/env python3
"""compare_sources.py - the Z80's AC characteristics across manuals.

sources/*.json hold the AC tables of Zilog's and second sources' data
books, each as printed (hand-transcribed from the scans, with provenance).
This compares them and writes sources/DISCREPANCIES.md:

  RZ80     where the values pin_timing.py checks (z8400_nmos.json,
           z8400_nmos_1983.json) differ from what other primary sources
           print, or from the footnote formula at pin_timing's own clock;
  (a)      a starred (period-dependent) value that its own footnote
           formula does not give at the grade's limit clock -- with the
           edges the source says it assumed, or the grade's maximum edges;
  (b)      the same parameter and grade printed differently by different
           sources (NMOS and CMOS listed apart);
  (c)      symbols and numbering: the same parameter under different
           names, and the early (1977-82) symbols mapped to the numbers;
  (d)      measurement conditions: output levels, load, derating.

usage: compare_sources.py [--out FILE] [--check]
       --check: exit 1 if FILE is not what this would write (CI-style)
"""
import argparse, glob, json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "sources")
RZ80 = {"4": "z8400_nmos.json", "6": "z8400_nmos.json", "8": "z8400_nmos.json",
        "2.5": "z8400_nmos_1983.json"}
GORDER = ["1.0", "1.5", "2.5", "4", "6", "8"]


def load():
    s = []
    for f in sorted(glob.glob(os.path.join(SRC, "*.json"))):
        d = json.load(open(f))
        d["key"] = os.path.splitext(os.path.basename(f))[0]
        d["by_id"] = {}
        for p in d["parameters"]:
            if p.get("id"):
                d["by_id"].setdefault(p["id"], p)
        s.append(d)
    return s


def nmos(d):
    return d["technology"] == "NMOS" and d["role"].startswith(("primary", "second"))


def ev(expr, clk):
    return eval(expr, {"__builtins__": {}}, dict(clk))


def limit_clock(d, g, edge):
    """the grade's limit clock: TcC, TwCh, TwCl at their printed minimums,
    the edges `edge` ns (None: the grade's printed maximum)"""
    b = d["by_id"]
    def v(i):
        p = b.get(i)
        return p["values"].get(g) if p else None
    tr = v(5) if edge is None else edge
    tf = v(4) if edge is None else edge
    if None in (v(1), v(2), v(3), tr, tf):
        return None
    return {"TcC": v(1), "TwCh": v(2), "TwCl": v(3), "TrC": tr, "TfC": tf}


def fmt(x):
    if x is None:
        return "-"
    return ("%g" % x) if abs(x - round(x)) > 1e-9 else "%d" % round(x)


def label(d, g):
    for x in d["grades"]:
        if x["key"] == g:
            return x["label"]
    return g


def cite(d):
    s = d["source"]
    pp = s.get("printed_pages") or s.get("pdf_pages")
    return "%s, p. %s" % (s["title"], "-".join(str(x) for x in pp[:1] + pp[-1:]) if len(pp) > 1 else pp[0])


# ------------------------------------------------------------------ (a)
def candidates(d):
    """the edge times a source's starred values may assume: as it states,
    else Zilog's 20 ns (most second sources do not say), and the grade's
    printed maximum edges"""
    stated = (d.get("formula_assumption") or {}).get("TrC_ns")
    c = [("stated %g ns" % stated, stated)] if stated is not None else [("20 ns (unstated)", 20)]
    return c + [("max edges", None)]


def own_formula(srcs):
    rows, tcc, edge_use = [], [], []
    for d in srcs:
        for g in [x["key"] for x in d["grades"]]:
            fit = {}
            for p in d["parameters"]:
                f, i = p.get("formula"), p.get("id")
                if not f or g not in f["offset_ns"] or p["values"].get(g) is None:
                    continue
                if i != 1 and g not in p.get("starred", []):
                    continue
                printed = p["values"][g]
                got = {}
                for name, e in candidates(d):
                    c = limit_clock(d, g, e)
                    if c:
                        got[name] = ev(f["expr"], c) + f["offset_ns"][g]
                if not got:
                    continue
                if i == 1:      # TcC: the minimum period vs the minimum widths + edges
                    tcc.append((d, g, printed, got))
                    continue
                ok = [n for n, v in got.items() if abs(v - printed) < 0.01]
                if "TrC" in f["expr"] or "TfC" in f["expr"]:
                    for n in got:
                        fit.setdefault(n, [0, 0])[0 if n in ok else 1] += 1
                if not ok:
                    rows.append((d, g, p, printed, got))
            if fit:
                edge_use.append((d, g, fit))
    return rows, tcc, edge_use


# ------------------------------------------------------------------ (b)
def cross(srcs, tech):
    pick = [d for d in srcs if (nmos(d) if tech == "NMOS" else d["technology"] == tech)]
    out = []
    for i in range(1, 54):
        for g in GORDER:
            vals = {}
            for d in pick:
                p = d["by_id"].get(i)
                if not p or g not in p["values"] or p["values"][g] is None:
                    continue
                vals.setdefault(p["values"][g], []).append(d)
            if len(vals) > 1:
                out.append((i, g, vals, ""))
            mx = {}
            for d in pick:
                p = d["by_id"].get(i)
                if p and p.get("max_values", {}).get(g) is not None:
                    mx.setdefault(p["max_values"][g], []).append(d)
            if len(mx) > 1:
                out.append((i, g, mx, " (maximum)"))
    forms = []
    for i in range(1, 54):
        for g in GORDER:
            offs = {}
            for d in pick:
                p = d["by_id"].get(i)
                f = p and p.get("formula")
                if f and g in f["offset_ns"]:
                    offs.setdefault((f["expr"], f["offset_ns"][g]), []).append(d)
            if len(offs) > 1:
                forms.append((i, g, offs))
    return out, forms


# ------------------------------------------------------------------ RZ80
class Clock:
    """pin_timing.py's clock for a grade: the minimum period, the edges
    that leave TwCh = TwCl = their minimums (no longer than the maximum)"""
    def __init__(self, req, g):
        T = float(req[1]["table_ns"][g])
        tr = min((T - 2 * float(req[2]["table_ns"][g])) / 2, float(req[5]["table_ns"][g]))
        hi = (T - 2 * tr) / 2
        self.env = {"period_ns": T, "high_ns": hi, "low_ns": T - 2 * tr - hi, "rise_ns": tr, "fall_ns": tr}
        self.desc = "TcC %s, TwCh = TwCl %s, edges %s" % (fmt(T), fmt(hi), fmt(tr))


def rz80(srcs):
    rows, clocks = [], {}
    for g, fn in RZ80.items():
        req = {r["id"]: r for r in json.load(open(os.path.join(HERE, fn)))["requirements"]}
        c = Clock(req, g)
        clocks[g] = c.desc
        for i, r in req.items():
            printed = r["table_ns"][g]
            f = r.get("formula")
            form = None
            if f and r["bound"] == "min":
                form = eval(f["base"], {"__builtins__": {}}, c.env) + f["offset_ns"][g]
            others, cmos = {}, {}
            for d in srcs:
                p = d["by_id"].get(i)
                if not p or p["values"].get(g) is None or p["values"][g] == printed:
                    continue
                if nmos(d):
                    others.setdefault(p["values"][g], []).append(d)
                elif d["technology"] == "CMOS":
                    cmos.setdefault(p["values"][g], []).append(d)
            gap = form is not None and printed - form > 0.5
            if others or gap:
                rows.append((i, g, r, printed, form, others, cmos))
    return rows, clocks


def symbols(srcs):
    out = {}
    for d in srcs:
        for p in d["parameters"]:
            s = p.get("source_symbol")
            if s and p.get("id") and d["nomenclature"] == "zilog_numbered":
                out.setdefault(p["id"], {}).setdefault(s, []).append(d)
    return out


def write(srcs):
    L = []
    w = L.append
    w("# The Z80's AC characteristics across manuals")
    w("")
    w("Generated by `../compare_sources.py` from the transcriptions in this")
    w("directory; do not edit. Values in ns. A starred value (`*`) is period-")
    w("dependent: the source gives a footnote formula for other clocks.")
    w("")
    w("## Sources")
    w("")
    w("| File | Part | Grades | Source | Pages | SHA-256 |")
    w("|---|---|---|---|---|---|")
    for d in srcs:
        s = d["source"]
        pp = ", ".join(str(x) for x in (s.get("printed_pages") or []))
        w("| `%s` | %s (%s; %s) | %s | [%s](%s)%s | %s | `%s...` |" % (
            d["key"], d["part"], d["technology"], d["role"],
            ", ".join(x["key"] for x in d["grades"]), s["title"], s["url"],
            (", " + s["document"]) if s.get("document") else "", pp, s["sha256"][:16]))
    w("")

    rows, clocks = rz80(srcs)
    w("## What RZ80's pin timing checks against the other sources")
    w("")
    w("`pin_timing.py` checks the printed value, and for a starred minimum the")
    w("greater of it and the formula at its own clock (%s)." % "; ".join(
        "%s MHz: %s" % (g, clocks[g]) for g in sorted(clocks, key=GORDER.index)))
    w("Rows: a primary or second NMOS source prints another value, or the")
    w("printed value is stricter than the formula at that clock (so the printed")
    w("one is what gets checked).")
    w("")
    w("Sorted by how much stricter the checked value is than the formula, then")
    w("by grade.")
    w("")
    w("| # | Symbol | Grade | RZ80 checks | Formula at pin_timing's clock | NMOS sources print | CMOS sources print |")
    w("|---|---|---|---|---|---|---|")
    def lst(m):
        return "; ".join("%s: %s" % (fmt(v), ", ".join("`%s`" % d["key"] for d in ds)) for v, ds in sorted(m.items()))
    for i, g, r, printed, form, others, cmos in sorted(rows, key=lambda x: (-(x[3] - x[4]) if x[4] is not None else 0, GORDER.index(x[1]), x[0])):
        w("| %d | %s | %s | %s %s | %s | %s | %s |" % (i, r["symbol"], g, r["bound"], fmt(printed),
                                                    fmt(form) if form is not None else "", lst(others), lst(cmos)))
    w("")

    a, tcc, edge_use = own_formula(srcs)
    w("## (a) Printed values their own formulas do not give")
    w("")
    w("At each grade's limit clock: TcC, TwCh and TwCl at their printed")
    w("minimums; the edges as the source states its starred values assume")
    w("(or Zilog's 20 ns where it does not say), or the grade's printed")
    w("maximum edges.")
    w("")
    w("| Source | Grade | # | Symbol | Printed | Formula |")
    w("|---|---|---|---|---|---|")
    for d, g, p, printed, got in a:
        w("| `%s` | %s | %d | %s | %s | %s |" % (d["key"], label(d, g), p["id"], p["symbol"], fmt(printed),
                                            ", ".join("%s (%s)" % (fmt(v), n) for n, v in got.items())))
    w("")
    w("The limit clock itself: TcC = TwCh + TwCl + TrC + TfC (footnote 1), so a")
    w("printed minimum period shorter than the minimum widths plus the edges")
    w("cannot have all of them at once; the starred values combine them anyway.")
    w("")
    w("| Source | Grade | TcC min | TwCh + TwCl + TrC + TfC |")
    w("|---|---|---|---|")
    for d, g, printed, got in tcc:
        if all(printed < v - 0.01 for v in got.values()):
            w("| `%s` | %s | %s | %s |" % (d["key"], label(d, g), fmt(printed),
                                        ", ".join("%s (%s)" % (fmt(v), n) for n, v in got.items())))
    w("")
    w("Edges the edge-dependent starred values fit (matching / not):")
    w("")
    w("| Source | Grade | Fit |")
    w("|---|---|---|")
    for d, g, fit in edge_use:
        w("| `%s` | %s | %s |" % (d["key"], label(d, g), "; ".join("%s %d / %d" % (n, k[0], k[1]) for n, k in fit.items())))
    w("")

    for tech in ("NMOS", "CMOS"):
        b, forms = cross(srcs, tech)
        w("## (b) %s: the same parameter printed differently" % tech)
        w("")
        if tech == "NMOS":
            w("Primary and second sources only (not the Z8300 low-power part).")
            w("")
        w("| # | Symbol | Grade | Values (sources) |")
        w("|---|---|---|---|")
        for i, g, vals, what in b:
            sym = next(d["by_id"][i]["symbol"] for ds in vals.values() for d in ds) + what
            w("| %d | %s | %s | %s |" % (i, sym, g, "; ".join(
                "**%s**: %s" % (fmt(v), ", ".join("`%s`" % d["key"] for d in ds)) for v, ds in sorted(vals.items()))))
        w("")
        if forms:
            w("Footnote formulas that differ:")
            w("")
            w("| # | Grade | Formula (sources) |")
            w("|---|---|---|")
            for i, g, offs in forms:
                w("| %d | %s | %s |" % (i, g, "; ".join("%s %+d: %s" % (e, o, ", ".join("`%s`" % d["key"] for d in ds))
                                                       for (e, o), ds in sorted(offs.items(), key=lambda x: x[0][1]))))
            w("")

    w("## (c) Symbols and numbering")
    w("")
    w("Numbered tables printing another symbol for a parameter:")
    w("")
    w("| # | Zilog symbol | Printed as (sources) |")
    w("|---|---|---|")
    for i, m in sorted(symbols(srcs).items()):
        sym = next(d["by_id"][i]["symbol"] for ds in m.values() for d in ds)
        w("| %d | %s | %s |" % (i, sym, "; ".join("%s: %s" % (s, ", ".join("`%s`" % d["key"] for d in ds)) for s, ds in m.items())))
    w("")
    early = [d for d in srcs if d["nomenclature"] == "early_zilog_symbols"]
    if early:
        w("The early tables (%s) use other symbols and no numbers; the mapping:" % ", ".join("`%s`" % d["key"] for d in early))
        w("")
        w("| Early symbol | # | Numbered symbol | Note |")
        w("|---|---|---|---|")
        for p in early[0]["parameters"]:
            w("| %s | %s | %s | %s |" % (p["source_symbol"], p.get("id") or "-", p.get("symbol", "-"), p.get("note", "")))
        have = {p["id"] for p in early[0]["parameters"] if p.get("id")}
        numbered = {p["id"]: p["symbol"] for d in srcs if d["nomenclature"] == "zilog_numbered" for p in d["parameters"]}
        w("")
        w("Numbered parameters with no early counterpart: %s." % ", ".join(
            "%d %s" % (i, numbered[i]) for i in range(1, 54) if i not in have))
        w("")

    w("## (d) Measurement conditions")
    w("")
    w("| Source | VIH / VIL | VOH / VOL | Load | Load rule |")
    w("|---|---|---|---|---|")
    for d in srcs:
        c = d.get("ac_test_conditions") or {}
        w("| `%s` | %s / %s V | %s / %s V | %s | %s |" % (
            d["key"], c.get("VIH_V", "-"), c.get("VIL_V", "-"), c.get("VOH_V", "-"), c.get("VOL_V", "-"),
            ("%s pF" % c["load_pF"]) if "load_pF" in c else "not recorded",
            " ".join(x for x in (c.get("load_derating", ""), c.get("note", "")) if x)))
    w("")
    w("Notes on individual parameters (as printed):")
    w("")
    for d in srcs:
        for p in d["parameters"]:
            n = [x for x in (p.get("note"), p.get("max_note")) if x]
            if p.get("min_values"):
                n.append("minimum " + ", ".join("%s: %s" % (g, v) for g, v in p["min_values"].items()))
            if n and p.get("id"):
                w("- `%s` #%d %s: %s" % (d["key"], p["id"], p.get("symbol", p.get("source_symbol")), "; ".join(n)))
        for n in d.get("notes", []):
            w("- `%s`: %s" % (d["key"], n))
    w("")
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=os.path.join(SRC, "DISCREPANCIES.md"))
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    text = write(load())
    if a.check:
        old = open(a.out).read() if os.path.exists(a.out) else ""
        if old != text:
            print("%s is stale: run %s" % (a.out, os.path.basename(__file__)))
            return 1
        return 0
    open(a.out, "w").write(text)
    print("wrote", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
