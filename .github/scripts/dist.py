#!/usr/bin/env python3
"""dist.py - the downloads and the GitHub Pages site of the Targets runs.

  dist.py fpga <board> <out>   fpga/<board>'s bitstreams after `make ci`,
                               named for download, + rz80-<board>.json
  dist.py asic <pdk> <out>     asic/<pdk>'s latest LibreLane run: the GDS,
                               a picture of it, a map of its standard
                               cells by kind (needs gdstk, numpy and
                               Pillow) + rz80-<pdk>.json
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
}
PDKS = {
    'sky130':     'SkyWater SKY130',
    'ihp-sg13g2': 'IHP SG13G2',
    'gf180mcu':   'GlobalFoundries GF180MCU',
}
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
    'flip-flops': (r'(s?df|edf)', (255, 90, 54)),
    'latches': (r'(dl[xrhl]|s?dlclkp|lat|icg|s?lgcp)', (236, 72, 153)),
    'buffers and delays': (r'(buf|inv|clkbuf|clkinv|clkdlybuf|dly)', (20, 184, 166)),
    'logic': (None, (79, 111, 174)),   # everything else
    'fill/decap/tap': (r'(fill|decap|tap|endcap|diode|antenna|tie|conb)', (43, 49, 59)),
}
CELL_LIBRARY_PREFIX = r'^(sky130_fd_sc_\w+?__|sg13g2_|gf180mcu_fd_sc_mcu\w+?__)'
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


def cell_kind(name):
    base = re.sub(r'_\d+$', '', re.sub(CELL_LIBRARY_PREFIX, '', name))
    return next((kind for kind, (pattern, _) in CELL_KINDS.items() if pattern and re.match(pattern, base)), 'logic')


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
    cells = [(cell_kind(r.cell.name), r) for r in top.references if isinstance(r.cell, gdstk.Cell)]
    cells.sort(key=lambda c: c[0] != 'fill/decap/tap')   # fillers first: real cells draw over them
    for kind, r in cells:
        counts[kind] += 1
        rgb = CELL_KINDS[kind][1]
        (a, b), (c, d) = r.bounding_box()
        draw.rectangle(((a - x0) * k, (y1 - d) * k, (c - x0) * k, (y1 - b) * k),
                       fill=rgb, outline=tuple(int(v * .55) for v in rgb), width=max(1, supersample // 2))
    im.resize((w, h), Image.LANCZOS).quantize(256).save(png, optimize=True)
    return counts


def asic(pdk, out):
    runs = sorted(glob.glob(os.path.join('asic', pdk, 'runs', '*', '')), key=os.path.getmtime)
    if not runs or not os.path.isfile(runs[-1] + 'final/metrics.json'):
        return
    run = runs[-1]
    cfg = json.load(open(run + 'resolved.json'))
    m = json.load(open(run + 'final/metrics.json'))
    design = cfg['DESIGN_NAME']
    os.makedirs(out, exist_ok=True)
    files, cells = [], None
    gds = run + f'final/gds/{design}.gds'
    if os.path.isfile(gds):
        shutil.copy(gds, os.path.join(out, f'rz80-{pdk}.gds'))
        files.append(f'rz80-{pdk}.gds')
        png = os.path.join(out, f'rz80-{pdk}.png')
        try:
            import gdstk
            top = gdstk.read_gds(gds).top_level()[0]
            render(top, png, STACKS[pdk])
            files.append(f'rz80-{pdk}.png')
            cells = render_cells(top, os.path.join(out, f'rz80-{pdk}-cells.png'))
            files.append(f'rz80-{pdk}-cells.png')
        except Exception as e:
            print(f"::warning::dist.py: no pictures of {pdk} ({e!r}), LibreLane's render instead")
            if os.path.isfile(run + f'final/render/{design}.png') and not os.path.isfile(png):
                shutil.copy(run + f'final/render/{design}.png', png)
                files.append(f'rz80-{pdk}.png')
    x0, y0, x1, y1 = (float(v) for v in m['design__die__bbox'].split())
    # worst setup slack over the corners the flow signs off (summary.py's rule)
    gate = cfg.get('TIMING_VIOLATION_CORNERS') or ['*tt*']
    ws = [v for k, v in m.items() if k.startswith('timing__setup__ws__corner:')
          and any(fnmatch.fnmatch(k.split('corner:')[1], g) for g in gate)]
    info = {'pdk': pdk, 'name': PDKS.get(pdk, pdk), 'pdk_variant': cfg.get('PDK'),
            'library': cfg.get('STD_CELL_LIBRARY'), 'design': design, 'files': files,
            'die_um': [round(x1 - x0, 1), round(y1 - y0, 1)],
            'std_cells': m.get('design__instance__count__stdcell'),
            'utilisation': m.get('design__instance__utilization'),
            'clock_period_ns': cfg.get('CLOCK_PERIOD'),
            'signoff_corners': gate, 'setup_ws_ns': min(ws) if ws else None,
            'cells': cells, 'built_from': os.environ.get('BUILT_FROM', ''), 'run': RUN_URL}
    json.dump(info, open(os.path.join(out, f'rz80-{pdk}.json'), 'w'), indent=1)


def load(src):
    """The rz80-<board>.json and rz80-<pdk>.json in src, in BOARDS / PDKS order."""
    infos = [json.load(open(p)) for p in glob.glob(os.path.join(src, 'rz80-*.json'))]
    def order(table, key):
        known = [i for i in infos if i.get(key) in table]
        return sorted(known, key=lambda i: list(table).index(i[key]))
    return order(BOARDS, 'board'), order(PDKS, 'pdk')


def size(path):
    n = os.path.getsize(path)
    return f'{n / 1e6:.1f} MB' if n >= 1e6 else f'{n / 1e3:.0f} KB'


def asic_facts(p):
    w, h = p['die_um']
    facts = [('Die', f'{w:,.0f} × {h:,.0f} µm ({w * h / 1e6:.2f} mm²)')]
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
        legend = ''.join(f'<li><span class="swatch" style="background: rgb{rgb}"></span>{kind}'
                         f' <span class="muted">{p["cells"][kind]:,}</span></li>'
                         for kind, (_, rgb) in CELL_KINDS.items() if (p.get('cells') or {}).get(kind))
        links = [f'<a href="{f}"{" download" if f.endswith(".gds") else ""}>{label}</a>'
                 f' <span class="size">{size(os.path.join(out, f))}</span>'
                 for f, label in ((f"rz80-{p['pdk']}.gds", 'GDS'), (f"rz80-{p['pdk']}.png", 'layout PNG'),
                                  (f"rz80-{p['pdk']}-cells.png", 'cells PNG')) if f in p['files']]
        links.append(f'<a href="{readme_url("asic", p["pdk"])}">README</a>')
        cards.append(f"""<article class="pdk">
{picture(f"rz80-{p['pdk']}.png", 'layout')}
{picture(f"rz80-{p['pdk']}-cells.png", 'standard cells')}
<div class="body"><h3>{e(p['name'])}</h3><p class="muted">{e(p.get('pdk_variant') or '')} · {e(p.get('library') or '')}</p>
<dl>{facts}</dl>{f'<ul class="legend">{legend}</ul>' if legend else ''}<p class="links">{' · '.join(links)}</p></div>
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
.pdk {{ display: grid; grid-template-columns: 1fr 1fr minmax(240px, .8fr); background: var(--card);
  border: 1px solid var(--line); border-radius: 10px; overflow: hidden; margin: 0 0 16px; }}
figure {{ margin: 0; position: relative; background: #0d1117; }}
figure img, .noimg {{ display: block; width: 100%; aspect-ratio: 1; object-fit: contain; }}
.noimg {{ display: grid; place-items: center; color: #888; }}
figcaption {{ position: absolute; left: 8px; top: 8px; padding: 1px 7px; border-radius: 4px; font-size: 12px;
  color: #d6dbe1; background: rgba(13, 17, 23, .75); }}
.body {{ padding: 14px 16px 6px; border-left: 1px solid var(--line); }}
.body .muted {{ font-size: 13px; margin: 0 0 8px; }}
.legend {{ list-style: none; padding: 0; margin: 0 0 10px; font-size: 13px; }}
.legend li {{ display: flex; align-items: center; gap: 7px; }}
.swatch {{ width: 10px; height: 10px; border-radius: 2px; flex: none; }}
@media (max-width: 820px) {{
  .pdk {{ grid-template-columns: 1fr 1fr; }}
  .body {{ grid-column: 1 / -1; border-left: 0; border-top: 1px solid var(--line); }}
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
the layout shows the cell rows and the metal stack, bottom metal blue to top metal yellow; the cell map
shows every standard cell, coloured by kind.</p>
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

    def kinds(p):
        return ', '.join(f'{n:,} {kind}' for kind, n in (p.get('cells') or {}).items() if n)
    print(f'## Layouts\n\nDrawn from each GDS, as on the [site]({url}) this run deployed: the cell rows and '
          'metal stack, then every standard cell by kind (flip-flops orange, buffers and delays teal, '
          'other logic blue, fill/decap/tap dark).\n')
    print('| ' + ' | '.join(p['name'] for p in pdks) + ' |')
    print('|' + '---|' * len(pdks))
    print('| ' + ' | '.join(picture(p, '') for p in pdks) + ' |')
    print('| ' + ' | '.join(picture(p, '-cells') for p in pdks) + ' |')
    print('| ' + ' | '.join('<br>'.join([f'{k}: {v}' for k, v in asic_facts(p)] + ([f'Cells: {kinds(p)}'] if kinds(p) else []))
                            for p in pdks) + ' |')


if __name__ == '__main__':
    cmd, args = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else ('', [])
    commands = {'fpga': fpga, 'asic': asic, 'site': site, 'summary': summary}
    if cmd not in commands or len(args) != 2:
        sys.exit(__doc__)
    commands[cmd](*args)
