#!/usr/bin/env python3
"""wavefolder's screen (e9): te-op1-cfw's ports/knl/wavefolder/gen_fold_knl_svg.py (the canvas "Fold screen", design A
"Wave, aligned", 2026-10-27) made lean for coop and moved to coop's conventions.

    python3 gen_wavefolder_svg.py wavefolder.svg

The drawing is kennel's: a blue input-gain gauge at the far left (0 dB at its centre mark), one cycle of the folded
output wave drawn large between two dim fold lines, the green offset line the screen slides with symmetry, the white
low-pass glyph at the right (its corner walking left as tone darkens) and the orange mix gauge at the far right. What
changed (README "Screen"):
- The wave (kennel's eight 97-point polylines, 12 KB of the 16 KB file) is drawn by the screen code with raster lines
  (coop_scr_line, wavefolder_screen.cc), the same curve y = CY - AMP sin(g pi/2 sin th), g following the fold turn
  continuously (1 .. 5.2, kennel's eight poses' range) instead of in eight steps. This file keeps its colours in the
  palette (palblue at rest, paldim while fn is held).
- The gain readout glyphs (kennel's page-2 "+1.5" in dB) are gone: the core shows the control's name and value when
  fn + blue turns it. The bracket round the gauge shows while fn is held (the held layer).
- Colours: coop's brights (blue 698EFF, green 00ED95, white FFFFFF, orange FF3A5D) and body tone 38354D.

Rules (guide/plugin-guidelines.md §5.3, tools/plugin/pack.py): ids letters and digits, at most 2 decimals, lines,
polylines, rects and circles only, no polyline turning back. Stdlib only.
"""
import math
import re
import sys

DIM, BLUE, GREEN, WHITE, ORANGE = '#38354D', '#698EFF', '#00ED95', '#FFFFFF', '#FF3A5D'

TOP, BOTTOM, CY = 28, 124, 76              # the gauges' rows (as the bitcrusher's)
GAIN_X, MIX_X = 16, 304
GAIN_BAR = 24
X0, X1, AMP = 44, 236, 36                  # the wave's span and amplitude about CY (wavefolder_screen.cc)
TONE_X, TONE_CORNER, TONE_END = 248, 276, 288   # the glyph (the corner at its right)
TONE_Y0, TONE_Y1 = 64, 88


def num(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


def stroke(colour):
    return 'fill="none" stroke="%s" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"' % colour


def attrs(id_, hidden):
    return (' id="%s"' % id_ if id_ else '') + (' display="none"' if hidden else '')


def line(x1, y1, x2, y2, colour, id_=None, hidden=False):
    return '<line%s %s x1="%s" y1="%s" x2="%s" y2="%s"></line>' % (attrs(id_, hidden), stroke(colour), num(x1), num(y1),
                                                                     num(x2), num(y2))


def rect(x, y, w, h, fill, stroke_colour='none', id_=None, hidden=False):
    return '<rect%s fill="%s" stroke="%s" stroke-width="1.5" x="%s" y="%s" width="%s" height="%s"></rect>' % (
        attrs(id_, hidden), fill, stroke_colour, num(x), num(y), num(w), num(h))


def gauge(x):
    return line(x, TOP, x, BOTTOM, DIM) + line(x - 4, TOP, x + 4, TOP, DIM) + line(x - 4, BOTTOM, x + 4, BOTTOM, DIM)


def build():
    e = ['<?xml version="1.0"?>',
         '<svg xmlns="http://www.w3.org/2000/svg" version="1.1" x="0px" y="0px" width="320px" height="160px" '
         'viewBox="0 0 320 160">']
    e.append('<g id="palette" display="none">' + ''.join(line(0, 0, 1, 0, c, i) for i, c in (
        ('palblue', BLUE), ('paldim', DIM))) + '</g>')
    # The fold lines (the wave's +-1, drawn by the code between them).
    e.append(line(X0, CY - AMP, X1, CY - AMP, DIM) + line(X0, CY + AMP, X1, CY + AMP, DIM))
    # Symmetry: the offset line (translated -24..24 units).
    e.append(line(X0, CY, X1, CY, GREEN, 'symmetry'))
    # Tone: the low-pass glyph in two parts (the flat scaled about its left end, the slope translated with the corner).
    e.append(line(TONE_X, TONE_Y0, TONE_CORNER, TONE_Y0, WHITE, 'tonef'))
    e.append(line(TONE_CORNER, TONE_Y0, TONE_END, TONE_Y1, WHITE, 'tones'))
    # Gain: the rails, the centre mark and the bar (drawn at +6 dB, scaled -1..1 about the centre); the bracket (fn).
    e.append(gauge(GAIN_X) + line(GAIN_X - 4, CY, GAIN_X + 4, CY, BLUE, 'gzero'))
    e.append(rect(GAIN_X - 2, CY - GAIN_BAR, 4, GAIN_BAR, BLUE, id_='gbar'))
    e.append(rect(8, 22, 16, 108, 'none', BLUE, id_='gbox', hidden=True))
    # Mix: the bar drawn full, scaled about the foot.
    e.append(gauge(MIX_X))
    e.append(rect(MIX_X - 2, TOP, 4, BOTTOM - TOP, ORANGE, id_='mbar'))
    e.append('</svg>')
    return '\n'.join(e) + '\n'


def check(svg):
    ids = re.findall(r'id="([^"]*)"', svg)
    for i in ids:
        if not re.fullmatch(r'[A-Za-z][A-Za-z0-9]*', i):
            raise ValueError('bad id %r' % i)
    if len(ids) != len(set(ids)):
        raise ValueError('duplicate id')
    if re.search(r'\d\.\d{3,}', svg):
        raise ValueError('more than 2 decimals')
    for bad in ('<text', '<path', '<style', 'url(', '/>'):
        if bad in svg:
            raise ValueError('forbidden: %r' % bad)
    for pts in re.findall(r'points="([^"]*)"', svg):
        p = [tuple(map(float, q.split(','))) for q in pts.split()]
        for a, b, c in zip(p, p[1:], p[2:]):
            ux, uy, vx, vy = b[0] - a[0], b[1] - a[1], c[0] - b[0], c[1] - b[1]
            lu, lv = math.hypot(ux, uy), math.hypot(vx, vy)
            if lu == 0 or lv == 0 or (ux * vx + uy * vy) / (lu * lv) < -0.985:
                raise ValueError('polyline turns back at %r' % (b,))
    return svg


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    svg = check(build())
    with open(sys.argv[1], 'w', encoding='utf-8', newline='\n') as f:
        f.write(svg)
    print('%s: %d bytes, %d ids' % (sys.argv[1], len(svg), len(re.findall(r'id="', svg))))


if __name__ == '__main__':
    main()
