#!/usr/bin/env python3
"""petri's screen (e9): a stompbox in 3/4 view, drawn as flat faces that petri_screen.cc moves with one matrix each.

    python3 gen_petri_svg.py petri.svg

The design is the lead's mock-up ("petri screen", Petri.dc.html): an orthographic box 190 x 100 x 30 units, yawed a
little and swaying, seen from above; it tips forward while fn is held. Every part here is FLAT art in its own local
coordinates; the screen code gives each top-level group one affine matrix a frame (a face's origin and its two unit
axes through the projection), so no group transform is nested in another.

  ffront  the front face (u across, z up: local y is z): the 11 program LEDs (pl0..pl10) at z 21, the filter and space
          tracks at z 9 (their white / orange fills are raster lines from the code)
  fside   the right side face (v across, z up)
  ftop    the top face (u, v): the dish's base ring, the knobs' base rings in their encoders' dark tones, the footswitch
          nuts, the reverse and hold LEDs (lrev, lhold)
  flid    the dish's lid, 4 units up: its rim (lidr: pencil, orange while held), the guide rings r 31 and r 15
  ks0..5  the knobs' and footswitches' sides (a rect: the cylinder's silhouette between its two tangent points)
  kt0..5  their tops (a circle in the encoder's colour); knobs blue time, green activity, white repeats, orange mix,
          then the reverse (blue) and hold (orange) footswitches
  vd0..7  the voices: a dot each (vc0..7), placed and tinted by the code

Colours: coop's brights (blue 698EFF, green 00ED95, white FFFFFF, orange FF3A5D), their dark tones (2B3A70, 00573A,
5C5870, 6E1A2B), the body tone 38354D, the pencil DFD9FF, the occluder 010101.

Rules (guide/plugin-guidelines.md section 5.3, tools/plugin/pack.py): ids letters and digits, at most 2 decimals,
lines, rects and circles only, no self-closing tags, strokes 1.5. Stdlib only, Python 3.8.
"""
import re
import sys

PENCIL, BODY, BLACK = '#DFD9FF', '#38354D', '#010101'
BLUE, GREEN, WHITE, ORANGE = '#698EFF', '#00ED95', '#FFFFFF', '#FF3A5D'
BLUE_D, GREEN_D, WHITE_D, ORANGE_D = '#2B3A70', '#00573A', '#5C5870', '#6E1A2B'

W, D, H = 95, 50, 30                       # the box's half-width, half-depth, height (petri_screen.cc kW, kD, kH)
DISH_U, DISH_V, DISH_R = -45, 5, 36
# u, v, radius, height, bright, dark (petri_screen.cc kKnobs: the same order)
KNOBS = [(20, 30, 10, 8, BLUE, BLUE_D), (58, 30, 10, 8, GREEN, GREEN_D), (20, 2, 10, 8, WHITE, WHITE_D),
         (58, 2, 10, 8, ORANGE, ORANGE_D), (20, -34, 7, 7, BLUE, BLUE_D), (58, -34, 7, 7, ORANGE, ORANGE_D)]


def num(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


def ident(i):
    return ' id="%s"' % i if i else ''


def line(x1, y1, x2, y2, colour, id_=None):
    return ('<line%s fill="none" stroke="%s" stroke-width="1.5" stroke-linecap="square" x1="%s" y1="%s" x2="%s" '
            'y2="%s"></line>' % (ident(id_), colour, num(x1), num(y1), num(x2), num(y2)))


def circle(cx, cy, r, stroke, fill='none', id_=None):
    return ('<circle%s fill="%s" stroke="%s" stroke-width="1.5" cx="%s" cy="%s" r="%s"></circle>'
            % (ident(id_), fill, stroke, num(cx), num(cy), num(r)))


def dot(cx, cy, fill, id_):
    return '<circle id="%s" fill="%s" stroke="none" cx="%s" cy="%s" r="2.5"></circle>' % (id_, fill, num(cx), num(cy))


def rect(x, y, w, h, stroke, fill=BLACK, id_=None):
    return ('<rect%s fill="%s" stroke="%s" stroke-width="1.5" x="%s" y="%s" width="%s" height="%s"></rect>'
            % (ident(id_), fill, stroke, num(x), num(y), num(w), num(h)))


def group(id_, inner):
    return '<g id="%s">%s</g>' % (id_, inner)


def build():
    e = ['<?xml version="1.0"?>',
         '<svg xmlns="http://www.w3.org/2000/svg" version="1.1" x="0px" y="0px" width="320px" height="160px" '
         'viewBox="0 0 320 160">']
    # the front: the face, the program LEDs (the first lit), the filter and space tracks
    inner = rect(-W, 0, 2 * W, H, PENCIL)
    for k in range(11):
        on = k == 0
        inner += circle(-60 + 12 * k, 21, 2.5, BLUE if on else BODY, BLUE if on else BLACK, 'pl%d' % k)
    inner += line(-60, 9, -6, 9, BODY) + line(6, 9, 60, 9, BODY)
    e.append(group('ffront', inner))
    e.append(group('fside', rect(-D, 0, 2 * D, H, PENCIL)))
    # the top: the face, the dish's base ring, the knobs' base rings, the footswitch nuts, the two LEDs
    inner = rect(-W, -D, 2 * W, 2 * D, PENCIL) + circle(DISH_U, DISH_V, DISH_R, BODY)
    for u, v, r, h, bright, dark in KNOBS:
        inner += circle(u, v, r + 3, BODY) if r < 10 else circle(u, v, r, dark)
    inner += circle(36, -34, 2.5, BLUE_D, BLACK, 'lrev') + circle(74, -34, 2.5, ORANGE_D, BLACK, 'lhold')
    e.append(group('ftop', inner))
    # the lid
    e.append(group('flid', circle(DISH_U, DISH_V, DISH_R, PENCIL, BLACK, 'lidr') + circle(DISH_U, DISH_V, 31, BODY) +
                   circle(DISH_U, DISH_V, 15, BODY)))
    # the knobs and footswitches, back to front: a side (local x across the tangent, y up), a top (u, v)
    for k in sorted(range(len(KNOBS)), key=lambda i: -KNOBS[i][1]):
        u, v, r, h, bright, dark = KNOBS[k]
        e.append(group('ks%d' % k, rect(-r, 0, 2 * r, h, dark)))
        e.append(group('kt%d' % k, circle(0, 0, r, bright, BLACK)))
    # the voices
    for k in range(8):
        e.append(group('vd%d' % k, dot(0, 0, PENCIL, 'vc%d' % k)))
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
    for bad in ('<text', '<path', '<polygon', '<polyline', '<style', 'url(', '/>', 'transform='):
        if bad in svg:
            raise ValueError('forbidden: %r' % bad)
    if len(svg.encode()) > 8192:
        raise ValueError('over 8 KiB: %d bytes' % len(svg.encode()))
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
