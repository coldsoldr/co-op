#!/usr/bin/env python3
"""ensemble's screen (e9): te-op1-cfw's ports/knl/ensemble/gen_ensemble_knl_svg.py (the canvas "Ensemble screen",
design C "Drift", 2026-10-27) made lean for coop and moved to coop's conventions.

    python3 gen_ensemble_svg.py ensemble.svg

The drawing is kennel's: each voice a vertical white string, the L lane above and the R lane below, swaying about its
home mark with the LFO (the screen translates the strings every frame); green ticks either side of each home mark
show how far it can drift (the mark-and-ticks group is scaled about the mark by depth). At the right the blue sweep
dial (its dot circling at the rate) with the rate bar under it; the input-gain gauge at the far left (0 dB at its
centre mark, the bar up for +, down for -); the orange mix gauge at the far right. What changed (README "Screen"):
- The gain readout glyphs (kennel's page-2 "+1.5" in dB, 4.3 KB of the 11 KB file) are gone: the core shows the
  control's name and value when fn + blue turns it. The gain bar is blue's dark tone at rest and lights blue while fn
  is held (the held layer), with the bracket round it; the rate dot and bar dim then, as kennel's page 2 did.
- The R lane is always drawn (coop is stereo at every position; kennel hid it on the mono positions), so it is part
  of the still drawing and has no id.
- Colours: coop's brights (blue 698EFF, green 00ED95, white FFFFFF, orange FF3A5D); one body tone 38354D for the
  lanes, the dial ring, the home marks and the gauges' rails (kennel drew the ring and the marks in a second grey).
  The palette keeps only the three colours the code writes.

Rules (guide/plugin-guidelines.md §5.3, tools/plugin/pack.py): ids letters and digits, at most 2 decimals, lines,
polylines, rects and circles only, no polyline turning back. Stdlib only.
"""
import math
import re
import sys

DIM, BLUE, DIMBLUE, GREEN, WHITE, ORANGE = '#38354D', '#698EFF', '#2A3966', '#00ED95', '#FFFFFF', '#FF3A5D'

TOP, BOTTOM, CY = 28, 124, 76              # the gauges' rows (as the bitcrusher's)
GAIN_X, MIX_X = 16, 304
GAIN_BAR = 24                              # the gain bar's height at +6 dB (drawn, scaled -1..1)
X0, X1 = 44, 236                           # the lanes' span
LANES = ((30, 70), (86, 126))              # L, R: top and bottom of the strings
HOMES = (80, 140, 200)                     # the voices' home marks (ensemble_screen.cc kHomes)
SPAN = 28                                  # a voice's drift at full depth (units, ensemble_screen.cc kSpan)
DIAL_X, DIAL_Y, DIAL_R = 268, 66, 10       # ensemble_screen.cc kDialR
RATE_X0, RATE_X1, RATE_Y = 252, 284, 92    # ensemble_screen.cc kRateX0


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


def circle(cx, cy, r, colour, fill='none', id_=None):
    return '<circle%s fill="%s" stroke="%s" stroke-width="1.5" cx="%s" cy="%s" r="%s"></circle>' % (
        attrs(id_, False), fill, colour, num(cx), num(cy), num(r))


def rect(x, y, w, h, fill, stroke_colour='none', id_=None, hidden=False):
    return '<rect%s fill="%s" stroke="%s" stroke-width="1.5" x="%s" y="%s" width="%s" height="%s"></rect>' % (
        attrs(id_, hidden), fill, stroke_colour, num(x), num(y), num(w), num(h))


def group(id_, inner, hidden=False):
    return '<g id="%s"%s>%s</g>' % (id_, ' display="none"' if hidden else '', inner)


def rails(x):
    return line(x, TOP, x, BOTTOM, DIM) + line(x - 4, TOP, x + 4, TOP, DIM) + line(x - 4, BOTTOM, x + 4, BOTTOM, DIM)


def build():
    e = ['<?xml version="1.0"?>',
         '<svg xmlns="http://www.w3.org/2000/svg" version="1.1" x="0px" y="0px" width="320px" height="160px" '
         'viewBox="0 0 320 160">']
    e.append(group('palette', ''.join(line(0, 0, 1, 0, c, i) for i, c in (
        ('paldim', DIM), ('palblue', BLUE), ('paldimblue', DIMBLUE))), hidden=True))
    # The still drawing: both lanes' guides, the dial's ring, the rate bar's track, the gauges' rails.
    e.append(group('static', ''.join(line(X0, (y0 + y1) / 2, X1, (y0 + y1) / 2, DIM) for y0, y1 in LANES) +
                   circle(DIAL_X, DIAL_Y, DIAL_R, DIM) + line(RATE_X0, RATE_Y, RATE_X1, RATE_Y, DIM) +
                   rails(GAIN_X) + rails(MIX_X)))
    # Per lane and voice: the home mark with its depth ticks (one group, scaled about the mark), and the string
    # (translated). The third voice of each lane starts hidden (the count set; kennel's default voices shows two).
    for (y0, y1), tag in ((LANES[0], 'L'), (LANES[1], 'R')):
        cy = (y0 + y1) / 2
        for i, hx in enumerate(HOMES):
            marks = (line(hx, cy - 3, hx, cy + 3, DIM) + line(hx - SPAN, y0 - 2, hx - SPAN, y0 + 2, GREEN) +
                     line(hx + SPAN, y0 - 2, hx + SPAN, y0 + 2, GREEN))
            e.append(group('h%s%d' % (tag, i + 1), marks, hidden=i >= 2))
            e.append(group('v%s%d' % (tag, i + 1), line(hx, y0, hx, y1, WHITE), hidden=i >= 2))
    # Rate: the dot on the dial (its group translated round the ring) and the bar (scaled about its left end).
    e.append(group('sweep', circle(DIAL_X, DIAL_Y - DIAL_R, 2.5, 'none', BLUE, 'sdot')))
    e.append(rect(RATE_X0, RATE_Y - 1.5, RATE_X1 - RATE_X0, 3, BLUE, id_='rbar'))
    # Gain: the centre mark and the bar (drawn at +6 dB in blue's dark tone, scaled -1..1 about the centre); the
    # bracket while fn is held.
    e.append(line(GAIN_X - 4, CY, GAIN_X + 4, CY, BLUE, 'gzero'))
    e.append(rect(GAIN_X - 2, CY - GAIN_BAR, 4, GAIN_BAR, DIMBLUE, id_='gbar'))
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
    if len(svg.encode('utf-8')) > 16384:
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
