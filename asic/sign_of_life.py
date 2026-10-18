#!/usr/bin/env python3
"""Signs of life: run tb_sign_of_life.v on z80_top.

usage: sign_of_life.py <LibreLane run dir>   gate level: the run's final
                                             netlist + its PDK's cell models
       sign_of_life.py --rtl                 the RTL in rtl/

The gate-level run takes the cell models from the run's resolved.json
(CELL_VERILOG_MODELS), so it works for any PDK. A chip run (design
z80_chip, <pdk>/chip/) also gets the pad cells' models (PAD_VERILOG_MODELS;
for a blackbox view, the PDK's own model of those cells), the netlists of
its hard macros (z80_top) and chip_shim.v, which puts z80_top's ports on
the chip's pins (-DDUT=z80_chip_shim). IVERILOG / VVP may point
at another simulator build; iverilog >= 12 is needed for the IHP SG13G2
models (their flops are driven through $setuphold delayed nets, which
iverilog 11 leaves undriven -- everything stays X).
"""
import glob, json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IVERILOG = os.environ.get("IVERILOG", "iverilog")
VVP = os.environ.get("VVP", "vvp")
TB = os.path.join(HERE, "tb_sign_of_life.v")

def pad_models(cfg):
    """Simulation models of the run's pad cells: PAD_VERILOG_MODELS, with
    each blackbox view swapped for the PDK model defining the same cells."""
    models = []
    for m in cfg.get("PAD_VERILOG_MODELS") or []:
        text = open(m).read()
        if "(* blackbox *)" not in text:
            models.append(m)
            continue
        cells = re.findall(r"module\s+(\w+)", text)
        pdk = os.path.join(cfg["PDK_ROOT"], cfg["PDK"], "libs.ref")
        for f in sorted(glob.glob(os.path.join(pdk, "*", "verilog", "*.v"))):
            body = open(f).read()
            if "(* blackbox *)" not in body and all(re.search(r"module\s+%s\b" % c, body) for c in cells):
                models.append(f)
                break
        else:
            sys.exit(f"sign_of_life: no simulation model for the cells in {m}")
    return models

def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    if sys.argv[1] == "--rtl":
        rtl = os.path.join(ROOT, "rtl")
        srcs = sorted(os.path.join(rtl, f) for f in os.listdir(rtl) if f.endswith(".v"))
        args, what = [f"-I{rtl}"] + srcs, "RTL"
    else:
        run = sys.argv[1]
        cfg = json.load(open(os.path.join(run, "resolved.json")))
        netlist = os.path.join(run, "final", "nl", cfg["DESIGN_NAME"] + ".nl.v")
        if not os.path.isfile(netlist):
            sys.exit(f"sign_of_life: {netlist} missing -- did the flow finish?")
        args = ["-DFUNCTIONAL", "-DUNIT_DELAY=#1", netlist] + list(cfg["CELL_VERILOG_MODELS"])
        if cfg["DESIGN_NAME"] == "z80_chip":
            macros = [nl for m in (cfg.get("MACROS") or {}).values() for nl in m.get("nl") or []]
            args += ["-DDUT=z80_chip_shim"] + macros + pad_models(cfg) + [os.path.join(HERE, "chip_shim.v")]
        what = f"{cfg['PDK']} gate-level netlist ({os.path.basename(os.path.normpath(run))})"
    with tempfile.TemporaryDirectory() as tmp:
        vvp = os.path.join(tmp, "sol.vvp")
        c = subprocess.run([IVERILOG, "-g2005", "-o", vvp, TB] + args, capture_output=True, text=True)
        errs = [l for l in c.stderr.splitlines() if "error" in l.lower()]
        if c.returncode != 0:
            sys.exit("sign_of_life: iverilog failed\n" + "\n".join(errs or c.stderr.splitlines()[-20:]))
        r = subprocess.run([VVP, "-n", vvp], capture_output=True, text=True)
    out = [l for l in r.stdout.splitlines() if l.startswith(("FAIL", "SIGN OF LIFE"))]
    print(f"{what}:")
    print("\n".join("  " + l for l in out))
    sys.exit(0 if any(l.startswith("SIGN OF LIFE: PASS") for l in out) else 1)

if __name__ == "__main__":
    main()
