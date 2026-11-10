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
    for f in fails:
        print("engines: FAIL", f)
    if fails:
        sys.exit(1)
    print("engines: PASS (plain, comment, include, section)")


if __name__ == "__main__":
    main()
