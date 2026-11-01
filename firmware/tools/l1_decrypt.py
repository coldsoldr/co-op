#!/usr/bin/env python3
"""XTEA tools for the OP-1 app image's encrypted L1 blocks (firmware 246).

Facts (te-op1-cfw/docs/app-boot-chain.md): the Boot ROM's decrypt callback runs standard XTEA
(delta 0x9E3779B9, 32 rounds, 128-bit key from OTP page 0xD0) over the first 8 bytes of every
24-byte chunk of each CALLBACK block's payload. Nothing is authenticated.

Modes:
  analyze LDR PATCHES_JSON
      Check kennel's l1_patches.json against the stock OP1_vdk.ldr: every recorded ciphertext must
      match the stock bytes, and every changed 8-byte unit must sit on a 24-byte chunk boundary (the
      chunking model). Prints the known plaintext/ciphertext pairs the key check uses.
  check-key LDR PATCHES_JSON KEYHEX
      Try a 128-bit key (32 hex digits) in every word order and byte order against those pairs.
      Prints the arrangement(s) that decrypt them. Exit 1 when none does.
  decrypt LDR KEYHEX OUT_DIR [--swap --order --words]
      Decrypt every CALLBACK block and write OUT_DIR/<target>.bin per block plus one image per L1
      region (region_ffa00000.bin ...).
  compare LDR KEYHEX DUMP.bin [--base ADDR]
      Decrypt and byte-compare against a device dump of an L1 region (default base 0xffa00000).

No firmware bytes or keys are stored here; everything comes from the caller's own files.
"""

import argparse
import itertools
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ldr_dump import parse  # noqa: E402

DELTA = 0x9E3779B9
ROUNDS = 32
CHUNK = 24
ENC = 8
MASK = 0xFFFFFFFF
HEADER = 16


def xtea_decipher(v0, v1, key):
    """Standard XTEA decipher of one 64-bit block (two u32), key = 4 x u32."""
    s = (DELTA * ROUNDS) & MASK
    for _ in range(ROUNDS):
        v1 = (v1 - ((((v0 << 4) ^ (v0 >> 5)) + v0) ^ (s + key[(s >> 11) & 3]))) & MASK
        s = (s - DELTA) & MASK
        v0 = (v0 - ((((v1 << 4) ^ (v1 >> 5)) + v1) ^ (s + key[s & 3]))) & MASK
    return v0, v1


def xtea_encipher(v0, v1, key):
    s = 0
    for _ in range(ROUNDS):
        v0 = (v0 + ((((v1 << 4) ^ (v1 >> 5)) + v1) ^ (s + key[s & 3]))) & MASK
        s = (s + DELTA) & MASK
        v1 = (v1 + ((((v0 << 4) ^ (v0 >> 5)) + v0) ^ (s + key[(s >> 11) & 3]))) & MASK
    return v0, v1


def decrypt_unit(unit8, key, words='<'):
    v0, v1 = struct.unpack(words + 'II', unit8)
    p0, p1 = xtea_decipher(v0, v1, key)
    return struct.pack(words + 'II', p0, p1)


def encrypt_unit(unit8, key, words='<'):
    v0, v1 = struct.unpack(words + 'II', unit8)
    c0, c1 = xtea_encipher(v0, v1, key)
    return struct.pack(words + 'II', c0, c1)


def decrypt_payload(payload, key, words='<'):
    """Decrypt a CALLBACK block payload: the first 8 bytes of every 24-byte chunk."""
    out = bytearray(payload)
    for off in range(0, len(out) - ENC + 1, CHUNK):
        out[off:off + ENC] = decrypt_unit(bytes(out[off:off + ENC]), key, words)
    return bytes(out)


def encrypt_payload(payload, key, words='<'):
    out = bytearray(payload)
    for off in range(0, len(out) - ENC + 1, CHUNK):
        out[off:off + ENC] = encrypt_unit(bytes(out[off:off + ENC]), key, words)
    return bytes(out)


def callback_blocks(data):
    blocks = []
    for dxe in parse(data):
        for b in dxe:
            if b['flags'] & 0x0400 and not b['flags'] & 0x1000 and b['payload']:
                blocks.append(b)
    return blocks


def key_arrangements(keyhex):
    """All plausible arrangements of a 16-byte key: 2 byte orders x 4! word orders."""
    raw = bytes.fromhex(keyhex)
    if len(raw) != 16:
        raise SystemExit('key must be 16 bytes (32 hex digits)')
    for swap, fmt in (('le', '<4I'), ('be', '>4I')):
        base = list(struct.unpack(fmt, raw))
        for perm in itertools.permutations(range(4)):
            yield swap, perm, [base[i] for i in perm]


def pairs_from_patches(data, patches):
    """Encrypted 8-byte units kennel's patches change, with the plaintext bytes the pokes reveal.

    Returns ([(file_off, l1_addr, cipher_old, cipher_new, known_old, known_new)], problems) where
    known_* map a byte position 0..7 to its plaintext value.

    Each op in l1_patches.json records stock ciphertext (old) and patched ciphertext (new) at a file
    offset; a poke records the plaintext bytes (expect = stock plaintext, new = patched plaintext) at
    an L1 address. A single 8-byte XTEA unit can be touched by several short ops and pokes, so units
    are built from the image: for every encrypted unit the ops overlap, cipher_old is the stock image
    bytes, cipher_new is the stock bytes with every overlapping op applied, and the known plaintext
    bytes come from every overlapping poke.
    """
    blocks = callback_blocks(data)
    rec = json.load(open(patches))
    pokes = [(int(a, 16), bytes.fromhex(new), bytes.fromhex(exp)) for a, new, exp in rec['pokes']]
    problems = []

    # Index ops by file offset, after checking they match the stock image.
    ops = []
    for off, old_hex, new_hex in rec['ops']:
        old, new = bytes.fromhex(old_hex), bytes.fromhex(new_hex)
        if len(old) != len(new):
            problems.append('op at {:#x}: old and new differ in length'.format(off))
            continue
        if data[off:off + len(old)] != old:
            problems.append('op at file offset {:#x}: stock bytes differ from the recorded original'.format(off))
            continue
        ops.append((off, old, new))

    # The set of encrypted units (file offset of the unit start) any op touches.
    touched = set()
    for off, old, _new in ops:
        blk = None
        for b in blocks:
            start = b['offset'] + HEADER
            if start <= off < start + len(b['payload']):
                blk = b
                break
        if blk is None:
            continue  # a change in a plain region or an unencrypted block
        pay_start = blk['offset'] + HEADER
        for i in range(len(old)):
            rel = off + i - pay_start
            if rel % CHUNK < ENC:
                unit_start = (off + i) - (rel % CHUNK)
                touched.add((blk['offset'], unit_start))

    units = []
    for blk_off, unit_foff in sorted(touched):
        blk = next(b for b in blocks if b['offset'] == blk_off)
        pay_start = blk['offset'] + HEADER
        addr_unit = blk['target'] + (unit_foff - pay_start)
        c_old = bytearray(data[unit_foff:unit_foff + ENC])
        c_new = bytearray(c_old)
        for off, old, new in ops:
            for i in range(len(old)):
                if unit_foff <= off + i < unit_foff + ENC:
                    c_new[off + i - unit_foff] = new[i]
        known_old, known_new = {}, {}
        for a, pnew, pexp in pokes:
            for k in range(len(pexp)):
                if addr_unit <= a + k < addr_unit + ENC:
                    known_old[a + k - addr_unit] = pexp[k]
                    known_new[a + k - addr_unit] = pnew[k]
        units.append((unit_foff, addr_unit, bytes(c_old), bytes(c_new), known_old, known_new))
    return units, problems


def fmt_known(known):
    return ''.join('{:02x}'.format(known[i]) if i in known else '??' for i in range(ENC))


def cmd_analyze(args):
    data = open(args.ldr, 'rb').read()
    blocks = callback_blocks(data)
    print('{} CALLBACK blocks, {} bytes'.format(len(blocks), sum(len(b['payload']) for b in blocks)))
    for b in blocks:
        print('  file {:#09x} -> {:#010x} {:6} B'.format(b['offset'], b['target'], len(b['payload'])))
    units, problems = pairs_from_patches(data, args.patches)
    for p in problems:
        print('PROBLEM:', p)
    print('{} encrypted units with known plaintext bytes:'.format(len(units)))
    for foff, addr, c_old, c_new, k_old, k_new in units:
        print('  L1 {:#010x} (file {:#x}): cipher {} -> {}; plain {} -> {}'.format(
            addr, foff, c_old.hex(), c_new.hex(), fmt_known(k_old), fmt_known(k_new)))
    if problems:
        sys.exit(1)


def check_units(units, key, words):
    """True when every unit's known plaintext bytes decrypt as recorded.

    XTEA is a 64-bit block cipher, so decrypt(c_old) and decrypt(c_new) differ in every byte once any
    ciphertext byte changes; the only constraint is that the bytes the pokes name decrypt correctly,
    for both the stock and the patched ciphertext.
    """
    checked = 0
    for _, _, c_old, c_new, k_old, k_new in units:
        p_old = decrypt_unit(c_old, key, words)
        p_new = decrypt_unit(c_new, key, words)
        for i, v in k_old.items():
            checked += 1
            if p_old[i] != v:
                return False
        for i, v in k_new.items():
            checked += 1
            if p_new[i] != v:
                return False
    return checked > 0


def cmd_check_key(args):
    data = open(args.ldr, 'rb').read()
    units, problems = pairs_from_patches(data, args.patches)
    if problems:
        for p in problems:
            print('PROBLEM:', p)
        sys.exit(1)
    hits = []
    for swap, perm, key in key_arrangements(args.key):
        for words in ('<', '>'):
            if check_units(units, key, words):
                hits.append((swap, perm, words, key))
    if not hits:
        print('no arrangement of this key decrypts the known units: wrong key (or wrong chunk model)')
        sys.exit(1)
    for swap, perm, words, key in hits:
        print('MATCH: --swap {} --order {} --words "{}"  (key words {})'.format(
            swap, ''.join(str(p) for p in perm), words, ' '.join('{:08x}'.format(k) for k in key)))


def resolve_key(args):
    raw = bytes.fromhex(args.key)
    base = list(struct.unpack(('<' if args.swap == 'le' else '>') + '4I', raw))
    return [base[int(c)] for c in args.order], args.words


def cmd_decrypt(args):
    data = open(args.ldr, 'rb').read()
    key, words = resolve_key(args)
    os.makedirs(args.out, exist_ok=True)
    regions = {}
    for b in callback_blocks(data):
        plain = decrypt_payload(b['payload'], key, words)
        open(os.path.join(args.out, '{:08x}.bin'.format(b['target'])), 'wb').write(plain)
        base = b['target'] & 0xFFF00000
        reg = regions.setdefault(base, bytearray())
        rel = b['target'] - base
        if len(reg) < rel + len(plain):
            reg.extend(b'\0' * (rel + len(plain) - len(reg)))
        reg[rel:rel + len(plain)] = plain
    for base, reg in sorted(regions.items()):
        path = os.path.join(args.out, 'region_{:08x}.bin'.format(base))
        open(path, 'wb').write(bytes(reg))
        print('{}: {} bytes'.format(path, len(reg)))


def cmd_compare(args):
    data = open(args.ldr, 'rb').read()
    key, words = resolve_key(args)
    dump = open(args.dump, 'rb').read()
    mismatches = covered = 0
    for b in callback_blocks(data):
        if not args.base <= b['target'] < args.base + len(dump):
            continue
        plain = decrypt_payload(b['payload'], key, words)
        rel = b['target'] - args.base
        got = dump[rel:rel + len(plain)]
        covered += min(len(plain), len(got))
        for i in range(min(len(plain), len(got))):
            if plain[i] != got[i]:
                if mismatches < 20:
                    print('mismatch at {:#010x}: decrypted {:02x}, dump {:02x}'.format(
                        b['target'] + i, plain[i], got[i]))
                mismatches += 1
    print('{} bytes compared, {} mismatches'.format(covered, mismatches))
    sys.exit(1 if mismatches else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd')
    sub.required = True
    p = sub.add_parser('analyze')
    p.add_argument('ldr')
    p.add_argument('patches')
    p.set_defaults(fn=cmd_analyze)
    p = sub.add_parser('check-key')
    p.add_argument('ldr')
    p.add_argument('patches')
    p.add_argument('key')
    p.set_defaults(fn=cmd_check_key)
    for name, fn in (('decrypt', cmd_decrypt), ('compare', cmd_compare)):
        p = sub.add_parser(name)
        p.add_argument('ldr')
        p.add_argument('key')
        if name == 'decrypt':
            p.add_argument('out')
        else:
            p.add_argument('dump')
            p.add_argument('--base', type=lambda s: int(s, 0), default=0xFFA00000)
        p.add_argument('--swap', choices=('le', 'be'), default='le', help='byte order of the key words')
        p.add_argument('--order', default='0123', help='key word permutation (from check-key)')
        p.add_argument('--words', choices=('<', '>'), default='<', help='byte order inside an 8-byte unit')
        p.set_defaults(fn=fn)
    args = ap.parse_args()
    args.fn(args)


if __name__ == '__main__':
    main()
