#!/usr/bin/env python3
"""dist.py - the downloads and the GitHub Pages site of the Targets runs.

  dist.py fpga <board> <out>   fpga/<board>'s bitstreams after `make ci`,
                               named for download, + rz80-<board>.json
  dist.py asic <pdk> <out>     asic/<pdk>'s latest LibreLane run: the GDS,
                               a picture of it, maps of its standard cells
                               by kind and by Z80 block (needs gdstk,
                               numpy and Pillow) + rz80-<pdk>.json
  dist.py site <in> <out>      index.html over everything fpga/asic made
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
PAD_BLOCK = ('I/O pads', (148, 163, 184))
# the pad ring's corners, fillers and bond pads, darker than the pads
RING = (58, 66, 80)
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
    # a chip's GDS renames the cells inside a hard macro: UF_<library cell>
    name = re.sub(r'^UF_', '', name)
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
    """The cell map; returns the instance count per kind."""
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
        rgb = RING if kind == 'I/O pads' and not is_pad(name) else CELL_KINDS[kind][1]
        draw.rectangle(((a - x0) * k, (y1 - d) * k, (c - x0) * k, (y1 - b) * k),
                       fill=rgb, outline=tuple(int(v * .55) for v in rgb), width=max(1, supersample // 2))
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
        block = next(b for b, (pattern, _) in BLOCKS.items() if re.match(pattern, name) or not pattern)
        counts[block] += 1
        cells.append((block, BLOCKS[block][1], master, x, y, orient))
    cells.sort(key=lambda c: c[0] is not None)   # fillers first: real cells draw over them
    for block, rgb, master, x, y, orient in cells:
        cw, ch = sizes[master]
        if orient in ('E', 'W', 'FE', 'FW'):
            cw, ch = ch, cw
        draw.rectangle(((x - x0) * k, (y1 - y - ch) * k, (x + cw - x0) * k, (y1 - y) * k),
                       fill=rgb, outline=tuple(int(v * .55) for v in rgb), width=max(1, supersample // 2))
    im.resize((w, h), Image.LANCZOS).quantize(256).save(png, optimize=True)
    return counts


def collect(run, key, name, stack, out, extra=None, macro_runs=None):
    """One LibreLane run: rz80-<key>.gds, its pictures and rz80-<key>.json;
    macro_runs as for render_blocks."""
    cfg = json.load(open(run + 'resolved.json'))
    m = json.load(open(run + 'final/metrics.json'))
    design = cfg['DESIGN_NAME']
    os.makedirs(out, exist_ok=True)
    files, cells, blocks = [], None, None
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
            'cells': cells, 'blocks': blocks, 'built_from': os.environ.get('BUILT_FROM', ''), 'run': RUN_URL,
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
        facts.append(('Pads', f"{p['pads']}, placed like the Z80's"))
    if p.get('std_cells') is not None:
        u = p.get('utilisation')
        facts.append(('Std cells', f"{p['std_cells']:,}" + (f' ({u * 100:.0f} % utilisation)' if u else '')))
    if p.get('clock_period_ns'):
        clk = f"{1000 / float(p['clock_period_ns']):g} MHz"
        if p.get('setup_ws_ns') is not None:
            clk += f", setup slack {p['setup_ws_ns']:+.2f} ns (typical)"
        facts.append(('Clock', clk))
    return facts


def site(src, out):
    os.makedirs(out, exist_ok=True)
    for f in glob.glob(os.path.join(src, 'rz80-*')):
        shutil.copy(f, out)
    boards, pdks = load(out)
    e = html.escape
    sha = os.environ.get('GITHUB_SHA', '')
    when = datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M UTC')
    commit = f'<a href="{SERVER}/{REPO}/commit/{sha}">main @ {sha[:7]}</a>' if sha else 'main'
    run = f'<a href="{RUN_URL}">Targets run</a>' if RUN_URL else 'Targets run'

    cards = []
    for p in pdks:
        def picture(f, caption):
            img = (f'<a href="{f}"><img src="{f}" alt="{e(p["name"])}: {caption}" loading="lazy"></a>'
                   if f in p['files'] else '<div class="noimg">no picture</div>')
            return f'<figure>{img}<figcaption>{caption}</figcaption></figure>'
        facts = ''.join(f'<dt>{k}</dt><dd>{e(v)}</dd>' for k, v in asic_facts(p))

        def legend(title, table, counts):
            items = ''.join(f'<li><span class="swatch" style="background: rgb{colour}"></span>{e(name)}'
                            f' <span class="muted">{counts[name]:,}</span></li>'
                            for name, (_, colour) in table.items() if (counts or {}).get(name))
            return f'<div><h4>{title}</h4><ul class="legend">{items}</ul></div>' if items else ''
        links = [f'<a href="{f}"{" download" if f.endswith(".gds") else ""}>{label}</a>'
                 f' <span class="size">{size(os.path.join(out, f))}</span>'
                 for f, label in ((f"rz80-{p['pdk']}.gds", 'GDS'), (f"rz80-{p['pdk']}.png", 'layout'),
                                  (f"rz80-{p['pdk']}-cells.png", 'cells'), (f"rz80-{p['pdk']}-blocks.png", 'blocks'))
                 if f in p['files']]
        links.append(f'<a href="{p.get("readme") or readme_url("asic", p["pdk"])}">README</a>')
        cards.append(f"""<article class="pdk">
<div class="pics">
{picture(f"rz80-{p['pdk']}.png", 'layout')}
{picture(f"rz80-{p['pdk']}-cells.png", 'cells by kind')}
{picture(f"rz80-{p['pdk']}-blocks.png", 'cells by Z80 block')}
</div>
<div class="body">
<div><h3>{e(p['name'])}</h3><p class="muted">{e(p.get('pdk_variant') or '')} · {e(p.get('library') or '')}</p>
<dl>{facts}</dl><p class="links">{' · '.join(links)}</p></div>
{legend('Cells by kind', CELL_KINDS, p.get('cells'))}
{legend('Cells by Z80 block', {PAD_BLOCK[0]: (None, PAD_BLOCK[1]), **BLOCKS}, p.get('blocks'))}
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
:root {{ --bg: #f7f7f5; --fg: #1c1c1a; --muted: #6a6a64; --card: #fff; --line: #e2e1dc; --link: #1f55c0; }}
@media (prefers-color-scheme: dark) {{
  :root {{ --bg: #141414; --fg: #ebeae6; --muted: #a2a19b; --card: #1d1d1c; --line: #34332f; --link: #8fb1ff; }}
}}
* {{ box-sizing: border-box; }}
body {{ margin: 0; background: var(--bg); color: var(--fg); font: 15px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }}
main {{ max-width: 1120px; margin: 0 auto; padding: 32px 16px 48px; }}
h1 {{ font-size: 26px; margin: 0 0 6px; }}
h2 {{ font-size: 19px; margin: 36px 0 6px; }}
h3 {{ font-size: 16px; margin: 0; }}
p {{ margin: 0 0 12px; }}
a {{ color: var(--link); }}
.muted, .size {{ color: var(--muted); }}
.size {{ font-size: 12px; }}
.pdk {{ background: var(--card); border: 1px solid var(--line); border-radius: 10px; overflow: hidden;
  margin: 0 0 16px; }}
.pics {{ display: grid; grid-template-columns: repeat(3, 1fr); gap: 1px; background: var(--line); }}
figure {{ margin: 0; position: relative; background: #0d1117; }}
figure img, .noimg {{ display: block; width: 100%; aspect-ratio: 1; object-fit: contain; }}
.noimg {{ display: grid; place-items: center; color: #888; }}
figcaption {{ position: absolute; left: 8px; top: 8px; padding: 1px 7px; border-radius: 4px; font-size: 12px;
  color: #d6dbe1; background: rgba(13, 17, 23, .75); }}
.body {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(230px, 1fr)); gap: 4px 28px;
  padding: 14px 16px 6px; border-top: 1px solid var(--line); }}
.body .muted {{ font-size: 13px; margin: 0 0 8px; }}
h4 {{ font-size: 13px; color: var(--muted); font-weight: 600; margin: 2px 0 6px; }}
.legend {{ list-style: none; padding: 0; margin: 0 0 10px; font-size: 13px; }}
.legend li {{ display: flex; align-items: center; gap: 7px; }}
.swatch {{ width: 10px; height: 10px; border-radius: 2px; flex: none; }}
@media (max-width: 640px) {{
  .pics {{ grid-template-columns: 1fr; }}
}}
dl {{ display: grid; grid-template-columns: auto 1fr; gap: 2px 12px; margin: 0 0 10px; font-size: 14px; }}
dt {{ color: var(--muted); }}
dd {{ margin: 0; font-variant-numeric: tabular-nums; }}
.scroll {{ overflow-x: auto; }}
table {{ width: 100%; border-collapse: collapse; background: var(--card); border: 1px solid var(--line); font-size: 14px; }}
th, td {{ text-align: left; padding: 10px 12px; border-bottom: 1px solid var(--line); vertical-align: top; }}
th {{ color: var(--muted); font-weight: 600; font-size: 13px; white-space: nowrap; }}
td a {{ white-space: nowrap; }}
footer {{ color: var(--muted); font-size: 13px; margin-top: 36px; }}
</style>
</head>
<body>
<main>
<h1>rz80 builds</h1>
<p class="muted">The Z80 core of <a href="{SERVER}/{REPO}">{e(REPO)}</a> on every ASIC PDK and FPGA board,
from the latest {run} on main where all of them passed: {commit}, published {when}.</p>

<h2>ASIC layouts</h2>
<p class="muted">The bare core (no pad ring), RTL to GDS with LibreLane. Both pictures are drawn from the GDS:
the layout shows the cell rows and the metal stack, bottom metal blue to top metal yellow; the two cell maps
show every standard cell, coloured by kind and by the Z80 block it was synthesized in.</p>
{chr(10).join(cards) or '<p>none in this build</p>'}

<h2>FPGA bitstreams</h2>
<p class="muted">BASIC computer: NASCOM BASIC 4.7 on the board's serial console. Tiny BASIC: the same computer
with 1K Tiny BASIC. Chip: the board as a Z80 replacement, the bus on its pins. Each board's README says how to program it.</p>
<div class="scroll"><table>
<thead><tr><th>Board</th><th>BASIC computer</th><th>Tiny BASIC</th><th>Z80 chip replacement</th></tr></thead>
<tbody>
{chr(10).join(rows)}
</tbody>
</table></div>

<footer>Published by <a href="{SERVER}/{REPO}/blob/main/.github/workflows/targets.yml">targets.yml</a>
(every push to main and nightly). Every run, failed ones included, also keeps its bitstreams, GDS and logs
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
    print(f'## Layouts\n\nDrawn from each GDS, as on the [site]({url}) this run deployed: the cell rows and '
          'metal stack; every standard cell by kind (flip-flops orange, buffers and delays teal, other logic '
          'blue, fill/decap/tap dark); every cell by the Z80 block it was synthesized in (ALU red, register '
          'file yellow, decoder PLA purple, execute blue, address green, timing cyan, core sequencer pink, '
          'pin logic white, I/O pads grey).\n')
    print('| ' + ' | '.join(p['name'] for p in pdks) + ' |')
    print('|' + '---|' * len(pdks))
    for suffix in ('', '-cells', '-blocks'):
        print('| ' + ' | '.join(picture(p, suffix) for p in pdks) + ' |')
    print('| ' + ' | '.join(facts(p) for p in pdks) + ' |')


if __name__ == '__main__':
    cmd, args = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else ('', [])
    commands = {'fpga': fpga, 'asic': asic, 'site': site, 'summary': summary}
    if cmd not in commands or len(args) != 2:
        sys.exit(__doc__)
    commands[cmd](*args)
