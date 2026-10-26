#!/usr/bin/env python3
"""The decoder (rtl/z80_pla.v) as an explicit PLA: an AND plane of product
terms over the 11 input lines (prefix[2:0], op[7:0]) and their complements,
and an OR plane that sums them into the 53 output bits -- the form of the
die's own instruction PLA, for the ASIC flows that lay it out as one.

    planes.py [--out FILE]   derive the truth table (iverilog, all 2048
                             inputs), minimise it, write the planes module
    planes.py --check FILE   simulate FILE and rtl/z80_pla.v on all 2048
                             inputs; fail on any difference

The planes module has z80_pla's name and ports, so a flow takes it in
place of rtl/z80_pla.v. Every output is matched exactly on all 2048 input
combinations, the prefix codes the core never produces included: no
don't-cares are taken, so there is nothing to argue about.

Minimisation: each output bit's prime implicants (Quine-McCluskey over 11
variables), a cover that prefers the terms other outputs already use (the
AND plane is shared), and for each output whichever of the on-set and the
off-set needs fewer terms -- an output from its off-set is the OR line
inverted, as a PLA's programmable output polarity does it.
"""
import argparse, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
RTL = os.path.join(ROOT, "rtl")
INPUTS = [("prefix", 3), ("op", 8)]          # input lines, msb first: in[10:8] = prefix, in[7:0] = op
NIN = 11


def ports():
    """z80_pla's outputs, in declaration order: [(name, width)]"""
    src = open(os.path.join(RTL, "z80_pla.v")).read()
    hdr = src[src.index("module z80_pla"):src.index(");")]
    out = []
    for m in re.finditer(r"output\s+(?:reg|wire)?\s*(?:\[(\d+):0\])?\s*(\w+)", hdr):
        out.append((m.group(2), int(m.group(1)) + 1 if m.group(1) else 1))
    return out


def simulate(pla_file, module, outs, extra=(), defines=()):
    """all 2048 input combinations of `module` (in pla_file, with the extra
    files) -> list of output words (int, outs concatenated msb first)"""
    width = sum(w for _, w in outs)
    with tempfile.TemporaryDirectory() as tmp:
        tb = os.path.join(tmp, "tb.v")
        conns = ", ".join(f".{n}({n})" for n, _ in outs)
        decl = "\n".join(f"    wire [{w - 1}:0] {n};" for n, w in outs)
        cat = ", ".join(n for n, _ in outs)
        open(tb, "w").write(f"""`timescale 1ns/1ps
module tb;
    reg [10:0] in;
{decl}
    {module} dut(.prefix(in[10:8]), .op(in[7:0]), {conns});
    integer i;
    initial begin
        for (i = 0; i < 2048; i = i + 1) begin
            in = i; #10;
            $display("R %h", {{{cat}}});
        end
        $finish;
    end
endmodule
""")
        vvp = os.path.join(tmp, "tb.vvp")
        subprocess.run(["iverilog", "-g2012", "-I", RTL, *[f"-D{d}" for d in defines], "-o", vvp, tb, pla_file, *extra],
                       check=True)
        lines = subprocess.run(["vvp", "-n", vvp], check=True, capture_output=True, text=True).stdout.splitlines()
    words = [int(x.split()[1], 16) for x in lines if x.startswith("R ")]
    if len(words) != 2048:
        sys.exit(f"planes: expected 2048 results, got {len(words)}")
    return words, width


def reference(outs):
    """rtl/z80_pla.v's truth table (module renamed so a planes module can sit beside it)"""
    with tempfile.TemporaryDirectory() as tmp:
        ref = os.path.join(tmp, "z80_pla_ref.v")
        open(ref, "w").write(open(os.path.join(RTL, "z80_pla.v")).read().replace("module z80_pla", "module z80_pla_ref", 1))
        return simulate(ref, "z80_pla_ref", outs)


# ---------------------------------------------------------------- minimiser
# a cube is (mask, value): mask bit 1 = the literal is present, value its polarity
def primes(onset):
    level = {(0x7FF, m) for m in onset}
    found = set()
    while level:
        nxt, merged = set(), set()
        by_mask = {}
        for mask, val in level:
            by_mask.setdefault(mask, set()).add(val)
        for mask, vals in by_mask.items():
            for val in vals:
                b = mask
                while b:
                    bit = b & -b
                    b ^= bit
                    if not val & bit and (val | bit) in vals:
                        nxt.add((mask & ~bit, val))
                        merged.add((mask, val)); merged.add((mask, val | bit))
        found |= level - merged
        level = nxt
    return found


def covers(cube, m):
    mask, val = cube
    return (m & mask) == val


def cover(onset, prims, shared):
    """a small set of primes covering onset; terms already in `shared` cost less"""
    left = set(onset)
    chosen = []
    cov = {p: {m for m in onset if covers(p, m)} for p in prims}
    # essential primes first
    for m in sorted(onset):
        ps = [p for p in prims if m in cov[p]]
        if len(ps) == 1 and ps[0] not in chosen:
            chosen.append(ps[0])
    for p in chosen:
        left -= cov[p]
    while left:
        def score(p):
            gain = len(cov[p] & left)
            lits = bin(p[0]).count("1")
            return (gain, p in shared, -lits, p)
        best = max(prims, key=score)
        chosen.append(best)
        left -= cov[best]
    # drop any that the others cover
    for p in list(chosen):
        rest = set().union(*(cov[q] for q in chosen if q != p)) if len(chosen) > 1 else set()
        if cov[p] <= rest:
            chosen.remove(p)
    return chosen


def minimise(table, width):
    shared = set()
    lines = []                                # per output bit (msb first): (polarity, [cubes]) or constant
    bits = []
    for b in range(width - 1, -1, -1):
        on = {i for i, w in enumerate(table) if w >> b & 1}
        bits.append(on)
    # outputs with most minterms first: the big ones seed the shared plane
    order = sorted(range(width), key=lambda k: -min(len(bits[k]), 2048 - len(bits[k])))
    result = [None] * width
    for k in order:
        on = bits[k]
        if not on:
            result[k] = ("const", 0); continue
        if len(on) == 2048:
            result[k] = ("const", 1); continue
        off = set(range(2048)) - on
        best = None
        for pol, s in ((1, on), (0, off)):
            c = cover(s, primes(s), shared)
            cost = (len([t for t in c if t not in shared]), sum(bin(t[0]).count("1") for t in c), len(c))
            if best is None or cost < best[0]:
                best = (cost, pol, c)
        _, pol, c = best
        shared |= set(c)
        result[k] = (pol, sorted(c, key=lambda t: (-bin(t[0]).count("1"), t)))
    return result


# ---------------------------------------------------------------- Verilog
def literal(i, positive):
    return f"t[{i}]" if positive else f"c[{i}]"


def emit(result, outs, path, unused):
    """write the planes: z80_pla over three modules -- the input lines, the
    AND plane, the OR plane -- so synthesis maps each plane on its own and
    keeps the terms between them (an attribute on a wire is lost when the
    flow re-reads its netlist; a module boundary is not). Returns (terms the
    core uses, their literals, OR-plane inputs, terms pruned)."""
    bits = [(n, i, w) for n, w in outs for i in range(w - 1, -1, -1)]       # msb first, as result
    used = [n not in unused for n, _, _ in bits]
    terms, index = [], {}
    for want in (True, False):
        for r, u in zip(result, used):
            if r[0] != "const" and u == want:
                for t in r[1]:
                    if t not in index:
                        index[t] = len(terms); terms.append(t)
    nkeep = len({t for r, u in zip(result, used) if u and r[0] != "const" for t in r[1]})
    nt = len(terms)
    line = lambda n, i, w: f"{n}{i if w > 1 else ''}"
    lines = [line(n, i, w) for n, i, w in bits]
    L = ["// Generated by asic/pla/planes.py from rtl/z80_pla.v; do not edit.",
         "// z80_pla as a PLA: input lines (true and complement), an AND plane of",
         f"// {nt} product terms, an OR plane of one line per output bit (inverted where",
         "// the bit is the OR of its off-set) -- each plane a module of its own, so",
         "// synthesis keeps the terms between them. Exactly rtl/z80_pla.v on all 2048",
         f"// inputs: make -C asic/<pdk> pla_check. {', '.join(sorted(unused))} (left unconnected by",
         f"// z80_core) and the {nt - nkeep} terms only they need are pruned by synthesis.",
         "`default_nettype none",
         "module z80_pla ("]
    L += [f"    input  wire [{w - 1}:0] {n}," for n, w in INPUTS]
    L += [f"    output wire {f'[{w - 1}:0] ' if w > 1 else ''}{n}{',' if k < len(outs) - 1 else ''}" for k, (n, w) in enumerate(outs)]
    L += [");",
          "    wire [10:0] t, c;          // input lines: 10..8 prefix, 7..0 op",
          f"    wire [{nt - 1}:0] term;      // the AND plane's product terms",
          f"    wire [{len(lines) - 1}:0] sum;       // the OR plane's lines, msb first as the outputs",
          "    z80_pla_lines u_lines (.in({prefix, op}), .t(t), .c(c));",
          "    z80_pla_and u_and (.t(t), .c(c), .term(term));",
          "    z80_pla_or u_or (.term(term), .sum(sum));"]
    pos = len(lines) - 1
    for n, w in outs:
        parts = []
        for (bn, i, bw), r in zip(bits, result):
            if bn == n:
                parts.append(None)
        parts = []
        for j, ((bn, i, bw), r) in enumerate(zip(bits, result)):
            if bn == n:
                parts.append(("~" if r[0] == 0 else "") + f"sum[{len(lines) - 1 - j}]")
        L.append(f"    assign {n} = {{{', '.join(parts)}}};" if w > 1 else f"    assign {n} = {parts[0]};")
    L += ["endmodule", "",
          "// the input lines: each input and its complement",
          "module z80_pla_lines (input wire [10:0] in, output wire [10:0] t, output wire [10:0] c);",
          "    assign t = in;",
          "    assign c = ~in;",
          "endmodule", "",
          f"// the AND plane: term k is the AND of its literals (t true, c complement)",
          f"module z80_pla_and (input wire [10:0] t, input wire [10:0] c, output wire [{nt - 1}:0] term);"]
    for k, (mask, val) in enumerate(terms):
        lits = [literal(i, val >> i & 1) for i in range(NIN - 1, -1, -1) if mask >> i & 1]
        L.append(f"    assign term[{k}] = {' & '.join(lits) if lits else chr(39).join(['1', 'b1'])};")
    L += ["endmodule", "",
          "// the OR plane: one line per output bit (z80_pla's outputs, msb first)",
          f"module z80_pla_or (input wire [{nt - 1}:0] term, output wire [{len(lines) - 1}:0] sum);"]
    for j, ((n, i, w), r) in enumerate(zip(bits, result)):
        k = len(lines) - 1 - j
        if r[0] == "const":
            L.append(f"    assign sum[{k}] = 1'b{r[1]};   // {lines[j]}")
        else:
            L.append(f"    assign sum[{k}] = {' | '.join(f'term[{index[t]}]' for t in r[1])};   // {lines[j]}{'' if r[0] else ', inverted'}")
    L += ["endmodule", "`default_nettype wire", ""]
    open(path, "w").write("\n".join(L))
    lits = sum(bin(t[0]).count("1") for t in terms[:nkeep])
    ors = sum(len(r[1]) for r, u in zip(result, used) if u and r[0] != "const")
    return nkeep, lits, ors, nt - nkeep


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(HERE, "z80_pla_planes.v"))
    ap.add_argument("--check", metavar="FILE", help="compare FILE's z80_pla with rtl/z80_pla.v on all 2048 inputs")
    ap.add_argument("--check-run", metavar="RUN",
                    help="the same with the planes as a LibreLane run synthesized them: the mapped z80_pla* modules of "
                         "RUN's netlist (before flattening), on the PDK's cell models")
    ap.add_argument("--unused", default="uses_cc,valid",
                    help="outputs z80_core leaves unconnected: not kept, so synthesis prunes them (default: %(default)s)")
    a = ap.parse_args()
    outs = ports()
    ref, width = reference(outs)
    if a.check_run:
        import glob, json
        run = a.check_run.rstrip("/")
        net = glob.glob(os.path.join(run, "*-yosys-synthesis", "*.nl.v.hierarchy.nl.v"))
        if not net:
            sys.exit(f"pla_check: no synthesized hierarchy netlist in {run}")
        text = open(net[0]).read()
        mods = [m for m, name in re.findall(r"(module\s+(\\?\S+)\s*\(.*?endmodule)", text, re.S)
                if name.lstrip("\\").startswith("z80_pla")]
        models = json.load(open(os.path.join(run, "resolved.json")))["CELL_VERILOG_MODELS"]
        with tempfile.TemporaryDirectory() as tmp:
            gl = os.path.join(tmp, "z80_pla_gl.v")
            open(gl, "w").write("\n".join(mods))
            got, _ = simulate(gl, "z80_pla", outs, extra=models, defines=["FUNCTIONAL"])
        bad = [i for i in range(2048) if got[i] != ref[i]]
        if bad:
            sys.exit(f"pla_check: the synthesized planes differ from rtl/z80_pla.v on {len(bad)} of 2048 inputs")
        print(f"pla_check: {run}'s synthesized planes ({len(mods)} modules, the PDK's cells) equal rtl/z80_pla.v "
              f"on all 2048 inputs")
        return
    if a.check:
        got, _ = simulate(a.check, "z80_pla", outs)
        bad = [i for i in range(2048) if got[i] != ref[i]]
        if bad:
            for i in bad[:10]:
                print(f"prefix {i >> 8} op {i & 0xFF:02x}: planes {got[i]:0{(width + 3) // 4}x} rtl {ref[i]:0{(width + 3) // 4}x}")
            sys.exit(f"pla_check: {len(bad)} of 2048 inputs differ")
        print(f"pla_check: {os.path.relpath(a.check, ROOT)} equals rtl/z80_pla.v on all 2048 inputs ({width} output bits)")
        return
    result = minimise(ref, width)
    n, lits, ors, pruned = emit(result, outs, a.out, set(a.unused.split(",")) - {""})
    print(f"planes: {n} product terms ({lits} literals) in the AND plane, {ors} term inputs in the OR plane, "
          f"{pruned} more terms only for {a.unused} -> {os.path.relpath(a.out, ROOT)}")


if __name__ == "__main__":
    main()
