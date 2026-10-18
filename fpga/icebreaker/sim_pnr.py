#!/usr/bin/env python3
"""sim_pnr.py - nextpnr's routed sbc netlist, made simulatable (`make sim_pnr`).

    sim_pnr.py <yosys share/ice40/cells_sim.v> <top_sbc.pnr.json> <out.json> <out_cells.v>

The routed netlist (nextpnr --write) is what icepack packs into the
bitstream: ICESTORM_LC / ICESTORM_RAM / ICESTORM_SPRAM / ICESTORM_PLL /
SB_GB / SB_IO cells, placed and routed. Two things keep it from
simulating as it is:

  - Inputs nextpnr left unrouted. The hardware reads a fixed level there,
    which yosys' cell models give with `(X === 1'bz) ? 1'b1 : X` style
    pull-ups and pull-downs -- and Verilator has no z. Each unrouted input
    of a logic or RAM cell is tied to its model's level in <out.json>,
    and <out_cells.v> is cells_sim.v without those checks.
  - The PLL's dedicated paths, pad -> REFERENCECLK and PLLOUT -> its global
    buffers, are not nets in the netlist. The testbench clock drives them,
    as in the RTL sims: both PLL global buffers follow the
    `clk` pad (sim_pnr_cells.v models the PLL itself as always locked).

Any other unrouted input stops the script: its level would be a guess.
"""
import collections
import json
import re
import sys

cells_v, routed, out_json, out_cells = sys.argv[1:]
models = open(cells_v).read()

# The inputs each cell model pulls up when undriven; the others pull down.
GUARD = r"\((\w+) === 1'bz\) \? 1'b([01]) : \1"
pull_up = {}
for name, body in re.findall(r'^module\s+(\w+)(.*?)^endmodule', models, re.S | re.M):
    pull_up[name] = {p for p, level in re.findall(GUARD, body) if level == '1'}
    pull_up[name] |= set(re.findall(r'\bpu\((\w+)\)', body))

design = json.load(open(routed))
top = design['modules']['top']
clk = top['ports']['clk']['bits']
tied, clocked = collections.Counter(), []
for name, cell in top['cells'].items():
    kind, conn = cell['type'], cell['connections']
    for port, direction in cell['port_directions'].items():
        if direction != 'input' or conn.get(port):
            continue
        if kind in ('ICESTORM_LC', 'ICESTORM_RAM'):
            conn[port] = ['1' if port in pull_up[kind] else '0']
            tied[kind] += 1
        elif kind == 'ICESTORM_PLL' and port == 'REFERENCECLK' or \
             kind == 'SB_GB' and '_pllout_' in name:
            conn[port] = clk
            clocked.append(name)
        elif kind not in ('SB_IO', 'ICESTORM_PLL'):
            # (the pads become wires, sim_pnr_io.v; the PLL stand-in,
            # sim_pnr_cells.v, reads nothing but REFERENCECLK)
            sys.exit(f'sim_pnr.py: unrouted input {name}.{port} ({kind}): no known level')
json.dump(design, open(out_json, 'w'))

# Every input of the netlist's cells is driven now: drop the z checks.
stripped = re.sub(GUARD, r'\1', models)
for kind in {cell['type'] for cell in top['cells'].values()} - {'SB_IO'}:
    m = re.search(r'^module\s+%s\b(.*?)^endmodule' % kind, stripped, re.S | re.M)
    if m and "1'bz" in m.group(1):
        sys.exit(f'sim_pnr.py: the {kind} model still compares with z')
open(out_cells, 'w').write(stripped)

print('sim_pnr.py: unrouted inputs tied: ' + ', '.join(f'{k} {v}' for k, v in sorted(tied.items()))
      + f'; from the clk pad: {", ".join(sorted(clocked))}')
