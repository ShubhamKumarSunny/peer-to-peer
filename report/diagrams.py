# Small helpers that draw the report's diagrams as inline SVG.
from html import escape as E

COL = {
    'tracker': ('#2f6fb7', '#e8f1fb'),
    'client':  ('#2e8b57', '#e9f6ee'),
    'data':    ('#c9741d', '#fdf1e3'),
    'bad':     ('#c0392b', '#fdecea'),
    'sec':     ('#6b4fbb', '#f0ecfa'),
    'plain':   ('#5f6b7a', '#f3f5f7'),
    'white':   ('#9aa4b1', '#ffffff'),
}

def svg(w, h, body):
    defs = ''.join(
        '<marker id="a-%s" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
        '<path d="M0,0 L10,5 L0,10 z" fill="%s"/></marker>' % (k, v[0]) for k, v in COL.items())
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="100%%" '
            'font-family="-apple-system, Helvetica Neue, Arial, sans-serif"><defs>%s</defs>%s</svg>' % (w, h, defs, body))

def text(x, y, s, size=12, anchor='middle', color='#1f2933', weight='400', mono=False, italic=False, halo=False,
         rotate=None):
    fam = ' font-family="Menlo, Consolas, monospace"' if mono else ''
    it = ' font-style="italic"' if italic else ''
    hl = ' stroke="#fcfdfe" stroke-width="4" paint-order="stroke" stroke-linejoin="round"' if halo else ''
    rt = ' transform="rotate(%s %s %s)"' % (rotate, x, y) if rotate is not None else ''
    return '<text x="%s" y="%s" font-size="%s" text-anchor="%s" fill="%s" font-weight="%s"%s%s%s%s>%s</text>' % (
        x, y, size, anchor, color, weight, fam, it, hl, rt, E(s))

def box(x, y, w, h, lines, kind='plain', size=12, bold_first=True, mono=False, r=8, dash=False):
    stroke, fill = COL[kind]
    d = ' stroke-dasharray="5 4"' if dash else ''
    out = '<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="%s" stroke="%s" stroke-width="1.4"%s/>' % (
        x, y, w, h, r, fill, stroke, d)
    if isinstance(lines, str):
        lines = [lines]
    lh = size + 4
    y0 = y + h / 2 - (len(lines) - 1) * lh / 2 + size * 0.36
    for i, s in enumerate(lines):
        out += text(x + w / 2, y0 + i * lh, s, size=size, weight='600' if (bold_first and i == 0) else '400',
                    mono=mono and i > 0, color='#1f2933' if i == 0 else '#3e4c59')
    return out

def arrow(x1, y1, x2, y2, kind='plain', label=None, both=False, dash=False, lx=None, ly=None, size=11, width=1.6, anchor='middle'):
    stroke = COL[kind][0]
    d = ' stroke-dasharray="6 4"' if dash else ''
    s = ' marker-start="url(#a-%s)"' % kind if both else ''
    out = '<line x1="%s" y1="%s" x2="%s" y2="%s" stroke="%s" stroke-width="%s"%s marker-end="url(#a-%s)"%s/>' % (
        x1, y1, x2, y2, stroke, width, d, kind, s)
    if label:
        if lx is None: lx = (x1 + x2) / 2
        if ly is None: ly = (y1 + y2) / 2 - 6
        for i, part in enumerate(label.split('\n')):
            out += text(lx, ly + i * (size + 3), part, size=size, color=stroke, weight='600', anchor=anchor)
    return out

def path(d, kind='plain', dash=False, width=1.6, end=True):
    stroke = COL[kind][0]
    dd = ' stroke-dasharray="6 4"' if dash else ''
    m = ' marker-end="url(#a-%s)"' % kind if end else ''
    return '<path d="%s" fill="none" stroke="%s" stroke-width="%s"%s%s/>' % (d, stroke, width, dd, m)

def seq(parts, rows, w=760, row_h=30, top=14, pad=70):
    """Sequence diagram. parts: [(name, kind)]. rows: ('msg', a, b, text, kind[, dashed]) |
    ('note', a, text) | ('band', text, kind) | ('gap',)"""
    n = len(parts)
    xs = [pad + i * (w - 2 * pad) / (n - 1) for i in range(n)]
    head_h = 34
    h = top + head_h + 14 + row_h * len(rows) + 16
    out = ''
    for (name, kind), x in zip(parts, xs):
        out += '<line x1="%s" y1="%s" x2="%s" y2="%s" stroke="#c5ccd6" stroke-width="1.2" stroke-dasharray="3 4"/>' % (
            x, top + head_h, x, h - 8)
    y = top + head_h + 14
    for r in rows:
        ym = y + row_h / 2
        if r[0] == 'msg':
            a, b, t, kind = r[1], r[2], r[3], r[4]
            dashed = len(r) > 5 and r[5]
            x1, x2 = xs[a], xs[b]
            off = 4 if x2 > x1 else -4
            out += arrow(x1 + off, ym + 6, x2 - off, ym + 6, kind, dash=dashed)
            out += text((x1 + x2) / 2, ym, t, size=11, mono=True, color=COL[kind][0], weight='600', halo=True)
        elif r[0] == 'note':
            a, t = r[1], r[2]
            tw = max(120, len(t) * 6.1 + 16)
            x = min(max(xs[a] - tw / 2, 4), w - tw - 4)
            out += '<rect x="%s" y="%s" width="%s" height="%s" rx="5" fill="#fffbea" stroke="#e0c97a" stroke-width="1"/>' % (
                x, ym - 10, tw, 21)
            out += text(x + tw / 2, ym + 4.5, t, size=10.5, color='#5c4a12')
        elif r[0] == 'band':
            t, kind = r[1], r[2]
            out += '<rect x="6" y="%s" width="%s" height="20" rx="4" fill="%s" stroke="%s" stroke-width="0.8"/>' % (
                ym - 10, w - 12, COL[kind][1], COL[kind][0])
            out += text(w / 2, ym + 4, t, size=11, color=COL[kind][0], weight='600')
        y += row_h
    for (name, kind), x in zip(parts, xs):
        out += box(x - 62, top, 124, head_h, name, kind, size=12)
    return svg(w, h, out)
