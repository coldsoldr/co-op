#!/usr/bin/env python3
"""The rings kennel plugin's screen (design: the Claude Design canvas
"Rings kennel screen", 2026-10-27; driven by rings_screen.cc).

    python3 -I gen_rings_svg.py rings.svg   (coop: plugins/rings; the te-op1-cfw kennel original, unchanged)

Six concentric rings about (100, 68), lit from the core out by structure;
eight rays off the outer ring's upper left (brightness); a white pad on the
lower right that slides in over the rings (damping); the orange strike dot
on the radius to the right (position); six ripple circles, one per voice,
scaled from the strike outwards and faded with the voice; the core's glow.
At the right the MODEL and POLY rows: a label (stroked glyphs: the OP-1's
parser has no text), six dots, six ticks, and a bracket each for the fn layer.
Composition sits 8 px left and 12 px up of centre for the viewing angle;
the rings fill the height (rays from y 9 to the pad's foot at y 134).

Rules enforced (docs/HANDOVER.md "SVG lessons", hundekraft's svg_check):
ids ASCII letters and digits only; at most 2 decimals; no arcs, no text,
no CSS; one M per path (there are no paths: circles, lines, rects and
polylines only); polylines never turn back on themselves. Stdlib only.
"""
import math
import re
import sys

CX, CY = 104, 72
RING_RADII = [9, 18, 27, 36, 45, 54]
RIPPLE_R = 30                 # drawn mid-way; the screen scales it 4..58 px
RAY_ANGLES = [120, 150, 180, 210, 240, 270, 300, 330]
RAY_R0, RAY_R1 = 58, 63
PAD_R = 14
PAD_DIST = 34                 # pad centre from C at damping 0 (deep in), along 45 degrees;
                              # the screen moves it 25.5 px out as damping rises (a clockwise
                              # turn lifts the damper off: in Rings more damping = a longer ring)
POS_X0 = 110                  # strike dot at position 0 (the screen moves it 48 px)

WHITE, DIM, BLUE, GREEN, DIMGREEN, GREY, BLACK, ORANGE, GUIDE = (
    '#DED7FF', '#38354D', '#638BFF', '#00EE92', '#1B3D33', '#514959', '#010101', '#FF395A', '#2E2C5C')

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # coop: knl_glyphs.py beside it
from knl_glyphs import GLYPHS   # noqa: E402  (the shared 5 x 7 stroke font)

GLYPH_SCALE = 1.4            # the 5 x 7 cell drawn 7 x 9.8 (round 3: larger rows)
ADVANCE = 10

MODEL_LABELS = ['MODAL', 'SYMP', 'STRING', 'FM', 'QUANT', 'VERB']
POLY_LABELS = ['ALL', '1', '2', '4']
LABEL_X, MODEL_LABEL_Y, POLY_LABEL_Y = 224, 33, 80     # top of the cell
DOT_Y, DOT_X0, DOT_DX, DOT_R = 54, 229, 14, 3.5
TICK_Y0, TICK_Y1, TICK_X0, TICK_DX = 96, 108, 229, 10


def num(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


def stroke(colour, extra=''):
    return 'fill="none" stroke="%s" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"%s' % (colour, extra)


def line(x1, y1, x2, y2, colour, id_=None):
    i = ' id="%s"' % id_ if id_ else ''
    return '<line%s %s x1="%s" y1="%s" x2="%s" y2="%s"></line>' % (i, stroke(colour), num(x1), num(y1), num(x2), num(y2))


def circle(cx, cy, r, stroke_colour, fill='none', id_=None):
    i = ' id="%s"' % id_ if id_ else ''
    s = 'none' if stroke_colour is None else stroke_colour
    return ('<circle%s fill="%s" stroke="%s" stroke-width="1.5" cx="%s" cy="%s" r="%s"></circle>'
            % (i, fill, s, num(cx), num(cy), num(r)))


def polyline(points, colour):
    return '<polyline %s points="%s"></polyline>' % (stroke(colour), ' '.join('%s,%s' % (num(x), num(y)) for x, y in points))


def label(text, x, y, colour):
    out = []
    for ch in text:
        for pl in GLYPHS[ch]:
            out.append(polyline([(x + px * GLYPH_SCALE, y + py * GLYPH_SCALE) for px, py in pl], colour))
        x += ADVANCE
    return ''.join(out)


def build():
    e = ['<?xml version="1.0"?>',
         '<svg xmlns="http://www.w3.org/2000/svg" version="1.1" x="0px" y="0px" width="320px" height="160px" viewBox="0 0 320 160">']
    # Colours the screen reads once (hidden): white, dim, blue, green, dim green, grey, black.
    e.append('<g id="palette" display="none">')
    for name, colour in (('palwhite', WHITE), ('paldim', DIM), ('palblue', BLUE), ('palgreen', GREEN),
                         ('paldimgreen', DIMGREEN), ('palgrey', GREY), ('palblack', BLACK)):
        e.append(line(0, 0, 1, 0, colour, name))
    e.append('</g>')
    e.append(line(POS_X0, CY, CX + RING_RADII[-1], CY, GUIDE))                       # the strike radius
    # Outer ring first: the core draws on top.
    for i in range(5, -1, -1):
        e.append(circle(CX, CY, RING_RADII[i], BLUE if i < 4 else DIM, id_='ring%d' % i))
    for v in range(6):                                                     # one ripple per voice, hidden
        e.append('<g id="rip%d" display="none">%s</g>' % (v, circle(CX, CY, RIPPLE_R, WHITE)))
    e.append(circle(CX, CY, 4, None, fill='#2B2733', id_='core'))          # the glow
    e.append(circle(CX, CY, 1.5, None, fill=BLUE, id_='coredot'))
    for i, a in enumerate(RAY_ANGLES):
        ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
        e.append(line(CX + RAY_R0 * ca, CY + RAY_R0 * sa, CX + RAY_R1 * ca, CY + RAY_R1 * sa,
                      GREEN if i < 5 else DIMGREEN, 'ray%d' % i))
    d = PAD_DIST * math.cos(math.radians(45))
    e.append('<g id="pad">%s</g>' % circle(CX + d, CY + d, PAD_R, WHITE, fill=BLACK))
    e.append('<g id="pos">%s</g>' % circle(POS_X0, CY, 3, ORANGE, fill=ORANGE))
    # MODEL row.
    e.append('<rect id="mbox" display="none" fill="none" stroke="%s" stroke-width="1.5" x="220" y="27" width="90" height="36"></rect>' % BLUE)
    for i, t in enumerate(MODEL_LABELS):
        e.append('<g id="mlabel%d"%s>%s</g>' % (i, '' if i == 0 else ' display="none"', label(t, LABEL_X, MODEL_LABEL_Y, BLUE)))
    for i in range(6):
        e.append(circle(DOT_X0 + DOT_DX * i, DOT_Y, DOT_R, BLUE if i == 0 else DIM, fill=BLUE if i == 0 else BLACK, id_='model%d' % i))
    # POLY row.
    e.append('<rect id="pbox" display="none" fill="none" stroke="%s" stroke-width="1.5" x="220" y="74" width="90" height="40"></rect>' % GREEN)
    for i, t in enumerate(POLY_LABELS):
        e.append('<g id="plabel%d"%s>%s</g>' % (i, '' if i == 0 else ' display="none"', label(t, LABEL_X, POLY_LABEL_Y, GREEN)))
    for i in range(6):
        x = TICK_X0 + TICK_DX * i
        e.append(line(x, TICK_Y0, x, TICK_Y1, GREEN, 'voice%d' % i))
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
    for bad in ('<text', '<path', '<style', ' A ', 'url('):
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
