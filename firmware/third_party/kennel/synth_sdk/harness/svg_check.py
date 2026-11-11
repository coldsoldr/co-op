#!/usr/bin/env python3
"""Tier 1 check: no screen SVG has a number with more than 4 decimals (R246's SVG parser panics at boot on float
noise such as 1.6500000000000001). pack.py refuses such plugin files and kn_boot.c's svg_ok skips them, but SVGs
compiled into the core (make_firmware copies them to content/display) get no check on the device, so every SVG is
scanned here: synth_sdk/ (build output left out) and, when this checkout sits in the full project, its tools/engines and
tools/fx. Same rule as svg_ok: a '.' after a digit, then more than 4 digits.
Stroke-join constraint: R246's stroke join draws a polyline point where the line turns
back on itself (more than about 170 degrees) or repeats as a spike to the screen edge; oprender draws a clean join,
so the goldens can't show it. Same test as OP1-Emulator's docs/design/engine-screens/round5/check_svg.py (cosine
below -0.985 or a zero-length segment), on polyline / polygon points and on path data made of M / L / Z only
(curve control points are not vertices), each subpath (from one M to the next) on its own.
Third rule (Amp's voice box, 2026-10-29): R246's parser has only the path commands M L H V C S Z; anything else (Q, A,
T, an exponent) panics at boot (Parser.cpp:525). Same pattern as pack.py and kn_boot.c's svg_err.

    python3 synth_sdk/harness/svg_check.py [DIR ...]        (make -C synth_sdk/harness tier1)
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

SDK = Path(__file__).resolve().parents[1]
LONG = re.compile(rb'\d\.\d{5,}')                    # = pack.py's check, = kn_boot.c svg_err (SVG_MAX_DECIMALS 4)
CMD = re.compile(rb'\sd="[^"]*?([A-BD-GI-KN-RT-UW-Yabd-gi-kn-rt-uw-y])')   # a path command R246 lacks (= pack.py)


def roots():
    out = [SDK]
    for d in SDK.parents:                            # the project's own engines and effects, if this is its ext/
        if (d / 'tools/engines').is_dir() and (d / 'tools/fx').is_dir():
            out += [d / 'tools/engines', d / 'tools/fx']
            break
    return out


NUM = re.compile(r'-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')
TURN_COS = -0.985                                    # = round5/check_svg.py


def turn_backs(pts):
    """-> the first point where a run of vertices turns back or repeats, else None."""
    for a, b, c in zip(pts, pts[1:], pts[2:]):
        v1, v2 = (b[0] - a[0], b[1] - a[1]), (c[0] - b[0], c[1] - b[1])
        l1, l2 = (v1[0] ** 2 + v1[1] ** 2) ** 0.5, (v2[0] ** 2 + v2[1] ** 2) ** 0.5
        if l1 == 0 or l2 == 0 or (v1[0] * v2[0] + v1[1] * v2[1]) / (l1 * l2) < TURN_COS:
            return b
    return None


def vertex_runs(el):
    """-> lists of (x, y) vertices drawn as one stroke, for polyline / polygon / M-L-Z paths."""
    tag = el.tag.split('}')[-1]
    if tag in ('polyline', 'polygon'):
        n = [float(v) for v in NUM.findall(el.get('points', ''))]
        return [list(zip(n[0::2], n[1::2]))]
    if tag != 'path':
        return []
    d = el.get('d', '')
    if re.search(r'[^MLZmlz0-9.eE\s,\-+]', d):
        return []                                    # curves or H / V: not checked
    runs, cur, pos, start = [], [], (0.0, 0.0), (0.0, 0.0)
    for cmd, args in re.findall(r'([MLZmlz])([^MLZmlz]*)', d):
        n = [float(v) for v in NUM.findall(args)]
        if cmd in 'Zz':
            pos = start
            continue
        for i, (x, y) in enumerate(zip(n[0::2], n[1::2])):
            if cmd in 'ml':
                x, y = x + pos[0], y + pos[1]
            if cmd in 'Mm' and i == 0:
                if len(cur) > 2:
                    runs.append(cur)
                cur, start = [], (x, y)
            cur.append((x, y))
            pos = (x, y)
    if len(cur) > 2:
        runs.append(cur)
    return runs


def turn_backs_in(data):
    """-> ['<tag id=..> turns back or repeats at x,y', ...] for SVG text (bytes); pack.py calls this too"""
    try:
        root = ET.fromstring(data)
    except ET.ParseError as e:
        return [f'not parsed ({e})']
    out = []
    for el in root.iter():
        for run in vertex_runs(el):
            b = turn_backs(run)
            if b is not None:
                out.append(f'<{el.tag.split("}")[-1]} id={el.get("id", "-")}> turns back or repeats at {b[0]:g},{b[1]:g}')
    return out


def filled_subpaths_in(data):
    """Opt-in new generated-art contract; not part of the existing global scan.

    R246's filled-path walker uses MOVE as LINE after its first vertex. Keep one
    contour per filled path and paint holes separately. Stroke-only paths are
    outside this restriction. Native rendering still needs separate acceptance.
    """
    root = ET.fromstring(data)
    errors = []
    def walk(el, inherited='black'):
        fill = el.get('fill', inherited)
        style = dict(part.split(':', 1) for part in el.get('style', '').split(';') if ':' in part)
        fill = style.get('fill', fill).strip().lower()
        if fill == 'inherit':
            fill = inherited
        if el.tag.split('}')[-1] == 'path' and fill != 'none' and len(re.findall('[Mm]', el.get('d', ''))) > 1:
            errors.append(f'<path id={el.get("id", "-")}> has multiple MOVE commands with fill; R246 joins contours')
        for child in el:
            walk(child, fill)
    walk(root)
    return errors


def turn_scan(p):
    return [f'{p}: {x}' for x in turn_backs_in(Path(p).read_bytes())]


def scan(dirs):
    hits, turns, cmds, n = [], [], [], 0
    for root in dirs:
        for p in sorted(Path(root).rglob('*.svg')):
            if 'build' in p.relative_to(root).parts:
                continue
            n += 1
            data = p.read_bytes()
            for m in LONG.finditer(data):
                line = data.count(b'\n', 0, m.start()) + 1
                hits.append(f'{p}:{line}: {m.group().decode()}')
            for m in CMD.finditer(data):
                line = data.count(b'\n', 0, m.start()) + 1
                cmds.append(f'{p}:{line}: path command {m.group(1).decode()}')
            turns += turn_scan(p)
    return n, hits, turns, cmds


def main(argv):
    assert LONG.search(b'x="1.6500000000000001"') and not LONG.search(b'x="12.3456" y="10000.5"')
    assert turn_backs([(0, 0), (10, 0), (0, 0.5)]) == (10, 0) and turn_backs([(0, 0), (1, 1), (1, 1)]) == (1, 1)
    assert turn_backs([(0, 0), (10, 0), (10, 10), (0, 10)]) is None
    assert [len(r) for r in vertex_runs(ET.fromstring('<path d="M0 0 L5 0 L9 0 M1 1 l1 0 l1 1"/>'))] == [3, 3]
    assert vertex_runs(ET.fromstring('<path d="M0 0 C1 1 2 2 3 3"/>')) == []
    assert CMD.search(b' d="M1,2Q3,4,5,6"') and CMD.search(b' d="M0,0A1,1,0,1,0,2,2"') and not CMD.search(b' d="M0,0C1,1,2,2,3,3s1,1,2,2h1v1l1,1z"')
    dirs = [Path(a) for a in argv] or roots()
    n, hits, turns, cmds = scan(dirs)
    for h in hits:
        print(f'  FAIL  {h} (more than 4 decimals: round it)')
    for t in turns:
        print(f'  FAIL  {t} (R246 draws a spike there: drop the point)')
    for c in cmds:
        print(f'  FAIL  {c} (R246 has only M L H V C S Z: write it as C curves)')
    bad = [f'{len(hits)} numbers with more than 4 decimals'] * bool(hits) + [f'{len(turns)} turn-backs'] * bool(turns) + [f'{len(cmds)} unsupported path commands'] * bool(cmds)
    print(f'svg_check: {n} SVGs in {", ".join(str(d) for d in dirs)}: ' + ('; '.join(bad) or 'ok'))
    return 1 if hits or turns or cmds else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
