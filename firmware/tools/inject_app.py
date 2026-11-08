#!/usr/bin/env python3
"""Add our own code to OP1_vdk.ldr and hook the phase engine to it.

Takes an image linked for the free SDRAM region (ports/rings/op1/link.ld) and:
  1. adds its load segments as plain LDR blocks, and its .bss as a zero FILL
     block, inserted just before the FINAL block (so every existing block keeps
     its file offset; only FINAL and the trailing IGNORE block move);
  2. optionally fills the rest of the region with a pattern (--fill-pattern),
     which the probe build checks to catch other users of the region;
  3. redirects the phase engine's vtable entries (render, note on, note off)
     to the image's entry points, read from its header at the region start;
  4. applies the image's patch table, if its header points to one (header
     words 5 and 6: table address and entry count). Each entry is
     {address, kind, stock, value}: kind 0 replaces a 32-bit word, kinds 1
     and 2 replace the 16-bit immediate of an instruction with the low or high
     half of `value` (an address in the image; the linker can't emit masked
     halves in data), kind 3 retargets a CALL, and kind 4 replaces a 16-bit
     instruction with the halfword the image holds at `value`. Patches must
     land in the app's own blocks.
Every patch checks the stock value first, like tools/patch_app.py.

--set-word NAME=VALUE (repeatable) sets a 32-bit data word of the image
before it's injected: NAME is a symbol of the ELF (with or without the
leading underscore bfin adds to C names) in a load segment's initialised
data, e.g. the module switch `op1_module_mask` (ports/fx/op1/modules.h).
This is how the builder app makes every module combination from one
shipped ELF. --set-string NAME=TEXT (repeatable) likewise rewrites a
NUL-terminated char array of the image: TEXT plus its NUL must fit the
symbol's size (the rest is zeroed), e.g. the engine browser's query
`op1_type_list_query` (ports/rings/op1/patches.cc), which names the lists
for the modules that are on.

The image must stay inside 0x02c00000-0x02ffffff (docs/memory-map.md) and
must not overlap anything the app loads.

Usage:
    python tools/inject_app.py IN.ldr OUT.ldr IMAGE.elf [--fill-pattern 0xa5a5a5a5]
        [--set-word op1_module_mask=0x1fff] [--set-string op1_type_list_query="SELECT id, type FROM %s"]

As a library: inject(ldr_bytes, image_path, words={...}, strings={...}) -> (bytes, report).
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ldr_dump import LdrError, memory_runs, parse, serialize  # noqa: E402
from patch_app import apply_patch  # noqa: E402

REGION_START = 0x02c00000
# A small window in free region C, within pcrel24 reach of the stock code (region A is not): code that must JUMP.L
# straight back into the stock (the grid / spring tank thunks, core/sdram/coop_cb.ld .near). The region-C
# trampolines that l1_stereo_insert.py writes sit below it, at 0x01e00000..0x01e000ff.
NEAR_START = 0x01e00400
NEAR_END = 0x01e01000
# p6c: coop's cold text (everything the main loop and the key thread run) lives in region B, its own stock ICPLB page,
# so that only the hot page 0x02c00000 is marked LRUPRIO (core/sdram/coop_codeprio.cc, coop_cb.ld). 64 KB above the
# region's start: the 187 KB stock array ends at 0x027ffb04 (docs/memory-map.md), an overrun there must not reach it.
COLD_START = 0x02810000
COLD_END = 0x02c00000
REGION_END = 0x03000000
HEADER_MAGIC = 0x5831504f  # "OP1X"
DMA_CODE = 6               # as every other block in OP1_vdk.ldr

# Phase engine voice vtable (0x191113c): slot address -> stock function.
PHASE_RENDER = (0x01911150, 0x01a0bc5c)
PHASE_NOTE_ON = (0x01911158, 0x01a0be82)
PHASE_NOTE_OFF = (0x01911168, 0x01a1b5ec)

PT_LOAD = 1
SHT_SYMTAB = 2

PATCH_WORD32, PATCH_IMM16_LO, PATCH_IMM16_HI, PATCH_CALL24, PATCH_HALF16 = 0, 1, 2, 3, 4
PATCH_KIND_NAMES = {PATCH_WORD32: 'word', PATCH_IMM16_LO: 'imm16 lo', PATCH_IMM16_HI: 'imm16 hi',
                    PATCH_CALL24: 'call', PATCH_HALF16: 'insn16'}
MAX_PATCHES = 64


def read_elf(path):
    data = open(path, 'rb').read()
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        raise LdrError('{}: not a little-endian ELF32 file'.format(path))
    return data


def read_elf_segments(path):
    """Return [(vaddr, file_bytes, memsz)] for the PT_LOAD segments of an ELF32 LE file."""
    data = read_elf(path)
    (e_phoff,) = struct.unpack_from('<I', data, 28)
    e_phentsize, e_phnum = struct.unpack_from('<HH', data, 42)
    segments = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
            struct.unpack_from('<8I', data, e_phoff + i * e_phentsize)
        if p_type != PT_LOAD or p_memsz == 0:
            continue
        segments.append((p_vaddr, data[p_offset:p_offset + p_filesz], p_memsz))
    return sorted(segments)


def read_elf_symbols(path):
    """{name: (value, size)} from the ELF's symbol table."""
    data = read_elf(path)
    (e_shoff,) = struct.unpack_from('<I', data, 32)
    e_shentsize, e_shnum = struct.unpack_from('<HH', data, 46)
    sections = [struct.unpack_from('<10I', data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    symbols = {}
    for sh in sections:
        sh_type, sh_offset, sh_size, sh_link, sh_entsize = sh[1], sh[4], sh[5], sh[6], sh[9]
        if sh_type != SHT_SYMTAB:
            continue
        strtab = sections[sh_link]
        strings = data[strtab[4]:strtab[4] + strtab[5]]
        for i in range(sh_size // sh_entsize):
            st_name, st_value, st_size, st_info, st_other, st_shndx = \
                struct.unpack_from('<IIIBBH', data, sh_offset + i * sh_entsize)
            if not st_name:
                continue
            end = strings.index(b'\0', st_name)
            symbols[strings[st_name:end].decode('ascii', 'replace')] = (st_value, st_size)
    return symbols


def symbol_address(symbols, name):
    """The address and size of a C symbol (bfin prefixes an underscore)."""
    for candidate in ('_' + name, name):
        if candidate in symbols:
            return symbols[candidate]
    raise LdrError('symbol {} not found in the image'.format(name))


def set_words(segments, symbols, words):
    """Patch 32-bit words of the image's initialised data: {name: value}.
    Returns the new segments and [(name, address, old, new)]."""
    report = []
    segments = [(v, bytearray(p), m) for v, p, m in segments]
    for name, value in words.items():
        address, size = symbol_address(symbols, name)
        if size not in (0, 4):
            raise LdrError('{} is {} bytes, not a 32-bit word'.format(name, size))
        if address & 3:
            raise LdrError('{} at {:#x} is not word-aligned'.format(name, address))
        if not 0 <= value <= 0xffffffff:
            raise LdrError('{}: {:#x} is not a 32-bit value'.format(name, value))
        for vaddr, payload, _ in segments:
            if vaddr <= address and address + 4 <= vaddr + len(payload):
                offset = address - vaddr
                old = struct.unpack_from('<I', payload, offset)[0]
                struct.pack_into('<I', payload, offset, value)
                report.append((name, address, old, value))
                break
        else:
            raise LdrError('{} at {:#x} is not in the initialised data of the image'.format(name, address))
    return [(v, bytes(p), m) for v, p, m in segments], report


def set_strings(segments, symbols, strings):
    """Patch NUL-terminated char arrays of the image's initialised data:
    {name: text}. The text and its NUL must fit the symbol's size; the rest of
    the array is zeroed. Returns the new segments and [(name, address, old
    text, new text)]."""
    report = []
    segments = [(v, bytearray(p), m) for v, p, m in segments]
    for name, text in strings.items():
        address, size = symbol_address(symbols, name)
        encoded = text.encode('ascii')
        if size == 0:
            raise LdrError('{} has no size in the symbol table'.format(name))
        if len(encoded) + 1 > size:
            raise LdrError('{}: {} bytes do not fit the {}-byte array {}'.format(name, len(encoded) + 1, size, name))
        for vaddr, payload, _ in segments:
            if vaddr <= address and address + size <= vaddr + len(payload):
                offset = address - vaddr
                old = bytes(payload[offset:offset + size]).split(b'\0', 1)[0].decode('ascii', 'replace')
                payload[offset:offset + size] = encoded + bytes(size - len(encoded))
                report.append((name, address, old, text))
                break
        else:
            raise LdrError('{} at {:#x} is not in the initialised data of the image'.format(name, address))
    return [(v, bytes(p), m) for v, p, m in segments], report


def read_image(segments, addr, size):
    """Bytes of the image's initialised data at addr (must be in one segment)."""
    for vaddr, payload, _ in segments:
        if vaddr <= addr and addr + size <= vaddr + len(payload):
            return payload[addr - vaddr:addr - vaddr + size]
    raise LdrError('{:#x}: not in the initialised data of the image'.format(addr))


def read_patch_table(segments, table, count):
    """[(address, kind, stock, value)] from the image's patch table."""
    if count > MAX_PATCHES:
        raise LdrError('patch table has {} entries (max {})'.format(count, MAX_PATCHES))
    raw = read_image(segments, table, 16 * count)
    entries = [struct.unpack_from('<4I', raw, 16 * i) for i in range(count)]
    addresses = [e[0] for e in entries]
    if len(set(addresses)) != len(addresses):
        raise LdrError('patch table patches an address twice')
    # kind 4: the new halfword is the image's data at `value`.
    return [(a, k, s, struct.unpack('<H', read_image(segments, v, 2))[0] if k == PATCH_HALF16 else v)
            for a, k, s, v in entries]


def patch_bytes(entry):
    """(old, new) bytes for a patch table entry."""
    address, kind, stock, value = entry
    if kind == PATCH_WORD32:
        return struct.pack('<I', stock), struct.pack('<I', value)
    if kind in (PATCH_IMM16_LO, PATCH_IMM16_HI):
        if address & 1 or stock > 0xffff:
            raise LdrError('{:#x}: bad imm16 patch'.format(address))
        half = value & 0xffff if kind == PATCH_IMM16_LO else value >> 16
        return struct.pack('<H', stock), struct.pack('<H', half)
    if kind == PATCH_CALL24:
        return call24(address, stock), call24(address, value)
    if kind == PATCH_HALF16:
        if address & 1 or stock > 0xffff or value > 0xffff:
            raise LdrError('{:#x}: bad insn16 patch'.format(address))
        return struct.pack('<H', stock), struct.pack('<H', value)
    raise LdrError('{:#x}: unknown patch kind {}'.format(address, kind))


def call24(address, target):
    """Blackfin 'CALL pcrel24' at address to target: two halfwords,
    0xE300 | offset[23:16], offset[15:0], offset = (target - address) / 2."""
    if address & 1 or target & 1:
        raise LdrError('{:#x}: CALL to {:#x}: odd address'.format(address, target))
    offset = (target - address) >> 1
    if not -(1 << 23) <= offset < (1 << 23):
        raise LdrError('{:#x}: CALL to {:#x}: out of range'.format(address, target))
    offset &= 0xffffff
    return struct.pack('<HH', 0xe300 | (offset >> 16), offset & 0xffff)


def plain_block(target, payload):
    if len(payload) % 4:
        payload = payload + b'\0' * (4 - len(payload) % 4)
    return {'flags': 0, 'dmacode': DMA_CODE, 'target': target, 'count': len(payload),
            'argument': 0, 'payload': bytes(payload)}


def fill_block(target, count, value):
    return {'flags': 0x0100, 'dmacode': DMA_CODE, 'target': target, 'count': count,
            'argument': value, 'payload': b''}


def inject(ldr_bytes, image_path, words=None, fill_pattern=None, strings=None, region=None):
    """Inject IMAGE into the LDR bytes. Returns (new LDR bytes, report), where
    report is a dict: image_end, blocks, hooks, patches, words, strings (each
    a list of tuples as printed by the CLI) and lines (the printed text)."""
    region_start, region_end = region or (REGION_START, REGION_END)
    dxes = parse(ldr_bytes)
    if len(dxes) != 1:
        raise LdrError('expected a single-DXE app LDR, found {}'.format(len(dxes)))
    dxe = dxes[0]
    loaded = [(start, start + len(run)) for start, run in memory_runs(dxe)]

    segments = read_elf_segments(image_path)
    if not segments:
        raise LdrError('{}: no load segments'.format(image_path))
    word_report = []
    string_report = []
    if words or strings:
        symbols = read_elf_symbols(image_path)
        if words:
            segments, word_report = set_words(segments, symbols, words)
        if strings:
            segments, string_report = set_strings(segments, symbols, strings)
    new_blocks = []
    image_end = region_start
    header = None
    for vaddr, payload, memsz in segments:
        end = vaddr + memsz
        near = (NEAR_START <= vaddr and end <= NEAR_END) or (COLD_START <= vaddr and end <= COLD_END)
        if (vaddr < region_start or end > region_end) and not near:
            raise LdrError('segment {:#x}-{:#x} is outside {:#x}-{:#x}'.format(
                vaddr, end, region_start, region_end))
        for lo, hi in loaded:
            if vaddr < hi and lo < end:
                raise LdrError('segment {:#x}-{:#x} overlaps app data {:#x}-{:#x}'.format(
                    vaddr, end, lo, hi))
        if payload:
            new_blocks.append(plain_block(vaddr, payload))
            if vaddr == region_start:
                header = struct.unpack_from('<8I', payload, 0)
        filled = (len(payload) + 3) & ~3
        if memsz > filled:
            new_blocks.append(fill_block(vaddr + filled, (memsz - filled + 3) & ~3, 0))
        image_end = max(image_end, end)

    if header is None or header[0] != HEADER_MAGIC:
        raise LdrError('no OP1X header at {:#x}'.format(region_start))
    _, version, render, note_on, note_off, patch_table, patch_count = header[:7]
    patches = read_patch_table(segments, patch_table, patch_count) if patch_table else []

    if fill_pattern is not None:
        fill_start = (image_end + 4095) & ~4095
        new_blocks.append(fill_block(fill_start, region_end - fill_start, fill_pattern))

    final = [i for i, b in enumerate(dxe) if b['flags'] & 0x8000]
    if len(final) != 1:
        raise LdrError('expected exactly one FINAL block')
    dxe[final[0]:final[0]] = new_blocks

    hooks = [(PHASE_RENDER, render, 'render'), (PHASE_NOTE_ON, note_on, 'note on'),
             (PHASE_NOTE_OFF, note_off, 'note off')]
    for (slot, stock), target, name in hooks:
        if not target:
            continue
        if not region_start <= target < image_end:
            raise LdrError('{} entry {:#x} is outside the image'.format(name, target))
        apply_patch(dxes, slot, struct.pack('<I', stock), struct.pack('<I', target))
    for entry in patches:
        address = entry[0]
        if region_start <= address < region_end:
            raise LdrError('patch at {:#x} is inside our own region'.format(address))
        old, new = patch_bytes(entry)
        apply_patch(dxes, address, old, new)

    out = serialize(dxes)
    parse(out)  # re-check headers, checksums and the FIRST block's length

    lines = ['Image {} (header version {}): {:#x}-{:#x}, {} bytes'.format(
        os.path.basename(image_path), version, region_start, image_end, image_end - region_start)]
    for name, address, old, new in word_report:
        lines.append('  word   {} at {:#010x}: {:#010x} -> {:#010x}'.format(name, address, old, new))
    for name, address, old, new in string_report:
        lines.append('  string {} at {:#010x}: {!r} -> {!r}'.format(name, address, old, new))
    for b in new_blocks:
        lines.append('  + {:<5} {:#010x} {:#9x} bytes{}'.format(
            'FILL' if b['flags'] & 0x0100 else 'block', b['target'], b['count'],
            ' value {:#010x}'.format(b['argument']) if b['flags'] & 0x0100 else ''))
    hook_report = []
    for (slot, stock), target, name in hooks:
        if target:
            hook_report.append((slot, name, stock, target))
            lines.append('  vtable {:#010x} ({}): {:#010x} -> {:#010x}'.format(slot, name, stock, target))
    patch_report = []
    for entry in patches:
        old, new = patch_bytes(entry)
        patch_report.append((entry[0], PATCH_KIND_NAMES[entry[1]], old[::-1].hex(), new[::-1].hex(), entry[3]))
        lines.append('  patch  {:#010x} ({}): {} -> {}'.format(
            entry[0], PATCH_KIND_NAMES[entry[1]], old[::-1].hex(), new[::-1].hex()))
    report = {'image_end': image_end, 'version': version, 'blocks': new_blocks, 'hooks': hook_report,
              'patches': patch_report, 'words': word_report, 'strings': string_report, 'lines': lines}
    return out, report


def parse_region(text):
    start, end = text.split(':')
    start, end = int(start, 0), int(end, 0)
    if start & 3 or end <= start:
        raise argparse.ArgumentTypeError('region must be START:END, START word-aligned, END > START')
    return start, end


def parse_set_word(text):
    name, _, value = text.partition('=')
    if not name or not value:
        raise argparse.ArgumentTypeError('expected NAME=VALUE, got {!r}'.format(text))
    return name, int(value, 0)


def parse_set_string(text):
    name, _, value = text.partition('=')
    if not name:
        raise argparse.ArgumentTypeError('expected NAME=TEXT, got {!r}'.format(text))
    return name, value


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('ldr_in')
    parser.add_argument('ldr_out')
    parser.add_argument('image')
    parser.add_argument('--fill-pattern', type=lambda s: int(s, 0),
                        help='fill the rest of the region with this 32-bit value')
    parser.add_argument('--set-word', type=parse_set_word, action='append', default=[],
                        metavar='NAME=VALUE', help='set a 32-bit data word of the image (a symbol)')
    parser.add_argument('--set-string', type=parse_set_string, action='append', default=[],
                        metavar='NAME=TEXT', help='set a NUL-terminated char array of the image (a symbol)')
    parser.add_argument('--region', type=parse_region, default=None, metavar='START:END',
                        help='injection region (default {:#x}:{:#x}; region C is 0x01e00000:0x02000000)'.format(
                            REGION_START, REGION_END))
    args = parser.parse_args()

    ldr_in = open(args.ldr_in, 'rb').read()
    out, report = inject(ldr_in, args.image, dict(args.set_word), args.fill_pattern, dict(args.set_string),
                         args.region)
    with open(args.ldr_out, 'wb') as f:
        f.write(out)
    for line in report['lines']:
        print(line)
    print('{}: {} -> {} bytes'.format(args.ldr_out, len(ldr_in), len(out)))


if __name__ == '__main__':
    try:
        main()
    except LdrError as e:
        print('ERROR: {}'.format(e))
        sys.exit(1)
