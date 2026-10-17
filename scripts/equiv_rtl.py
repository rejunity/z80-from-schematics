#!/usr/bin/env python3
"""Formal equivalence of z80_core between a git revision and the work tree.

usage: scripts/equiv_rtl.py [REF]          (REF defaults to main)

For a *pure restructuring* -- the same registers, renamed or moved into
submodules at most -- this proves the new core identical to REF's for
every state, not just the reachable ones:

  1. both cores are flattened; every flip-flop becomes a cut point
     (`expose -evert-dff`: Q is a free input, D / reset value / reset are
     outputs), with instance prefixes stripped from the register names
     (u_regs.rf[0] -> rf[0]) so the two sides line up;
  2. a miter compares every output pin image and every flip-flop's next
     state, reset and reset value;
  3. SAT proves the miter's trigger can never fire.

A changed register set (a new or retimed register) is not a pure
restructuring: the miter then fails to build (port mismatch) -- verify
those steps by simulation instead (fuse_rtl, compare, z80test, ZEXALL).

Needs a native yosys on PATH (or YOSYS=...). Runs in a scratch directory
under build/equiv/.
"""
import os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = sys.argv[1] if len(sys.argv) > 1 else "main"
YOSYS = os.environ.get("YOSYS", "yosys")
W = os.path.join(ROOT, "build", "equiv")

def sh(*a, **k):
    return subprocess.run(a, check=True, capture_output=True, text=True, **k).stdout

def stage(dst, files):
    os.makedirs(dst, exist_ok=True)
    for name, text in files.items():
        open(os.path.join(dst, name), "w").write(text)

shutil.rmtree(W, ignore_errors=True)
# gold: REF's rtl/, with z80_core renamed so both designs can be loaded at once
gold = {}
for path in sh("git", "-C", ROOT, "ls-tree", "--name-only", REF, "rtl/").split():
    if path.endswith((".v", ".vh")) and not path.endswith("z80_top.v"):
        gold[os.path.basename(path)] = sh("git", "-C", ROOT, "show", f"{REF}:{path}")
gate = {f: open(os.path.join(ROOT, "rtl", f)).read() for f in os.listdir(os.path.join(ROOT, "rtl"))
        if f.endswith((".v", ".vh")) and f != "z80_top.v"}
def rename_modules(files, suffix):
    mods = [m for t in files.values() for m in re.findall(r"^module\s+(\w+)", t, re.M)]
    out = {}
    for f, t in files.items():
        for m in mods:
            t = re.sub(rf"\b{m}\b", m + suffix, t)
        out[f] = t
    return out
stage(os.path.join(W, "gold"), rename_modules(gold, "_gold"))
stage(os.path.join(W, "gate"), gate)

def read_cmds():
    g = " ".join(f"gold/{f}" for f in sorted(gold) if f.endswith(".v"))
    n = " ".join(f"gate/{f}" for f in sorted(gate) if f.endswith(".v"))
    return [f"read_verilog -Igold {g}", f"read_verilog -Igate {n}",
            "hierarchy -check", "proc", "flatten", "opt_clean"]

# pass 1: list the flip-flop outputs of both cores
listing = os.path.join(W, "ffq.txt")
sh(YOSYS, "-q", "-p", "; ".join(read_cmds() + [
    f"tee -q -o {listing} select -list z80_core_gold/t:$adff %x:+[Q] z80_core_gold/w:* %i "
    f"z80_core/t:$adff %x:+[Q] z80_core/w:* %i"]), cwd=W)
renames = []
for line in open(listing):
    mod, _, wire = line.strip().partition("/")
    base = wire.rsplit(".", 1)[-1] if re.match(r"u_\w+\.", wire) else wire
    if base != wire:
        renames += [f"cd {mod}", f"rename {wire} {base}", "cd .."]

# pass 2: the miter
script = read_cmds() + renames + [
    "select -set ffq t:$adff %x:+[Q] w:* %i",
    "rename -hide w:* x:* %d @ffq %d",
    "opt_clean",
    "expose -evert-dff t:$adff",
    "opt_clean",
    "miter -equiv -flatten -make_outputs -ignore_gold_x z80_core_gold z80_core miter",
    "hierarchy -top miter",
    "opt -fast",
    "sat -verify -prove trigger 0 miter",
]
open(os.path.join(W, "miter.ys"), "w").write("\n".join(script) + "\n")
print(f"equiv_rtl: z80_core of {REF} vs work tree ({len(renames)//3} registers renamed) ...", flush=True)
r = subprocess.run([YOSYS, "-q", "-l", "miter.log", "miter.ys"], cwd=W, capture_output=True, text=True)
log = open(os.path.join(W, "miter.log")).read()
if r.returncode == 0 and "SUCCESS" in log:
    m = re.search(r"Solving problem with (\d+) variables and (\d+) clauses", log)
    print(f"PASS: equivalent for every state and input ({m.group(1)} vars, {m.group(2)} clauses)" if m else "PASS")
    sys.exit(0)
print("FAIL: see build/equiv/miter.log")
print("\n".join(log.splitlines()[-25:]))
sys.exit(1)
