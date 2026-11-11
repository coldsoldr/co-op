#!/usr/bin/env python3
"""cs_plate's screen (e9): te-op1-cfw's ports/knl/cs_plate/plate.svg (the firmware build's plate.svg with its category
dots removed; no generator was kept for it) rebuilt from its structure for coop's conventions.

    python3 gen_cs_plate_svg.py cs_plate.svg

How it was made: kennel's plate.svg was read element by element; every coordinate here is kennel's (the curves are
its formula: y = 134 - h exp(-i / 4) at x = 34 + 12.5 i, h 32 for decay and 24 for damp; the diffusion bars and the
motion zigzags are its literal points). The drawing is kennel's: the plate seen side-on with up to four rings (decay),
the pre-delay gap, the decay and damp envelopes, the mix bar, the freeze star; with fn held the size outline, the
diffusion bars and the motion zigzags. What changed (README "Screen"):
- Kennel's page dots (page1ind, page2ind) are gone: coop has no pages, fn held is the layer. The low-cut mark and its
  track are gone: the low cut is fixed (no encoder moves it), and a bright means an encoder. The page-2 "tail" (a dim
  copy of the decay curve) is gone: under fn the screen code dims the decay curve itself.
- Kennel's polygons are closed paths (M L Z) and its ellipses four-curve paths (M C Z): the manifesto's subset
  (guidelines §5.3) has no polygon or ellipse. Rings, diffusion bars and outlines carry their ids themselves (kennel
  wrapped them in groups).
- Colours: coop's brights (blue 698EFF, green 00ED95, white FFFFFF, orange FF3A5D), one body tone 38354D (kennel's
  514959 and 38354D both). The pre-delay mark and the motion zigzags are white (white's turn and fn layer; kennel
  drew them in pencil); the freeze star is orange while frozen (the orange click; kennel used the damp green).

Rules (guide/plugin-guidelines.md §5.3, tools/plugin/pack.py): ids letters and digits, at most 2 decimals, lines,
polylines and paths (M L C Z) only, no polyline turning back, explicit closing tags. Stdlib only, Python 3.8.
"""
import math
import re
import sys

DIM, BLUE, GREEN, WHITE, ORANGE = '#38354D', '#698EFF', '#00ED95', '#FFFFFF', '#FF3A5D'

CX, CY = 150, 54                              # the plate's centre (the rings' and the size outline's)
PLATE = [(100, 36), (220, 36), (200, 72), (80, 72)]
DIFF_X = [60, 92, 74, 110, 66, 82, 100, 120]  # kennel's diff1..diff8 (count-set order)
STAR = [(112, 9), (113.44, 11.5), (116.33, 11.5), (114.89, 14), (116.33, 16.5), (113.44, 16.5), (112, 19),
        (110.56, 16.5), (107.67, 16.5), (109.11, 14), (107.67, 11.5), (110.56, 11.5), (112, 9)]


def num(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


def stroke(colour):
    return 'fill="none" stroke="%s" stroke-width="1.5" stroke-linecap="square"' % colour


def attrs(id_, hidden):
    return (' id="%s"' % id_ if id_ else '') + (' display="none"' if hidden else '')


def pt(p):
    return '%s,%s' % (num(p[0]), num(p[1]))


def line(x1, y1, x2, y2, colour, id_=None, hidden=False):
    return '<line%s %s x1="%s" y1="%s" x2="%s" y2="%s"></line>' % (attrs(id_, hidden), stroke(colour), num(x1), num(y1),
                                                                     num(x2), num(y2))


def polyline(points, colour, id_=None, hidden=False):
    return '<polyline%s %s points="%s"></polyline>' % (attrs(id_, hidden), stroke(colour), ' '.join(map(pt, points)))


def path(d, colour, id_=None, hidden=False):
    return '<path%s %s d="%s"></path>' % (attrs(id_, hidden), stroke(colour), d)


def closed(points):
    return 'M' + pt(points[0]) + ''.join(' L' + pt(p) for p in points[1:]) + ' Z'


def ellipse(rx, ry):
    """four cubic quarter arcs (k = 0.5523), as kennel's <ellipse cx cy rx ry>"""
    k = 0.5523
    a, b = rx * k, ry * k
    q = [((CX + rx, CY), (CX + rx, CY + b), (CX + a, CY + ry), (CX, CY + ry)),
         (None, (CX - a, CY + ry), (CX - rx, CY + b), (CX - rx, CY)),
         (None, (CX - rx, CY - b), (CX - a, CY - ry), (CX, CY - ry)),
         (None, (CX + a, CY - ry), (CX + rx, CY - b), (CX + rx, CY))]
    d = 'M' + pt(q[0][0])
    for _, c1, c2, e in q:
        d += ' C' + ' '.join(map(pt, (c1, c2, e)))
    return d + ' Z'


def envelope(h):
    return [(34, 134)] + [(34 + 12.5 * i, 134 - h * math.exp(-i / 4)) for i in range(17)] + [(234, 134)]


def build():
    e = ['<?xml version="1.0"?>',
         '<svg xmlns="http://www.w3.org/2000/svg" version="1.1" x="0px" y="0px" width="320px" height="160px" '
         'viewBox="0 0 320 160">']
    e.append('<g id="palette" display="none">' + line(0, 0, 1, 0, ORANGE, 'palorange') + '</g>')
    e.append(line(24, 134, 288, 134, DIM) + line(24, 134, 24, 102, DIM))          # the floor and the level axis
    e.append(path(closed(PLATE), DIM, 'plate'))
    e.append(path(closed(PLATE), BLUE, 'size', hidden=True))                      # fn: scaled by the size step
    for i in range(4):
        e.append(path(ellipse(9 * (i + 1), 3 * (i + 1)), BLUE, 'ring%d' % (i + 1), hidden=i == 3))
    e.append(line(27, 98, 35, 98, WHITE, 'predelay'))
    e.append(polyline(envelope(32), BLUE, 'decay'))
    e.append(polyline(envelope(24), GREEN, 'damp'))
    for i, x in enumerate(DIFF_X):
        e.append(line(x, 134, x, 109.2 + 0.2 * (x - 60), GREEN, 'diff%d' % (i + 1), hidden=True))
    for id_, x in (('motionL', 56), ('motionR', 244)):
        pts = [(x, 38)] + [(x + (4 if j % 2 else -4), 42 + 4 * j) for j in range(7)] + [(x, 70)]
        e.append(polyline(pts, WHITE, id_, hidden=True))
    e.append('<rect fill="%s" stroke="none" x="232" y="12" width="56" height="4"></rect>' % DIM)
    e.append('<rect id="mix" fill="%s" stroke="none" x="232" y="12" width="56" height="4"></rect>' % ORANGE)
    e.append(polyline(STAR, DIM, 'freeze'))
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
    for bad in ('<text', '<style', '<polygon', '<ellipse', 'url(', '/>'):
        if bad in svg:
            raise ValueError('forbidden: %r' % bad)
    for d in re.findall(r' d="([^"]*)"', svg):
        if re.search(r'[^MLCZ0-9.,\s-]', d):
            raise ValueError('path command outside M L C Z: %r' % d)
    for pts in re.findall(r'points="([^"]*)"', svg):
        p = [tuple(map(float, q.split(','))) for q in pts.split()]
        for a, b, c in zip(p, p[1:], p[2:]):
            ux, uy, vx, vy = b[0] - a[0], b[1] - a[1], c[0] - b[0], c[1] - b[1]
            lu, lv = math.hypot(ux, uy), math.hypot(vx, vy)
            if lu == 0 or lv == 0 or (ux * vx + uy * vy) / (lu * lv) < -0.985:
                raise ValueError('polyline turns back at %r' % (b,))
    if len(svg) > 16384:
        raise ValueError('over 16,384 bytes')
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
