#!/usr/bin/env python3
"""drive's screen (e9): te-op1-cfw's ports/knl/drive/gen_drive_knl_svg.py (the canvas "Drive screen", design A
"Schematic", 2026-10-27) made lean for coop and moved to coop's conventions.

    python3 gen_drive_svg.py drive.svg

The drawing is kennel's: the signal path as a circuit, left to right. The input jack, the input gain as a
potentiometer (a resistor with a blue wiper arrow the screen slides along it, -6 dB at the left end, +6 dB at the
right), the node, the clipper (two anti-parallel diodes to ground, blue), the triode (its filament and the glow under
the cathode warm with bias), the tilt EQ after it as its S-shaped response (green, drawn at full tilt; the screen
scales it about the pivot), the output arrow and the orange mix gauge at the far right. What changed (README
"Screen"):
- The diodes' fill (kennel's seven variants clip1..clip7 of filled triangles and conduction rays) is drawn by the
  screen code with raster lines (coop_scr_line): one scan line a unit from the tip, the rays as short lines.
- The gain readout glyphs (kennel's page-2 "+1.5" in dB and its rule, 9 KB of the 18 KB file) are gone: the core shows
  the control's name and value when fn + blue turns it. While fn is held a blue bracket marks the pot (the held
  layer, over the wiper's travel) and the diodes dim, as kennel's page 2 did.
- Kennel's polygons (diode triangles, the wiper's arrowhead) are closed polylines; wires that run on are merged into
  one polyline each.
- Colours: coop's brights (blue 698EFF, green 00ED95, white FFFFFF, orange FF3A5D); the wiring in the body tone
  38354D, the triode's envelope, plate and cathode in the pencil DFD9FF (structure no encoder owns); the filament
  warms from the body tone to white (bias is white's).

Rules (guide/plugin-guidelines.md §5.3, tools/plugin/pack.py): ids letters and digits, at most 2 decimals, lines,
polylines, rects and circles only, no polyline turning back. Stdlib only.
"""
import math
import re
import sys

PENCIL, DIM, BLUE, GREEN, WHITE, ORANGE, BLACK, GLOW = (
    '#DFD9FF', '#38354D', '#698EFF', '#00ED95', '#FFFFFF', '#FF3A5D', '#010101', '#7A6E8C')

Y = 78                                       # the signal rail
TOP, BOTTOM = 28, 124                        # the gauge's rows (as the bitcrusher's)
MIX_X = 304
POT_X0, POT_X1 = 30, 62                      # the pot's resistor
WIPER_X = 46                                 # the wiper's centre (the screen moves it -13..13 units)
NODE_X = 66
DIODE_L, DIODE_R = 52, 80                    # the two diodes' columns (drive_screen.cc kDiodeL / kDiodeR)
DIODE_A, DIODE_B = 96, 108                   # the diodes' triangles, top and bottom rows (drive_screen.cc)
TUBE_X, TUBE_R = 152, 34                     # the triode
PIVOT_X, TILT_HALF = 248, 22                 # the tilt's pivot and half width


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


def circle(cx, cy, r, colour, fill='none', id_=None):
    return '<circle%s fill="%s" stroke="%s" stroke-width="1.5" cx="%s" cy="%s" r="%s"></circle>' % (
        attrs(id_, False), fill, colour, num(cx), num(cy), num(r))


def rect(x, y, w, h, fill, stroke_colour='none', id_=None, hidden=False):
    return '<rect%s fill="%s" stroke="%s" stroke-width="1.5" x="%s" y="%s" width="%s" height="%s"></rect>' % (
        attrs(id_, hidden), fill, stroke_colour, num(x), num(y), num(w), num(h))


def group(id_, inner, hidden=False):
    return '<g id="%s"%s>%s</g>' % (id_, ' display="none"' if hidden else '', inner)


def zigzag(x0, x1, y, amp=4, teeth=6):
    pts = []
    w = (x1 - x0) / (teeth * 2)
    for i in range(teeth * 2):
        pts.append((x0 + w * (i + 0.5), y - amp if i % 2 == 0 else y + amp))
    return pts


def ground(cx, y):
    return line(cx - 9, y, cx + 9, y, DIM) + line(cx - 6, y + 3.5, cx + 6, y + 3.5, DIM) + line(cx - 3, y + 7, cx + 3, y + 7, DIM)


def diodes(colour):
    """The two diodes' outlines: the left one pointing down, the right one up (triangle and bar each)."""
    l = polyline([(DIODE_L - 5, DIODE_A), (DIODE_L + 5, DIODE_A), (DIODE_L, DIODE_B), (DIODE_L - 5, DIODE_A)], colour)
    l += line(DIODE_L - 5, DIODE_B, DIODE_L + 5, DIODE_B, colour)
    r = polyline([(DIODE_R - 5, DIODE_B), (DIODE_R + 5, DIODE_B), (DIODE_R, DIODE_A), (DIODE_R - 5, DIODE_B)], colour)
    r += line(DIODE_R - 5, DIODE_A, DIODE_R + 5, DIODE_A, colour)
    return l + r


def build():
    e = ['<?xml version="1.0"?>',
         '<svg xmlns="http://www.w3.org/2000/svg" version="1.1" x="0px" y="0px" width="320px" height="160px" '
         'viewBox="0 0 320 160">']
    e.append(group('palette', ''.join(line(0, 0, 1, 0, c, i) for i, c in (
        ('palwhite', WHITE), ('paldim', DIM), ('palblue', BLUE), ('palblack', BLACK), ('palglow', GLOW))), hidden=True))
    s = []
    cx, cy, r = TUBE_X, Y, TUBE_R
    # Input: the jack, then one wire through the pot's resistor and the node to the grid.
    s.append(circle(20, Y, 3, DIM))
    s.append(polyline([(23, Y), (POT_X0, Y)] + zigzag(POT_X0, POT_X1, Y) + [(POT_X1, Y), (cx - r, Y)], DIM))
    s.append(circle(NODE_X, Y, 2, 'none', DIM))
    # The clipper's wires and ground.
    s.append(line(NODE_X, Y, NODE_X, 88, DIM))
    s.append(polyline([(DIODE_L, DIODE_A), (DIODE_L, 88), (DIODE_R, 88), (DIODE_R, DIODE_A)], DIM))
    s.append(polyline([(DIODE_L, DIODE_B), (DIODE_L, 116), (DIODE_R, 116), (DIODE_R, DIODE_B)], DIM))
    s.append(line(NODE_X, 116, NODE_X, 122, DIM))
    s.append(ground(NODE_X, 122))
    # The triode: envelope, grid (dashes), plate and its lead on to the tilt, cathode and its lead to ground.
    s.append(circle(cx, cy, r, PENCIL))
    for gx in range(cx - 18, cx + 18, 8):
        s.append(line(gx, cy, gx + 4, cy, DIM))
    s.append(line(cx - 12, cy - 15, cx + 12, cy - 15, PENCIL))
    s.append(line(cx, cy - 15, cx, cy - r, PENCIL))
    s.append(polyline([(cx, cy - r), (cx, 34), (214, 34), (214, Y), (PIVOT_X - TILT_HALF - 4, Y)], DIM))
    s.append(polyline([(cx - 9, cy + 8), (cx - 9, cy + 11), (cx + 9, cy + 11), (cx + 9, cy + 8)], PENCIL))
    s.append(line(cx, cy + 11, cx, 122, DIM))
    s.append(ground(cx, 122))
    # After the tilt: the output arrow.
    s.append(circle(PIVOT_X, Y, 1.5, DIM))
    s.append(line(PIVOT_X + TILT_HALF, Y, 280, Y, DIM))
    s.append(polyline([(277, Y - 4), (281, Y), (277, Y + 4)], DIM))
    # The mix gauge's rail.
    s.append(line(MIX_X, TOP, MIX_X, BOTTOM, DIM) + line(MIX_X - 4, TOP, MIX_X + 4, TOP, DIM)
             + line(MIX_X - 4, BOTTOM, MIX_X + 4, BOTTOM, DIM))
    e.append(group('static', ''.join(s)))
    # The diodes: the blue outlines, and the dim copy shown while fn is held (their fill is drawn by the code).
    e.append(group('dio', diodes(BLUE)))
    e.append(group('diodim', diodes(DIM), hidden=True))
    # The input gain: the wiper (the screen translates it -13..13 units), and the bracket while fn is held.
    e.append(group('wiper', line(WIPER_X, 58, WIPER_X, 67, BLUE)
                   + polyline([(WIPER_X - 3, 67), (WIPER_X + 3, 67), (WIPER_X, 72), (WIPER_X - 3, 67)], BLUE)))
    e.append(polyline([(27, 62), (27, 52), (65, 52), (65, 62)], BLUE, 'gbox', hidden=True))   # over the wiper's travel
    # Bias: the glow under the cathode (fill) and the filament (line colour).
    e.append(circle(cx, cy + 20, 6, 'none', '#29252D', id_='glow'))
    fx, ft, fb = cx, cy + 15, cy + 26
    e.append(polyline([(fx - 4.5, fb), (fx - 4.5, ft), (fx - 2.25, ft + 6), (fx, ft), (fx + 2.25, ft + 6), (fx + 4.5, ft),
                       (fx + 4.5, fb)], '#8B86A6', 'fil'))
    # Tone: the S response at full tilt (the screen scales it about the pivot); 13 points read as smooth at this size.
    e.append(polyline([(PIVOT_X - TILT_HALF + 2 * TILT_HALF * i / 12, Y - 12 * math.tanh(3.0 * (2 * i / 12 - 1)))
                       for i in range(13)], GREEN, 'tilt'))
    # Mix: the bar drawn full, scaled about the foot.
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
    for bad in ('<text', '<path', '<polygon', '<style', 'url(', '/>'):
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
        raise ValueError('%d bytes: pack.py takes at most 16,384' % len(svg))
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
