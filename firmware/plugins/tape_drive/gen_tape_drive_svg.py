#!/usr/bin/env python3
"""tape_drive's screen (e9): te-op1-cfw's ports/knl/tape_drive/gen_tape_drive_knl_svg.py (the canvas "Tape screen",
design A "Reels, aligned", 2026-10-27) made lean for coop and moved to coop's conventions.

    python3 gen_tape_drive_svg.py tape_drive.svg

The drawing is kennel's: the two reels with their green hubs (the screen jitters them at the wobble's rate), the tape
path down to the rail with the blue saturation wave on it (eight variants), the head under the gap, the wow -> flutter
scale with its green marker, the white low-pass glyph (its corner walking left as the tape wears) with the hiss specks
above it, the input-gain gauge at the left and the orange mix gauge at the right. What changed (README "Screen"):
- The gain readout glyphs (kennel's page-2 sign, digit, dot and tenth sets, 4.3 KB) are gone: the core shows the
  control's name and value when fn + blue turns it. The bracket round the gauge shows while fn is held (the held
  layer), and the saturation wave dims then, as the plain blue turn is not what fn + blue moves.
- The wave variants have 27 points (2 units apart over the 52-unit gap; kennel's had 53, 1 unit apart) and one decimal.
- Kennel's two polygons (the hubs' triangles and the head) are closed polylines; the head's black fill went (it sat on
  black).
- The palette keeps only the colours the screen code reads (white, the worn white, the body tone, blue).
- Colours: coop's brights (blue 698EFF, green 00ED95, white FFFFFF, orange FF3A5D); the fixed structure (reels, tape
  path, head, scale and gauge rails) in the one body tone 38354D (kennel drew the reels in a lighter 514959).

Rules (guide/plugin-guidelines.md §5.3, tools/plugin/pack.py): ids letters and digits, at most 2 decimals, lines,
polylines, rects and circles only, no polyline turning back. Stdlib only.
"""
import math
import re
import sys

DIM, BLUE, GREEN, WHITE, ORANGE, WORN = '#38354D', '#698EFF', '#00ED95', '#FFFFFF', '#FF3A5D', '#8E86B0'

TOP, BOTTOM, CY = 28, 124, 76              # the gauges' rows (as the bitcrusher's)
GAIN_X, MIX_X = 16, 304
GAIN_BAR = 24                              # the gain bar's height at +6 dB (drawn, scaled -1..1)
REEL_L, REEL_R, REEL_Y, REEL_R_PX = 96, 208, 48, 30
HUB_R = 10
RAIL_Y = 100
WAVE_AMP = 8
WAVE_STEPS = 26
SCALE_X0, SCALE_X1, SCALE_Y = 100, 204, 124
AGE_X, AGE_CORNER, AGE_END = 248, 276, 288
AGE_Y0, AGE_Y1 = 64, 88
SPECKS = [(252, 52), (272, 44), (260, 56), (284, 50), (256, 40), (278, 56)]


def num(v, places=2):
    s = ('%.*f' % (places, v)).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


def stroke(colour, join=True):
    """a line has no joins, so it goes without stroke-linejoin (about 600 bytes over the file)"""
    return 'fill="none" stroke="%s" stroke-width="1.5" stroke-linecap="round"%s' % (
        colour, ' stroke-linejoin="round"' if join else '')


def attrs(id_, hidden):
    return (' id="%s"' % id_ if id_ else '') + (' display="none"' if hidden else '')


def line(x1, y1, x2, y2, colour, id_=None, hidden=False):
    return '<line%s %s x1="%s" y1="%s" x2="%s" y2="%s"></line>' % (attrs(id_, hidden), stroke(colour, False), num(x1), num(y1),
                                                                     num(x2), num(y2))


def polyline(points, colour, id_=None, hidden=False, places=2):
    return '<polyline%s %s points="%s"></polyline>' % (attrs(id_, hidden), stroke(colour), ' '.join(
        '%s,%s' % (num(x, places), num(y, places)) for x, y in points))


def circle(cx, cy, r, colour):
    return '<circle %s cx="%s" cy="%s" r="%s"></circle>' % (stroke(colour), num(cx), num(cy), num(r))


def rect(x, y, w, h, fill, stroke_colour='none', id_=None, hidden=False):
    return '<rect%s fill="%s" stroke="%s" stroke-width="1.5" x="%s" y="%s" width="%s" height="%s"></rect>' % (
        attrs(id_, hidden), fill, stroke_colour, num(x), num(y), num(w), num(h))


def group(id_, inner, hidden=False):
    return '<g id="%s"%s>%s</g>' % (id_, ' display="none"' if hidden else '', inner)


def gauge(x):
    return line(x, TOP, x, BOTTOM, DIM) + line(x - 4, TOP, x + 4, TOP, DIM) + line(x - 4, BOTTOM, x + 4, BOTTOM, DIM)


def hub(cx, cy):
    pts = [(cx + HUB_R * math.cos(math.radians(-90 + 120 * i)), cy + HUB_R * math.sin(math.radians(-90 + 120 * i)))
           for i in range(4)]                                       # closed: the first corner again
    return polyline(pts, GREEN)


def wave(k, x0, x1, steps=WAVE_STEPS):
    g = 0.6 * 1.45 ** k
    return [(x0 + (x1 - x0) * i / steps,
             RAIL_Y - WAVE_AMP * math.tanh(g * math.sin(2 * math.pi * 2 * i / steps)) / math.tanh(g))
            for i in range(steps + 1)]


def build():
    e = ['<?xml version="1.0"?>',
         '<svg xmlns="http://www.w3.org/2000/svg" version="1.1" x="0px" y="0px" width="320px" height="160px" '
         'viewBox="0 0 320 160">']
    e.append(group('palette', ''.join(line(0, 0, 1, 0, c, i) for i, c in (
        ('palwhite', WHITE), ('paldim', DIM), ('palblue', BLUE), ('palworn', WORN))), hidden=True))
    s = []
    lx, rx, ry, r = REEL_L, REEL_R, REEL_Y, REEL_R_PX
    s.append(circle(lx, ry, r, DIM))
    s.append(circle(rx, ry, r, DIM))
    s.append(polyline([(lx - r, ry), (lx - r + 2, RAIL_Y), (lx + r, RAIL_Y)], DIM))
    s.append(polyline([(rx - r, RAIL_Y), (rx + r - 2, RAIL_Y), (rx + r, ry)], DIM))
    hx = (lx + rx) / 2
    s.append(polyline([(hx - 6, 108), (hx + 6, 108), (hx + 4, 114), (hx - 4, 114), (hx - 6, 108)], DIM))
    # The wow -> flutter scale: slow-wave glyph, track, fast-wave glyph.
    s.append(polyline([(SCALE_X0 - 18 + 1.5 * i, SCALE_Y + 3 * math.sin(math.pi * i / 4)) for i in range(9)], DIM))
    s.append(polyline([(SCALE_X1 + 6 + i, SCALE_Y + 3 * (1 if i % 2 else -1)) for i in range(13)], DIM))
    s.append(line(SCALE_X0, SCALE_Y, SCALE_X1, SCALE_Y, DIM))
    s.append(gauge(GAIN_X))
    s.append(gauge(MIX_X))
    e.append(group('static', ''.join(s)))
    # The hubs (the screen translates them).
    e.append(group('reelL', hub(lx, ry)))
    e.append(group('reelR', hub(rx, ry)))
    # Saturation: the wave on the rail, eight variants.
    for k in range(8):
        e.append(polyline(wave(k, lx + r, rx - r), BLUE, 'sat%d' % k, hidden=k != 3, places=1))
    # Wobble: the marker (translated 0..104 units).
    e.append(line(SCALE_X0, SCALE_Y - 5, SCALE_X0, SCALE_Y + 5, GREEN, 'wobble'))
    # Age: the low-pass glyph in two parts (the flat scaled about its left end, the slope translated with the
    # corner), and the hiss specks (a count set).
    e.append(line(AGE_X, AGE_Y0, AGE_CORNER, AGE_Y0, WHITE, 'agef'))
    e.append(polyline([(AGE_CORNER, AGE_Y0), (AGE_END, AGE_Y1)], WHITE, 'ages'))
    for i, (x, y) in enumerate(SPECKS):
        e.append(line(x, y, x + 0.5, y, WHITE, 'age%d' % i, hidden=i >= 2))
    # Gain: the centre mark and the bar (drawn at +6 dB, scaled -1..1 about the centre); the bracket while fn is held.
    e.append(line(GAIN_X - 4, CY, GAIN_X + 4, CY, BLUE, 'gzero'))
    e.append(rect(GAIN_X - 2, CY - GAIN_BAR, 4, GAIN_BAR, BLUE, id_='gbar'))
    e.append(rect(8, 22, 16, 108, 'none', BLUE, id_='gbox', hidden=True))
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
        raise ValueError('%d bytes: pack.py takes at most 16384' % len(svg))
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
