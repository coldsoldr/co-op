"""A 5 x 7 stroke font for the kennel plugins' screens (the OP-1's parser has
no text): every glyph a list of polylines in a cell with x right, y down and
the origin at the top left. Corners at 90 degrees or gentler, so no vertex
turns back on itself (the renderer spikes on those; hundekraft's svg_check
and our generators refuse them). Used by gen_rings_knl_svg.py and
gen_crush_knl_svg.py.
"""

GLYPHS = {
    'A': [[(0, 7), (0, 2), (2.5, 0), (5, 2), (5, 7)], [(0, 4), (5, 4)]],
    'B': [[(0, 7), (0, 0), (4, 0), (5, 1), (5, 3), (4, 3.5), (5, 4), (5, 6), (4, 7), (0, 7)], [(0, 3.5), (4, 3.5)]],
    'D': [[(0, 7), (0, 0), (3, 0), (5, 2), (5, 5), (3, 7), (0, 7)]],
    'E': [[(5, 0), (0, 0), (0, 7), (5, 7)], [(0, 3.5), (4, 3.5)]],
    'F': [[(5, 0), (0, 0), (0, 7)], [(0, 4), (4, 4)]],
    'G': [[(5, 0), (0, 0), (0, 7), (5, 7), (5, 4), (3, 4)]],
    'I': [[(2.5, 0), (2.5, 7)]],
    'L': [[(0, 0), (0, 7), (5, 7)]],
    'M': [[(0, 7), (0, 0), (2.5, 3.5), (5, 0), (5, 7)]],
    'N': [[(0, 7), (0, 0), (5, 7), (5, 0)]],
    'O': [[(0, 0), (5, 0), (5, 7), (0, 7), (0, 0)]],
    'P': [[(0, 7), (0, 0), (5, 0), (5, 4), (0, 4)]],
    'Q': [[(0, 0), (5, 0), (5, 7), (0, 7), (0, 0)], [(3, 5), (6, 8)]],
    'R': [[(0, 7), (0, 0), (5, 0), (5, 4), (0, 4)], [(2, 4), (5, 7)]],
    'S': [[(5, 0), (0, 0), (0, 3.5), (5, 3.5), (5, 7), (0, 7)]],
    'T': [[(0, 0), (5, 0)], [(2.5, 0), (2.5, 7)]],
    'U': [[(0, 0), (0, 7), (5, 7), (5, 0)]],
    'V': [[(0, 0), (2.5, 7), (5, 0)]],
    'Y': [[(0, 0), (2.5, 3), (5, 0)], [(2.5, 3), (2.5, 7)]],
    '0': [[(0, 0), (5, 0), (5, 7), (0, 7), (0, 0)]],
    '1': [[(1, 2), (2.5, 0), (2.5, 7)]],
    '2': [[(0, 0), (5, 0), (5, 3.5), (0, 3.5), (0, 7), (5, 7)]],
    '3': [[(0, 0), (5, 0), (5, 7), (0, 7)], [(2, 3.5), (5, 3.5)]],
    '4': [[(4, 7), (4, 0), (0, 5), (5, 5)]],
    '5': [[(5, 0), (0, 0), (0, 3.5), (5, 3.5), (5, 7), (0, 7)]],
    '6': [[(5, 0), (0, 0), (0, 7), (5, 7), (5, 3.5), (0, 3.5)]],
    '7': [[(0, 0), (5, 0), (2.5, 7)]],
    '8': [[(0, 0), (5, 0), (5, 7), (0, 7), (0, 0)], [(0, 3.5), (5, 3.5)]],
    '9': [[(5, 3.5), (0, 3.5), (0, 0), (5, 0), (5, 7), (0, 7)]],
    '+': [[(2.5, 1), (2.5, 6)], [(0, 3.5), (5, 3.5)]],
    '-': [[(0, 3.5), (5, 3.5)]],
    '.': [[(0.5, 7), (1.5, 7)]],
    '/': [[(0, 7), (5, 0)]],
}


def num(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


def glyph_polylines(ch, x, y, colour, scale=1.0, width=1.5):
    """The polylines of one glyph with its cell's top left at (x, y)."""
    out = []
    for pl in GLYPHS[ch]:
        pts = ' '.join('%s,%s' % (num(x + px * scale), num(y + py * scale)) for px, py in pl)
        out.append('<polyline fill="none" stroke="%s" stroke-width="%s" stroke-linecap="round" '
                   'stroke-linejoin="round" points="%s"></polyline>' % (colour, num(width), pts))
    return ''.join(out)


def label(text, x, y, colour, scale=1.0, advance=7):
    """A run of glyphs, `advance` cell units apart (scaled)."""
    out = []
    for ch in text:
        out.append(glyph_polylines(ch, x, y, colour, scale))
        x += advance * scale
    return ''.join(out)
