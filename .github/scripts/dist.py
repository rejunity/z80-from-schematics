#!/usr/bin/env python3
"""dist.py - the downloads and the GitHub Pages site of the Targets runs.

  dist.py fpga <board> <out>   fpga/<board>'s bitstreams after `make ci`,
                               named for download, + rz80-<board>.json
  dist.py asic <pdk> <out>     asic/<pdk>'s latest LibreLane runs, the core
                               and the chip: the GDS, a picture of it, a map
                               of the cells by Z80 block, and a map of the
                               core's cells by kind or of the chip's clock
                               and power (needs gdstk, numpy and Pillow);
                               for a chip simulated at transistor level
                               (asic/spice/chip/<pdk>/), its pins as a logic
                               analyser shows them, and all of that
                               simulation's figures in spice/chip/<pdk>/
                               (other corners' in spice/chip/<pdk>/<corner>/)
                               + rz80-<pdk>.json
  dist.py site <in> <out>      index.html over everything fpga/asic made,
                               and over <in>/spice/: the pads and the NMI
                               catcher at transistor level (spice/pads/,
                               spice/nmi/; targets.yml)
  dist.py summary <in> <url>   job-summary markdown: the layout renders as
                               served by the site deployed at <url>

fpga and asic run in the repository root at the end of each job of
fpga-board.yml / asic-pdk.yml (artifacts fpga-<board>-bitstreams and
asic-<pdk>-gds); targets.yml downloads them all into one directory for
site and summary.
"""
import fnmatch
import glob
import html
import json
import os
import re
import shutil
import sys
from datetime import datetime, timezone

SERVER = os.environ.get('GITHUB_SERVER_URL', 'https://github.com')
REPO = os.environ.get('GITHUB_REPOSITORY', 'rejunity/z80-from-schematics')
RUN_ID = os.environ.get('GITHUB_RUN_ID', '')
RUN_URL = f'{SERVER}/{REPO}/actions/runs/{RUN_ID}' if RUN_ID else ''

BOARDS = {
    'icepi-zero':      ('IcePi Zero', 'Lattice ECP5 LFE5U-25F'),
    'tang-primer-25k': ('Sipeed Tang Primer 25K', 'Gowin GW5A-LV25MG121NES'),
    'icebreaker':      ('iCEBreaker', 'Lattice iCE40UP5K'),
    'pico-ice':        ('pico-ice', 'Lattice iCE40UP5K + RP2040'),
    'icesugar':        ('iCESugar', 'Lattice iCE40UP5K'),
}
PDKS = {
    'sky130':     'SkyWater SKY130',
    'ihp-sg13g2': 'IHP SG13G2',
    'gf180mcu':   'GlobalFoundries GF180MCU',
    'gf180mcu-9t': 'GlobalFoundries GF180MCU, 9-track cells',
}
# site and summary order: each PDK's core, then the chips with pad rings
ENTRIES = list(PDKS) + [f'{p}-chip' for p in PDKS]
# Layout pictures: each PDK's drawn device and metal layers, bottom to top,
# blended as translucent colours over a dark background, so the cell rows,
# the routing and the power straps all show at die scale. (LibreLane's own
# KLayout render uses the PDK's layer properties, which read badly at this
# scale: sky130's is one magenta block of met2. It stays in the results
# artifact and stands in if render() fails.)
STYLE = {'diff': ((70, 110, 80), .5), 'poly': ((150, 70, 70), .5), 'li': ((150, 130, 100), .45),
         'm1': ((60, 130, 246), .55), 'm2': ((217, 70, 239), .5), 'm3': ((34, 211, 238), .5),
         'm4': ((132, 204, 22), .5), 'm5': ((245, 158, 11), .55),
         'top1': ((253, 224, 71), .55), 'top2': ((249, 115, 22), .55)}
STACKS = {  # (GDS layer, datatype, style) of the drawing layers, bottom to top
    'sky130':     [(65, 20, 'diff'), (66, 20, 'poly'), (67, 20, 'li'), (68, 20, 'm1'), (69, 20, 'm2'),
                   (70, 20, 'm3'), (71, 20, 'm4'), (72, 20, 'm5')],
    'ihp-sg13g2': [(1, 0, 'diff'), (5, 0, 'poly'), (8, 0, 'm1'), (10, 0, 'm2'), (30, 0, 'm3'),
                   (50, 0, 'm4'), (67, 0, 'm5'), (126, 0, 'top1'), (134, 0, 'top2')],
    'gf180mcu':   [(22, 0, 'diff'), (30, 0, 'poly'), (34, 0, 'm1'), (36, 0, 'm2'), (42, 0, 'm3'),
                   (46, 0, 'm4'), (81, 0, 'm5'), (53, 0, 'top1')],
}
STACKS['gf180mcu-9t'] = STACKS['gf180mcu']
BACKGROUND = (13, 17, 23)
# Cell map: every standard-cell instance (a reference in the GDS) as its
# outline, coloured by the kind of library cell. First match wins, on the
# cell name without library prefix and drive strength (sky130 dfrtp_1,
# IHP dfrbpq_1, gf180 dffrnq_1; dlygate / dlyb are delay buffers, not
# latches).
CELL_KINDS = {  # kind: (name pattern, colour), in legend order
    'I/O pads': (r'(gf180mcu_fd_io__|sg13g2_IOPad|sg13g2_Corner|sg13g2_Filler|bondpad)', (148, 163, 184)),
    'flip-flops': (r'(s?df|edf)', (255, 90, 54)),
    'latches': (r'(dl[xrhl]|s?dlclkp|lat|icg|s?lgcp)', (236, 72, 153)),
    'buffers and delays': (r'(buf|inv|clkbuf|clkinv|clkdlybuf|dly)', (20, 184, 166)),
    'logic': (None, (79, 111, 174)),   # everything else
    'fill/decap/tap': (r'(fill|decap|tap|endcap|diode|antenna|tie|conb)', (43, 49, 59)),
}
CELL_LIBRARY_PREFIX = r'^(sky130_fd_sc_\w+?__|sg13g2_|gf180mcu_fd_sc_mcu\w+?__)'
# Block map: every placed instance (final DEF, sizes from the cell LEFs)
# coloured by the RTL block it was synthesized in. Synthesis flattens only
# after mapping (SYNTH_HIERARCHY_MODE deferred_flatten), so instance names
# keep their path: u_core.u_alu._123_. Top-level _N_ cells are z80_top's
# pin logic; other unprefixed names are what place-and-route added.
BLOCKS = {  # block: (instance name pattern, colour), first match wins, in legend order
    'ALU': (r'u_core\.u_alu\.', (239, 68, 68)),
    'register file': (r'u_core\.u_regs\.', (250, 204, 21)),
    'decoder PLA': (r'u_core\.u_pla\.', (168, 85, 247)),
    'execute': (r'u_core\.u_exec\.', (59, 130, 246)),
    'address': (r'u_core\.u_addr\.', (34, 197, 94)),
    'timing': (r'u_core\.u_timing', (34, 211, 238)),
    'core sequencer, state': (r'u_core\.', (244, 114, 182)),
    'pin logic': (r'_\d+_$', (226, 232, 240)),
    'P&R buffers, clock tree': (r'', (90, 110, 120)),
}
# the cells RZ80.DieFloorplan fixed where the die has their counterparts
# (asic/librelane_plugin_rz80, its anchors.json): by the die block they
# stand in, which for the register file's PC, IR, WZ, SP, IX and IY columns
# (z80_core's own flip-flops) and the latches beside it is not the RTL block
ANCHOR_BLOCKS = [  # (anchor role pattern, block)
    (r'(regfile|bank) ', 'register file'),
    (r'pla ', 'decoder PLA'),
    (r'datapath (addr_hold|m_addr)\[', 'address'),
    (r'datapath (tmp|wdata)\[', 'ALU'),
    (r'alu ', 'ALU'),
    (r'addr ', 'address'),
]
# the anchors that stand for one bit of the datapath (the register file's
# rows, the ALU's and the address unit's bit planes, the latches beside
# them) take their block's colour on even bits and a darker shade of it on
# odd bits, so each bit reads as its own band; shared strips keep the
# plain colour. Runs without anchors render as before.
BIT_ROLE = r'(?:regfile \S+ bit |alu bit |addr bit |datapath \w+\[)(\d+)'
BIT_SHADED = ('register file', 'ALU', 'address')
ODD_BITS = {f'{block} (odd bits)': (None, tuple(int(v * .58) for v in BLOCKS[block][1])) for block in BIT_SHADED}
PAD_BLOCK = ('I/O pads', (148, 163, 184))
# the pad ring's corners, fillers and bond pads, darker than the pads
RING = (58, 66, 80)
# Clock and power picture of a chip, from its final DEF and its macro's:
# the power nets' rings, stripes and rails (rails faint), the supply pads in
# their net's colour, and on top the clock -- the CLK pad and its bond pad,
# every USE CLOCK wire, every clock buffer -- in green with a dark halo.
CLOCK = (74, 222, 128)
POWER = {  # net: (legend, colour)
    'VDD': ('core supply (VDD)', (248, 113, 113)),
    'VSS': ('ground (VSS)', (96, 165, 250)),
    'IOVDD': ('I/O supply (IOVDD)', (251, 191, 36)),
    'IOVSS': ('I/O ground (IOVSS)', (45, 212, 191)),
}
SUPPLY_PADS = [  # (pad master pattern, its net), first match wins
    (r'gf180mcu_fd_io__dvdd', 'VDD'), (r'gf180mcu_fd_io__dvss', 'VSS'),
    (r'sg13g2_IOPadIOVdd', 'IOVDD'), (r'sg13g2_IOPadIOVss', 'IOVSS'),
    (r'sg13g2_IOPadVdd', 'VDD'), (r'sg13g2_IOPadVss', 'VSS'),
]
SIGNAL_PAD = (100, 112, 130)
# (make ci's build, download name): the sbc build with NASCOM BASIC, the
# same with Tiny BASIC (build_tiny/), the Z80 chip replacement
VARIANTS = (('build/top_sbc', 'basic'), ('build_tiny/top_sbc', 'tinybasic'), ('build/top_chip', 'chip'))
BITSTREAM_EXTS = ('bit', 'bin', 'fs', 'uf2')


def readme_url(kind, name):
    branch = f'fpga-{name}-basic' if kind == 'fpga' else f'asic-{name}'
    return f'{SERVER}/{REPO}/blob/{branch}/{kind}/{name}/README.md'


def fpga(board, out):
    files = []
    for build, variant in VARIANTS:
        for ext in BITSTREAM_EXTS:
            src = os.path.join('fpga', board, f'{build}.{ext}')
            if os.path.isfile(src):
                os.makedirs(out, exist_ok=True)
                files.append(f'rz80-{board}-{variant}.{ext}')
                shutil.copy(src, os.path.join(out, files[-1]))
    if not files:
        return
    name, device = BOARDS.get(board, (board, ''))
    info = {'board': board, 'name': name, 'device': device, 'files': files,
            'built_from': os.environ.get('BUILT_FROM', ''), 'run': RUN_URL}
    json.dump(info, open(os.path.join(out, f'rz80-{board}.json'), 'w'), indent=1)
    w = len(f'rz80-{board}-tinybasic.*  ')
    lines = [f'rz80 for the {name} ({device})',
             f"built from {info['built_from'] or '?'}" + (f', GitHub Actions run {RUN_URL}' if RUN_URL else ''), '',
             f'rz80-{board}-basic.*'.ljust(w) + "BASIC single-board computer: NASCOM BASIC 4.7",
             ' ' * w + "in ROM, console on the board's serial port",
             f'rz80-{board}-tinybasic.*'.ljust(w) + 'the same computer with 1K Tiny BASIC',
             f'rz80-{board}-chip.*'.ljust(w) + "Z80 chip replacement: the board's pins are a",
             ' ' * w + "Z80's bus, CLK comes from outside"]
    if any(f.endswith('.uf2') for f in files):
        lines.append('(.uf2: the same bitstream, for the board\'s USB drive)')
    lines += ['', 'Pins, console settings and how to program the board:', readme_url('fpga', board)]
    open(os.path.join(out, f'rz80-{board}-README.txt'), 'w').write('\n'.join(lines) + '\n')


def render(top, png, stack, width=1200, supersample=3):
    """Draw each layer's polygons (flattened) at supersample x the final
    size, box-filter that down to the layer's coverage per pixel and blend
    its colour in by coverage x opacity; save as a 256-colour PNG."""
    import numpy as np
    from PIL import Image, ImageDraw
    (x0, y0), (x1, y1) = top.bounding_box()
    w, h = width, round(width * (y1 - y0) / (x1 - x0))
    k = w * supersample / (x1 - x0)
    img = np.empty((h, w, 3))
    img[:] = BACKGROUND
    for layer, datatype, style in stack:
        polygons = top.get_polygons(layer=layer, datatype=datatype)
        if not polygons:
            continue
        mask = Image.new('L', (w * supersample, h * supersample), 0)
        draw = ImageDraw.Draw(mask)
        for p in polygons:
            draw.polygon(((p.points - (x0, y1)) * (k, -k)).ravel().tolist(), fill=255)
        rgb, opacity = STYLE[style]
        a = np.asarray(mask.resize((w, h), Image.BOX), float)[..., None] / 255 * opacity
        img = img * (1 - a) + np.array(rgb, float) * a
    Image.fromarray(img.round().astype(np.uint8)).quantize(256).save(png, optimize=True)


def is_pad(master):
    """A signal or supply pad (not a pad-ring corner, filler or bond pad)."""
    return master.startswith(('gf180mcu_fd_io__bi', 'gf180mcu_fd_io__in', 'gf180mcu_fd_io__dv', 'sg13g2_IOPad'))


def cell_kind(name):
    # a chip's GDS renames the cells inside a hard macro with a short tag:
    # UF_<library cell>, Y3_<library cell>, ...
    name = re.sub(r'^\w*?_(?=sky130_fd_sc|sg13g2_|gf180mcu_fd_sc)', '', name)
    if re.match(CELL_KINDS['I/O pads'][0], name):
        return 'I/O pads'
    base = re.sub(r'_\d+$', '', re.sub(CELL_LIBRARY_PREFIX, '', name))
    return next((kind for kind, (pattern, _) in CELL_KINDS.items()
                 if pattern and kind != 'I/O pads' and re.match(pattern, base)), 'logic')


def transform(ref, point):
    """A point of a referenced cell in the referencing cell (gdstk order:
    x reflection, magnification, rotation, then the origin)."""
    import math
    x, y = point
    if ref.x_reflection:
        y = -y
    m = ref.magnification or 1
    c, s = math.cos(ref.rotation or 0), math.sin(ref.rotation or 0)
    return (ref.origin[0] + m * (x * c - y * s), ref.origin[1] + m * (x * s + y * c))


def transform_box(ref):
    """The corners of a reference's bounding box, in the referencing cell."""
    (a, b), (c, d) = ref.bounding_box()
    return [(a, b), (a, d), (c, b), (c, d)]


def render_cells(top, png, width=1200, supersample=3):
    """The cell map; returns the instance count per kind (png None: the
    counts only)."""
    import gdstk
    from PIL import Image, ImageDraw
    (x0, y0), (x1, y1) = top.bounding_box()
    w, h = width, round(width * (y1 - y0) / (x1 - x0))
    k = w * supersample / (x1 - x0)
    im = Image.new('RGB', (w * supersample, h * supersample), BACKGROUND)
    draw = ImageDraw.Draw(im)
    counts = dict.fromkeys(CELL_KINDS, 0)

    def placed(cell, outer=lambda p: p):
        """(library cell name, bounding box) of every standard or pad cell
        instance, through hard macros (a chip's z80_top) -- not into the
        library cells themselves, whose GDS has subcells of its own."""
        for r in cell.references:
            if not isinstance(r.cell, gdstk.Cell):
                continue
            if r.cell.references and not re.match(CELL_LIBRARY_PREFIX, r.cell.name) and cell_kind(r.cell.name) != 'I/O pads':
                yield from placed(r.cell, lambda p, r=r, outer=outer: outer(transform(r, p)))
            else:
                corners = [outer(q) for q in transform_box(r)]
                xs, ys = [q[0] for q in corners], [q[1] for q in corners]
                yield r.cell.name, (min(xs), min(ys), max(xs), max(ys))
    cells = [(cell_kind(name), name, box) for name, box in placed(top)]
    cells.sort(key=lambda c: c[0] != 'fill/decap/tap')   # fillers first: real cells draw over them
    for kind, name, (a, b, c, d) in cells:
        counts[kind] += kind != 'I/O pads' or is_pad(name)
        if png is None:
            continue
        rgb = RING if kind == 'I/O pads' and not is_pad(name) else CELL_KINDS[kind][1]
        draw.rectangle(((a - x0) * k, (y1 - d) * k, (c - x0) * k, (y1 - b) * k),
                       fill=rgb, outline=tuple(int(v * .55) for v in rgb), width=max(1, supersample // 2))
    if png is not None:
        im.resize((w, h), Image.LANCZOS).quantize(256).save(png, optimize=True)
    return counts


def read_def(run):
    """The die box and the placed components (name, master, x, y, orientation)
    of a run's final DEF, in um; None without one."""
    defs = glob.glob(run + 'final/def/*.def')
    if not defs:
        return None
    text = open(defs[0]).read()
    dbu = int(re.search(r'UNITS DISTANCE MICRONS (\d+)', text).group(1))
    die = tuple(int(v) / dbu for v in re.search(
        r'DIEAREA\s*\(\s*(-?\d+)\s+(-?\d+)\s*\)\s*\(\s*(-?\d+)\s+(-?\d+)\s*\)', text).groups())
    components = text[text.index('COMPONENTS'):text.index('END COMPONENTS')]
    placed = [(name.replace('\\', ''), master, int(x) / dbu, int(y) / dbu, orient) for name, master, x, y, orient in
              re.findall(r'-\s+(\S+)\s+(\S+)[^;]*?\+\s*(?:PLACED|FIXED)\s*\(\s*(-?\d+)\s+(-?\d+)\s*\)\s*(\w+)', components)]
    return die, placed


def anchor_blocks(run):
    """{instance: block} for the cells a run's RZ80.DieFloorplan step fixed"""
    out = {}
    for path in glob.glob(run + '*-rz80-diefloorplan/anchors.json'):
        for a in json.load(open(path))['anchors']:
            if block := next((b for pattern, b in ANCHOR_BLOCKS if re.match(pattern, a['role'])), None):
                bit = re.match(BIT_ROLE, a['role'])
                if block in BIT_SHADED and bit and int(bit.group(1)) % 2:
                    block = f'{block} (odd bits)'
                out[a['inst'].replace('\\', '')] = block
    return out


def block_legend():
    """the block map's legend: each block, its odd-bit shade after it"""
    table = {}
    for block, entry in BLOCKS.items():
        table[block] = entry
        if f'{block} (odd bits)' in ODD_BITS:
            table[f'{block} (odd bits)'] = ODD_BITS[f'{block} (odd bits)']
    return table


def render_blocks(run, png, width=1200, supersample=3, macro_runs=None):
    """The block map; returns the instance count per block, or None when
    the run's instance names carry no hierarchy (flattened before synthesis).
    macro_runs: {macro master: its run}, whose DEF supplies the macro's
    cells (placed unrotated, as the chip's z80_top is)."""
    from PIL import Image, ImageDraw
    cfg = json.load(open(run + 'resolved.json'))
    if not (got := read_def(run)):
        return None
    (x0, y0, x1, y1), placed = got
    for comp in [c for c in placed if c[1] in (macro_runs or {})]:
        name, master, mx, my, orient = comp
        if (inner := read_def(macro_runs[master])) and orient == 'N':
            placed.remove(comp)
            placed += [(n, m, mx + x, my + y, o) for n, m, x, y, o in inner[1]]
    if not any(re.match(r'(u_top\.)?u_core\.', name) for name, *_ in placed):
        return None
    sizes = {}
    for lef in (cfg.get('CELL_LEFS') or []) + (cfg.get('PAD_LEFS') or []) + (cfg.get('EXTRA_LEFS') or []):
        for macro, body in re.findall(r'MACRO\s+(\S+)(.*?)END\s+\1\b', open(lef).read(), re.S):
            if size := re.search(r'SIZE\s+([\d.]+)\s+BY\s+([\d.]+)', body):
                sizes[macro] = (float(size.group(1)), float(size.group(2)))
    w, h = width, round(width * (y1 - y0) / (x1 - x0))
    k = w * supersample / (x1 - x0)
    im = Image.new('RGB', (w * supersample, h * supersample), BACKGROUND)
    draw = ImageDraw.Draw(im)
    counts = {block: 0 for block in BLOCKS}
    if cfg.get('PAD_LEFS'):
        counts = {PAD_BLOCK[0]: 0, **counts}
    fill = CELL_KINDS['fill/decap/tap']
    anchored = anchor_blocks(run)
    for macro_run in (macro_runs or {}).values():
        anchored.update(anchor_blocks(macro_run))
    cells = []
    for name, master, x, y, orient in placed:
        if master not in sizes:
            continue
        if cell_kind(master) == 'I/O pads':
            counts[PAD_BLOCK[0]] += is_pad(master)
            cells.append((PAD_BLOCK[0], PAD_BLOCK[1] if is_pad(master) else RING, master, x, y, orient))
            continue
        if cell_kind(master) == 'fill/decap/tap' or name.startswith(('FILLER', 'TAP_', 'PHY_EDGE')):
            cells.append((None, fill[1], master, x, y, orient))
            continue
        name = re.sub(r'^u_top\.', '', name)   # the chip's z80_top instance
        block = anchored.get(name) or next(b for b, (pattern, _) in BLOCKS.items() if re.match(pattern, name) or not pattern)
        counts[block] = counts.get(block, 0) + 1
        cells.append((block, (BLOCKS.get(block) or ODD_BITS[block])[1], master, x, y, orient))
    cells.sort(key=lambda c: c[0] is not None)   # fillers first: real cells draw over them
    for block, rgb, master, x, y, orient in cells:
        cw, ch = sizes[master]
        if orient in ('E', 'W', 'FE', 'FW'):
            cw, ch = ch, cw
        draw.rectangle(((x - x0) * k, (y1 - y - ch) * k, (x + cw - x0) * k, (y1 - y) * k),
                       fill=rgb, outline=tuple(int(v * .55) for v in rgb), width=max(1, supersample // 2))
    im.resize((w, h), Image.LANCZOS).quantize(256).save(png, optimize=True)
    return counts



def def_routes(text, section, keep):
    """The routed shapes of the nets in a DEF section ('NETS', 'SPECIALNETS')
    that keep(name, statement) accepts, in DEF units: (net, shape, width,
    (x1, y1, x2, y2), kind) -- kind 'path' for a centreline segment (width
    the special wire's, None for a regular net's), 'rect' for a rectangle."""
    start = text.find('\n' + section + ' ')
    if start < 0:
        return []
    out = []
    for stmt in text[start:text.find('\nEND ' + section, start)].split(';'):
        m = re.match(r'\s*-\s+(\S+)', stmt)
        if not m or not keep(m.group(1), stmt):
            continue
        net = m.group(1)
        for a, b, c, d in re.findall(r'\+\s*RECT\s+\S+\s*\(\s*(-?\d+)\s+(-?\d+)\s*\)\s*\(\s*(-?\d+)\s+(-?\d+)\s*\)', stmt):
            out.append((net, 'RECT', None, (int(a), int(b), int(c), int(d)), 'rect'))
        for seg in re.split(r'\b(?:ROUTED|FIXED|COVER|SHIELD|NEW)\b', stmt)[1:]:
            toks = seg.split()
            if not toks:
                continue
            width = int(toks[1]) if len(toks) > 1 and toks[1].isdigit() else None
            shape = (re.search(r'\+\s*SHAPE\s+(\w+)', seg) or [None, None])[1]
            pts, last = [], None
            for item in re.finditer(r'(RECT\s*)?\(\s*([^)]*)\)', seg):
                vals = item.group(2).split()
                if item.group(1):          # RECT ( dx1 dy1 dx2 dy2 ) at the last point
                    if last and len(vals) == 4:
                        dx1, dy1, dx2, dy2 = map(int, vals)
                        out.append((net, shape, None, (last[0] + dx1, last[1] + dy1, last[0] + dx2, last[1] + dy2), 'rect'))
                    continue
                if len(vals) < 2:
                    continue
                last = (last[0] if vals[0] == '*' else int(vals[0]), last[1] if vals[1] == '*' else int(vals[1]))
                pts.append(last)
            out += [(net, shape, width, (p[0], p[1], q[0], q[1]), 'path') for p, q in zip(pts, pts[1:])]
    return out


def render_power_clock(run, png, macro_runs, width=1200, supersample=3):
    """A chip's clock and power distribution (see CLOCK / POWER); returns
    the legend entries drawn: {label: (colour, count)}."""
    import numpy as np
    from PIL import Image, ImageDraw
    cfg = json.load(open(run + 'resolved.json'))
    if not (got := read_def(run)):
        return None
    (x0, y0, x1, y1), placed = got
    sizes = {}
    lefs = (cfg.get('CELL_LEFS') or []) + (cfg.get('PAD_LEFS') or []) + (cfg.get('EXTRA_LEFS') or [])
    lefs += [f for m in (cfg.get('MACROS') or {}).values() for f in (m.get('lef') or [])]
    for lef in lefs:
        for macro, body in re.findall(r'MACRO\s+(\S+)(.*?)END\s+\1\b', open(lef).read(), re.S):
            if size := re.search(r'SIZE\s+([\d.]+)\s+BY\s+([\d.]+)', body):
                sizes[macro] = (float(size.group(1)), float(size.group(2)))
    w, h = width, round(width * (y1 - y0) / (x1 - x0))
    k = w * supersample / (x1 - x0)
    W, H = w * supersample, h * supersample
    px = lambda x, y: ((x - x0) * k, (y1 - y) * k)
    img = np.empty((h, w, 3))
    img[:] = BACKGROUND

    def layer(draw_fn, rgb, opacity):
        nonlocal img
        mask = Image.new('L', (W, H), 0)
        draw_fn(ImageDraw.Draw(mask))
        a = np.asarray(mask.resize((w, h), Image.BOX), float)[..., None] / 255 * opacity
        img = img * (1 - a) + np.array(rgb, float) * a

    def box(name, master, x, y, orient):
        cw, ch = sizes[master]
        if orient in ('E', 'W', 'FE', 'FW'):
            cw, ch = ch, cw
        return x, y, x + cw, y + ch

    # the macro's own cells and wiring, moved to where the chip placed it
    dbu = lambda t: int(re.search(r'UNITS DISTANCE MICRONS (\d+)', t).group(1))
    text = open(glob.glob(run + 'final/def/*.def')[0]).read()
    sources = [(text, dbu(text), 0.0, 0.0)]
    macro_boxes = []
    for name, master, mx, my, orient in placed:
        if master in (macro_runs or {}) and master in sizes:
            macro_boxes.append(box(name, master, mx, my, orient))
            if orient == 'N' and (defs := glob.glob(macro_runs[master] + 'final/def/*.def')):
                inner = open(defs[0]).read()
                sources.append((inner, dbu(inner), mx, my))
                placed = placed + [(n, m, mx + x, my + y, o) for n, m, x, y, o in read_def(macro_runs[master])[1]]

    # pads: signal grey, supply in its net's colour, CLK green; the ring's
    # corners and fillers dark; IHP's bond pads (IO_BOND_<pad>) as their pad
    masters = {name: master for name, master, *_ in placed}
    def pad_colour(name, master):
        if name.startswith('IO_BOND_'):
            pad = name[len('IO_BOND_'):]
            return pad_colour(pad, masters.get(pad, ''))
        if re.search(r'(^|_)CLK$', name):
            return CLOCK
        net = next((n for pat, n in SUPPLY_PADS if re.match(pat, master)), None)
        if net:
            return POWER[net][1]
        return SIGNAL_PAD if is_pad(master) or master.startswith('bondpad') else RING
    pads = [(pad_colour(n, m), m, box(n, m, x, y, o)) for n, m, x, y, o in placed
            if m in sizes and (cell_kind(m) == 'I/O pads' or m.startswith('bondpad'))]
    pad_count = lambda rgb: sum(1 for c, m, _ in pads if c == rgb and is_pad(m))
    for rgb in {c for c, *_ in pads}:
        def draw(d, rgb=rgb):
            for c, _, (a, b, cc, dd) in pads:
                if c == rgb:
                    d.rectangle((*px(a, dd), *px(cc, b)), fill=255)
        layer(draw, rgb, .85)
    def outline(d):
        for a, b, c, dd in macro_boxes:
            d.rectangle((*px(a, dd), *px(c, b)), outline=255, width=supersample * 2)
    layer(outline, (148, 163, 184), .6)

    # power: every power net's special wiring (no fill); the cell rows'
    # rails faint, rings and stripes solid
    def is_power(name, stmt):
        return name in POWER
    for net in POWER:
        segs = []
        for t, u, ox, oy in sources:
            segs += [(shape, wd, kind, (ax / u + ox, ay / u + oy, bx / u + ox, by / u + oy), wd and wd / u)
                     for n, shape, wd, (ax, ay, bx, by), kind in def_routes(t, 'SPECIALNETS', lambda n, s: n == net)
                     if shape != 'DRCFILL']
        if not segs:
            continue
        def shapes(d, rails):
            for shape, wd, kind, (ax, ay, bx, by), wum in segs:
                if (shape == 'FOLLOWPIN') != rails:
                    continue
                if kind == 'rect':
                    d.rectangle((*px(min(ax, bx), max(ay, by)), *px(max(ax, bx), min(ay, by))), fill=255)
                else:
                    half = (wum or 0) / 2
                    d.rectangle((*px(min(ax, bx) - half, max(ay, by) + half),
                                 *px(max(ax, bx) + half, min(ay, by) - half)), fill=255)
        layer(lambda d: shapes(d, True), POWER[net][1], .3)
        layer(lambda d: shapes(d, False), POWER[net][1], .8)

    # clock: every USE CLOCK wire, chip and macro, and the clock buffers, on
    # top: a dark halo, then green; never thinner than 3 px
    wires = []
    for t, u, ox, oy in sources:
        wires += [((ax / u + ox, ay / u + oy), (bx / u + ox, by / u + oy))
                  for n, shape, wd, (ax, ay, bx, by), kind in def_routes(t, 'NETS', lambda n, s: '+ USE CLOCK' in s)
                  if kind == 'path']
    bufs = [box(n, m, x, y, o) for n, m, x, y, o in placed
            if m in sizes and re.match(r'(clkbuf|clkload|clkinv)', n.split('.')[-1])]
    def clock(d, grow):
        lw = (3 + 2 * grow) * supersample
        for p, q in wires:
            d.line((*px(*p), *px(*q)), fill=255, width=lw)
        for a, b, c, dd in bufs:
            (ax, ay), (bx, by) = px(a, dd), px(c, b)
            g = max(0, (3 * supersample - (bx - ax)) / 2) + grow * supersample
            d.rectangle((ax - g, ay - g, bx + g, by + g), fill=255)
    layer(lambda d: clock(d, 2), BACKGROUND, .9)
    layer(lambda d: clock(d, 0), CLOCK, 1.0)
    # legend: the clock buffers; the pads of each supply net and the signals
    counts = {'clock: CLK pad, wires, buffers': (CLOCK, len(bufs))}
    counts.update({label: (rgb, pad_count(rgb)) for label, rgb in POWER.values() if pad_count(rgb)})
    counts['signal pads'] = (SIGNAL_PAD, pad_count(SIGNAL_PAD))
    Image.fromarray(img.round().astype(np.uint8)).quantize(256).save(png, optimize=True)
    return counts


def collect(run, key, name, stack, out, extra=None, macro_runs=None):
    """One LibreLane run: rz80-<key>.gds, its pictures and rz80-<key>.json;
    macro_runs as for render_blocks."""
    cfg = json.load(open(run + 'resolved.json'))
    m = json.load(open(run + 'final/metrics.json'))
    design = cfg['DESIGN_NAME']
    os.makedirs(out, exist_ok=True)
    files, cells, blocks, power = [], None, None, None
    gds = run + f'final/gds/{design}.gds'
    if os.path.isfile(gds):
        shutil.copy(gds, os.path.join(out, f'rz80-{key}.gds'))
        files.append(f'rz80-{key}.gds')
        png = os.path.join(out, f'rz80-{key}.png')
        try:
            import gdstk
            top = gdstk.read_gds(gds).top_level()[0]
            render(top, png, stack)
            files.append(f'rz80-{key}.png')
            if macro_runs:   # a chip: its clock and power, not a cell map
                cells = render_cells(top, None)
                power = render_power_clock(run, os.path.join(out, f'rz80-{key}-power.png'), macro_runs)
                if power:
                    files.append(f'rz80-{key}-power.png')
            else:
                cells = render_cells(top, os.path.join(out, f'rz80-{key}-cells.png'))
                files.append(f'rz80-{key}-cells.png')
            blocks = render_blocks(run, os.path.join(out, f'rz80-{key}-blocks.png'), macro_runs=macro_runs)
            if blocks:
                files.append(f'rz80-{key}-blocks.png')
        except Exception as e:
            print(f"::warning::dist.py: no pictures of {key} ({e!r}), LibreLane's render instead")
            if os.path.isfile(run + f'final/render/{design}.png') and not os.path.isfile(png):
                shutil.copy(run + f'final/render/{design}.png', png)
                files.append(f'rz80-{key}.png')
    x0, y0, x1, y1 = (float(v) for v in m['design__die__bbox'].split())
    # worst setup slack over the corners the flow signs off (summary.py's rule)
    gate = cfg.get('TIMING_VIOLATION_CORNERS') or ['*tt*']
    ws = [v for k, v in m.items() if k.startswith('timing__setup__ws__corner:')
          and any(fnmatch.fnmatch(k.split('corner:')[1], g) for g in gate)]
    info = {'pdk': key, 'name': name, 'pdk_variant': cfg.get('PDK'),
            'library': cfg.get('STD_CELL_LIBRARY'), 'design': design, 'files': files,
            'die_um': [round(x1 - x0, 1), round(y1 - y0, 1)],
            'std_cells': m.get('design__instance__count__stdcell'),
            'utilisation': m.get('design__instance__utilization'),
            'clock_period_ns': cfg.get('CLOCK_PERIOD'),
            'signoff_corners': gate, 'setup_ws_ns': min(ws) if ws else None,
            'cells': cells, 'blocks': blocks, 'power': power, 'built_from': os.environ.get('BUILT_FROM', ''), 'run': RUN_URL,
            **(extra or {})}
    json.dump(info, open(os.path.join(out, f'rz80-{key}.json'), 'w'), indent=1)


def latest(pattern):
    runs = sorted(glob.glob(pattern), key=os.path.getmtime)
    return runs[-1] if runs and os.path.isfile(runs[-1] + 'final/metrics.json') else None


def asic(pdk, out):
    readme = readme_url('asic', pdk)
    core = latest(os.path.join('asic', pdk, 'runs', '*', ''))
    if core:
        collect(core, pdk, PDKS.get(pdk, pdk), STACKS[pdk], out, {'readme': readme})
    # the chip: z80_top, hardened by that run, in its pad ring (asic/<pdk>/chip/, `make chip`)
    if run := latest(os.path.join('asic', pdk, 'chip', 'runs', '*', '')):
        cfg = json.load(open(run + 'resolved.json'))
        pads = sum(len(cfg.get(side) or []) for side in ('PAD_SOUTH', 'PAD_EAST', 'PAD_NORTH', 'PAD_WEST'))
        collect(run, f'{pdk}-chip', f'{PDKS.get(pdk, pdk)} chip, {pads} pads', STACKS[pdk], out,
                {'readme': readme, 'pads': pads}, macro_runs={'z80_top': core} if core else None)
        # its pins, as asic/spice/chip.py simulated them at transistor level (committed: hours of ngspice)
        pins = os.path.join('asic', 'spice', 'chip', pdk, 'program-dark.svg')
        if os.path.isfile(pins):
            shutil.copy(pins, os.path.join(out, f'rz80-{pdk}-chip-pins.svg'))
            info = json.load(open(os.path.join(out, f'rz80-{pdk}-chip.json')))
            info['files'].append(f'rz80-{pdk}-chip-pins.svg')
            json.dump(info, open(os.path.join(out, f'rz80-{pdk}-chip.json'), 'w'), indent=1)
            # and all of that run's figures, for the site's transistor-level section
            res = os.path.join(os.path.dirname(pins), 'results.json')
            if os.path.isfile(res):
                dst = os.path.join(out, 'spice', 'chip', pdk)
                os.makedirs(dst, exist_ok=True)
                for f in json.load(open(res))['figures'] + ['results.json']:
                    shutil.copy(os.path.join(os.path.dirname(pins), f), dst)
                # the other corners' runs, committed in <corner>/ beside it
                for cres in sorted(glob.glob(os.path.join(os.path.dirname(pins), '*', 'results.json'))):
                    cdst = os.path.join(dst, os.path.basename(os.path.dirname(cres)))
                    os.makedirs(cdst, exist_ok=True)
                    for f in json.load(open(cres))['figures'] + ['results.json']:
                        shutil.copy(os.path.join(os.path.dirname(cres), f), cdst)


def load(src):
    """The rz80-<board>.json and rz80-<pdk>.json in src, in BOARDS / PDKS order."""
    infos = [json.load(open(p)) for p in glob.glob(os.path.join(src, 'rz80-*.json'))]
    def order(table, key):
        known = [i for i in infos if i.get(key) in table]
        return sorted(known, key=lambda i: list(table).index(i[key]))
    return order(BOARDS, 'board'), order(dict.fromkeys(ENTRIES), 'pdk')


def size(path):
    n = os.path.getsize(path)
    return f'{n / 1e6:.1f} MB' if n >= 1e6 else f'{n / 1e3:.0f} KB'


def asic_facts(p):
    w, h = p['die_um']
    facts = [('Die', f'{w:,.0f} × {h:,.0f} µm ({w * h / 1e6:.2f} mm²)')]
    if p.get('pads'):
        facts.append(('Pads', f"{p['pads']}, each on the side of the Z80's own"))
    if p.get('std_cells') is not None and not p.get('pads'):   # a chip's are its pad glue only
        u = p.get('utilisation')
        facts.append(('Std cells', f"{p['std_cells']:,}" + (f' ({u * 100:.0f} % utilisation)' if u else '')))
    if p.get('clock_period_ns'):
        clk = f"{1000 / float(p['clock_period_ns']):g} MHz"
        if p.get('setup_ws_ns') is not None:
            clk += f", setup slack {p['setup_ws_ns']:+.2f} ns (typical)"
        facts.append(('Clock', clk))
    return facts


PAD_SETS = {  # spice/pads/<pdk>/ (spice-pads.yml): (name, its pads)
    'gf180mcu': ('GF180', "gf180mcu_fd_io's 5 V pads"),
    'ihp-sg13g2': ('IHP SG13G2', "sg13g2_io's 3.3 V pads"),
}
CHIP_CORNERS = ('tt', 'ss', 'ff')
# the corner switch over the transistor-level figures, in its order (chip.py's and nmi.py's corners)
SWITCH_CORNERS = (('ss', 'Slow', '4.5 V, 125 °C'), ('tt', 'Typical', '5.0 V, 25 °C'),
                  ('ff', 'Fast', '5.5 V, −40 °C'))
CHIP_FIGURES = (('fetch', 'opcode fetch (M1)'), ('mread', 'memory read'), ('mwrite', 'memory write'),
                ('iowrite', 'I/O write'), ('ioread', 'I/O read'))


CODE_SPICE = f'{SERVER}/{REPO}/blob/main/asic/spice'


def spice_helpers(base):
    """there / fig / grid / links over a page's spice/ directory (base), linked as spice/..."""
    e = html.escape
    there = lambda p: os.path.isfile(os.path.join(base, p))

    def fig(path, caption, wide=False):
        if not there(path):
            return ''
        cls = ' class="wide"' if wide else ''
        return (f'<figure{cls}><a href="spice/{path}"><img src="spice/{path}" alt="{e(caption)}" loading="lazy"></a>'
                f'<figcaption>{caption}</figcaption></figure>')

    def grid(*figs, two=False):   # one figure a row, at a readable size; two for thumbnails
        figs = [f for f in figs if f]
        return f'<div class="figs{" two" if two else ""}">{"".join(figs)}</div>' if figs else ''

    def links(items):
        return ' · '.join(f'<a href="spice/{p}">{label}</a>' for p, label in items if there(p))
    return there, fig, grid, links


def switched(there, slots, default='tt'):
    """Figures that change corner in place: a switch (slow, typical, fast) over a grid whose every image
    holds each corner's figure (data-<corner>, spice/...); SWITCH_JS swaps them, every switch on the page
    together. slots: [(caption, wide, {corner: path under spice/})]. One corner: no switch."""
    e = html.escape
    have = [c for c, *_ in SWITCH_CORNERS if any(there(ps.get(c, '')) for _, _, ps in slots)]
    if not have:
        return ''
    default = default if default in have else have[0]
    figs = []
    for caption, wide, ps in slots:
        ok = {c: p for c, p in ps.items() if c in have and there(p)}
        if not ok:
            continue
        first = ok.get(default) or next(iter(ok.values()))
        data = ''.join(f' data-{c}="spice/{p}"' for c, p in ok.items())
        attrs = (' class="wide"' if wide else '') + ('' if default in ok else ' hidden')
        figs.append(f'<figure{attrs}><a href="spice/{first}"><img class="sw" src="spice/{first}"{data} '
                    f'alt="{e(caption)}" loading="lazy"></a><figcaption>{caption}, '
                    f'<span class="cname">{default}</span></figcaption></figure>')
    grid = f'<div class="figs">{"".join(figs)}</div>'
    if len(have) < 2:
        return grid
    buttons = []
    for c, name, cond in SWITCH_CORNERS:
        if c in have:
            on = c == default
            buttons.append(f'<button type="button" role="radio" data-corner="{c}" aria-checked="{"true" if on else "false"}"'
                           f'{"" if on else " tabindex=-1"}>{name}<span class="cv">{c} · {cond}</span></button>')
    return (f'<div class="cornerset"><div class="cornersw" role="radiogroup" aria-label="Corner">'
            f'{"".join(buttons)}</div>{grid}</div>')


def whole_chip(base, pdk, how='hours, so run now and then and committed'):
    """A chip's runs at transistor level (chip.py's results.json and figures, in base/chip/<pdk>/: tt's,
    the other corners' in <corner>/): its pins against the RTL and the AC parameters at every corner, and
    the figures, under a switch between the corners. how: where the runs come from."""
    e = html.escape
    there, fig, grid, links = spice_helpers(base)
    code = CODE_SPICE
    ns = lambda x: '–' if x is None else f'{x:.1f} ns'
    d = f'chip/{pdk}'
    if not there(f'{d}/results.json'):
        return ''
    # tt's run in chip/<pdk>/, the other corners' in chip/<pdk>/<corner>/
    runs = [(json.load(open(os.path.join(base, d, 'results.json'))), d)]
    runs += [(json.load(open(p)), f'{d}/{os.path.basename(os.path.dirname(p))}')
             for p in sorted(glob.glob(os.path.join(base, d, '*', 'results.json')))]
    runs.sort(key=lambda r: CHIP_CORNERS.index(r[0].get('corner')) if r[0].get('corner') in CHIP_CORNERS else 9)
    res = runs[0][0]

    def mismatches(r):
        mism = r.get('functional_mismatches') or []
        if not mism:  # [clock edge (ns), pin, the RTL's value(, the transistors')]
            return '0'
        more = ', ...' if len(mism) > 6 else ''
        return f"<strong>{len(mism)}</strong> ({', '.join(f'{m[1]} at {float(m[0]):.0f} ns' for m in mism[:6])}{more})"

    def spice(r):   # the range over every cycle, and edges the RTL makes that the pins did not
        miss = len(r.get('missing_at_ns') or [])
        rng = (f"{r['spice_min']:.1f} – {r['spice_max']:.1f} ns <span class=\"muted\">({r['n']})</span>"
               if r['n'] else '–')
        return rng + (f" <strong>+ {miss} missing</strong>" if miss else '')

    def margin(r):  # the parameter closest to its limit, over every cycle
        ps = [p for p in r.get('parameters') or [] if p['n']]
        if not ps:
            return '–'
        gap = lambda p: p['limit'] - p['spice_max'] if p['bound'] == 'max' else p['spice_min'] - p['limit']
        p = min(ps, key=gap)
        return f"{gap(p):.1f} ns <span class=\"muted\">{e(p['symbol'])}</span>"

    rows = []
    for r, rd in runs:
        ps = r.get('parameters') or []
        figs = links([(f'{rd}/program.svg', 'program')] + [(f'{rd}/{k}.svg', label) for k, label in CHIP_FIGURES])
        rows.append(f"<tr data-col=\"{e(r.get('corner', ''))}\"><td class=\"nw\"><strong>{e(r.get('corner', ''))}</strong></td><td>{mismatches(r)}</td>"
                    f"<td>{sum(p['pass'] for p in ps)} of {len(ps)}</td><td>{margin(r)}</td><td>{figs}</td></tr>")
    # every parameter at every corner, in the first run's order
    by = [{p['id']: p for p in r.get('parameters') or []} for r, _ in runs]

    def cell(p):
        if not p:
            return '–'
        s = spice(p) + (f" <span class=\"muted\">STA {p['sta']:.1f}</span>" if p.get('sta') is not None else '')
        return s if p['pass'] else f'{s} <strong>fails</strong>'
    params = res.get('parameters') or []
    met = sum(all(c.get(p['id'], {}).get('pass', True) for c in by) for p in params)
    heads = ''.join(f"<th data-col=\"{e(r.get('corner', ''))}\">{e(r.get('corner', ''))}: SPICE (cycles)</th>"
                    for r, _ in runs)
    cs = [e(r.get('corner', '')) for r, _ in runs]

    def prow(p):
        cells = ''.join(f'<td data-col="{c}">{cell(b.get(p["id"]))}</td>' for c, b in zip(cs, by))
        return (f"<tr><td>{p['id']}</td><td>{e(p['symbol'])}</td><td>{'≤' if p['bound'] == 'max' else '≥'} "
                f"{p['limit']:g} ns</td>{cells}</tr>")
    trs = ''.join(prow(p) for p in params)
    table = (f"""<details><summary>The datasheet's AC parameters: {met} of {len(params)} met at every corner,
over every cycle that has them</summary><div class="scroll"><table>
<thead><tr><th>#</th><th>Parameter</th><th>Limit</th>{heads}</tr></thead>
<tbody>{trs}</tbody></table></div></details>""" if params else '')

    # the whole run, then each cycle as the manual draws it; every corner's, switched in place
    g = switched(there, [(caption, k == 'program', {r.get('corner'): f'{rd}/{k}.svg' for r, rd in runs})
                         for k, caption in [('program', 'power-up, RESET, and the program at 4 MHz')] + list(CHIP_FIGURES)])
    shown = ("each corner's: the switch over them changes the corner in place, the dimensions too"
             if len(runs) > 1 else f"{e(res.get('corner', ''))}'s")
    name = PDKS.get(pdk, pdk)
    corners = ', '.join(e(r.get('corner', '')) for r, _ in runs)
    return (f"""<h3 class="sec">The whole chip, {e(name)}</h3>
<p class="muted">The chip's layout extracted with Magic to its transistors and their wiring, and run in
ngspice at each corner ({corners}): power-up, RESET, and a short program
-- a memory read, a memory write, an I/O write and read -- at 4 MHz with 15 ns edges, its outputs into
100 pF (<a href="{code}/chip.py">chip.py</a>; {how}). Its pins are
checked against the RTL at every clock edge after RESET, and the cycles measured as the Z80 manual
draws them; the figures below are {shown}.</p>
<div class="scroll"><table>
<thead><tr><th>Corner</th><th>Pins against the RTL: mismatches</th><th>AC parameters met</th>
<th>Closest to its limit</th><th>Figures</th></tr></thead>
<tbody>
{chr(10).join(rows)}
</tbody>
</table></div>
{table}
{g}""")


def transistor(src, out):
    """The site's transistor-level section, over <src>/spice/ (copied to <out>/spice/): the whole chip
    (asic/spice/chip.py's committed run, as asic() copies it), the GF180 chip's NMI catcher
    (asic/spice/nmi.py, in its ASIC job) and the pads (spice-pads.yml's asic/spice/pads.py)."""
    if not os.path.isdir(os.path.join(src, 'spice')):
        return ''
    base = os.path.join(out, 'spice')
    shutil.copytree(os.path.join(src, 'spice'), base, dirs_exist_ok=True)
    e = html.escape
    there, fig, grid, links = spice_helpers(base)
    code = CODE_SPICE
    ns = lambda x: '–' if x is None else f'{x:.1f} ns'
    parts = []

    pads = []
    for pdk, (name, what) in PAD_SETS.items():
        d = f'pads/{pdk}'
        g = grid(fig(f'{d}/out.svg', 'outputs: edges into 1–250 pF, and their drive'),
                 fig(f'{d}/tristate.svg', 'the data bus: turning on, and floating'),
                 fig(f'{d}/in.svg', 'inputs: pin to core on edges up to 30 ns, and where they switch'),
                 fig(f'{d}/clk_reset.svg', 'CLK and RESET: the Schmitt-trigger input'), two=True)
        if not g:
            continue
        libs = [(f'{d}/{os.path.basename(f)}', re.sub(r'^(gf180mcu_fd_io__|sg13g2_io_)', '', os.path.basename(f)[:-4]))
                for f in sorted(glob.glob(os.path.join(base, d, '*.lib')))]
        more = links([(f'{d}/summary.txt', 'summary'), (f'{d}/delay_vs_load.svg', 'delay vs load'),
                      (f'{d}/inputs.svg', 'inputs vs edge'), (f'{d}/waveforms.svg', 'waveforms'), (f'{d}/pads.json', 'pads.json')])
        liberty = ', '.join(f'<a href="spice/{p}">{e(label)}</a>' for p, label in libs)
        pads.append(f"""<p class="figtitle"><strong>{e(name)}</strong> <span class="muted">{e(what)} · {more}
{'· Liberty: ' + liberty if liberty else ''}</span></p>
{g}""")
    pad_part = []
    if pads:
        pad_part.append(f"""<h3 class="sec">The pads</h3>
<p class="muted">Every pad cell the chips use, from the PDK's own netlist with its transistor models, at the
corners the chips are signed off at (<a href="{code}/pads.py">pads.py</a>): the outputs into 1–250 pF, the
data bus's tri-state pads turning on and floating, the inputs on edges up to 30 ns, and the Schmitt-trigger
input CLK and RESET take. Checked in every run against the committed results; their Liberty files
replace the PDKs' own in the chips' pin-timing checks
(<a href="{SERVER}/{REPO}/blob/main/asic/timing/pin_timing.py">pin_timing.py</a>).</p>
{chr(10).join(pads)}""")

    corners = [c for c in ('tt', 'ss', 'ff') if there(f'nmi/nmi_{c}.json')]
    if corners:
        nmi = {c: json.load(open(os.path.join(base, 'nmi', f'nmi_{c}.json'))) for c in corners}
        main_c = 'tt' if 'tt' in nmi else corners[0]
        about = nmi[main_c].get('about', {})
        rows = []
        for c, m in nmi.items():
            cases = m['cases']
            a = cases.get('A', {})
            caught = sum(v['set_ns'] is not None and v['clear_after_clk_ns'] is not None for v in cases.values())
            figs = links([(f'nmi/nmi_{c}.svg', 'all')] + [(f'nmi/nmi_{k}_{c}.svg', k) for k in cases])
            rows.append(f"<tr data-col=\"{c}\"><td class=\"nw\"><strong>{c}</strong> <span class=\"muted\">{m.get('volts', '')} V, "
                        f"{m.get('temp_c', '')} °C</span></td><td>{caught} of {len(cases)}</td>"
                        f"<td>{ns(a.get('set_ns'))}</td><td>{ns(a.get('clear_after_clk_ns'))}</td>"
                        f"<td>{ns(a.get('held_low_ns'))}</td><td>{figs}</td></tr>")
        g = switched(there, [('the six cases', True, {c: f'nmi/nmi_{c}.svg' for c in corners})]
                     + [(f'{k}: {e(about.get(k, ""))}', False, {c: f'nmi/nmi_{k}_{c}.svg' for c in corners})
                        for k in nmi[main_c]['cases']], default=main_c)
        parts.append(f"""<h3 class="sec">The NMI pulse catcher</h3>
<p class="muted">The NMOS Z80 takes an NMI pulse of any width -- TwNMI, 80 ns at 4 MHz, is under half a
clock -- not only one a clock edge sees: a flip-flop clocked by the NMI pin itself catches it and holds it
for the core until the next rising CLK edge, which clears it
(<a href="{SERVER}/{REPO}/blob/main/rtl/z80_top.v">z80_top.v</a>). Here it is cut out of the GF180 7-track
chip, from the NMI and CLK pins to the flip-flops -- the pads, the clock tree to its leaf, the cells as Magic
extracts them from the PDK's layouts, on the routed chip's wiring -- and run at each corner with six pulses
(<a href="{code}/nmi.py">nmi.py</a>).</p>
<div class="scroll"><table>
<thead><tr><th>Corner</th><th>Pulses caught and cleared</th><th>NMI pin falls → caught (A)</th>
<th>CLK pin rises → cleared (A)</th><th>The core sees NMI for (A)</th><th>Figures</th></tr></thead>
<tbody>
{chr(10).join(rows)}
</tbody>
</table></div>
{g}""")

    chips = []
    for pdk in sorted(os.listdir(os.path.join(base, 'chip'))) if os.path.isdir(os.path.join(base, 'chip')) else []:
        if (chip := whole_chip(base, pdk)):
            ci = (f'''<p class="muted">The run on GitHub's runners, every corner's figures:
<a href="transistor/">transistor/</a> (<a href="{SERVER}/{REPO}/actions/workflows/transistor.yml">transistor.yml</a>,
by hand).</p>''' if os.path.isfile(os.path.join(out, 'transistor', 'index.html')) else '')
            chips.append(chip + ci)

    parts = chips + parts + pad_part
    if not parts:
        return ''
    return f"""<h2 id="transistor">At transistor level</h2>
<p class="muted">The timing above is OpenSTA's, from the PDKs' Liberty models. Here the chips run as
transistors, in ngspice with the PDKs' own netlists and transistor models, against the NMOS Z80
datasheet's test conditions (<a href="{SERVER}/{REPO}/tree/main/asic/spice">asic/spice</a>).</p>
{chr(10).join(parts)}
{SWITCH_JS}"""


def ci_page(src, out):
    """transistor/index.html: the whole chip as transistor.yml last ran it on GitHub's runners -- its
    artifact transistor-gf180mcu, downloaded into <src>/transistor/spice/ (chip/<pdk>/ as chip.py's
    committed runs, and run.json: the run, its commit, the Targets run whose GDS it extracted) -- with
    every corner's figures. False when there is no such run."""
    s = os.path.join(src, 'transistor', 'spice')
    if not os.path.isdir(os.path.join(s, 'chip')):
        return False
    # a run whose pins differ from the RTL's or that fails an AC parameter fails (chip.py), and is not
    # shown -- nor are the runs from before that check
    bad = []
    for p in sorted(glob.glob(os.path.join(s, 'chip', '**', 'results.json'), recursive=True)):
        r = json.load(open(p))
        if r.get('functional_mismatches') or not all(q['pass'] for q in r.get('parameters') or []):
            bad.append(os.path.relpath(p, s))
    if bad:
        print(f'{s}: not published: its pins or AC parameters failed ({", ".join(bad)})')
        return False
    page_dir = os.path.join(out, 'transistor')
    base = os.path.join(page_dir, 'spice')
    shutil.copytree(s, base, dirs_exist_ok=True)
    e = html.escape
    meta = json.load(open(os.path.join(s, 'run.json'))) if os.path.isfile(os.path.join(s, 'run.json')) else {}
    run = (f'<a href="{SERVER}/{REPO}/actions/runs/{meta["run_id"]}">transistor.yml run</a>'
           if meta.get('run_id') else 'transistor.yml run')
    sha = meta.get('sha', '')
    commit = f'<a href="{SERVER}/{REPO}/commit/{sha}">{e(meta.get("ref", "main"))} @ {sha[:7]}</a>' if sha else ''
    gds = (f', the layout from <a href="{SERVER}/{REPO}/actions/runs/{meta["targets_run"]}">Targets run '
           f'{meta["targets_run"]}</a>' if meta.get('targets_run') else '')
    chips = [whole_chip(base, pdk,
                        how=f'on GitHub\'s runners by <a href="{SERVER}/{REPO}/blob/main/.github/workflows/'
                            f'transistor.yml">transistor.yml</a>, each corner in windows of one job each')
             for pdk in sorted(os.listdir(os.path.join(base, 'chip')))]
    page = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>rz80 at transistor level</title>
<style>
{SITE_CSS}</style>
</head>
<body>
<main>
<h1>rz80 at transistor level</h1>
<p class="muted">The chip's layout run as transistors in ngspice, from the latest {run}{' on ' + commit if commit else ''}
{e(meta.get('date', ''))}{gds}. <a href="../">All builds</a>.</p>
{chr(10).join(c for c in chips if c) or '<p>no runs in this artifact</p>'}
<footer>transistor.yml runs by hand (Actions, "Transistor", Run workflow); its artifact transistor-gf180mcu
stays on the <a href="{SERVER}/{REPO}/actions/workflows/transistor.yml">Actions</a> tab for 90 days.</footer>
</main>
{SWITCH_JS}
</body>
</html>
"""
    open(os.path.join(page_dir, 'index.html'), 'w').write(page)
    print(f'{page_dir}/index.html: {sum(bool(c) for c in chips)} chips at transistor level')
    return True


def md_inline(text):
    """the Markdown inside a table cell -- [links](url) and `code` -- as HTML"""
    t = html.escape(text, quote=False)
    t = re.sub(r'\[([^\]]+)\]\(([^)\s]+)\)', lambda m: f'<a href="{html.escape(m.group(2))}">{m.group(1)}</a>', t)
    return re.sub(r'`([^`]+)`', r'<code>\1</code>', t)


def cfast_bench(src, out):
    """The C model's speed: ci.yml's latest cfast-bench artifact (scripts/cfast_bench.py's bench.json and
    bench.svg, in <src>/cfast-bench/), its chart at <out>/cfast-bench.svg -- a fixed address, which
    cfast/BENCHMARK.md shows -- and its table. '' when there is none."""
    d = os.path.join(src, 'cfast-bench')
    if not os.path.isfile(os.path.join(d, 'bench.json')):
        return ''
    e = html.escape
    b = json.load(open(os.path.join(d, 'bench.json')))
    m, res = b['machine'], b['results']
    if os.path.isfile(os.path.join(d, 'bench.svg')):
        shutil.copy(os.path.join(d, 'bench.svg'), os.path.join(out, 'cfast-bench.svg'))
    base = next((r for r in res if r['binary'] == 'zexfast'), res[0])
    rows = ''.join(
        f"<tr><td class=\"nw\">{'<strong>' + e(r['name']) + '</strong>' if r is base else e(r['name'])}</td>"
        f"<td>{e(r['steps'])}</td><td>{r['minstr']:.1f}</td><td>{r['mhz']:.0f} MHz</td>"
        f"<td>{r['minstr'] / base['minstr']:.2f}×</td><td class=\"muted\">{md_inline(r.get('about', ''))}</td></tr>"
        for r in res)
    ref = m.get('ref') or 'main'
    run = (f'<a href="{SERVER}/{REPO}/actions/runs/{m["run"]}">ci.yml run</a>' if m.get('run') else 'ci.yml')
    commit = (f'<a href="{SERVER}/{REPO}/commit/{m["commit"]}">{e(m["ref"]) + " @ " if m.get("ref") else ""}'
              f'{e(m["commit"])}</a>') if m.get('commit') else ''
    return f"""<h2 id="cfast">The C model, for emulators</h2>
<p class="muted">cfast is the C model generated into a phase engine: every half clock the same pins as the model
and the RTL (make cfast_identity), at the speed an emulator needs. Here it runs against four other C Z80
emulators on the same {b['instructions'] / 1e6:g} million ZEXDOC instructions, the best of {b['rounds']}
rounds of CPU time on GitHub's runner ({e(m.get('cpu', ''))}): the latest {run}{' on ' + commit if commit else ''},
{e(m.get('date', ''))}. Every push runs it; how, and what the columns mean:
<a href="{SERVER}/{REPO}/blob/{e(ref)}/cfast/BENCHMARK.md">cfast/BENCHMARK.md</a>.</p>
<div class="figs"><figure><img src="cfast-bench.svg" alt="ZEXDOC million instructions a second, per emulator"
loading="lazy"></figure></div>
<div class="scroll"><table>
<thead><tr><th>Emulator</th><th>Sees every</th><th>ZEXDOC Minstr/s</th><th>Z80 clock</th><th>Against cfast</th>
<th>What it is</th></tr></thead>
<tbody>{rows}</tbody></table></div>"""


SITE_CSS = ''':root { --bg: #f7f7f5; --fg: #1c1c1a; --muted: #6a6a64; --card: #fff; --line: #e2e1dc; --link: #1f55c0; }
@media (prefers-color-scheme: dark) {
  :root { --bg: #141414; --fg: #ebeae6; --muted: #a2a19b; --card: #1d1d1c; --line: #34332f; --link: #8fb1ff; }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 15px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
main { max-width: 1120px; margin: 0 auto; padding: 32px 16px 48px; }
h1 { font-size: 26px; margin: 0 0 6px; }
h2 { font-size: 19px; margin: 36px 0 6px; }
h3 { font-size: 16px; margin: 0; }
p { margin: 0 0 12px; }
a { color: var(--link); }
.muted, .size { color: var(--muted); }
.size { font-size: 12px; }
.pdk { background: var(--card); border: 1px solid var(--line); border-radius: 10px; overflow: hidden;
  margin: 0 0 16px; }
.pics { display: grid; grid-template-columns: repeat(3, 1fr); gap: 1px; background: var(--line); }
figure { margin: 0; position: relative; background: #0d1117; }
figure img, .noimg { display: block; width: 100%; aspect-ratio: 1; object-fit: contain; }
.noimg { display: grid; place-items: center; color: #888; }
.pics .wide { grid-column: 1 / -1; }
.pics .wide img { aspect-ratio: auto; }
figcaption { position: absolute; left: 8px; top: 8px; padding: 1px 7px; border-radius: 4px; font-size: 12px;
  color: #d6dbe1; background: rgba(13, 17, 23, .75); }
.body { display: grid; grid-template-columns: repeat(auto-fit, minmax(230px, 1fr)); gap: 4px 28px;
  padding: 14px 16px 6px; border-top: 1px solid var(--line); }
.body .muted { font-size: 13px; margin: 0 0 8px; }
h4 { font-size: 13px; color: var(--muted); font-weight: 600; margin: 2px 0 6px; }
.legend { list-style: none; padding: 0; margin: 0 0 10px; font-size: 13px; }
.legend li { display: flex; align-items: center; gap: 7px; }
.swatch { width: 10px; height: 10px; border-radius: 2px; flex: none; }
.figs { display: grid; gap: 12px; margin: 8px 0 18px; align-items: start; }
.figs.two { grid-template-columns: repeat(2, 1fr); }
.figs figure { background: #fff; border: 1px solid var(--line); border-radius: 8px; overflow: hidden; }
.figs .wide { grid-column: 1 / -1; }
.figs figure img { aspect-ratio: auto; height: auto; }
.figs figcaption { position: static; display: block; border-radius: 0; padding: 5px 10px; font-size: 13px;
  color: var(--muted); background: var(--card); border-top: 1px solid var(--line); }
h3.sec { margin: 26px 0 4px; }
.figtitle { margin: 14px 0 0; font-size: 14px; }
details { margin: 0 0 14px; }
summary { cursor: pointer; color: var(--link); font-size: 14px; margin: 0 0 6px; }
@media (max-width: 640px) {
  .pics, .figs.two { grid-template-columns: 1fr; }
}
dl { display: grid; grid-template-columns: auto 1fr; gap: 2px 12px; margin: 0 0 10px; font-size: 14px; }
dt { color: var(--muted); }
dd { margin: 0; font-variant-numeric: tabular-nums; }
.scroll { overflow-x: auto; }
table { width: 100%; border-collapse: collapse; background: var(--card); border: 1px solid var(--line); font-size: 14px; }
th, td { text-align: left; padding: 10px 12px; border-bottom: 1px solid var(--line); vertical-align: top; }
th { color: var(--muted); font-weight: 600; font-size: 13px; white-space: nowrap; }
td a, td.nw { white-space: nowrap; }
footer { color: var(--muted); font-size: 13px; margin-top: 36px; }
[hidden] { display: none !important; }
.cornerset { margin: 8px 0 18px; }
.cornerset .figs { margin: 0; }
.cornersw { position: sticky; top: 0; z-index: 2; display: flex; flex-wrap: wrap; gap: 6px; padding: 8px 0;
  background: var(--bg); }
.cornersw button { font: inherit; font-size: 14px; padding: 4px 12px; border: 1px solid var(--line);
  border-radius: 999px; background: var(--card); color: var(--fg); cursor: pointer; }
.cornersw button[aria-checked="true"] { background: var(--link); border-color: var(--link); color: var(--bg); }
.cornersw button:focus-visible { outline: 2px solid var(--link); outline-offset: 2px; }
.cornersw .cv { margin-left: 7px; font-size: 12px; opacity: .8; }
tr.sel > td, th.sel, td.sel { background: color-mix(in srgb, var(--link) 10%, var(--card)); }
'''

# the corner switch (switched()): every switch on the page shows one corner -- its figures' images
# swapped in place, its rows and columns in the tables marked; arrow keys step through the corners
SWITCH_JS = '''<script>
(function () {
  var groups = document.querySelectorAll('.cornersw');
  if (!groups.length) return;
  function pick(c) {
    document.querySelectorAll('.cornersw button').forEach(function (b) {
      var on = b.getAttribute('data-corner') === c;
      b.setAttribute('aria-checked', on ? 'true' : 'false');
      b.tabIndex = on ? 0 : -1;
    });
    document.querySelectorAll('img.sw').forEach(function (img) {
      var p = img.getAttribute('data-' + c), f = img.closest('figure');
      f.hidden = !p;
      if (p && img.getAttribute('src') !== p) { img.setAttribute('src', p); img.parentNode.setAttribute('href', p); }
    });
    document.querySelectorAll('.cname').forEach(function (n) { n.textContent = c; });
    document.querySelectorAll('[data-col]').forEach(function (n) {
      n.classList.toggle('sel', n.getAttribute('data-col') === c);
    });
  }
  var loaded = false;
  function preload() {   // every corner's figures, so the first switch is instant too
    if (loaded) return;
    loaded = true;
    document.querySelectorAll('img.sw').forEach(function (img) {
      ['ss', 'tt', 'ff'].forEach(function (c) {
        var p = img.getAttribute('data-' + c);
        if (p) new Image().src = p;
      });
    });
  }
  groups.forEach(function (g) {
    g.addEventListener('pointerenter', preload);
    g.addEventListener('focusin', preload);
    g.addEventListener('click', function (ev) {
      var b = ev.target.closest('button');
      if (b) pick(b.getAttribute('data-corner'));
    });
    g.addEventListener('keydown', function (ev) {
      var bs = Array.prototype.slice.call(g.querySelectorAll('button')), i = bs.indexOf(document.activeElement);
      var d = {ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1}[ev.key];
      if (i < 0 || !d) return;
      ev.preventDefault();
      var b = bs[(i + d + bs.length) % bs.length];
      pick(b.getAttribute('data-corner'));
      b.focus();
    });
  });
  var on = document.querySelector('.cornersw [aria-checked="true"]');
  if (on) pick(on.getAttribute('data-corner'));
})();
</script>'''


def site(src, out):
    os.makedirs(out, exist_ok=True)
    ci_page(src, out)
    for f in glob.glob(os.path.join(src, 'rz80-*')):
        shutil.copy(f, out)
    boards, pdks = load(out)
    e = html.escape
    # the Targets run the site is built from: this run, or the one pages.yml names (SITE_RUN_ID, SITE_SHA)
    sha = os.environ.get('SITE_SHA') or os.environ.get('GITHUB_SHA', '')
    run_url = f"{SERVER}/{REPO}/actions/runs/{os.environ['SITE_RUN_ID']}" if os.environ.get('SITE_RUN_ID') else RUN_URL
    when = datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M UTC')
    commit = f'<a href="{SERVER}/{REPO}/commit/{sha}">main @ {sha[:7]}</a>' if sha else 'main'
    run = f'<a href="{run_url}">Targets run</a>' if run_url else 'Targets run'

    chip_cards, core_cards = [], []
    for p in pdks:
        chip = bool(p.get('pads'))

        def picture(f, caption):
            img = (f'<a href="{f}"><img src="{f}" alt="{e(p["name"])}: {caption}" loading="lazy"></a>'
                   if f in p['files'] else '<div class="noimg">no picture</div>')
            return f'<figure>{img}<figcaption>{caption}</figcaption></figure>'
        facts = ''.join(f'<dt>{k}</dt><dd>{e(v)}</dd>' for k, v in asic_facts(p))

        def wide(f, caption):   # a logic analyser's view, under the three pictures
            if f not in p['files']:
                return ''
            return (f'<figure class="wide"><a href="{f}"><img src="{f}" alt="{e(p["name"])}: {caption}" '
                    f'loading="lazy"></a><figcaption>{caption}</figcaption></figure>')

        def legend(title, table, counts):
            items = ''.join(f'<li><span class="swatch" style="background: rgb{colour}"></span>{e(name)}'
                            f' <span class="muted">{counts[name]:,}</span></li>'
                            for name, (_, colour) in table.items() if (counts or {}).get(name))
            return f'<div><h4>{title}</h4><ul class="legend">{items}</ul></div>' if items else ''
        second = (f"rz80-{p['pdk']}-power.png", 'clock and power') if chip else (f"rz80-{p['pdk']}-cells.png", 'cells by kind')
        links = [f'<a href="{f}"{" download" if f.endswith(".gds") else ""}>{label}</a>'
                 f' <span class="size">{size(os.path.join(out, f))}</span>'
                 for f, label in ((f"rz80-{p['pdk']}.gds", 'GDS'), (f"rz80-{p['pdk']}.png", 'layout'),
                                  (second[0], second[1].split()[0]), (f"rz80-{p['pdk']}-blocks.png", 'blocks'),
                                  (f"rz80-{p['pdk']}-pins.svg", 'pins'))
                 if f in p['files']]
        links.append(f'<a href="{p.get("readme") or readme_url("asic", p["pdk"])}">README</a>')
        if chip:   # the clock and power picture's legend: (colour, count) per entry
            power = {name: (None, tuple(colour)) for name, (colour, _) in (p.get('power') or {}).items()}
            second_legend = legend('Clock and power', power, {n: c for n, (_, c) in (p.get('power') or {}).items()})
        else:
            second_legend = legend('Cells by kind', CELL_KINDS, p.get('cells'))
        (chip_cards if chip else core_cards).append(f"""<article class="pdk">
<div class="pics">
{picture(f"rz80-{p['pdk']}.png", 'layout')}
{picture(*second)}
{picture(f"rz80-{p['pdk']}-blocks.png", 'cells by Z80 block')}
{wide(f"rz80-{p['pdk']}-pins.svg", 'its pins at transistor level (ngspice): power-up, RESET, a short program at 4 MHz')}
</div>
<div class="body">
<div><h3>{e(p['name'])}</h3><p class="muted">{e(p.get('pdk_variant') or '')} · {e(p.get('library') or '')}</p>
<dl>{facts}</dl><p class="links">{' · '.join(links)}</p></div>
{second_legend}
{legend('Cells by Z80 block', {PAD_BLOCK[0]: (None, PAD_BLOCK[1]), **block_legend()}, p.get('blocks'))}
</div>
</article>""")

    rows = []
    for b in boards:
        cells = []
        for _, variant in VARIANTS:
            fs = [f for f in b['files'] if f.startswith(f"rz80-{b['board']}-{variant}.")]
            cells.append('<td>' + ('<br>'.join(f'<a href="{f}" download>.{f.rsplit(".", 1)[1]}</a>'
                                               f' <span class="size">{size(os.path.join(out, f))}</span>'
                                               for f in fs) or '–') + '</td>')
        rows.append(f"""<tr><td><strong>{e(b['name'])}</strong><br><span class="muted">{e(b['device'])}</span>
· <a href="{readme_url('fpga', b['board'])}">README</a></td>{''.join(cells)}</tr>""")

    page = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>rz80 builds</title>
<style>
{SITE_CSS}</style>
</head>
<body>
<main>
<h1>rz80 builds</h1>
<p class="muted">The Z80 core of <a href="{SERVER}/{REPO}">{e(REPO)}</a> on every ASIC PDK and FPGA board,
from the latest {run} on main where all of them passed: {commit}, published {when}.</p>

<h2>ASIC chip layouts</h2>
<p class="muted">The core as a chip: z80_top, hardened on its own, as a hard macro in a ring of I/O pads,
each pin's pad on the side of the die where the Z80 has its own. The layout shows the metal stack, bottom
metal blue to top metal yellow; the clock and power picture the clock in green -- the CLK pad, every clock
wire and buffer -- over the power rings, stripes and rails (core supply red, ground blue; a split I/O
supply amber and teal) and the supply pads in their net's colour; the block map every cell by the Z80
block it was synthesized in. Where a chip has been simulated at transistor level -- its layout extracted
to transistors and wires and run in ngspice (<a href="{SERVER}/{REPO}/tree/main/asic/spice">asic/spice</a>)
-- its pins are shown under it as a logic analyser shows them.</p>
{chr(10).join(chip_cards) or '<p>none in this build</p>'}

{transistor(src, out)}

<h2>ASIC core layouts</h2>
<p class="muted">The bare core (no pad ring), RTL to GDS with LibreLane. The pictures are drawn from the GDS
and the DEF: the layout shows the cell rows and the metal stack, bottom metal blue to top metal yellow; the
two cell maps show every standard cell, coloured by kind and by the Z80 block it was synthesized in.</p>
{chr(10).join(core_cards) or '<p>none in this build</p>'}

<h2>FPGA bitstreams</h2>
<p class="muted">BASIC computer: NASCOM BASIC 4.7 on the board's serial console. Tiny BASIC: the same computer
with 1K Tiny BASIC. Chip: the board as a Z80 replacement, the bus on its pins. Each board's README says how to program it.</p>
<div class="scroll"><table>
<thead><tr><th>Board</th><th>BASIC computer</th><th>Tiny BASIC</th><th>Z80 chip replacement</th></tr></thead>
<tbody>
{chr(10).join(rows)}
</tbody>
</table></div>

{cfast_bench(src, out)}

<footer>Published by <a href="{SERVER}/{REPO}/blob/main/.github/workflows/pages.yml">pages.yml</a> after
<a href="{SERVER}/{REPO}/blob/main/.github/workflows/targets.yml">targets.yml</a> (every push to main and nightly)
or <a href="{SERVER}/{REPO}/blob/main/.github/workflows/transistor.yml">transistor.yml</a> (by hand). Every run, failed ones included, also keeps its bitstreams, GDS and logs
as artifacts on the <a href="{SERVER}/{REPO}/actions/workflows/targets.yml">Actions</a> tab for 14 days.</footer>
</main>
</body>
</html>
"""
    open(os.path.join(out, 'index.html'), 'w').write(page)
    print(f'{out}/index.html: {len(pdks)} layouts, {len(boards)} boards')


def summary(src, url):
    _, pdks = load(src)
    url = url.rstrip('/') + '/'

    def picture(p, suffix):
        f = f"rz80-{p['pdk']}{suffix}.png"
        return f'[![{f}]({url}{f}?run={RUN_ID})]({url}{f})' if f in p['files'] else '–'

    def counts(p, key):
        return ', '.join(f'{n:,} {name}' for name, n in (p.get(key) or {}).items() if n)

    def facts(p):
        lines = [f'{k}: {v}' for k, v in asic_facts(p)]
        lines += [f'{label}: {counts(p, key)}' for key, label in (('cells', 'Cells'), ('blocks', 'Blocks')) if counts(p, key)]
        return '<br>'.join(lines)
    chips = [p for p in pdks if p.get('pads')]
    cores = [p for p in pdks if not p.get('pads')]
    print(f'## Layouts\n\nDrawn from each GDS and DEF, as on the [site]({url}) this run deployed.\n')
    for title, group, middle, note in (
            ('Chip layouts', chips, '-power', 'the metal stack; the clock (green: CLK pad, clock wires and '
             'buffers) over the power distribution (core supply red, ground blue, a split I/O supply amber '
             'and teal); every cell by Z80 block'),
            ('Core layouts', cores, '-cells', 'the cell rows and metal stack; every standard cell by kind '
             '(flip-flops orange, buffers and delays teal, other logic blue, fill/decap/tap dark); every cell '
             'by the Z80 block it was synthesized in (ALU red, register file yellow, decoder PLA purple, '
             'execute blue, address green, timing cyan, core sequencer pink, pin logic white, I/O pads grey)')):
        if not group:
            continue
        print(f'### {title}\n\n{note[0].upper() + note[1:]}.\n')
        print('| ' + ' | '.join(p['name'] for p in group) + ' |')
        print('|' + '---|' * len(group))
        for suffix in ('', middle, '-blocks'):
            print('| ' + ' | '.join(picture(p, suffix) for p in group) + ' |')
        print('| ' + ' | '.join(facts(p) for p in group) + ' |\n')

if __name__ == '__main__':
    cmd, args = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else ('', [])
    commands = {'fpga': fpga, 'asic': asic, 'site': site, 'summary': summary}
    if cmd not in commands or len(args) != 2:
        sys.exit(__doc__)
    commands[cmd](*args)
