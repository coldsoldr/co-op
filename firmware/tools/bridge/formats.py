"""Readers for the plugin files coop's bridge looks at (docs/bridge.md): kennel's plugin file (.knl, magic KNPL; coop's
own .coop is the same format with the magic COOP) and the Opus 2 plugin file (.opmod, magic OPMD). Headers, tables and
names only: nothing here runs or relocates a file. Layouts: third_party/kennel/docs/plugin-format.md and
core/sdram/plugin_load.cc (kennel); the Opus plugin kit's documented format 1 (.opmod).

Stdlib only, Python 3.8. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later.
"""
import struct
import zlib

KN_MAGICS = {b'KNPL': 'knl', b'COOP': 'coop'}
KN_HEADER = 144
KN_NONE = 0xFFFFFFFF
KIND = {1: 'synth', 2: 'fx', 3: 'feature'}
F_USES_SAMPLE, F_PAGE2, F_STEREO, F_INPUT, F_BIGBUF = 1, 2, 4, 8, 0x10
COMMON_FLAGS = {1: 'uses_sample', 2: 'page2', 4: 'stereo', 8: 'input', 0x10: 'bigbuf'}
# bits whose meaning depends on the magic (kennel 3.3.1 kn_load.h / coop plugin_load.h): 0x20 is kennel's own-envelope
# engine and coop's scratch effect; extension 7 is kennel's settings rows and coop's scratch bytes
KNL_FLAGS = {0x20: 'own_env', 0x40: 'sum', 0x80: 't2_page', 0x100: 't2_swallow', 0x200: 'eng_input', 0x400: 't4_page',
             0x800: 'eng_service', 0x1000: 'cold'}
COOP_FLAGS = {0x20: 'scratch'}
COARSE_MASK = 0xF0000

OPMOD_MAGIC = 0x444D504F
OPMOD_HDR = 0x60


class BadFile(ValueError):
    pass


def _cstr(blob, off):
    end = blob.find(b'\0', off)
    if off >= len(blob) or end < 0:
        raise BadFile('name runs past the string table')
    return blob[off:end].decode('ascii', 'replace')


def kn_header_offset(d):
    """0, or the 4-byte boundary after a leading screen SVG's NUL; -1 when there is no header."""
    if d[:4] in KN_MAGICS:
        return 0
    if d[:1] != b'<':
        return -1
    end = d.find(b'\0')
    if end < 0:
        return -1
    h = (end + 4) & ~3
    return h if d[h:h + 4] in KN_MAGICS else -1


def read_kn(d):
    """A .knl / .coop file -> dict. Checks the size and CRC words and the tables' bounds, as kn_parse starts."""
    ho = kn_header_offset(d)
    if ho < 0:
        raise BadFile('not a kennel-format plugin (magic)')
    if len(d) < ho + KN_HEADER:
        raise BadFile('header too short')
    ver, hsize = struct.unpack_from('<HH', d, ho + 4)
    fsize, crc, api, kind, order = struct.unpack_from('<IIHBB', d, ho + 8)
    if ver != 1 or hsize < KN_HEADER:
        raise BadFile('format version not supported')
    if fsize != len(d):
        raise BadFile('file size does not match the header')
    if zlib.crc32(d[:ho + 12] + b'\0\0\0\0' + d[ho + 16:]) & 0xFFFFFFFF != crc:
        raise BadFile('CRC mismatch')
    name = d[ho + 20:ho + 36].split(b'\0')[0].decode('ascii', 'replace')
    flags, = struct.unpack_from('<I', d, ho + 36)
    defaults = struct.unpack_from('<4h', d, ho + 40)
    page2 = struct.unpack_from('<4h', d, ho + 48)
    (text, ro_off, ro_size, da_off, da_size, bss_off, bss_size, image_size, align, entry, screen, image_file, rel_off,
     rel_n, imp_off, imp_n, str_off, str_size, svg, svg_size, svg_name, ext) = struct.unpack_from('<22I', d, ho + 56)
    for off, n in ((image_file, image_size), (rel_off, rel_n * 12), (imp_off, imp_n * 4), (str_off, str_size)):
        if off + n > len(d):
            raise BadFile('a table outside the file')
    strings = d[str_off:str_off + str_size]
    imports = [_cstr(strings, struct.unpack_from('<I', d, imp_off + 4 * i)[0]) for i in range(imp_n)]
    fmt = KN_MAGICS[d[ho:ho + 4]]
    names = dict(COMMON_FLAGS)
    names.update(KNL_FLAGS if fmt == 'knl' else COOP_FLAGS)
    flag_names = [n for b, n in sorted(names.items()) if flags & b]
    unknown = flags & ~(sum(names) | COARSE_MASK)
    exts = {}
    eo = ext
    while eo:
        if eo + 4 > len(d):
            raise BadFile('extension table runs past the file')
        tag, n = struct.unpack_from('<HH', d, eo)
        if not tag:
            break
        exts[tag] = d[eo + 4:eo + 4 + n]
        eo += 4 + ((n + 3) & ~3)
    out = {
        'format': fmt, 'api': api, 'kind': KIND.get(kind, str(kind)), 'kind_n': kind, 'order': order, 'name': name,
        'flags': flags, 'flag_names': flag_names, 'unknown_flags': unknown, 'coarse': (flags >> 16) & 15,
        'defaults': defaults, 'page2': page2, 'text': text, 'image': image_size, 'bss': bss_size,
        'load': bss_off + bss_size, 'align': align, 'entry': entry, 'screen': screen, 'svg': svg_size if ho else 0,
        'relocs': rel_n, 'imports': imports, 'size': len(d), 'crc': crc, 'exts': exts,
        'author': exts.get(2, b'').split(b'\0')[0].decode('ascii', 'replace'),
        'version': exts.get(3, b'').split(b'\0')[0].decode('ascii', 'replace'),
        'claims': [c.decode('ascii', 'replace') for c in exts.get(1, b'').split(b'\0') if c],
        'bigbuf': None, 'scratch': 0, 'image_file': image_file, 'hdr': ho,
    }
    if flags & F_BIGBUF and 4 in exts and len(exts[4]) == 4:
        bd, = struct.unpack_from('<I', exts[4])
        if bd + 24 <= image_size:
            m, _v, lo, hi = struct.unpack_from('<4I', d, image_file + bd)
            if m == 0x42424E4B:
                out['bigbuf'] = (lo, hi)
    if fmt == 'coop' and 7 in exts and len(exts[7]) == 4:
        out['scratch'], = struct.unpack_from('<I', exts[7])
    return out


def read_opmod(d):
    """An .opmod -> dict (format 1). Size, CRC, table bounds, import names, the manifest's lines."""
    if len(d) < OPMOD_HDR or struct.unpack_from('<I', d)[0] != OPMOD_MAGIC:
        raise BadFile('not an Opus plugin file (magic)')
    fmt, hdr = struct.unpack_from('<HH', d, 4)
    size, crc = struct.unpack_from('<II', d, 8)
    if fmt != 1 or hdr < OPMOD_HDR:
        raise BadFile('a newer file format')
    if size != len(d):
        raise BadFile('damaged: size')
    if zlib.crc32(d[:12] + b'\0\0\0\0' + d[16:]) & 0xFFFFFFFF != crc:
        raise BadFile('damaged: crc')
    w = struct.unpack_from('<20I', d, 0x10)
    man_off, man_len, img_off, img_len, zero, entry, imp_off, imp_n, rel_off, rel_n = w[:10]
    str_off, str_len, flags, exp_off, exp_n, pl_n = w[12], w[13], w[14], w[16], w[17], w[19]
    for off, n in ((man_off, man_len), (img_off, img_len), (imp_off, 4 * imp_n), (rel_off, 12 * rel_n),
                   (str_off, str_len), (exp_off, 8 * exp_n)):
        if off + n > len(d):
            raise BadFile('damaged: a table outside the file')
    strings = d[str_off:str_off + str_len]
    imports = [_cstr(strings, struct.unpack_from('<I', d, imp_off + 4 * i)[0]) for i in range(imp_n)]
    exports = [_cstr(strings, struct.unpack_from('<I', d, exp_off + 8 * i)[0]) for i in range(exp_n)]
    lines = {}
    for line in d[man_off:man_off + man_len].decode('utf-8', 'replace').splitlines():
        k, _, v = line.strip().partition(' ')
        if k and not k.startswith('#'):
            lines.setdefault(k, []).append(v.strip())

    def first(k):
        return (lines.get(k) or [''])[0]
    kinds = [0, 0, 0, 0]
    for i in range(rel_n):
        word, = struct.unpack_from('<I', d, rel_off + 12 * i)
        kinds[(word >> 28) & 3] += 1
    return {
        'format': 'opmod', 'id': first('id'), 'name': first('name') or first('id'), 'author': first('author'),
        'version': first('version'), 'core': first('core'), 'adds': lines.get('adds', []),
        'needs': lines.get('needs', []), 'manifest': lines, 'image': img_len, 'zero': zero, 'load': img_len + zero,
        'entry': entry, 'flags': flags, 'imports': imports, 'exports': exports, 'places': pl_n, 'relocs': rel_n,
        'branch_relocs': kinds[3], 'size': len(d), 'crc': crc,
    }


def read_any(d):
    if d[:4] == b'OPMD':
        return read_opmod(d)
    return read_kn(d)


# ---- absolute addresses in Blackfin code (docs/bridge.md "Addresses a file carries") ----
# kennel's SDK reaches R246 through static inlines that carry firmware addresses as immediates (the knob words, the
# scratch table, the voice objects, calls into stock functions): they are not imports, so the import check does not see
# them. Blackfin has no literal pools: a 32-bit constant is a pair of LDIMMhalf instructions (reg.H = imm16 and
# reg.L = imm16, or reg = imm16 (Z) for values under 64 K), so walking the text finds every one of them.

def insn_len(w):
    """Bytes of the instruction starting with halfword w (binutils bfin-dis.c: 32-bit from 0xC000 up; a 32-bit DSP
    instruction with the multi-issue bit starts a 64-bit parallel group)."""
    if w < 0xC000:
        return 2
    if (w & 0x0800) and (w & 0xE800) != 0xE800:
        return 8
    return 4


def scan_abs(image, text_size, reloc_sites=()):
    """Walk image[0:text_size] -> (pairs, stats). pairs: [(value, site_a, site_b)] for every 32-bit constant built by
    an .H / .L pair on one register (core/bridge/bridge_knl.cc br_knl_audit walks the same way). stats:
    'relocated_seen' counts the relocated immediates the walk decoded as LDIMMhalf (it should equal the HI16 / LO16
    relocations in the text: the walk stayed aligned)."""
    sites = set(reloc_sites)
    half = {}                                   # the register's 5 bits -> (hi, imm, site)
    pairs = []
    seen = 0
    off = 0
    n = min(text_size, len(image)) & ~1
    while off + 2 <= n:
        w, = struct.unpack_from('<H', image, off)
        ln = insn_len(w)
        if ln >= 4 and (w & 0xFF00) == 0xE100 and off + 4 <= n:
            imm, = struct.unpack_from('<H', image, off + 2)
            hi, key = (w >> 6) & 1, w & 31
            if off + 2 in sites:
                seen += 1
            if w & 0xA0:                        # (Z) / (X): a whole register under 64 K, never a firmware address
                half.pop(key, None)
            elif key in half and half[key][0] != hi:
                phi, pimm, psite = half.pop(key)
                pairs.append(((imm << 16 | pimm) if hi else (pimm << 16 | imm), psite, off + 2))
            else:
                half[key] = (hi, imm, off + 2)
        off += ln
    return pairs, {'relocated_seen': seen}


REGIONS = (                                     # R246 with coop: what an address in a file can be
    (0xFFC00000, 0xFFFFFFFF, 'mmr'),            # system / core registers
    (0xFFB00000, 0xFFB00FFF, 'scratchpad'),     # L1 scratchpad: the six voice objects, the stock FX object
    (0xFFA00000, 0xFFA13FFF, 'l1_code'),
    (0xFF800000, 0xFF807FFF, 'l1_data_a'),      # the stock's audio state: knob words, pitch words, meters
    (0xFF900000, 0xFF907FFF, 'l1_data_b'),
    (0x01000000, 0x01FFFFFF, 'sdram_app'),      # R246's application: code, data, the sine table
    (0x00000000, 0x00FFFFFF, 'sdram_heap'),
    (0x02000000, 0x03FFFFFF, 'sdram_high'),
)


def region(v):
    for lo, hi, name in REGIONS:
        if lo <= v <= hi:
            return name
    return None


def tight(v):
    """R246's L1 data A, scratchpad and L1 code: a plain number is unlikely to land here."""
    return region(v) in ('l1_data_a', 'scratchpad', 'l1_code')


def kn_abs(d, info, known=()):
    """The firmware addresses a .knl / .coop file's code carries, as br_knl_audit counts them: a pair counts when its
    value is on the known list or in a tight region and neither half is relocated. -> ({addr: count}, stats)."""
    ho = info['hdr']
    rel_off, rel_n = struct.unpack_from('<II', d, ho + 104)
    sites, hl = set(), 0
    for i in range(rel_n):
        off, typ = struct.unpack_from('<IB', d, rel_off + 12 * i)
        sites.add(off)
        if typ in (2, 3) and off < info['text']:
            hl += 1
    img = d[info['image_file']:info['image_file'] + info['image']]
    pairs, st = scan_abs(img, info['text'], sites)
    st['relocated_text'] = hl
    out = {}
    for v, sa, sb in pairs:
        if (v in known or tight(v)) and sa not in sites and sb not in sites:
            out[v] = out.get(v, 0) + 1
    return out, st
