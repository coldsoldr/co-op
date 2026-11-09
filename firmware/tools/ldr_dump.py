#!/usr/bin/env python3
"""Parse Blackfin BF52x LDR boot streams (te-boot.ldr, OP1_vdk.ldr).

Every block starts with a 16-byte little-endian header:
    block code | target address | byte count | argument
The block code holds the DMA code (bits 0-3), the flags (bits 4-15), a header
checksum (bits 16-23, chosen so the XOR of all 16 header bytes is 0) and the
0xAD signature (bits 24-31). FILL blocks carry no payload and fill `count`
bytes at `target` with the 32-bit `argument`. FIRST blocks start a DXE and
their argument is the byte offset from the end of that header to the next DXE.

Usage:
    python tools/ldr_dump.py FILE.ldr [--blocks] [--dump OUT_DIR] [--elf OUT_FILE [--dxe N]]
"""

import argparse
import json
import os
import struct
import sys
from functools import reduce

HEADER_SIZE = 16
SIGNATURE = 0xAD

FLAGS = {
    0x0010: 'SAFE',
    0x0020: 'AUX',
    0x0100: 'FILL',
    0x0200: 'QUICKBOOT',
    0x0400: 'CALLBACK',
    0x0800: 'INIT',
    0x1000: 'IGNORE',
    0x2000: 'INDIRECT',
    0x4000: 'FIRST',
    0x8000: 'FINAL',
}

# BF52x memory map (ADSP-BF52x hardware reference)
REGIONS = [
    (0x00000000, 0x20000000, 'SDRAM'),
    (0x20000000, 0x20400000, 'Async memory'),
    (0xEF000000, 0xEF008000, 'Boot ROM'),
    (0xFF800000, 0xFF808000, 'L1 data A'),
    (0xFF900000, 0xFF908000, 'L1 data B'),
    (0xFFA00000, 0xFFA0C000, 'L1 instruction SRAM'),
    (0xFFA10000, 0xFFA14000, 'L1 instruction SRAM/cache'),
    (0xFFB00000, 0xFFB01000, 'L1 scratchpad'),
    (0xFFC00000, 0x100000000, 'MMR'),
]


class LdrError(Exception):
    pass


def region_name(addr):
    for start, end, name in REGIONS:
        if start <= addr < end:
            return name
    return 'unknown'


def flag_names(flags):
    return '|'.join(name for bit, name in FLAGS.items() if flags & bit) or '-'


def parse(data):
    """Return a list of DXEs, each a list of block dicts. Raises LdrError on malformed input."""
    dxes = []
    off = 0
    next_dxe = None
    while off < len(data):
        if off + HEADER_SIZE > len(data):
            raise LdrError('Truncated header at {:#x}'.format(off))
        code, target, count, arg = struct.unpack_from('<IIII', data, off)
        if code >> 24 != SIGNATURE:
            raise LdrError('Bad signature {:#04x} at {:#x}'.format(code >> 24, off))
        if reduce(lambda a, b: a ^ b, data[off:off + HEADER_SIZE]):
            raise LdrError('Header checksum mismatch at {:#x}'.format(off))

        flags = code & 0xFFF0
        payload_len = 0 if flags & 0x0100 else count
        payload_off = off + HEADER_SIZE
        if payload_off + payload_len > len(data):
            raise LdrError('Payload overruns file at {:#x}'.format(off))

        if flags & 0x4000:
            if next_dxe is not None and next_dxe != off:
                raise LdrError('DXE at {:#x} but previous FIRST block pointed to {:#x}'.format(off, next_dxe))
            next_dxe = payload_off + arg
            dxes.append([])
        elif not dxes:
            raise LdrError('First block is not a FIRST block')

        dxes[-1].append({
            'offset': off,
            'code': code,
            'dmacode': code & 0xF,
            'flags': flags,
            'target': target,
            'count': count,
            'argument': arg,
            'payload': data[payload_off:payload_off + payload_len],
        })
        off = payload_off + payload_len

    if next_dxe is not None and next_dxe != len(data):
        raise LdrError('Last FIRST block points to {:#x} but file ends at {:#x}'.format(next_dxe, len(data)))
    return dxes


def serialize(dxes):
    """Rebuild an LDR stream from parsed DXEs.

    Recomputes each header checksum and each FIRST block's next-DXE offset, so
    blocks can be edited, added or removed. Uses 'flags', 'dmacode', 'target',
    'count', 'argument' and 'payload'; for non-FILL blocks 'count' must equal
    len(payload).
    """
    out = bytearray()
    for dxe in dxes:
        start = len(out)
        for block in dxe:
            fill = block['flags'] & 0x0100
            if not fill and block['count'] != len(block['payload']):
                raise LdrError('Block at {:#x}: count {:#x} != payload length {:#x}'.format(
                    block['target'], block['count'], len(block['payload'])))
            arg = block['argument']
            if block['flags'] & 0x4000:
                arg = 0  # patched below once the DXE length is known
            code = (SIGNATURE << 24) | block['flags'] | block['dmacode']
            header = bytearray(struct.pack('<IIII', code, block['target'], block['count'], arg))
            header[2] = reduce(lambda a, b: a ^ b, header)  # hdrchk byte makes the XOR of all 16 bytes 0
            out += header
            if not fill:
                out += block['payload']
        # FIRST block argument: bytes from the end of its header to the next DXE
        first_arg = len(out) - (start + HEADER_SIZE)
        header = out[start:start + HEADER_SIZE]
        header[2] = 0
        header[12:16] = struct.pack('<I', first_arg)
        header[2] = reduce(lambda a, b: a ^ b, header)
        out[start:start + HEADER_SIZE] = header
    return bytes(out)


def loaded_bytes(block):
    """Bytes the block places in memory, or None if it loads nothing."""
    if block['flags'] & (0x1000 | 0x4000):
        return None
    if block['flags'] & 0x0100:
        pattern = struct.pack('<I', block['argument'])
        return (pattern * (block['count'] // 4 + 1))[:block['count']]
    return block['payload'] or None


def memory_runs(dxe):
    """Merge the blocks of one DXE into contiguous (start, bytearray) runs, applied in load order."""
    runs = []
    for block in dxe:
        chunk = loaded_bytes(block)
        if chunk is None:
            continue
        start = block['target']
        for run_start, run in runs:
            if run_start <= start <= run_start + len(run):
                pos = start - run_start
                run[pos:pos + len(chunk)] = chunk
                break
        else:
            runs.append((start, bytearray(chunk)))
    return sorted(runs, key=lambda r: r[0])


def print_summary(path, dxes, show_blocks):
    total = sum(len(d) for d in dxes)
    print('{}: {} bytes, {} DXE(s), {} blocks, all header checksums OK'.format(
        path, os.path.getsize(path), len(dxes), total))
    for i, dxe in enumerate(dxes):
        inits = [b for b in dxe if b['flags'] & 0x0800]
        final = [b for b in dxe if b['flags'] & 0x8000]
        print('\nDXE {} at {:#x}: {} blocks{}{}'.format(
            i, dxe[0]['offset'], len(dxe),
            ', INIT call -> {:#010x}'.format(inits[0]['target']) if inits else '',
            ', FINAL -> {:#010x}'.format(final[0]['target']) if final else ''))
        if show_blocks:
            for b in dxe:
                print('    {:#9x}  {:<18} dma={} target={:#010x} count={:#9x} arg={:#010x}'.format(
                    b['offset'], flag_names(b['flags']), b['dmacode'], b['target'], b['count'], b['argument']))
        print('    Memory loaded:')
        for start, run in memory_runs(dxe):
            print('      {:#010x}-{:#010x}  {:>9} bytes  {}'.format(
                start, start + len(run) - 1, len(run), region_name(start)))
        callbacks = [b for b in dxe if b['flags'] & 0x0400 and not b['flags'] & 0x1000]
        if callbacks:
            print('    WARNING: {} CALLBACK blocks ({} bytes, {}) are transformed by the loader at boot;'.format(
                len(callbacks), sum(b['count'] for b in callbacks),
                ', '.join(sorted(set(region_name(b['target']) for b in callbacks)))))
            print('             the dumped bytes for those ranges are the raw (scrambled) payload, not the final code.')


def dump(path, dxes, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    base = os.path.splitext(os.path.basename(path))[0]
    index = []
    for i, dxe in enumerate(dxes):
        for start, run in memory_runs(dxe):
            name = '{}_dxe{}_{:08x}.bin'.format(base, i, start)
            with open(os.path.join(out_dir, name), 'wb') as f:
                f.write(run)
            index.append({'file': name, 'dxe': i, 'address': '{:#010x}'.format(start),
                          'size': len(run), 'region': region_name(start)})
    with open(os.path.join(out_dir, base + '_map.json'), 'w') as f:
        json.dump(index, f, indent=2)
    print('\nWrote {} memory images and {}_map.json to {}'.format(len(index), base, out_dir))


def write_elf(dxes, out_file, dxe_index):
    """Write one DXE's memory image as a Blackfin ELF (one PT_LOAD + section per run, entry 0xffa00000)."""
    runs = memory_runs(dxes[dxe_index])
    final = [b for b in dxes[dxe_index] if b['flags'] & 0x8000]
    entry = final[0]['target'] if final else 0xFFA00000
    em_blackfin = 106
    ehdr_size, phdr_size, shdr_size = 52, 32, 40

    names = [''] + ['.mem_{:08x}'.format(start) for start, _ in runs] + ['.shstrtab']
    shstrtab = b'\0'.join(n.encode() for n in names) + b'\0'
    name_offsets = []
    pos = 0
    for n in names:
        name_offsets.append(pos)
        pos += len(n) + 1

    data_off = ehdr_size + phdr_size * len(runs)
    offsets = []
    for _, run in runs:
        offsets.append(data_off)
        data_off += len(run)
    shstrtab_off = data_off
    shdr_off = (shstrtab_off + len(shstrtab) + 3) & ~3

    out = bytearray()
    out += b'\x7fELF' + bytes([1, 1, 1, 0]) + bytes(8)
    out += struct.pack('<HHIIIIIHHHHHH', 2, em_blackfin, 1, entry, ehdr_size, shdr_off, 0,
                       ehdr_size, phdr_size, len(runs), shdr_size, len(names), len(names) - 1)
    for (start, run), off in zip(runs, offsets):
        out += struct.pack('<IIIIIIII', 1, off, start, start, len(run), len(run), 7, 4)
    for _, run in runs:
        out += run
    out += shstrtab
    out += bytes(shdr_off - len(out))
    out += bytes(shdr_size)
    for i, ((start, run), off) in enumerate(zip(runs, offsets)):
        # SHT_PROGBITS, SHF_WRITE|SHF_ALLOC|SHF_EXECINSTR
        out += struct.pack('<IIIIIIIIII', name_offsets[i + 1], 1, 7, start, off, len(run), 0, 0, 4, 0)
    out += struct.pack('<IIIIIIIIII', name_offsets[-1], 3, 0, 0, shstrtab_off, len(shstrtab), 0, 0, 1, 0)

    with open(out_file, 'wb') as f:
        f.write(out)
    print('\nWrote {} (DXE {}, {} sections, entry {:#010x})'.format(out_file, dxe_index, len(runs), entry))


def main():
    parser = argparse.ArgumentParser(description='Parse and dump Blackfin BF52x LDR files.')
    parser.add_argument('ldr', help='path to an .ldr file')
    parser.add_argument('--blocks', action='store_true', help='list every block')
    parser.add_argument('--dump', metavar='OUT_DIR', help='write loaded memory images to OUT_DIR')
    parser.add_argument('--elf', metavar='OUT_FILE', help='write one DXE as a Blackfin ELF for objdump/gdb sim')
    parser.add_argument('--dxe', type=int, default=-1, help='DXE index for --elf (default: last)')
    args = parser.parse_args()

    with open(args.ldr, 'rb') as f:
        data = f.read()
    try:
        dxes = parse(data)
    except LdrError as e:
        print('ERROR: {}'.format(e))
        sys.exit(1)

    print_summary(args.ldr, dxes, args.blocks)
    if args.dump:
        dump(args.ldr, dxes, args.dump)
    if args.elf:
        write_elf(dxes, args.elf, args.dxe % len(dxes))


if __name__ == '__main__':
    main()
