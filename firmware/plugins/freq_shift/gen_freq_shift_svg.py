#!/usr/bin/env python3
"""freq_shift's screen (e9): te-op1-cfw's ports/knl/freq_shift/gen_shift_knl_svg.py (the canvas "Freq shift screen",
design B "Dial", 2026-10-27) made lean for coop and moved to coop's conventions.

    python3 gen_freq_shift_svg.py freq_shift.svg

The drawing is kennel's: the shift as two blue needles on a bipolar dial (0 Hz at the top, -2 kHz at the left end,
+2 kHz at the right, 78 degrees each way; L at shift + spread, R at shift - spread, mirrored about the top in ring
mode), the white spread arc (seven spans the screen turns to the shift's angle), the green feedback arcs (a count set,
mirrored for a shift down, turned to the top in ring mode), the input's ripples from the hub, the green mode marks,
the blue gain gauge at the left and the orange mix gauge at the right. What changed (README "Screen"):
- The gain readout glyphs (kennel's page-2 "+1.5" in dB, most of the 16.5 KB) are gone: the core shows the control's
  name and value when fn + blue turns it. The gain bracket shows while fn is held; the needles dim then.
- The mode bracket (kennel's green page 2) is gone: the mode is a green click now, with no page to bracket.
- The ring-mode feedback set (fr1..fr4) is gone: the screen turns the up set (fu1..fu4) to the top instead.
- The mode marks moved to the top right (the core's popup draws top left).
- Fewer points on the arcs (invisible at 1.5 units), one decimal.
- Colours: coop's brights (blue 698EFF, green 00ED95, white FFFFFF, orange FF3A5D); the body tone 38354D for the dial,
  the rails and the mode mark that is off; the ripples born at 8E86B0, fading to 010101.

Rules (guide/plugin-guidelines.md §5.3, tools/plugin/pack.py): ids letters and digits, at most 2 decimals, lines,
polylines, rects and circles only, no polyline turning back. Stdlib only.
"""
import math
import re
import sys

DIM, MID, BLUE, GREEN, WHITE, ORANGE, BLACK = (
    '#38354D', '#8E86B0', '#698EFF', '#00ED95', '#FFFFFF', '#FF3A5D', '#010101')

TOP, BOTTOM, CY = 28, 124, 76              # the gauges' rows (as bitcrusher's)
GAIN_X, MIX_X = 16, 304
GAIN_BAR = 24
CX, CYD, R = 160, 124, 100                 # the dial: centre and radius (freq_shift_screen.cc kHubX, kHubY)
SWEEP = 78                                 # degrees from the top to either end
NEEDLE = R - 6
RIPPLE_R = 20                              # a ripple's drawn radius (scaled up to R)
MODE_X, MODE_Y = 244, 14                   # the mode marks (kennel: 48, 14)


def num(v):
    s = ('%.1f' % v).rstrip('0').rstrip('.')
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


def circle(cx, cy, r, colour, id_=None):
    return '<circle%s fill="none" stroke="%s" stroke-width="1.5" cx="%s" cy="%s" r="%s"></circle>' % (
        attrs(id_, False), colour, num(cx), num(cy), num(r))


def rect(x, y, w, h, fill, stroke_colour='none', id_=None, hidden=False):
    return '<rect%s fill="%s" stroke="%s" stroke-width="1.5" x="%s" y="%s" width="%s" height="%s"></rect>' % (
        attrs(id_, hidden), fill, stroke_colour, num(x), num(y), num(w), num(h))


def gauge(x):
    return line(x, TOP, x, BOTTOM, DIM) + line(x - 4, TOP, x + 4, TOP, DIM) + line(x - 4, BOTTOM, x + 4, BOTTOM, DIM)


def pt(r, deg):
    return (CX + r * math.cos(math.radians(deg)), CYD - r * math.sin(math.radians(deg)))


def arc(r, a0, a1, colour, steps, id_=None, hidden=False):
    return polyline([pt(r, a0 + (a1 - a0) * i / steps) for i in range(steps + 1)], colour, id_, hidden)


def build():
    e = ['<?xml version="1.0"?>',
         '<svg xmlns="http://www.w3.org/2000/svg" version="1.1" x="0px" y="0px" width="320px" height="160px" '
         'viewBox="0 0 320 160">']
    # The colours the screen writes (read at init, so a screen shown again starts from the same values).
    e.append('<g id="palette" display="none">' + ''.join(line(0, 0, 1, 0, c, i) for i, c in (
        ('paldim', DIM), ('palmid', MID), ('palblack', BLACK), ('palblue', BLUE), ('palgreen', GREEN))) + '</g>')
    # The dial: the arc, the 0 Hz mark, the end ticks, the hub.
    e.append(arc(R, 90 - SWEEP, 90 + SWEEP, DIM, 24))
    e.append(line(CX, CYD - R - 4, CX, CYD - R + 4, DIM))
    for t in (-1, 1):
        (x1, y1), (x2, y2) = pt(R - 4, 90 - SWEEP * t), pt(R + 4, 90 - SWEEP * t)
        e.append(line(x1, y1, x2, y2, DIM))
    e.append(circle(CX, CYD, 3, DIM))
    # The sound's ripples: four arcs at r 20 the screen scales out to the rim and fades.
    for i in range(4):
        e.append(arc(RIPPLE_R, 90 - SWEEP, 90 + SWEEP, MID, 16, 'rp%d' % (i + 1), hidden=True))
    # The mode marks: an arrow (the shifter) and a ring (the ring mod); green = the mode on.
    e.append(line(MODE_X, MODE_Y, MODE_X + 12, MODE_Y, GREEN, 'modea1'))
    e.append(polyline([(MODE_X + 8, MODE_Y - 4), (MODE_X + 12, MODE_Y), (MODE_X + 8, MODE_Y + 4)], GREEN, 'modea2'))
    e.append(circle(MODE_X + 28, MODE_Y, 4, DIM, 'moder'))
    # Feedback: arcs sweeping right from the top (mirrored for down, turned to the top in ring mode), a count set.
    for i in range(4):
        e.append(arc(R - 30 - 12 * i, 90, 90 - 70, GREEN, 8, 'fu%d' % (i + 1), hidden=i >= 2))
    # Spread: seven spans centred on the top (the screen turns the shown one to the shift).
    for k in range(1, 8):
        d = SWEEP * 0.33 * k / 7
        e.append(arc(R - 16, 90 + d, 90 - d, WHITE, k + 1, 'sp%d' % k, hidden=k != 4))
    # The needles, drawn pointing up (the screen turns them about the hub).
    e.append(line(CX, CYD, CX, CYD - NEEDLE, BLUE, 'ndlL'))
    e.append(line(CX, CYD, CX, CYD - NEEDLE, BLUE, 'ndlR'))
    # Gain: the rails, the centre mark and the bar (drawn at +6 dB, scaled -1..1 about the centre); the bracket.
    e.append(gauge(GAIN_X) + line(GAIN_X - 4, CY, GAIN_X + 4, CY, BLUE))
    e.append(rect(GAIN_X - 2, CY - GAIN_BAR, 4, GAIN_BAR, BLUE, id_='gbar'))
    e.append(rect(8, 22, 16, 108, 'none', BLUE, id_='gbox', hidden=True))
    # Mix: the rails and the bar drawn full, scaled about the foot.
    e.append(gauge(MIX_X) + rect(MIX_X - 2, TOP, 4, BOTTOM - TOP, ORANGE, id_='mbar'))
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
    if len(svg) > 16384:
        raise ValueError('%d bytes: over pack.py limit' % len(svg))
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
