#!/usr/bin/env python3
"""bitcrusher's screen (e9): te-op1-cfw's ports/knl/bitcrusher/gen_crush_knl_svg.py (the canvas "Bitcrusher screen",
option C "Ladder", 2026-10-27) made lean for coop and moved to coop's conventions.

    python3 gen_bitcrusher_svg.py bitcrusher.svg

The drawing is kennel's: a dim reference sine, the blue staircase (eight variants by rate), the green bit ladder, the
white sampling dot, the input-gain gauge at the left and the orange mix gauge at the right. What changed (README
"Screen"):
- The clock row (eight variants x two halves of ticks, 17 KB of the 29 KB file) is drawn by the screen code with
  raster lines (coop_scr_line), in white: the jitter (white) shakes its two halves apart.
- The gain readout glyphs (kennel's page-2 "+1.5" in dB) are gone: the core shows the control's name and value when
  fn + blue turns it. The gain is blue's (fn + blue), so its gauge is blue, brightening with the input level; the
  bracket round it shows while fn is held (the held layer), and the staircase dims then, as kennel's page 2 did.
- Colours: coop's brights (blue 698EFF, green 00ED95, white FFFFFF, orange FF3A5D); the pencil DFD9FF for the dot.

Rules (guide/plugin-guidelines.md §5.3, tools/plugin/pack.py): ids letters and digits, at most 2 decimals, lines,
polylines, rects and circles only, no polyline turning back. Stdlib only.
"""
import math
import re
import sys

PENCIL, DIM, BLUE, DIMBLUE, GREEN, WHITE, ORANGE, BLACK, MID = (
    '#DFD9FF', '#38354D', '#698EFF', '#2A3966', '#00ED95', '#FFFFFF', '#FF3A5D', '#010101', '#8E86B0')

TOP, BOTTOM, CY, AMP = 28, 124, 76, 48      # the plot's rows: nine levels 12 units apart
X0, X1 = 40, 288                             # the staircase's span
LEVELS = 9
STEP_WIDTHS = [round(8 * 1.32 ** k, 1) for k in range(8)]   # rate index 0..7: 8 .. 55.9 units (bitcrusher_screen.cc)
GAIN_X, MIX_X = 16, 304
GAIN_BAR = 24                                # the bar's height at +6 dB (drawn, scaled -1..1)
BIT_ROWS = [4, 1, 7, 2, 6, 3, 5, 0]          # ladder ticks in count-set order (level index)


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


def polyline(points, colour, id_=None, hidden=False):
    return '<polyline%s %s points="%s"></polyline>' % (attrs(id_, hidden), stroke(colour),
                                                         ' '.join('%s,%s' % (num(x), num(y)) for x, y in points))


def rect(x, y, w, h, fill, stroke_colour='none', id_=None, hidden=False):
    return '<rect%s fill="%s" stroke="%s" stroke-width="1.5" x="%s" y="%s" width="%s" height="%s"></rect>' % (
        attrs(id_, hidden), fill, stroke_colour, num(x), num(y), num(w), num(h))


def sine_y(x):
    return CY - AMP * math.sin(2 * math.pi * (x - X0) / (X1 - X0))


def level_y(x):
    j = round((sine_y(x) - TOP) / 12)
    return TOP + 12 * min(LEVELS - 1, max(0, j))


def staircase(width):
    pts, x, prev = [], X0, None
    while x <= X1 + 0.01:
        y = level_y(x)
        if prev is None:
            pts.append((x, y))
        elif y != prev:
            pts += [(x, prev), (x, y)]
        prev = y
        x += width
    end = min(x, X1)
    if end > pts[-1][0]:
        pts.append((end, prev))
    return pts


def build():
    e = ['<?xml version="1.0"?>',
         '<svg xmlns="http://www.w3.org/2000/svg" version="1.1" x="0px" y="0px" width="320px" height="160px" '
         'viewBox="0 0 320 160">']
    e.append('<g id="palette" display="none">' + ''.join(line(0, 0, 1, 0, c, i) for i, c in (
        ('palwhite', WHITE), ('paldim', DIM), ('palblue', BLUE), ('paldimblue', DIMBLUE), ('palmid', MID))) + '</g>')
    e.append(polyline([(X0 + (X1 - X0) * i / 48, sine_y(X0 + (X1 - X0) * i / 48)) for i in range(49)], DIM, 'sine'))
    for k, w in enumerate(STEP_WIDTHS):
        e.append(polyline(staircase(w), BLUE, 'stair%d' % k, hidden=k != 3))
    for i, j in enumerate(BIT_ROWS):
        e.append(line(30, TOP + 12 * j, 36, TOP + 12 * j, GREEN, 'bit%d' % i, hidden=i >= 5))
    e.append('<g id="dot"><circle fill="%s" stroke="none" cx="%s" cy="%s" r="2.5"></circle></g>' % (PENCIL, X0, CY))
    for x, colour in ((GAIN_X, DIMBLUE), (MIX_X, DIM)):                  # the gauges' rails
        e.append(line(x, TOP, x, BOTTOM, DIM) + line(x - 4, TOP, x + 4, TOP, DIM) + line(x - 4, BOTTOM, x + 4, BOTTOM, DIM))
    e.append(line(GAIN_X - 4, CY, GAIN_X + 4, CY, BLUE, 'gzero'))
    e.append(rect(GAIN_X - 2, CY - GAIN_BAR, 4, GAIN_BAR, BLUE, id_='gbar'))
    e.append(rect(8, 22, 16, 108, 'none', BLUE, id_='gbox', hidden=True))
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
    for bad in ('<text', '<path', '<style', 'url('):
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
    print('%s: %d bytes, %d ids, step widths %s' % (sys.argv[1], len(svg), len(re.findall(r'id="', svg)), STEP_WIDTHS))


if __name__ == '__main__':
    main()
