#!/usr/bin/env python3
"""Draw fickle's screen frames to PNG for the owner (tests/host/fickle "screens").

    python3 render_frames.py fickle.svg frames.txt OUTDIR

frames.txt comes from the host build with screen_rec.cc (fickle_test frames): for each frame, the matrix, visibility and
colours the screen gave every node it looked up, and the raster lines it drew over the SVG. This draws fickle.svg (the
subset gen_fickle_svg.py writes: groups, lines, polylines, rects, circles) with them, in document order, strokes 1.5
units wide whatever the matrix, then the raster lines, at 3 pixels a unit. It is a picture of the drawing, not of R246's
renderer: no anti-aliasing, square ends. Stdlib only, Python 3.8.
"""
import math
import os
import re
import struct
import sys
import zlib

SCALE = 3
WIDTH, HEIGHT = 320 * SCALE, 160 * SCALE


def parse_svg(text):
    """-> a list of (tag, attrs, groups) in document order; groups = the enclosing group ids, outermost first."""
    out, stack = [], []
    for m in re.finditer(r'<(/?)(\w+)([^>]*)>', text):
        close, tag, rest = m.group(1), m.group(2), m.group(3)
        attrs = dict(re.findall(r'([\w-]+)="([^"]*)"', rest))
        if tag == 'g':
            if close:
                stack.pop()
            else:
                stack.append((attrs.get('id', ''), attrs.get('display') == 'none'))
        elif not close and tag in ('line', 'polyline', 'rect', 'circle'):
            out.append((tag, attrs, list(stack)))
    return out


def parse_frames(text):
    frames, cur = [], None
    for ln in text.splitlines():
        p = ln.split()
        if not p:
            continue
        if p[0] == 'frame':
            cur = {'name': p[1], 'nodes': {}, 'lines': []}
            frames.append(cur)
        elif p[0] == 'node':
            v = list(map(int, p[2:]))
            cur['nodes'][p[1]] = {'set': v[0], 'm': [x / 1024.0 for x in v[1:7]], 'vis': v[7],
                                  'line': tuple(v[8:12]), 'fill': tuple(v[12:16])}
        elif p[0] == 'line':
            v = list(map(int, p[1:]))
            cur['lines'].append(([v[0] / 1024.0, v[1] / 1024.0], [v[2] / 1024.0, v[3] / 1024.0], v[4] / 1024.0,
                                 tuple(v[5:8])))
    return frames


def hex_rgb(s):
    if not s or s == 'none':
        return None
    s = s.lstrip('#')
    return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))


class Canvas:
    def __init__(self):
        self.px = bytearray(WIDTH * HEIGHT * 3)

    def span(self, y, x0, x1, c):
        if y < 0 or y >= HEIGHT:
            return
        x0, x1 = max(0, int(math.ceil(x0))), min(WIDTH - 1, int(math.floor(x1)))
        if x1 < x0:
            return
        row = (y * WIDTH) * 3
        self.px[row + 3 * x0:row + 3 * (x1 + 1)] = bytes(c) * (x1 - x0 + 1)

    def polygon(self, pts, c):
        """Even-odd scanline fill; pts in pixels."""
        ys = [p[1] for p in pts]
        n = len(pts)
        for y in range(max(0, int(min(ys))), min(HEIGHT - 1, int(max(ys)) + 1) + 1):
            yc = y + 0.5
            xs = []
            for i in range(n):
                (xa, ya), (xb, yb) = pts[i], pts[(i + 1) % n]
                if (ya <= yc < yb) or (yb <= yc < ya):
                    xs.append(xa + (yc - ya) * (xb - xa) / (yb - ya))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                self.span(y, xs[i] - 0.5, xs[i + 1] - 0.5, c)

    def segment(self, a, b, w, c):
        """A stroke of width w pixels from a to b, square ends: a rectangle polygon."""
        dx, dy = b[0] - a[0], b[1] - a[1]
        ln = math.hypot(dx, dy)
        if ln < 1e-6:
            dx, dy, ln = 1.0, 0.0, 1.0
        ux, uy = dx / ln * w / 2, dy / ln * w / 2
        nx, ny = -uy, ux
        self.polygon([(a[0] - ux + nx, a[1] - uy + ny), (b[0] + ux + nx, b[1] + uy + ny),
                      (b[0] + ux - nx, b[1] + uy - ny), (a[0] - ux - nx, a[1] - uy - ny)], c)

    def png(self, path):
        raw = b''.join(b'\x00' + bytes(self.px[y * WIDTH * 3:(y + 1) * WIDTH * 3]) for y in range(HEIGHT))
        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
        with open(path, 'wb') as f:
            f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', WIDTH, HEIGHT, 8, 2, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def draw(elems, frame, path):
    cv = Canvas()
    nodes = frame['nodes']
    for tag, a, groups in elems:
        # visibility: the element and every group round it, as the screen last set them (else the SVG's display)
        hidden = a.get('display') == 'none'
        if a.get('id') in nodes and nodes[a['id']]['vis'] >= 0:
            hidden = nodes[a['id']]['vis'] == 0
        for gid, ghid in groups:
            gh = ghid
            if gid in nodes and nodes[gid]['vis'] >= 0:
                gh = nodes[gid]['vis'] == 0
            hidden = hidden or gh
        if hidden:
            continue
        m = [1, 0, 0, 1, 0, 0]
        for gid, _ in groups:                         # the screen sets one matrix, on the outermost group it moves
            if gid in nodes and nodes[gid]['set']:
                m = nodes[gid]['m']
                break
        def T(x, y):
            return ((m[0] * x + m[2] * y + m[4]) * SCALE, (m[1] * x + m[3] * y + m[5]) * SCALE)
        stroke, fill = hex_rgb(a.get('stroke')), hex_rgb(a.get('fill'))
        nd = nodes.get(a.get('id'))
        if nd and nd['line'][3]:
            stroke = nd['line'][:3]
        if nd and nd['fill'][3]:
            fill = nd['fill'][:3]
        w = float(a.get('stroke-width', '1.5')) * SCALE
        f = lambda k: float(a.get(k, '0'))
        if tag == 'line':
            pts = [T(f('x1'), f('y1')), T(f('x2'), f('y2'))]
            closed = False
        elif tag == 'polyline':
            pts = [T(*map(float, q.split(','))) for q in a['points'].split()]
            closed = False
        elif tag == 'rect':
            x, y, ww, hh = f('x'), f('y'), f('width'), f('height')
            pts = [T(x, y), T(x + ww, y), T(x + ww, y + hh), T(x, y + hh)]
            closed = True
        else:
            cx, cy, r = f('cx'), f('cy'), f('r')
            pts = [T(cx + r * math.cos(2 * math.pi * i / 48), cy + r * math.sin(2 * math.pi * i / 48)) for i in range(48)]
            closed = True
        if closed and fill:
            cv.polygon(pts, fill)
        if stroke:
            segs = list(zip(pts, pts[1:] + (pts[:1] if closed else [])))
            for p0, p1 in segs:
                cv.segment(p0, p1, w, stroke)
    for p0, p1, w, c in frame['lines']:
        cv.segment((p0[0] * SCALE, p0[1] * SCALE), (p1[0] * SCALE, p1[1] * SCALE), w * SCALE, c)
    cv.png(path)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    elems = parse_svg(open(sys.argv[1], encoding='utf-8').read())
    frames = parse_frames(open(sys.argv[2], encoding='utf-8').read())
    os.makedirs(sys.argv[3], exist_ok=True)
    for fr in frames:
        path = os.path.join(sys.argv[3], fr['name'] + '.png')
        draw(elems, fr, path)
        print(path)


if __name__ == '__main__':
    main()
