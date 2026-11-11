#!/usr/bin/env python3
"""coop's boot picture: the co-op logo and the build's version line on the OP-1's power-on screen.

Where the stock picture comes from (docs/screens.md "e10: the boot picture"; R246, read in the bytes and in op1emu):
- It is the app's, not te-boot's: te-boot's init DXE never touches the PPI or its DMA, and op1emu, which loads the
  app LDR without te-boot, shows the same TE logo and "R. 00246".
- The logo is a raw 240 x 320 RGB565 picture loaded by the app LDR at 0x02000000 (0x25800 bytes, plain SDRAM blocks:
  short data runs and zero FILLs). Early in the boot (0x01976474) the stock re-strides it in place to the display's
  256-pixel rows at 0x02000000 + 0x5000 (0x3800 or 0 on other panel variants), source row r -> display line r + 40.
  The PPI DMA shows 400 lines of 200 pixels; on the landscape screen (400 x 200 px, the SVG units x 1.25) a picture
  pixel (row r, column c) lands at x = 359 - r, y = c. Columns 200..239 are never shown.
- The version is the console string at 0x018F7358: 17 newlines, 33 spaces, "R. 00246". The display interrupt
  (0x01A2FB68) prints it once through the stock console (0x019416EA, an 8-pixel font, cells drawn black where a
  glyph bit is clear) while the boot flag byte 0x018B6460 (1 in the image) is set, and points the DMA at the picture.
  The picture stays until the GUI's first frame moves the DMA to 0x03000000 (op1emu: 0.80 s to 2.85 s guest time).

What this tool does to an app LDR (plaintext or encrypted: it touches no CALLBACK block):
- checks the stock picture (sha256 of the loaded 0x25800 bytes) and the stock console string first;
- appends, just before the FINAL block, a zero FILL over the picture and plain blocks with coop's picture (so every
  stock block keeps its file offset, as inject_app.py does), and
- rewrites the console string in place (same 59 bytes) to "coop <tag> - <stock version>", centred as the stock's.

The picture is drawn here, stdlib only: four filled circles (coop's encoder brights) and the "co-op" wordmark as the
website sets it (Instrument Sans SemiBold, letter spacing -0.025 em), as outline paths taken once from the font
(SIL Open Font License 1.1, (c) 2022 The Instrument Sans Project Authors). The build tag comes from
core/include/coop_build.h at build time.

Usage:
    python tools/boot_logo.py IN.ldr OUT.ldr [--tag e10]          # apply (tag default: coop_build.h)
    python tools/boot_logo.py --png OUT.png [--stock OP1_vdk.ldr] [--tag e10]
                                                                   # preview: the landscape screen at 400 x 200
"""

import argparse
import hashlib
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ldr_dump import LdrError, memory_runs, parse, serialize  # noqa: E402
from patch_app import apply_patch  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD_H = os.path.join(HERE, '..', 'core', 'include', 'coop_build.h')
STOCK_LDR = os.path.join(HERE, '..', '..', '..', 'te-op1-cfw', 'op1 firmware', 'op1_246', 'OP1_vdk.ldr')

PIC_ADDR = 0x02000000
PIC_ROWS, PIC_COLS = 320, 240                  # rows run along the landscape x axis, columns along y
PIC_SIZE = PIC_ROWS * PIC_COLS * 2
PIC_STOCK_SHA256 = '40d73c1bf62a5073f66037901e120ab5389e00bcd1420b26b4fff3fee90ccf2b'
SCREEN_W, SCREEN_H = 400, 200                  # landscape px as op1emu shows the frame
X_OF_ROW0 = 359                                # x = 359 - row (panel offset 0x5000; the others move both alike)

VERSION_ADDR = 0x018F7358
VERSION_LINES = 17
VERSION_STOCK = b'\n' * 17 + b' ' * 33 + b'R. 00246\0'
VERSION_SIZE = len(VERSION_STOCK)              # 59: the next byte is padding, then RGB.cpp's assert text
CONSOLE_X0 = 43                                # console x 0 is landscape x 43 (line 40 + 316 - x)
FONT_WIDTHS, FONT_GLYPHS = 0x01001478, 0x01001AC8   # stock console font: width bytes, glyph pointers (ASCII 32..126)

# Encoder colours: coop's screen brights (docs/plugins.md), which differ from the website's tokens.
DOTS = [(0x69, 0x8E, 0xFF), (0x00, 0xED, 0x95), (0xFF, 0xFF, 0xFF), (0xFF, 0x3A, 0x5D)]
INK = (0xFF, 0xFF, 0xFF)

# The website's .brand row in CSS px (web/src/index.css .logo, .brand h1), scaled by K to screen px.
K = 1.6
WEB_DOT, WEB_DOT_GAP, WEB_TEXT_GAP, WEB_FONT = 14.0, 4.0, 12.0, 20.0
CENTRE_X, CENTRE_Y = 200.0, 84.0               # the dots' centre line; the version line stays at console line 17

# "co-op" in Instrument Sans SemiBold, font units (1000 / em), y down, baseline 0, letter spacing applied.
WORDMARK_ADVANCE = 2760
WORDMARK = (
    'M288 10Q211 10 152 -24Q94 -58 60 -118Q27 -179 27 -256Q27 -333 60 -392Q94 -452 152 -486Q211 -520 288 -520'
    'Q351 -520 402 -496Q453 -473 486 -430Q519 -388 528 -332H407Q396 -373 365 -396Q334 -419 290 -419Q249 -419 220 -399'
    'Q190 -379 174 -342Q158 -305 158 -256Q158 -206 174 -169Q190 -132 220 -112Q250 -91 290 -91Q334 -91 365 -114'
    'Q396 -137 407 -178H528Q519 -122 486 -80Q452 -37 402 -14Q351 10 288 10Z '
    'M826 10Q745 10 684 -24Q622 -58 588 -118Q554 -179 554 -257Q554 -335 588 -394Q622 -453 684 -486Q745 -520 826 -520'
    'Q908 -520 969 -486Q1030 -453 1064 -394Q1098 -335 1098 -257Q1098 -179 1064 -118Q1029 -58 968 -24Q907 10 826 10Z'
    'M826 -91Q866 -91 898 -112Q930 -132 948 -170Q966 -207 966 -258Q966 -333 926 -376Q887 -419 826 -419'
    'Q765 -419 725 -376Q685 -333 685 -258Q685 -207 704 -170Q722 -132 754 -112Q785 -91 826 -91Z '
    'M1166 -219V-325H1525V-219Z '
    'M1866 10Q1785 10 1724 -24Q1662 -58 1628 -118Q1594 -179 1594 -257Q1594 -335 1628 -394Q1662 -453 1724 -486'
    'Q1785 -520 1866 -520Q1948 -520 2009 -486Q2070 -453 2104 -394Q2138 -335 2138 -257Q2138 -179 2104 -118'
    'Q2069 -58 2008 -24Q1947 10 1866 10ZM1866 -91Q1906 -91 1938 -112Q1970 -132 1988 -170Q2006 -207 2006 -258'
    'Q2006 -333 1966 -376Q1927 -419 1866 -419Q1805 -419 1765 -376Q1725 -333 1725 -258Q1725 -207 1744 -170'
    'Q1762 -132 1794 -112Q1825 -91 1866 -91Z '
    'M2497 10Q2424 10 2379 -25Q2347 -48 2332 -85V205H2206V-510H2329V-426Q2346 -463 2383 -487Q2433 -520 2505 -520'
    'Q2573 -520 2624 -488Q2674 -455 2702 -396Q2730 -337 2730 -257Q2730 -176 2701 -116Q2672 -56 2620 -23'
    'Q2568 10 2497 10ZM2465 -94Q2526 -94 2562 -136Q2598 -179 2598 -256Q2598 -333 2562 -375Q2525 -417 2464 -417'
    'Q2404 -417 2367 -374Q2330 -332 2330 -255Q2330 -178 2367 -136Q2404 -94 2465 -94Z')

SS = 4  # supersampling per axis


def build_tag(path=BUILD_H):
    m = re.search(r'#define\s+COOP_BUILD_TAG\s+"([^"]+)"', open(path).read())
    if not m:
        raise LdrError('{}: no COOP_BUILD_TAG'.format(path))
    return m.group(1)


def stock_version(stock_string=VERSION_STOCK):
    m = re.search(rb'R\. (\d+)', stock_string)
    return str(int(m.group(1)))


def version_text(tag, stock=None):
    return 'coop {} - {}'.format(tag, stock or stock_version())


# ---------------------------------------------------------------- drawing

def path_polygons(d, scale, ox, oy):
    """SVG path (M L H V Q Z, absolute) -> closed polylines in screen px."""
    tokens = re.findall(r'[MLHVQZ]|-?\d+(?:\.\d+)?', d)
    polys, cur, i = [], [], 0
    x = y = 0.0
    sx = sy = 0.0
    cmd = None

    def pt(px, py):
        return (ox + px * scale, oy + py * scale)

    def num():
        nonlocal i
        v = float(tokens[i])
        i += 1
        return v
    while i < len(tokens):
        if tokens[i] in 'MLHVQZ':
            cmd = tokens[i]
            i += 1
        if cmd == 'M':
            if cur:
                polys.append(cur)
            x, y = num(), num()
            sx, sy = x, y
            cur = [pt(x, y)]
            cmd = 'L'
        elif cmd == 'L':
            x, y = num(), num()
            cur.append(pt(x, y))
        elif cmd == 'H':
            x = num()
            cur.append(pt(x, y))
        elif cmd == 'V':
            y = num()
            cur.append(pt(x, y))
        elif cmd == 'Q':
            cx, cy, ex, ey = num(), num(), num(), num()
            for k in range(1, 9):
                t = k / 8.0
                a, b, c = (1 - t) ** 2, 2 * (1 - t) * t, t * t
                cur.append(pt(a * x + b * cx + c * ex, a * y + b * cy + c * ey))
            x, y = ex, ey
        elif cmd == 'Z':
            if cur:
                polys.append(cur)
            cur = []
            x, y = sx, sy
            cmd = None
    if cur:
        polys.append(cur)
    return polys


def fill_polygons(cov, polys):
    """Non-zero fill of polylines into a coverage map {(x, y): samples}, SS x SS samples a pixel."""
    ys = [p[1] for poly in polys for p in poly]
    y0, y1 = int(min(ys) * SS), int(max(ys) * SS) + 1
    edges = []
    for poly in polys:
        for (ax, ay), (bx, by) in zip(poly, poly[1:] + poly[:1]):
            if ay != by:
                edges.append((ax, ay, bx, by, 1 if by > ay else -1))
    for sy in range(y0, y1 + 1):
        yc = (sy + 0.5) / SS
        xs = []
        for ax, ay, bx, by, w in edges:
            if (ay <= yc < by) or (by <= yc < ay):
                xs.append((ax + (yc - ay) * (bx - ax) / (by - ay), w))
        xs.sort()
        wind = 0
        for k in range(len(xs) - 1):
            wind += xs[k][1]
            if wind:
                lo, hi = xs[k][0], xs[k + 1][0]
                for sx in range(int(lo * SS - 0.5) + 1, int(hi * SS - 0.5) + 1):
                    xc = (sx + 0.5) / SS
                    if lo <= xc < hi:
                        key = (sx // SS, sy // SS)
                        cov[key] = cov.get(key, 0) + 1


def fill_circle(cov, cx, cy, r):
    for sy in range(int((cy - r) * SS) - 1, int((cy + r) * SS) + 2):
        for sx in range(int((cx - r) * SS) - 1, int((cx + r) * SS) + 2):
            if ((sx + 0.5) / SS - cx) ** 2 + ((sy + 0.5) / SS - cy) ** 2 <= r * r:
                key = (sx // SS, sy // SS)
                cov[key] = cov.get(key, 0) + 1


def draw_screen():
    """The landscape screen without the version line: rows of (r, g, b), 400 x 200."""
    img = [[(0, 0, 0)] * SCREEN_W for _ in range(SCREEN_H)]
    dot, gap, tgap, fs = WEB_DOT * K, WEB_DOT_GAP * K, WEB_TEXT_GAP * K, WEB_FONT * K
    scale = fs / 1000.0
    width = 4 * dot + 3 * gap + tgap + WORDMARK_ADVANCE * scale
    left = CENTRE_X - width / 2
    layers = []
    for n, colour in enumerate(DOTS):
        cov = {}
        fill_circle(cov, left + dot / 2 + n * (dot + gap), CENTRE_Y, dot / 2)
        layers.append((cov, colour))
    # flex centring as on the web: the line box (ascent 970, descent 250 units) centred on the dots
    baseline = CENTRE_Y - (970 + 250) / 2.0 * scale + 970 * scale
    cov = {}
    fill_polygons(cov, path_polygons(WORDMARK, scale, left + 4 * dot + 3 * gap + tgap, baseline))
    layers.append((cov, INK))
    full = SS * SS
    for cov, colour in layers:
        for (x, y), n in cov.items():
            if 0 <= x < SCREEN_W and 0 <= y < SCREEN_H:
                a = min(n, full) / full
                old = img[y][x]
                img[y][x] = tuple(int(round(o * (1 - a) + c * a)) for o, c in zip(old, colour))
    return img


def art_bottom(img):
    return max(y for y in range(SCREEN_H) if any(any(px) for px in img[y]))


def picture_bytes(img):
    """The landscape screen -> the 240 x 320 RGB565 picture as the LDR loads it."""
    out = bytearray(PIC_SIZE)
    for r in range(PIC_ROWS):
        x = X_OF_ROW0 - r
        for c in range(min(PIC_COLS, SCREEN_H)):
            red, green, blue = img[c][x]
            v = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
            struct.pack_into('<H', out, 2 * (r * PIC_COLS + c), v)
    return bytes(out)


# ---------------------------------------------------------------- the version line

def console_string(text, widths=None):
    """17 newlines, spaces to centre `text` on the screen as the stock does, the text, NUL-padded to 59 bytes."""
    if any(not 32 <= ord(ch) < 127 for ch in text):
        raise LdrError('version text {!r}: the stock console draws ASCII 32..126 only'.format(text))
    adv = (lambda ch: widths[ord(ch) - 32] + 1) if widths else (lambda ch: 6)
    px = sum(adv(ch) for ch in text) - 1
    space = adv(' ')
    spaces = max(0, int(round((CENTRE_X - CONSOLE_X0 - px / 2.0) / space)))
    raw = b'\n' * VERSION_LINES + b' ' * spaces + text.encode('ascii')
    room = VERSION_SIZE - 1
    if len(raw) > room:
        raw = b'\n' * VERSION_LINES + b' ' * (spaces - (len(raw) - room)) + text.encode('ascii')
        if len(raw) > room:
            raise LdrError('version text {!r} does not fit the stock string'.format(text))
    return raw + b'\0' * (VERSION_SIZE - len(raw))


class Memory(object):
    def __init__(self, dxes):
        self.runs = memory_runs(dxes[0])

    def read(self, addr, size):
        for start, run in self.runs:
            if start <= addr and addr + size <= start + len(run):
                return bytes(run[addr - start:addr - start + size])
        raise LdrError('{:#x}+{:#x}: not loaded by this LDR'.format(addr, size))


def console_font(mem):
    widths = list(mem.read(FONT_WIDTHS, 95))
    glyphs = []
    for i in range(95):
        ptr = struct.unpack('<I', mem.read(FONT_GLYPHS + 4 * i, 4))[0]
        raw = mem.read(ptr, 16)
        glyphs.append([raw[2 * k] for k in range(8)])
    return widths, glyphs


def draw_console(img, string, font):
    """What the stock console draws for `string` (preview only): cell rows y + 1 .. y + 8, black where a bit is clear."""
    widths, glyphs = font
    x = y = 0
    for b in string:
        if b == 0:
            break
        if b == 10:
            x, y = 0, y + 8
            continue
        i = b - 32
        for row in range(8):
            for col in range(widths[i]):
                on = (glyphs[i][row] >> col) & 1
                sx, sy = CONSOLE_X0 + x + col, y + row + 1
                if 0 <= sx < SCREEN_W and 0 <= sy < SCREEN_H:
                    img[sy][sx] = (255, 255, 255) if on else (0, 0, 0)
        x += widths[i] + 1


# ---------------------------------------------------------------- the LDR

def apply(ldr_bytes, tag):
    dxes = parse(ldr_bytes)
    if len(dxes) != 1:
        raise LdrError('expected a single-DXE app LDR, found {}'.format(len(dxes)))
    mem = Memory(dxes)
    have = hashlib.sha256(mem.read(PIC_ADDR, PIC_SIZE)).hexdigest()
    if have != PIC_STOCK_SHA256:
        raise LdrError('boot picture at {:#x} is not the stock one (sha256 {}); refusing'.format(PIC_ADDR, have))
    stock = mem.read(VERSION_ADDR, VERSION_SIZE)
    if stock != VERSION_STOCK:
        raise LdrError('{:#x}: expected the stock version string, found {!r}'.format(VERSION_ADDR, stock))
    font = console_font(mem)
    img = draw_screen()
    if art_bottom(img) >= VERSION_LINES * 8:
        raise LdrError('the picture reaches the version line')
    pic = picture_bytes(img)

    blocks = [{'flags': 0x0100, 'dmacode': 6, 'target': PIC_ADDR, 'count': PIC_SIZE, 'argument': 0, 'payload': b''}]
    pos = 0
    while pos < PIC_SIZE:                                  # plain blocks over the non-zero spans, word aligned
        while pos < PIC_SIZE and not any(pic[pos:pos + 4]):
            pos += 4
        if pos >= PIC_SIZE:
            break
        end, gap = pos, 0
        while end < PIC_SIZE and gap < 64:
            gap = 0 if any(pic[end:end + 4]) else gap + 4
            end += 4
        end -= gap
        blocks.append({'flags': 0, 'dmacode': 6, 'target': PIC_ADDR + pos, 'count': end - pos, 'argument': 0,
                       'payload': pic[pos:end]})
        pos = end
    dxe = dxes[0]
    final = [i for i, b in enumerate(dxe) if b['flags'] & 0x8000]
    if len(final) != 1:
        raise LdrError('expected exactly one FINAL block')
    dxe[final[0]:final[0]] = blocks

    text = version_text(tag, stock_version(stock))
    new = console_string(text, font[0])
    apply_patch(dxes, VERSION_ADDR, stock, new)

    out = serialize(dxes)
    check = Memory(parse(out))
    if check.read(PIC_ADDR, PIC_SIZE) != pic or check.read(VERSION_ADDR, VERSION_SIZE) != new:
        raise LdrError('round trip: the LDR does not load the new picture')
    return out, text, len(blocks)


def write_png(path, img):
    raw = bytearray()
    for row in img:
        raw.append(0)
        for px in row:
            raw += bytes(px)
    h, w = len(img), len(img[0])

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


def main():
    ap = argparse.ArgumentParser(description='Put coop\'s boot picture and version line into an app LDR.')
    ap.add_argument('ldr_in', nargs='?')
    ap.add_argument('ldr_out', nargs='?')
    ap.add_argument('--tag', help='build tag (default: COOP_BUILD_TAG in core/include/coop_build.h)')
    ap.add_argument('--png', help='write a preview of the screen (400 x 200) instead')
    ap.add_argument('--stock', default=STOCK_LDR, help='stock OP1_vdk.ldr for the preview\'s console font')
    args = ap.parse_args()
    tag = args.tag or build_tag()
    try:
        if args.png:
            dxes = parse(open(args.stock, 'rb').read())
            font = console_font(Memory(dxes))
            img = draw_screen()
            draw_console(img, console_string(version_text(tag), font[0]), font)
            write_png(args.png, img)
            print('wrote {} ({})'.format(args.png, version_text(tag)))
            return
        if not (args.ldr_in and args.ldr_out):
            ap.error('IN.ldr and OUT.ldr are required')
        out, text, n = apply(open(args.ldr_in, 'rb').read(), tag)
        open(args.ldr_out, 'wb').write(out)
        print('boot picture: {} blocks at {:#x}, version line {!r}'.format(n, PIC_ADDR, text))
    except LdrError as e:
        print('error: {}'.format(e), file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
