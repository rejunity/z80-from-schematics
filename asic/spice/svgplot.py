"""svgplot.py - small line charts as SVG, without dependencies.

    c = Chart("title", "x label", "y label")
    c.line(xs, ys, "label")             # a series; colour from the palette
    c.hline(1.5, "VOH = VOL")           # a labelled reference line
    write("out.svg", [c, ...], cols=2)  # charts in a grid, one file
"""
import html
import math

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
        clip = f"c{abs(hash((self.title, ox, oy))) % 10**8}"
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
