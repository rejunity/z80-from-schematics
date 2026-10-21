"""svgplot.py - small line charts as SVG, without dependencies.

    c = Chart("title", "x label", "y label")
    c.line(xs, ys, "label")             # a series; colour from the palette
    c.hline(1.5, "VOH = VOL")           # a labelled reference line
    write("out.svg", [c, ...], cols=2)  # charts in a grid, one file
"""
import bisect
import html
import math
import zlib

PALETTE = ["#1f6fb4", "#d1352b", "#2b9a3e", "#8b56b8", "#e07b10", "#7a5a48"]
FONT = "font-family='Helvetica, Arial, sans-serif'"


def ticks(lo, hi, n=6):
    """Round tick values covering [lo, hi], about n of them."""
    if hi <= lo:
        return [lo]
    raw = (hi - lo) / n
    mag = 10 ** math.floor(math.log10(raw))
    step = next(s * mag for s in (1, 2, 2.5, 5, 10) if s * mag >= raw * 0.999)
    t, out = math.ceil(lo / step - 1e-9) * step, []
    while t <= hi + step * 1e-9:
        out.append(round(t, 12))
        t += step
    return out


def fmt(v):
    return f"{v:g}" if abs(v) >= 1e-9 else "0"


class Chart:
    def __init__(self, title, xlabel, ylabel, xlim=None, ylim=None):
        self.title, self.xlabel, self.ylabel = title, xlabel, ylabel
        self.xlim, self.ylim = xlim, ylim
        self.series, self.hlines, self.vlines, self.notes = [], [], [], []

    def line(self, xs, ys, label=None, colour=None, dash=None, width=1.6):
        pts = [(x, y) for x, y in zip(xs, ys) if x is not None and y is not None]
        if colour is None:
            colour = PALETTE[len([s for s in self.series if s[5]]) % len(PALETTE)]
        self.series.append((pts, colour, dash, width, label, label is not None))
        return colour

    def hline(self, y, label=None, colour="#777", dash="5 4"):
        self.hlines.append((y, label, colour, dash))

    def vline(self, x, label=None, colour="#777", dash="5 4", top=False):
        self.vlines.append((x, label, colour, dash, top))

    def note(self, text):
        self.notes.append(text)

    def _range(self):
        xs = [p[0] for s in self.series for p in s[0]] + [v[0] for v in self.vlines]  # noqa: E501
        ys = [p[1] for s in self.series for p in s[0]] + [h[0] for h in self.hlines]
        x0, x1 = self.xlim or (min(xs), max(xs))
        y0, y1 = self.ylim or (min(ys), max(ys))
        if not self.ylim:
            pad = (y1 - y0) * 0.05 or 1.0
            y0, y1 = y0 - pad, y1 + pad
        return x0, x1, y0, y1

    def svg(self, ox, oy, w, h):
        L, R, T, B = 58, 16, 30, 44
        x0, x1, y0, y1 = self._range()
        pw, ph = w - L - R, h - T - B
        X = lambda x: ox + L + (x - x0) / (x1 - x0) * pw
        Y = lambda y: oy + T + (1 - (y - y0) / (y1 - y0)) * ph
        out = [f"<text x='{ox + L}' y='{oy + 19}' font-size='13' font-weight='bold' {FONT}>{html.escape(self.title)}</text>"]
        for t in ticks(x0, x1):
            out.append(f"<line x1='{X(t):.1f}' y1='{oy + T}' x2='{X(t):.1f}' y2='{oy + T + ph}' stroke='#e6e6e6'/>")
            out.append(f"<text x='{X(t):.1f}' y='{oy + T + ph + 15}' font-size='10.5' text-anchor='middle' {FONT}>{fmt(t)}</text>")
        for t in ticks(y0, y1, 5):
            out.append(f"<line x1='{ox + L}' y1='{Y(t):.1f}' x2='{ox + L + pw}' y2='{Y(t):.1f}' stroke='#e6e6e6'/>")
            out.append(f"<text x='{ox + L - 5}' y='{Y(t) + 3.5:.1f}' font-size='10.5' text-anchor='end' {FONT}>{fmt(t)}</text>")
        out.append(f"<rect x='{ox + L}' y='{oy + T}' width='{pw}' height='{ph}' fill='none' stroke='#555'/>")
        out.append(f"<text x='{ox + L + pw / 2}' y='{oy + h - 8}' font-size='11.5' text-anchor='middle' {FONT}>{html.escape(self.xlabel)}</text>")
        cy = oy + T + ph / 2
        out.append(f"<text x='{ox + 14}' y='{cy}' font-size='11.5' text-anchor='middle' transform='rotate(-90 {ox + 14} {cy})' {FONT}>{html.escape(self.ylabel)}</text>")
        clip = f"c{zlib.crc32(f'{self.title}|{ox}|{oy}'.encode()) % 10**8}"    # the same in every run
        out.append(f"<clipPath id='{clip}'><rect x='{ox + L}' y='{oy + T}' width='{pw}' height='{ph}'/></clipPath>")
        for y, label, colour, dash in self.hlines:
            if y0 <= y <= y1:
                out.append(f"<line x1='{ox + L}' y1='{Y(y):.1f}' x2='{ox + L + pw}' y2='{Y(y):.1f}' stroke='{colour}' stroke-dasharray='{dash}'/>")
                if label:
                    out.append(f"<text x='{ox + L + pw - 4}' y='{Y(y) - 4:.1f}' font-size='10' text-anchor='end' fill='{colour}' {FONT}>{html.escape(label)}</text>")
        for x, label, colour, dash, top in self.vlines:
            if x0 <= x <= x1:
                out.append(f"<line x1='{X(x):.1f}' y1='{oy + T}' x2='{X(x):.1f}' y2='{oy + T + ph}' stroke='{colour}' stroke-dasharray='{dash}'/>")
                if label:
                    ty = oy + T + 12 if top else oy + T + ph - 6
                    out.append(f"<text x='{X(x) + 4:.1f}' y='{ty}' font-size='10' fill='{colour}' {FONT}>{html.escape(label)}</text>")
        for pts, colour, dash, width, label, _ in self.series:
            if len(pts) < 2:
                continue
            d = " ".join(f"{'M' if i == 0 else 'L'}{X(x):.1f},{Y(y):.1f}" for i, (x, y) in enumerate(pts))
            da = f" stroke-dasharray='{dash}'" if dash else ""
            out.append(f"<path d='{d}' fill='none' stroke='{colour}' stroke-width='{width}'{da} clip-path='url(#{clip})'/>")
        ly = oy + T + 14
        for pts, colour, dash, width, label, shown in self.series:
            if not shown:
                continue
            da = f" stroke-dasharray='{dash}'" if dash else ""
            out.append(f"<line x1='{ox + L + 8}' y1='{ly - 4}' x2='{ox + L + 30}' y2='{ly - 4}' stroke='{colour}' stroke-width='{width + 0.4}'{da}/>")
            out.append(f"<text x='{ox + L + 35}' y='{ly}' font-size='10.5' {FONT}>{html.escape(label)}</text>")
            ly += 14
        for n in self.notes:
            out.append(f"<text x='{ox + L + 8}' y='{ly}' font-size='10' fill='#555' {FONT}>{html.escape(n)}</text>")
            ly += 13
        return "\n".join(out)


def write(path, charts, cols=1, w=560, h=330, title=None):
    rows = (len(charts) + cols - 1) // cols
    top = 28 if title else 0
    W, H = cols * w, rows * h + top
    out = [f"<svg xmlns='http://www.w3.org/2000/svg' width='{W}' height='{H}' viewBox='0 0 {W} {H}'>",
           f"<rect width='{W}' height='{H}' fill='white'/>"]
    if title:
        out.append(f"<text x='{W / 2}' y='19' font-size='14.5' font-weight='bold' text-anchor='middle' {FONT}>{html.escape(title)}</text>")
    for i, c in enumerate(charts):
        out.append(c.svg((i % cols) * w, top + (i // cols) * h, w, h))
    out.append("</svg>")
    open(path, "w").write("\n".join(out) + "\n")


def simplify(pts, tol=0.3):
    """drop the points of a polyline that lie on the line through their
    neighbours (within tol pixels), and points closer than half a pixel"""
    if len(pts) < 3:
        return pts
    out = [pts[0]]
    for k in range(1, len(pts) - 1):
        (x0, y0), (x1, y1), (x2, y2) = out[-1], pts[k], pts[k + 1]
        if x1 - x0 < 0.5 and abs(y1 - y0) < tol:
            continue
        if x2 == x0 or abs(y1 - (y0 + (y2 - y0) * (x1 - x0) / (x2 - x0))) > tol:
            out.append(pts[k])
    out.append(pts[-1])
    return out


def stacked(path, title, subtitle, rows, t0, t1, marks=(), notes=(), w=1180, row_h=46, vlines=()):
    """Signals one above the other on a shared time axis, as an oscilloscope
    or a datasheet draws them. rows: [(label, times, volts, full scale V,
    colour)]; marks: [(t_from, t_to, row, text)] drawn as dimensions under
    that row; vlines: [(t, label)] across all rows."""
    X0, X1 = 150, w - 20
    X = lambda t: X0 + (t - t0) / (t1 - t0) * (X1 - X0)
    per_row = {}
    for m in marks:
        per_row.setdefault(m[2], []).append(m)
    extra = {r: 12 * len(per_row.get(r, [])) for r in range(len(rows))}
    top = 64
    H = top + sum(row_h + extra[r] for r in range(len(rows))) + 34 + 15 * len(notes)
    o = [f"<svg xmlns='http://www.w3.org/2000/svg' width='{w}' height='{H}' viewBox='0 0 {w} {H}'>",
         f"<rect width='{w}' height='{H}' fill='white'/>",
         f"<text x='{X0}' y='22' font-size='15' font-weight='bold' {FONT}>{html.escape(title)}</text>",
         f"<text x='{X0}' y='40' font-size='11.5' fill='#444' {FONT}>{html.escape(subtitle)}</text>"]
    for t in ticks(t0, t1, 10):
        o.append(f"<line x1='{X(t):.1f}' y1='{top - 8}' x2='{X(t):.1f}' y2='{H - 30 - 15 * len(notes)}' stroke='#ececec'/>")
        o.append(f"<text x='{X(t):.1f}' y='{H - 16 - 15 * len(notes)}' font-size='10' text-anchor='middle' fill='#555' {FONT}>{fmt(t)} ns</text>")
    for t, label in vlines:
        o.append(f"<line x1='{X(t):.1f}' y1='{top - 8}' x2='{X(t):.1f}' y2='{H - 30 - 15 * len(notes)}' stroke='#c9a227' stroke-dasharray='4 3'/>")
        if label:
            o.append(f"<text x='{X(t) + 3:.1f}' y='{top - 12}' font-size='10' fill='#9a7b12' {FONT}>{html.escape(label)}</text>")
    y = top
    ys = {}
    for r, row in enumerate(rows):
        label, ts, vs, full, colour = row[:5]
        width, strong = (row[5] if len(row) > 5 else 1.4), len(row) > 5
        hi, lo = y + 4, y + row_h - 10
        ys[r] = (hi, lo)
        if strong:                         # a row to stand out: tinted, bold
            o.append(f"<rect x='0' y='{y - 2}' width='{w}' height='{row_h - 2}' fill='{colour}' fill-opacity='0.08'/>")
        bold = "font-weight='bold' " if strong else ""
        o.append(f"<text x='{X0 - 10}' y='{(hi + lo) / 2 + 4:.1f}' font-size='{13 if strong else 12}' text-anchor='end' "
                 f"{bold}fill='{colour if strong else '#111'}' {FONT}>{html.escape(label)}</text>")
        o.append(f"<line x1='{X0}' y1='{lo}' x2='{X1}' y2='{lo}' stroke='#f2f2f2'/>")
        pts = []
        k = max(bisect.bisect_left(ts, t0) - 1, 0)
        while k < len(ts) and ts[k] <= t1:
            pts.append((X(max(ts[k], t0)), lo - min(max(vs[k] / full, -0.05), 1.08) * (lo - hi)))
            k += 1
        if len(pts) > 1:
            o.append("<polyline points='" + " ".join(f"{a:.1f},{b:.1f}" for a, b in simplify(pts)) +
                     f"' fill='none' stroke='{colour}' stroke-width='{width}'/>")
        for j, (ta, tb, _, text) in enumerate(per_row.get(r, [])):
            ya = lo + 9 + 12 * j
            xa, xb = X(ta), X(tb)
            d = 4 if xb >= xa else -4          # the arrowhead points the way time runs from ta to tb
            o.append(f"<path d='M{xa:.1f},{ya:.1f} L{xb:.1f},{ya:.1f} M{xb - d:.1f},{ya - 3:.1f} L{xb:.1f},{ya:.1f} "
                     f"L{xb - d:.1f},{ya + 3:.1f}' stroke='#c0392b' fill='none' stroke-width='1'/>")
            o.append(f"<line x1='{xa:.1f}' y1='{ya - 5:.1f}' x2='{xa:.1f}' y2='{ya + 5:.1f}' stroke='#c0392b'/>")
            o.append(f"<text x='{max(xa, xb) + 5:.1f}' y='{ya + 4:.1f}' font-size='10.5' fill='#c0392b' {FONT}>{html.escape(text)}</text>")
        y += row_h + extra[r]
    for i, n in enumerate(notes):
        o.append(f"<text x='{X0}' y='{H - 8 - 15 * (len(notes) - 1 - i)}' font-size='11' {FONT}>{html.escape(n)}</text>")
    o.append("</svg>")
    open(path, "w").write("\n".join(o) + "\n")


class Dumbbell:
    """Rows of values on one axis, a few markers per row joined by a line:
    how far, and which way, a number moved. series: [(name, colour,
    marker)] with marker one of o (dot), d (diamond), s (square)."""

    def __init__(self, title, xlabel, rows, series, xlim=None, band=None):
        self.title, self.xlabel, self.rows, self.series = title, xlabel, rows, series
        self.values = {}            # (row, series name) -> x
        self.xlim, self.band = xlim, band   # band: (from, to, label) shaded
        self.vlines = []

    def set(self, row, name, x):
        if x is not None:
            self.values[(row, name)] = x

    def vline(self, x, label=None):
        self.vlines.append((x, label))

    def legend_rows(self, w=900):
        rows, lx = 1, 190
        for name, _, _ in self.series:
            width = 24 + 6.2 * len(name)
            if lx + width > w - 10 and lx > 190:
                rows, lx = rows + 1, 190
            lx += width + 16
        return rows

    def height(self, w=900):
        return 78 + 21 * len(self.rows) + 18 * self.legend_rows(w)

    def svg(self, ox, oy, w):
        L, R = 190, 20
        xs = list(self.values.values()) + [v for v, _ in self.vlines] + (list(self.band[:2]) if self.band else [])
        x0, x1 = self.xlim or (min(xs), max(xs))
        if not self.xlim:
            pad = (x1 - x0) * 0.06 or 1.0
            x0, x1 = x0 - pad, x1 + pad
        X = lambda v: ox + L + (v - x0) / (x1 - x0) * (w - L - R)
        top = oy + 34
        bottom = top + 21 * len(self.rows)
        o = [f"<text x='{ox + 10}' y='{oy + 18}' font-size='13' font-weight='bold' {FONT}>{html.escape(self.title)}</text>"]
        if self.band:
            a, b, label = self.band
            o.append(f"<rect x='{X(a):.1f}' y='{top - 4}' width='{X(b) - X(a):.1f}' height='{bottom - top + 4}' fill='#e3f2e1'/>")
            if label:
                o.append(f"<text x='{(X(a) + X(b)) / 2:.1f}' y='{bottom - 5}' font-size='10' text-anchor='middle' fill='#2b7a2b' {FONT}>{html.escape(label)}</text>")
        for t in ticks(x0, x1, 7):
            o.append(f"<line x1='{X(t):.1f}' y1='{top - 4}' x2='{X(t):.1f}' y2='{bottom}' stroke='#e6e6e6'/>")
            o.append(f"<text x='{X(t):.1f}' y='{bottom + 14}' font-size='10.5' text-anchor='middle' {FONT}>{fmt(t)}</text>")
        for v, label in self.vlines:
            o.append(f"<line x1='{X(v):.1f}' y1='{top - 4}' x2='{X(v):.1f}' y2='{bottom}' stroke='#888' stroke-dasharray='4 3'/>")
            if label:
                o.append(f"<text x='{X(v) + 3:.1f}' y='{top + 6}' font-size='10' fill='#666' {FONT}>{html.escape(label)}</text>")
        o.append(f"<text x='{ox + L + (w - L - R) / 2:.1f}' y='{bottom + 30}' font-size='11.5' text-anchor='middle' {FONT}>{html.escape(self.xlabel)}</text>")
        for i, row in enumerate(self.rows):
            y = top + 21 * i + 8
            o.append(f"<text x='{ox + L - 8}' y='{y + 4}' font-size='11' text-anchor='end' {FONT}>{html.escape(row)}</text>")
            vals = [self.values[(row, s[0])] for s in self.series if (row, s[0]) in self.values]
            if len(vals) > 1:
                o.append(f"<line x1='{X(min(vals)):.1f}' y1='{y}' x2='{X(max(vals)):.1f}' y2='{y}' stroke='#bbb' stroke-width='2'/>")
            for name, colour, marker in self.series:
                if (row, name) in self.values:
                    o.append(self.marker(marker, X(self.values[(row, name)]), y, colour))
        lx, yy = ox + L, bottom + 46
        for name, colour, marker in self.series:
            width = 24 + 6.2 * len(name)
            if lx + width > ox + w - 10 and lx > ox + L:
                lx, yy = ox + L, yy + 18
            o.append(self.marker(marker, lx + 4, yy - 4, colour))
            o.append(f"<text x='{lx + 14}' y='{yy}' font-size='11' {FONT}>{html.escape(name)}</text>")
            lx += width + 16
        return "\n".join(o)

    @staticmethod
    def marker(m, x, y, colour):
        if m == "d":
            return f"<path d='M{x:.1f},{y - 5} L{x + 5:.1f},{y} L{x:.1f},{y + 5} L{x - 5:.1f},{y} Z' fill='{colour}'/>"
        if m == "s":
            return f"<rect x='{x - 4:.1f}' y='{y - 4}' width='8' height='8' fill='{colour}'/>"
        if m == "^":
            return f"<path d='M{x:.1f},{y - 5} L{x + 5:.1f},{y + 4} L{x - 5:.1f},{y + 4} Z' fill='{colour}'/>"
        if m == "v":
            return f"<path d='M{x:.1f},{y + 5} L{x + 5:.1f},{y - 4} L{x - 5:.1f},{y - 4} Z' fill='{colour}'/>"
        return f"<circle cx='{x:.1f}' cy='{y}' r='4.5' fill='{colour}'/>"


def write_panels(path, panels, w=900, title=None):
    """Dumbbell panels one under the other, in one SVG."""
    top = 30 if title else 0
    H = top + sum(p.height(w) for p in panels) + 10
    o = [f"<svg xmlns='http://www.w3.org/2000/svg' width='{w}' height='{H}' viewBox='0 0 {w} {H}'>",
         f"<rect width='{w}' height='{H}' fill='white'/>"]
    if title:
        o.append(f"<text x='{w / 2}' y='20' font-size='14.5' font-weight='bold' text-anchor='middle' {FONT}>{html.escape(title)}</text>")
    y = top
    for p in panels:
        o.append(p.svg(0, y, w))
        y += p.height(w)
    o.append("</svg>")
    open(path, "w").write("\n".join(o) + "\n")
