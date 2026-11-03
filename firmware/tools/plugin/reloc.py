# coop-fw: kennel 3.1 tools/kennel/reloc.py (GPL-3.0-or-later, Hundekraft), the magic COOP.
#!/usr/bin/env python3
"""Reference loader for kennel plugin files (docs/plugin-format.md): check a file, then relocate its image.

    python3 tools/kennel/reloc.py FILE.knl BASE [SYMBOL=ADDR ...] [-o image.bin]

This is the spec the C loader follows: same checks, same order, same results. `parse` refuses anything the
loader must skip; `relocate` returns the image (text, rodata, data, then bss as zeros) as it must sit at BASE.
Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""
import struct
import sys
import zlib

MAGIC = b'COOP'   # coop-fw: kennel's format with its own magic
FORMAT_VERSION = 1
HEADER_SIZE = 144
HEADER = struct.Struct('<4sHHII HBB16sI 4h4h 22I')
EXT_CLAIMS, EXT_AUTHOR, EXT_VERSION = 1, 2, 3          # extension table tags (docs/plugin-format.md)
EXT_BIGBUF = 4                                         # u32 image offset of the op1_bigbuf_t (flag bit 4)
EXT_COST = 5                                           # u32 x 2: declared worst-block cost, bcore packets (information)
BIGBUF = struct.Struct('<6I')                          # magic, version, min, max, ptr, size
BIGBUF_MAGIC, BIGBUF_VERSION, BIGBUF_MAX = 0x42424E4B, 1, 0x80000   # BIGBUF_MAX = kn_mem.h KN_DECL_MAX
assert HEADER.size == HEADER_SIZE
RELOC = struct.Struct('<IBBHi')          # offset, type, reserved, sym, addend
IMPORT = struct.Struct('<I')             # name offset in the string table
NONE = 0xFFFFFFFF
SYM_LOCAL = 0xFFFF

KIND_SYNTH, KIND_FX, KIND_FEATURE = 1, 2, 3
KINDS = {'synth': KIND_SYNTH, 'fx': KIND_FX, 'feature': KIND_FEATURE}
FLAG_USES_SAMPLE, FLAG_PAGE2, FLAG_STEREO, FLAG_INPUT, FLAG_BIGBUF = 1, 2, 4, 8, 0x10
FLAG_COARSE_SHIFT, FLAG_COARSE_MASK = 16, 0xF0000      # synth: bit 16 + k = knob k in 8 steps ("coarse")

R_ABS32, R_HI16, R_LO16, R_PCREL24 = 1, 2, 3, 4
R_NAMES = {R_ABS32: 'ABS32', R_HI16: 'HI16', R_LO16: 'LO16', R_PCREL24: 'PCREL24'}
R_FIELD = {R_ABS32: (0, 4), R_HI16: (0, 2), R_LO16: (0, 2), R_PCREL24: (-2, 4)}   # field start rel. offset, size
PCREL24_MIN, PCREL24_MAX = -(1 << 24), (1 << 24) - 2                                # bytes, even
STUB_SIZE = 12                                   # call stub: P1.L = t; P1.H = t; JUMP (P1); NOP (kn_load.h)

FIELDS = ('magic format_version header_size file_size crc32 api_version kind legacy_order type_name flags '
          'd0 d1 d2 d3 p0 p1 p2 p3 text_size rodata_offset rodata_size data_offset data_size bss_offset '
          'bss_size image_size image_align entry_offset screen_offset image_file_offset reloc_offset '
          'reloc_count import_offset import_count strings_offset strings_size svg_offset svg_size '
          'svg_name ext_offset').split()


class PluginError(Exception):
    """The file is broken or does not fit this core: the loader skips it with this message."""


def crc32(data, crc_at=12):
    """CRC-32 (zlib) over the whole file with the 4 CRC bytes at crc_at taken as zero."""
    return zlib.crc32(data[:crc_at] + b'\0\0\0\0' + data[crc_at + 4:]) & 0xFFFFFFFF


def cstr(buf, off, what):
    end = buf.find(b'\0', off)
    if off >= len(buf) or end < 0:
        raise PluginError(f'{what}: string at {off} runs past the string table')
    try:
        return buf[off:end].decode('ascii', 'strict')
    except UnicodeDecodeError:
        raise PluginError(f'{what}: string at {off} not ASCII') from None


def header_offset(data):
    """0, or for a file that starts with its screen SVG: the 4-byte boundary after the SVG's NUL."""
    if data[:4] == MAGIC:
        if len(data) < 8:                               # magic, version and header size
            raise PluginError('not a kennel plugin (magic)')
        return 0
    end = data.find(b'\0')
    hoff = (end + 4) & ~3
    if end < 0 or not data[:1] == b'<' or data[hoff:hoff + 4] != MAGIC or len(data) < hoff + 8:
        raise PluginError('not a kennel plugin (magic)')
    return hoff


def parse_ext(data, off, first):
    """Extension table: {tag: bytes}. Entries u16 tag, u16 length, bytes, padding to 4; tag 0 ends it."""
    if off == 0:
        return {}
    if off % 4 or off < first or off >= len(data):
        raise PluginError('extension table outside the file or misaligned')
    out = {}
    while True:
        if off + 4 > len(data):
            raise PluginError('extension table runs past the file')
        tag, n = struct.unpack_from('<HH', data, off)
        if tag == 0:
            return out
        if off + 4 + n > len(data):
            raise PluginError('extension entry runs past the file')
        val = data[off + 4:off + 4 + n]
        if tag in (EXT_CLAIMS, EXT_AUTHOR, EXT_VERSION):
            if not val or val[-1] != 0 or any(b >= 0x80 for b in val):
                raise PluginError(f'extension {tag}: not NUL-terminated ASCII')
            if tag == EXT_CLAIMS and len(val) > 1 and val[-2] != 0:
                raise PluginError('claims list not double-NUL-terminated')
        if tag == EXT_BIGBUF and (n != 4 or tag in out):
            raise PluginError('bigbuf entry malformed')
        out[tag] = val
        off += 4 + ((n + 3) & ~3)


def parse(data, api_version=None):
    """Check a plugin file and return a dict of its header fields plus image, relocs, imports, svg."""
    hoff = header_offset(data)
    version, header_size = struct.unpack_from('<HH', data, hoff + 4)
    if version != FORMAT_VERSION:
        raise PluginError(f'format version {version} not supported (this loader: {FORMAT_VERSION})')
    if header_size < HEADER_SIZE or len(data) < hoff + header_size:
        raise PluginError('header too short')
    h = dict(zip(FIELDS, HEADER.unpack_from(data, hoff)))
    h['header_offset'] = hoff
    if h['file_size'] != len(data):
        raise PluginError(f'file size {len(data)} != header {h["file_size"]}')
    if crc32(data, hoff + 12) != h['crc32']:
        raise PluginError('CRC mismatch')
    if api_version is not None and h['api_version'] != api_version:
        raise PluginError(f'plugin API {h["api_version"]}, core has {api_version}')
    if h['kind'] not in KINDS.values():
        raise PluginError(f'unknown kind {h["kind"]}')
    if h['legacy_order'] > 31:
        raise PluginError(f'legacy order {h["legacy_order"]} > 31')
    name = h['type_name']
    if b'\0' not in name or name[0] == 0:
        raise PluginError('type name empty or not NUL-terminated')
    try:
        h['type_name'] = name[:name.index(b'\0')].decode('ascii', 'strict')
    except UnicodeDecodeError:
        raise PluginError('type name not ASCII') from None

    def span(off, size, what, align=4):
        if off % align or off + size > len(data) or off < hoff + header_size:
            raise PluginError(f'{what} ({off}+{size}) outside the file or misaligned')
        return data[off:off + size]

    a = h['image_align']
    if a < 4 or a & (a - 1):
        raise PluginError(f'image alignment {a} not a power of two >= 4')
    for k in ('rodata', 'data', 'bss'):
        if h[k + '_offset'] % 4:
            raise PluginError(f'{k} offset misaligned')
    if not (h['text_size'] <= h['rodata_offset'] <= h['rodata_offset'] + h['rodata_size'] <= h['data_offset']
            <= h['data_offset'] + h['data_size'] == h['image_size'] <= h['bss_offset']):
        raise PluginError('section layout out of order')
    total = h['bss_offset'] + h['bss_size']
    if total > 0xFFFFFFFF:                              # as kn_load.c: addresses are 32-bit
        raise PluginError('plugin too large')
    h['load_size'] = total
    h['image'] = span(h['image_file_offset'], h['image_size'], 'image')
    strings = span(h['strings_offset'], h['strings_size'], 'strings', 1)
    if strings and strings[-1] != 0:
        raise PluginError('string table not NUL-terminated')
    imports = span(h['import_offset'], h['import_count'] * IMPORT.size, 'imports')
    h['imports'] = [cstr(strings, IMPORT.unpack_from(imports, i * 4)[0], 'import')
                    for i in range(h['import_count'])]
    raw = span(h['reloc_offset'], h['reloc_count'] * RELOC.size, 'relocations')
    relocs = []
    for i in range(h['reloc_count']):
        off, typ, res, sym, addend = RELOC.unpack_from(raw, i * RELOC.size)
        if typ not in R_NAMES or res:
            raise PluginError(f'relocation {i}: unknown type {typ}')
        start, size = R_FIELD[typ]
        if off + start < 0 or off + start + size > h['image_size'] or (off + start) % 2:
            raise PluginError(f'relocation {i}: offset {off:#x} outside the image')
        if sym == SYM_LOCAL:
            if typ == R_PCREL24:
                raise PluginError(f'relocation {i}: local PCREL24 (must be resolved by the packer)')
            if not 0 <= addend <= total:
                raise PluginError(f'relocation {i}: target {addend:#x} outside the plugin')
        elif sym >= h['import_count']:
            raise PluginError(f'relocation {i}: import {sym} out of range')
        relocs.append((off, typ, sym, addend))
    if h['flags'] & ~(FLAG_USES_SAMPLE | FLAG_PAGE2 | FLAG_STEREO | FLAG_INPUT | FLAG_BIGBUF | FLAG_COARSE_MASK):
        raise PluginError('unknown plugin flags')
    if h['flags'] & FLAG_COARSE_MASK and h['kind'] != KIND_SYNTH:
        raise PluginError('coarse knobs require synth kind')
    if h['flags'] & FLAG_INPUT and (h['kind'] != KIND_SYNTH or h['flags'] & FLAG_USES_SAMPLE):
        raise PluginError('input flag requires nonsampler synth')
    if h['flags'] & FLAG_INPUT and h['entry_offset'] + 32 > h['image_size']:
        raise PluginError('input entry object truncated')
    if h['flags'] & FLAG_STEREO and h['kind'] != KIND_FX:
        raise PluginError('stereo flag requires FX kind')
    h['relocs'] = relocs
    for k in ('entry_offset', 'screen_offset'):
        if h[k] != NONE and (h[k] >= total or h[k] % 4):
            raise PluginError(f'{k} {h[k]:#x} outside the plugin')
    if h['entry_offset'] == NONE:
        raise PluginError('no entry object')
    entry_size = 84 if h['kind'] == KIND_FEATURE else 16 if h['kind'] == KIND_SYNTH else 12
    if h['entry_offset'] + entry_size > h['image_size']:
        raise PluginError('entry object truncated')
    if h['screen_offset'] != NONE and h['screen_offset'] + 8 > h['image_size']:
        raise PluginError('screen object truncated')
    if h['flags'] & FLAG_STEREO and h['entry_offset'] + 16 > h['image_size']:
        raise PluginError('stereo entry object truncated')
    if h['svg_offset'] or h['svg_name'] != NONE:     # reserved
        raise PluginError('reserved svg fields not 0 / none')
    if bool(hoff) != bool(h['svg_size']) or (hoff and (not h['svg_size'] <= hoff or data[h['svg_size'] - 1] != 0)):
        raise PluginError('svg prefix does not match the header')
    h['svg'] = data[:h['svg_size'] - 1] if hoff else b''   # the file starts with the SVG text and its NUL
    h['ext'] = parse_ext(data, h['ext_offset'], hoff + header_size)
    h['claims'] = [c.decode('ascii') for c in h['ext'].get(EXT_CLAIMS, b'').split(b'\0') if c]
    c = h['ext'].get(EXT_COST, b'')                     # declared cost: any other length is ignored, never refused
    h['cost'] = list(struct.unpack('<II', c)) if len(c) == 8 else [0, 0]
    h['bigbuf'] = None                                  # declared pool buffer (kn_load.c, same checks and order)
    if bool(h['flags'] & FLAG_BIGBUF) != (EXT_BIGBUF in h['ext']):
        raise PluginError('bigbuf flag and descriptor disagree')
    if h['flags'] & FLAG_BIGBUF:
        bd, = struct.unpack('<I', h['ext'][EXT_BIGBUF])
        if bd % 4 or bd < h['data_offset'] or bd + BIGBUF.size > h['image_size']:
            raise PluginError('bigbuf descriptor outside .data or misaligned')
        magic, version, mn, mx, ptr, size = BIGBUF.unpack_from(h['image'], bd)
        if magic != BIGBUF_MAGIC or version != BIGBUF_VERSION:
            raise PluginError('bigbuf descriptor magic or version')
        if mn > mx or not mx:
            raise PluginError('bigbuf min > max or max 0')
        if mn > BIGBUF_MAX:
            raise PluginError('bigbuf min larger than 512 KB')
        if ptr or size:
            raise PluginError('bigbuf ptr / size not 0 in the file')
        if any(off + 4 > bd and off < bd + BIGBUF.size + 2 for off, *_ in relocs):
            raise PluginError('relocation inside the bigbuf descriptor')
        h['bigbuf'] = {'offset': bd, 'min': mn, 'max': mx}
    h['defaults'] = [h.pop(f'd{i}') for i in range(4)]
    h['page2_defaults'] = [h.pop(f'p{i}') for i in range(3)] if h['flags'] & FLAG_PAGE2 else None
    h['coarse'] = [k for k in range(4) if h['flags'] >> (FLAG_COARSE_SHIFT + k) & 1]
    h.pop('p3')
    return h


def relocate(plugin, base, exports, stubs=False):
    """Image (load_size bytes) placed at `base`; exports maps import names to core addresses.
    stubs (kn_relocate_far, docs/plugin-format.md "Call stubs"): a PCREL24 into the core out of reach from `base` goes to
    a 12-byte stub after the image (from the first 4-byte boundary at or after load_size, one per distinct target in the
    order first needed) that jumps there absolutely; the result is then longer than load_size."""
    if base % plugin['image_align']:
        raise PluginError(f'base {base:#x} not aligned to {plugin["image_align"]}')
    addrs = []
    for name in plugin['imports']:
        if name not in exports:
            raise PluginError(f'import {name} not exported by the core')
        addrs.append(exports[name])
    img = bytearray(plugin['image']) + bytes(plugin['load_size'] - plugin['image_size'])
    s0, targets = (plugin['load_size'] + 3) & ~3, []
    for off, typ, sym, addend in plugin['relocs']:
        v = (base + addend if sym == SYM_LOCAL else addrs[sym] + addend) & 0xFFFFFFFF
        if typ == R_ABS32:
            struct.pack_into('<I', img, off, v)
        elif typ == R_HI16:
            struct.pack_into('<H', img, off, v >> 16)
        elif typ == R_LO16:
            struct.pack_into('<H', img, off, v & 0xFFFF)
        else:                                            # off = second halfword of a 32-bit CALL / JUMP.L
            pc = base + off - 2
            d = v - pc
            if stubs and not d % 2 and not PCREL24_MIN <= d <= PCREL24_MAX:
                if v not in targets:
                    targets.append(v)
                d = base + s0 + STUB_SIZE * targets.index(v) - pc
            if d % 2 or not PCREL24_MIN <= d <= PCREL24_MAX:
                raise PluginError(f'PCREL24 at {off:#x}: {plugin["imports"][sym]} out of reach from {pc:#x}')
            d = (d >> 1) & 0xFFFFFF
            hw = struct.unpack_from('<H', img, off - 2)[0]
            struct.pack_into('<HH', img, off - 2, (hw & 0xFF00) | (d >> 16), d & 0xFFFF)
    if plugin['flags'] & FLAG_STEREO:
        end = base + plugin['text_size']
        if end > 0x100000000:
            raise PluginError('stereo text address overflow')
        for offset in (0, 4, 12):
            ptr, = struct.unpack_from('<I', img, plugin['entry_offset'] + offset)
            if not ptr or ptr & 1 or not base <= ptr or ptr + 2 > end:
                raise PluginError('stereo callback outside plugin text')
    if plugin['flags'] & FLAG_INPUT:
        end = base + plugin['text_size']
        for offset in (0, 4, 16, 20, 24):
            ptr, = struct.unpack_from('<I', img, plugin['entry_offset'] + offset)
            if offset == 24 and not ptr: continue
            if not ptr or ptr & 1 or not base <= ptr or ptr + 2 > end:
                raise PluginError('input callback outside plugin text')
    if targets:
        img += bytes(s0 - plugin['load_size'])
        for v in targets:
            img += struct.pack('<6H', 0xE109, v & 0xFFFF, 0xE149, v >> 16, 0x0051, 0)
    return bytes(img)


def main(argv):
    out = None
    if '-o' in argv:
        i = argv.index('-o')
        out = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    if len(argv) < 2:
        sys.exit(__doc__)
    p = parse(open(argv[0], 'rb').read())
    exports = {k: int(v, 0) for k, v in (a.split('=', 1) for a in argv[2:])}
    img = relocate(p, int(argv[1], 0), exports)
    kind = {v: k for k, v in KINDS.items()}[p['kind']]
    print(f'{p["type_name"]} ({kind}, order {p["legacy_order"]}): image {p["image_size"]} + bss {p["bss_size"]}, '
          f'{len(p["relocs"])} relocations, imports {", ".join(p["imports"]) or "none"}')
    if out:
        open(out, 'wb').write(img)


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except PluginError as e:
        sys.exit(f'error: {e}')
