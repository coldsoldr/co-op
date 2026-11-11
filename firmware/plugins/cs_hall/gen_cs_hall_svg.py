#!/usr/bin/env python3
"""cs_hall's screen (e9): te-op1-cfw's ports/knl/cs_hall/hall.svg (the firmware build's hall screen, category dots
removed; kennel has no generator for it) rebuilt for coop from its own coordinates, moved to coop's conventions.

    python3 gen_cs_hall_svg.py cs_hall.svg

The drawing is kennel's: the hall's outline and a reflection path inside it (scaled about the floor by size), the
decay envelope and the damped (high-frequency) envelope below it, an infinity sign at infinite decay, the mix bar at
the top right and the freeze star at the top. The fn layer (kennel's page 2, shown while fn is held): the hall dimmed
to its body tone, the pre-delay gap, the early reflections and the late tail, the width marks, the lines' motion.
What changed (README "Screen"):
- Colour is the knob: damp is white's (kennel drew it in its pencil DED7FF), width is fn + white's (kennel: orange),
  the freeze star lights orange (orange's click; kennel lit it in decay's green). coop's brights blue 698EFF,
  green 00ED95, white FFFFFF, orange FF3A5D; one body tone 38354D (kennel's second dim 514959 folded into it).
- Kennel's page indicators (page1ind, page2ind) are gone: fn is a held layer, not a page; the layer is its own mark.
- The motion marks (the lines' modulation, fixed at 9830 in this build: kennel scaled them by it every frame) are drawn
  at that scale here, with no ids (nothing moves them), in the pencil DFD9FF (no encoder owns motion).
- The width triangles are paths (kennel: polygons, not in the renderer rules of guide/plugin-guidelines.md §5.3).
- A hidden palette line (palorange) gives the screen code the freeze star's lit colour.

Rules (guide/plugin-guidelines.md §5.3, tools/plugin/pack.py): ids letters and digits, at most 2 decimals, lines,
polylines, rects, circles and paths (M L C Z, one contour, kennel's) only, no polyline turning back, no self-closing
tags. Stdlib only, Python 3.8.
"""
import math
import re
import sys

BODY, PENCIL, BLUE, GREEN, WHITE, ORANGE = '#38354D', '#DFD9FF', '#698EFF', '#00ED95', '#FFFFFF', '#FF3A5D'
MOTION_SCALE = (9830 >> 5) / 1024.0          # cs_hall.cc kMotion, as kennel's ScaleX(node, motion >> 5, anchor)

# kennel's hall.svg coordinates
HALL = [(46, 84), (46, 44), (150, 26), (254, 44), (254, 84)]
REFL = [(118, 76), (46, 60), (100, 34.65), (184, 76)]
DECAY = [(34, 134), (34, 102), (46.5, 109.08), (59, 114.59), (71.5, 118.88), (84, 122.23), (96.5, 124.83),
         (109, 126.86), (121.5, 128.44), (134, 129.67), (146.5, 130.63), (159, 131.37), (171.5, 131.95),
         (184, 132.41), (196.5, 132.76), (209, 133.03), (221.5, 133.25), (234, 133.41), (234, 134)]
DAMP = [(34, 134), (34, 110), (46.5, 115.31), (59, 119.44), (71.5, 122.66), (84, 125.17), (96.5, 127.12),
        (109, 128.64), (121.5, 129.83), (134, 130.75), (146.5, 131.47), (159, 132.03), (171.5, 132.47),
        (184, 132.81), (196.5, 133.07), (209, 133.28), (221.5, 133.44), (234, 133.56), (234, 134)]
EARLY = [(40, 134), (40, 106), (41, 106), (41, 134), (47, 134), (47, 112), (48, 112), (48, 134), (55, 134),
         (55, 109), (56, 109), (56, 134), (64, 134), (64, 117), (65, 117), (65, 134)]
LATE = [(70, 134), (70, 114), (81.25, 118.42), (92.5, 121.87), (103.75, 124.55), (115, 126.64), (126.25, 128.27),
        (137.5, 129.54), (148.75, 130.52), (160, 131.29), (171.25, 131.89), (182.5, 132.36), (193.75, 132.72),
        (205, 133), (216.25, 133.22), (227.5, 133.4), (238.75, 133.53), (250, 133.63), (250, 134)]
FREEZE = [(112, 9), (113.44, 11.5), (116.33, 11.5), (114.89, 14), (116.33, 16.5), (113.44, 16.5), (112, 19),
          (110.56, 16.5), (107.67, 16.5), (109.11, 14), (107.67, 11.5), (110.56, 11.5), (112, 9)]
INF = 'M270,100 C270,96.5 273.5,96.5 276,100 C278.5,103.5 282,103.5 282,100 C282,96.5 278.5,96.5 276,100 ' \
      'C273.5,103.5 270,103.5 270,100 Z'


def motion(cx):
    pts = [(cx, 40)] + [(cx + (-4 if i % 2 == 0 else 4), 44 + 4 * i) for i in range(8)] + [(cx, 76)]
    return [(cx + (x - cx) * MOTION_SCALE, y) for x, y in pts]


def num(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


def stroke(colour):
    return 'fill="none" stroke="%s" stroke-width="1.5" stroke-linecap="square"' % colour


def attrs(id_, hidden):
    return (' id="%s"' % id_ if id_ else '') + (' display="none"' if hidden else '')


def line(x1, y1, x2, y2, colour, id_=None, hidden=False):
    return '<line%s %s x1="%s" y1="%s" x2="%s" y2="%s"></line>' % (attrs(id_, hidden), stroke(colour), num(x1), num(y1),
                                                                     num(x2), num(y2))


def polyline(points, colour, id_=None, hidden=False):
    return '<polyline%s %s points="%s"></polyline>' % (attrs(id_, hidden), stroke(colour),
                                                         ' '.join('%s,%s' % (num(x), num(y)) for x, y in points))


def rect(x, y, w, h, fill, id_=None):
    return '<rect%s fill="%s" stroke="none" x="%s" y="%s" width="%s" height="%s"></rect>' % (
        attrs(id_, False), fill, num(x), num(y), num(w), num(h))


def path(d, colour, filled, id_=None):
    return '<path%s fill="%s" stroke="%s" stroke-width="1.5" stroke-linecap="square" d="%s"></path>' % (
        attrs(id_, False), colour if filled else 'none', colour, d)


def build():
    e = ['<?xml version="1.0"?>',
         '<svg xmlns="http://www.w3.org/2000/svg" version="1.1" x="0px" y="0px" width="320px" height="160px" '
         'viewBox="0 0 320 160">']
    e.append('<g id="palette" display="none">' + line(0, 0, 1, 0, ORANGE, 'palorange') + '</g>')
    # still structure: the floor, the wall mark at the left, the hall's base line
    e.append(line(24, 134, 288, 134, BODY) + line(24, 134, 24, 102, BODY) + line(30, 84, 270, 84, BODY))
    e.append(polyline(REFL, BODY, 'refl'))
    e.append(polyline(FREEZE, BODY, 'freeze'))
    # the plain turns
    e.append('<g id="page1">')
    e.append(polyline(HALL, BLUE, 'size'))
    e.append(polyline(DECAY, GREEN, 'decay'))
    e.append('<g id="decayinf" display="none">' + path(INF, GREEN, False) + '</g>')
    e.append(polyline(DAMP, WHITE, 'damp'))
    e.append(rect(232, 12, 56, 4, BODY) + rect(232, 12, 56, 4, ORANGE, 'mix'))
    e.append('</g>')
    # the fn layer (kennel's page 2)
    e.append('<g id="page2" display="none">')
    e.append(polyline(HALL, BODY, 'room'))
    e.append(line(27, 98, 35, 98, BLUE, 'predelay'))
    e.append(polyline(EARLY, GREEN, 'early'))
    e.append(polyline(LATE, GREEN, 'late'))
    e.append(polyline(motion(30), PENCIL) + polyline(motion(270), PENCIL))
    e.append(line(260, 10, 260, 18, BODY))
    e.append(path('M256,10 L256,18 L250,14 Z', WHITE, True, 'widthL'))
    e.append(path('M264,10 L264,18 L270,14 Z', WHITE, True, 'widthR'))
    e.append('</g>')
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
    for bad in ('<text', '<polygon', '<style', 'url(', '/>'):
        if bad in svg:
            raise ValueError('forbidden: %r' % bad)
    for d in re.findall(r'\sd="([^"]*)"', svg):
        if re.search(r'[^MLCZ0-9., -]', d) or d.count('M') != 1:
            raise ValueError('path: M L C Z, one contour: %r' % d)
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
