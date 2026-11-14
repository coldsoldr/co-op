#!/usr/bin/env python3
"""fickle's screen (e9): a two-footswitch stompbox in 3/4 view, drawn as wireframe (the owner's design "Fickle", the
canvas mock-up Fickle.dc.html: its renderVals() holds the geometry this follows).

    python3 gen_fickle_svg.py fickle.svg

Every part here is FLAT art in its own plane's coordinates, in model units (u right, v back, z up; the box is 156 x 110 x
32, centred on u = v = 0, standing on z = 0). The screen (fickle_screen.cc) moves each group with one affine matrix a
frame (coop_scr_transform, the screen-space matrix of that plane under the orthographic projection), so nothing here
carries a transform (transforms flattened, guidelines §5.3) and every basis vector the screen uses is a unit one: a
group is never scaled up, so a stroke stays 1.5 whether or not R246 scales strokes with the matrix.

    front                the front face (z up) with its five vents
    side                 one side face (v right, z up): the screen puts it on whichever side faces the eye
    top                  the top face (u, v): the seam between the wet half and the loop half, the knobs' base rings,
                         the footswitch nuts, the routing switch's ring, the two LEDs, the loop ring, the dub ring and
                         the loop-mode glyphs (reels, env spikes, stretch strokes; the screen shows one)
    s0..s3, s4u s4d s5u s5d   the knobs' and footswitches' sides (a rect across the cylinder's silhouette, in its
                         vertical plane; the footswitches up and pressed)
    k0..k5               the knob and footswitch tops (a circle at the plane's origin)
    head, tip, wd0..wd6  dots the screen translates in screen space (the loop head, the routing lever's tip, the reverb
                         glyph's seven dots)

The pointers, the routing lever, the loop ring's ticks, the reel spokes, the delay bars and the slip zigzag are raster
lines (coop_scr_line) the screen draws over this every frame.

Rules (guide/plugin-guidelines.md §5.3, tools/plugin/pack.py): ids letters and digits, at most 2 decimals, lines,
polylines, rects and circles only, no polyline turning back, no self-closing tags, no transforms. Stdlib only.
"""
import math
import re
import sys

PENCIL, BODY, BLACK = '#DFD9FF', '#38354D', '#010101'
BLUE, GREEN, WHITE, ORANGE = '#698EFF', '#00ED95', '#FFFFFF', '#FF3A5D'
BLUE_D, GREEN_D, WHITE_D, ORANGE_D = '#2B3A70', '#00573A', '#5C5870', '#6E1A2B'

W, D, H = 78, 55, 32                      # the box's half width, half depth, height (fickle_screen.cc kW, kD, kH)
# the knobs and footswitches: u, v, radius, height(s), colour, dark (fickle_screen.cc kKnob)
KNOBS = [(-54, 34, 9, 8, BLUE, BLUE_D), (-22, 34, 9, 8, GREEN, GREEN_D), (16, 32, 13, 10, WHITE, WHITE_D),
         (54, 34, 9, 8, ORANGE, ORANGE_D)]
FOOT = [(-38, -38, 8, ORANGE, ORANGE_D), (36, -38, 8, WHITE, WHITE_D)]
FOOT_UP, FOOT_DOWN = 7, 2.5
RING_U, RING_V, RING_R = 36, 2, 18
WET_U, WET_V = -38, 2


def num(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


def stroke(colour, width=1.5):
    return ('fill="none" stroke="%s" stroke-width="%s" stroke-linecap="square" stroke-linejoin="miter"'
            % (colour, num(width)))


def attrs(id_, hidden):
    return (' id="%s"' % id_ if id_ else '') + (' display="none"' if hidden else '')


def line(x1, y1, x2, y2, colour, id_=None, hidden=False):
    return '<line%s %s x1="%s" y1="%s" x2="%s" y2="%s"></line>' % (attrs(id_, hidden), stroke(colour), num(x1), num(y1),
                                                                     num(x2), num(y2))


def polyline(points, colour, id_=None, hidden=False):
    return '<polyline%s %s points="%s"></polyline>' % (attrs(id_, hidden), stroke(colour),
                                                         ' '.join('%s,%s' % (num(x), num(y)) for x, y in points))


def circle(cx, cy, r, colour, fill='none', id_=None, hidden=False, width=1.5):
    return '<circle%s fill="%s" stroke="%s" stroke-width="%s" cx="%s" cy="%s" r="%s"></circle>' % (
        attrs(id_, hidden), fill, colour, num(width), num(cx), num(cy), num(r))


def dot(cx, cy, fill, id_=None):
    return '<circle%s fill="%s" stroke="none" cx="%s" cy="%s" r="2.5"></circle>' % (attrs(id_, False), fill, num(cx),
                                                                                     num(cy))


def rect(x, y, w, h, fill, colour, id_=None, hidden=False):
    return ('<rect%s fill="%s" stroke="%s" stroke-width="1.5" stroke-linejoin="miter" x="%s" y="%s" width="%s" '
            'height="%s"></rect>' % (attrs(id_, hidden), fill, colour, num(x), num(y), num(w), num(h)))


def group(id_, inner, hidden=False):
    return '<g id="%s"%s>%s</g>' % (id_, ' display="none"' if hidden else '', inner)


def build():
    e = ['<?xml version="1.0"?>',
         '<svg xmlns="http://www.w3.org/2000/svg" version="1.1" x="0px" y="0px" width="320px" height="160px" '
         'viewBox="0 0 320 160">']
    # the faces: front (x = u, y = z), side (x = v, y = z), top (x = u, y = v)
    e.append(group('front', rect(-W, 0, 2 * W, H, BLACK, PENCIL) +
                   ''.join(line(x, 8, x, 22, BODY) for x in (-12, -6, 0, 6, 12))))
    e.append(group('side', rect(-D, 0, 2 * D, H, BLACK, PENCIL)))
    top = [rect(-W, -D, 2 * W, 2 * D, BLACK, PENCIL)]
    top.append(line(0, -50, 0, -8, BODY) + line(0, 12, 0, 18, BODY))              # the seam: wet half | loop half
    for u, v, r, h, col, dark in KNOBS:
        top.append(circle(u, v, r, dark))                                          # the knobs' base rings
    for u, v, r, col, dark in FOOT:
        top.append(circle(u, v, r + 3, BODY))                                      # the footswitch nuts
    top.append(circle(0, 2, 4, GREEN_D))                                           # the routing switch's ring
    top.append(circle(-38, -23, 2.5, ORANGE_D, BLACK, 'ledf'))                     # freeze LED
    top.append(circle(36, -23, 2.5, WHITE_D, BLACK, 'ledl'))                       # loop LED
    top.append(circle(RING_U, RING_V, RING_R, WHITE_D, id_='ring'))                # the loop ring
    top.append(circle(RING_U, RING_V, 9, GREEN, id_='dubring', hidden=True, width=2))
    # the loop-mode glyphs in the ring's centre (white): tape reels, env spikes, stretch strokes
    for i, ox in enumerate((-4.5, 4.5)):
        top.append(circle(RING_U + ox, RING_V, 3, WHITE, id_='reel%d' % (i + 1)))
    for id_, k in (('envs', 4), ('envh', 7)):
        top.append(polyline([(29, 0), (33, 0), (34.5, k), (36, -k * 0.6), (37.5, 2), (43, 0)], WHITE, id_, True))
    for id_, half in (('strs', 4), ('strl', 7)):
        top.append(group(id_, ''.join(line(RING_U - half, 6 - 4 * i, RING_U + half, 6 - 4 * i, WHITE)
                                      for i in range(3)), hidden=True))
    e.append(group('top', ''.join(top)))
    # the knobs' and footswitches' sides (x across the silhouette, 0 .. 2r; y = z, 0 .. h), back row first
    for i, (u, v, r, h, col, dark) in enumerate(KNOBS):
        e.append(group('s%d' % i, rect(0, 0, 2 * r, h, BLACK, dark)))
    for i, (u, v, r, col, dark) in enumerate(FOOT):
        e.append(group('s%du' % (i + 4), rect(0, 0, 2 * r, FOOT_UP, BLACK, dark)))
        e.append(group('s%dd' % (i + 4), rect(0, 0, 2 * r, FOOT_DOWN, BLACK, dark), hidden=True))
    # the tops (a circle at the plane's origin)
    for i, (u, v, r, h, col, dark) in enumerate(KNOBS):
        e.append(group('k%d' % i, circle(0, 0, r, col, BLACK)))
    for i, (u, v, r, col, dark) in enumerate(FOOT):
        e.append(group('k%d' % (i + 4), circle(0, 0, r, col, BLACK)))
    # dots in screen space
    e.append(group('head', dot(0, 0, PENCIL, 'headc')))
    e.append(group('tip', dot(0, 0, GREEN)))
    for i in range(7):
        e.append(group('wd%d' % i, dot(0, 0, PENCIL, 'wdc%d' % i)))
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
    for bad in ('<text', '<path', '<polygon', '<style', 'url(', '/>', 'transform', 'opacity'):
        if bad in svg:
            raise ValueError('forbidden: %r' % bad)
    for pts in re.findall(r'points="([^"]*)"', svg):
        p = [tuple(map(float, q.split(','))) for q in pts.split()]
        for a, b, c in zip(p, p[1:], p[2:]):
            ux, uy, vx, vy = b[0] - a[0], b[1] - a[1], c[0] - b[0], c[1] - b[1]
            lu, lv = math.hypot(ux, uy), math.hypot(vx, vy)
            if lu == 0 or lv == 0 or (ux * vx + uy * vy) / (lu * lv) < -0.985:
                raise ValueError('polyline turns back at %r' % (b,))
    if len(svg) > 8192:
        raise ValueError('%d bytes: over the ~8 KiB of art (guidelines §5.3)' % len(svg))
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
