#!/usr/bin/env python3
"""tape_echo's screen (e9): te-op1-cfw's ports/knl/tape_echo/gen_tape_echo_knl_svg.py (the canvas "Tape echo screen",
design B "Schematic", 2026-10-28) made lean for coop and moved to coop's conventions.

    python3 gen_tape_echo_svg.py tape_echo.svg

The drawing is kennel's: the Space Echo's signal path, left to right on the tape row (y 56). The input arrow, the
record head, the tape (white: a wave the screen scales by age and breathes with the wow), the three play heads (white,
lit per pattern) tapping down to a bus, the sum, the output arrow. The return path runs under it with the green
feedback chevrons (a count set). A blue dot travels the tape at the tape speed, and the heads spread away from the
record head with the time (the screen translates them). Hold (green click): the input dims, the return path and the
hold glyph (top right) turn green. The blue input-gain gauge at the far left, the orange mix gauge at the far right.
What changed (README "Screen"):
- The readout glyphs are gone (kennel's division names dv0..dv10, head patterns pt0..pt3 and the page-2 gain in dB,
  about 16 KB of the 26.6 KB file): the core shows a control's name and value top left when it is turned or clicked,
  and the lit heads already show the pattern.
- The beat ticks (kennel's bt1..bt24, blue, while shift was held on the time page) are drawn by the screen code with
  raster lines (coop_scr_line), in green: sync is fn + green now.
- The white page-2 bracket round the heads (wbox) is gone: the head pattern is a white click, no held page.
- Kennel's polygons (the heads) are closed polylines with a rect inside for the fill (lit white, or the near-black
  occluder): lines, polylines, rects and circles only, as the other coop ports.
- Colours: coop's brights (blue 698EFF, green 00ED95, white FFFFFF, orange FF3A5D); the fixed path in the pencil
  DFD9FF, the bus, rails and foot in the body tone 38354D; unlit heads in white's dark tone, the hold glyph at rest in
  green's.

Rules (guide/plugin-guidelines.md §5.3, tools/plugin/pack.py): ids letters and digits, at most 2 decimals, lines,
polylines, rects and circles only, no polyline turning back, no self-closing tags. Stdlib only.
"""
import math
import re
import sys

PENCIL, DIM, BLUE, GREEN, DKGREEN, WHITE, OFF, ORANGE, BLACK = (
    '#DFD9FF', '#38354D', '#698EFF', '#00ED95', '#00573A', '#FFFFFF', '#5C5870', '#FF3A5D', '#010101')

TOP, BOTTOM, CY = 28, 124, 76              # the gauges' rows
GAIN_X, MIX_X = 16, 304
GAIN_BAR = 24
Y = 56                                     # the tape row
REC, SUM = 68, 258                         # the record head, the summing node (tape_echo_screen.cc kRec)
SPAN0 = 97                                 # head 3's distance from the record head as drawn (kSpan0Q10)
WAVE_AMP, WAVE_STEP, WAVE_PERIOD = 4, 3, 18
BUS_Y, RET_Y = Y + 22, 104
CHEV_X, CHEV_Y = 190, RET_Y
HOLD_X, HOLD_Y = 290, 14


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


def gauge(x):
    return line(x, TOP, x, BOTTOM, DIM) + line(x - 4, TOP, x + 4, TOP, DIM) + line(x - 4, BOTTOM, x + 4, BOTTOM, DIM)


def head(x, y, colour, fill, line_id=None, fill_id=None):
    """Kennel's head (a trapezoid, 10 wide at the tape, 6 at the tip): a closed polyline and a rect for its fill."""
    return (polyline([(x - 5, y), (x + 5, y), (x + 3, y + 6), (x - 3, y + 6), (x - 5, y)], colour, line_id) +
            rect(x - 2.5, y + 0.75, 5, 4.5, fill, id_=fill_id))


def lemniscate(cx, cy, a, steps=24):
    pts = []
    for i in range(steps + 1):
        t = 2 * math.pi * i / steps + 0.4
        d = 1 + math.sin(t) ** 2
        pts.append((cx + a * math.cos(t) / d, cy + a * 0.6 * math.sin(t) * math.cos(t) / d))
    return pts


def build():
    e = ['<?xml version="1.0"?>',
         '<svg xmlns="http://www.w3.org/2000/svg" version="1.1" x="0px" y="0px" width="320px" height="160px" '
         'viewBox="0 0 320 160">']
    e.append(group('palette', ''.join(line(0, 0, 1, 0, c, i) for i, c in (
        ('palwhite', WHITE), ('paloff', OFF), ('palpencil', PENCIL), ('paldim', DIM), ('palgreen', GREEN),
        ('paldkgreen', DKGREEN), ('palblack', BLACK))), hidden=True))
    # Fixed: the bus, its rise to the sum, the sum, the output, the tape's stub into the sum, the foot line.
    e.append(line(REC + 14, BUS_Y, SUM, BUS_Y, DIM) + line(SUM, BUS_Y, SUM, Y + 6, DIM))
    e.append(circle(SUM, Y, 6, PENCIL) + line(SUM - 3, Y, SUM + 3, Y, PENCIL) + line(SUM, Y - 3, SUM, Y + 3, PENCIL))
    e.append(line(SUM + 6, Y, 280, Y, PENCIL) + polyline([(276, Y - 3), (280, Y), (276, Y + 3)], PENCIL) +
             line(SUM - 14, Y, SUM - 6, Y, PENCIL))
    e.append(line(44, BOTTOM, 280, BOTTOM, DIM))
    # The input (dimmed while holding) and the record head.
    e.append(line(40, Y, REC - 8, Y, PENCIL, 'inl'))
    e.append(polyline([(REC - 12, Y - 3), (REC - 8, Y), (REC - 12, Y + 3)], PENCIL, 'ina'))
    e.append(head(REC, Y + 3, PENCIL, BLACK, 'rech'))
    # The tape: a wave at full amplitude (the screen scales it by age about y 56).
    pts, x = [], REC
    while x <= SUM - 14:
        pts.append((x, Y - WAVE_AMP * math.sin((x - REC) * 2 * math.pi / WAVE_PERIOD)))
        x += WAVE_STEP
    e.append(polyline(pts, WHITE, 'tape'))
    # The play heads at time 50 %, each with its tap to the bus; the file ships in pattern 0 (head 3 lit).
    for h in range(3):
        x = REC + SPAN0 * (h + 1) / 3
        on = h == 2
        e.append(group('hd%d' % (h + 1), head(x, Y + 3, WHITE if on else OFF, WHITE if on else BLACK,
                                              'hp%d' % (h + 1), 'hf%d' % (h + 1)) +
                       line(x, Y + 9, x, BUS_Y, WHITE if on else OFF, 'tp%d' % (h + 1))))
    # The signal on the tape: a dot the screen moves from the record head to head 3.
    e.append(group('dot', circle(REC, Y, 2.5, 'none', BLUE)))
    # The return path (green while holding) and the feedback chevrons, a count set.
    e.append(line(SUM, Y + 6, SUM, RET_Y, PENCIL, 'ret1'))
    e.append(line(SUM, RET_Y, REC, RET_Y, PENCIL, 'ret2'))
    e.append(line(REC, RET_Y, REC, Y + 9, PENCIL, 'ret3'))
    for i in range(4):
        x = CHEV_X - 12 * i
        e.append(polyline([(x + 4, CHEV_Y - 4), (x, CHEV_Y), (x + 4, CHEV_Y + 4)], GREEN, 'fb%d' % (i + 1), hidden=i >= 2))
    # The hold glyph, top right: green's dark tone at rest, green while holding.
    e.append(polyline(lemniscate(HOLD_X, HOLD_Y, 7), DKGREEN, 'hold'))
    # Gain: the rails, the centre mark and the bar (drawn at +6 dB, scaled -1..1 about the centre); the bracket (fn).
    e.append(gauge(GAIN_X))
    e.append(line(GAIN_X - 4, CY, GAIN_X + 4, CY, BLUE, 'gzero'))
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
