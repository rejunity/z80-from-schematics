#!/usr/bin/env python3
"""dist.py - the downloads and the GitHub Pages site of the Targets runs.

  dist.py fpga <board> <out>   fpga/<board>'s bitstreams after `make ci`,
                               named for download, + rz80-<board>.json
  dist.py asic <pdk> <out>     asic/<pdk>'s latest LibreLane run: the GDS,
                               its KLayout render (final/render, made by
                               LibreLane's Classic flow) + rz80-<pdk>.json
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


def asic(pdk, out):
    runs = sorted(glob.glob(os.path.join('asic', pdk, 'runs', '*', '')), key=os.path.getmtime)
    if not runs or not os.path.isfile(runs[-1] + 'final/metrics.json'):
        return
    run = runs[-1]
    cfg = json.load(open(run + 'resolved.json'))
    m = json.load(open(run + 'final/metrics.json'))
    design = cfg['DESIGN_NAME']
    os.makedirs(out, exist_ok=True)
    files = []
    for src, name in ((f'final/gds/{design}.gds', f'rz80-{pdk}.gds'),
                      (f'final/render/{design}.png', f'rz80-{pdk}.png')):
        if os.path.isfile(run + src):
            shutil.copy(run + src, os.path.join(out, name))
            files.append(name)
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
            'built_from': os.environ.get('BUILT_FROM', ''), 'run': RUN_URL}
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
        png = f"rz80-{p['pdk']}.png"
        img = (f'<a href="{png}"><img src="{png}" alt="{e(p["name"])} layout of {e(p["design"])}" loading="lazy"></a>'
               if png in p['files'] else '<div class="noimg">no render</div>')
        facts = ''.join(f'<dt>{k}</dt><dd>{e(v)}</dd>' for k, v in asic_facts(p))
        links = [f'<a href="{f}"{" download" if f.endswith(".gds") else ""}>{f.rsplit(".", 1)[1].upper()}</a>'
                 f' <span class="size">{size(os.path.join(out, f))}</span>' for f in p['files']]
        links.append(f'<a href="{readme_url("asic", p["pdk"])}">README</a>')
        cards.append(f"""<article class="card">{img}<div class="body">
<h3>{e(p['name'])}</h3><p class="muted">{e(p.get('pdk_variant') or '')} · {e(p.get('library') or '')}</p>
<dl>{facts}</dl><p class="links">{' · '.join(links)}</p></div></article>""")

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
.grid {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(300px, 1fr)); gap: 16px; }}
.card {{ background: var(--card); border: 1px solid var(--line); border-radius: 10px; overflow: hidden; }}
.card img, .noimg {{ display: block; width: 100%; aspect-ratio: 1; object-fit: contain; background: #fff; }}
.noimg {{ display: grid; place-items: center; color: #888; }}
.body {{ padding: 12px 14px 4px; border-top: 1px solid var(--line); }}
.body .muted {{ font-size: 13px; margin: 0 0 8px; }}
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
<p class="muted">The bare core (no pad ring), RTL to GDS with LibreLane; the pictures are its KLayout renders.</p>
<div class="grid">
{chr(10).join(cards) or '<p>none in this build</p>'}
</div>

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
    print(f'## Layouts\n\nKLayout renders of the GDS, from the [site]({url}) this run deployed.\n')
    print('| ' + ' | '.join(p['name'] for p in pdks) + ' |')
    print('|' + '---|' * len(pdks))
    print('| ' + ' | '.join(f"[![{p['pdk']}]({url}rz80-{p['pdk']}.png?run={RUN_ID})]({url}rz80-{p['pdk']}.png)"
                            if f"rz80-{p['pdk']}.png" in p['files'] else 'no render' for p in pdks) + ' |')
    print('| ' + ' | '.join('<br>'.join(f'{k}: {v}' for k, v in asic_facts(p)) for p in pdks) + ' |')


if __name__ == '__main__':
    cmd, args = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else ('', [])
    commands = {'fpga': fpga, 'asic': asic, 'site': site, 'summary': summary}
    if cmd not in commands or len(args) != 2:
        sys.exit(__doc__)
    commands[cmd](*args)
