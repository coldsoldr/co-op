#!/usr/bin/env python3
"""Take TE's Box2D physics out of an OP-1 R246 app image, with its only users (Tombola, the hidden Lander game).

The edits are op1-sdk's `nophysics` feature (BertBrandstifter, MIT; vendored unchanged in
third_party/op1-sdk/nophysics/feature.json, which this tool reads): 40 patches (30 boot-time static initialisers
of Box2D and 6 Lander / Tombola vtable slots pointed at the first RTS of the blanked range, the Lander combo
compare, the SeqIds "tombola" -> pattern, the player factory's case 2 -> case 0, a JUMP.S over the Tombola physics
setup) and 17 ranges (Box2D, dead Lander and Tombola code, 144,888 bytes) overwritten with RTS. coop's notes:
docs/nophysics.md; the reachability proof against coop's own image: tools/nophysics_prove.py.

Every patch checks its stock bytes and every blanked range the sha256 of its stock bytes before anything is
written, so the tool refuses an image that is not R246 or in which an earlier step (coop's inject / L1 inserts)
already changed any byte it would touch. Only plain SDRAM blocks are written; the tool works on the plaintext and
on the encrypted LDR alike (it touches no CALLBACK block), and keeps every block's file offset (same sizes).

Usage:
    python3 tools/nophysics.py IN.ldr OUT.ldr [--feature FEATURE.json]     (IN and OUT may be the same file)
    python3 tools/nophysics.py --check IN.ldr                               (is IN stock at every site?)
    python3 tools/nophysics.py --verify OUT.ldr                             (is every edit in OUT?)

As a library: apply(ldr_bytes) -> (bytes, report lines); edits() -> [(addr, stock, new, what)] / blanks.
"""

import argparse
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ldr_dump import LdrError, loaded_bytes, parse, serialize  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
FEATURE = os.path.join(HERE, '..', 'third_party', 'op1-sdk', 'nophysics', 'feature.json')
R246_OP1_SHA256 = 'c5315218f825f143b415ca554516541898abee843d3a236df0b54c04e1fb13a9'   # TE's op1_246.op1 (both repos)
RTS = b'\x10\x00'                       # Blackfin RTS, 16-bit
RTS_TARGET = 0x0191FE8C                 # the first RTS of the blanked Box2D range: where the 36 pointers lead
REFUSED = 0x0400 | 0x0100 | 0x1000 | 0x4000   # CALLBACK, FILL, IGNORE, FIRST


def load_feature(path=FEATURE):
    feat = json.load(open(path))
    if feat['firmware'].get('R246') != R246_OP1_SHA256:
        raise LdrError('{}: not the R246 feature this tool was checked against'.format(path))
    patches = [(int(p['addr'], 0), bytes.fromhex(p['expect']), bytes.fromhex(p['bytes']), p['what'])
               for p in feat['hooks']['patches']]
    blanks = [(int(b['addr'], 0), int(b['end'], 0), b['expect'].split(':', 1)[1], b['what'])
              for b in feat['hooks']['blank']]
    for a, old, new, _ in patches:
        if len(old) != len(new) or a & 1:
            raise LdrError('{:#x}: bad patch entry'.format(a))
    for lo, hi, _, _ in blanks:
        if lo & 1 or hi & 1 or hi <= lo:
            raise LdrError('{:#x}..{:#x}: bad blank entry'.format(lo, hi))
    # nothing may overlap anything else (a patch inside a blank would be ambiguous)
    spans = sorted([(a, a + len(o)) for a, o, _, _ in patches] + [(lo, hi) for lo, hi, _, _ in blanks])
    for (a0, a1), (b0, b1) in zip(spans, spans[1:]):
        if b0 < a1:
            raise LdrError('feature edits overlap at {:#x}'.format(b0))
    return patches, blanks


def _blocks_for(dxe, lo, hi, fill_ok=False):
    """The blocks covering [lo, hi) in order; refuses CALLBACK / IGNORE / gaps, and FILL unless fill_ok."""
    out, at = [], lo
    for b in sorted((b for b in dxe if loaded_bytes(b) is not None), key=lambda b: b['target']):
        start, end = b['target'], b['target'] + b['count']
        if end <= at or start >= hi:
            continue
        if start > at:
            break
        if b['flags'] & (REFUSED & ~(0x0100 if fill_ok else 0)):
            raise LdrError('{:#x}..{:#x} touches a CALLBACK/FILL/IGNORE block at {:#x}; refusing'.format(lo, hi, start))
        out.append(b)
        at = end
        if at >= hi:
            return out
    raise LdrError('{:#x}..{:#x}: not covered by plain blocks (gap at {:#x})'.format(lo, hi, at))


def read_span(dxe, lo, hi):
    data = bytearray()
    for b in _blocks_for(dxe, lo, hi, fill_ok=True):
        s = max(lo, b['target']) - b['target']
        e = min(hi, b['target'] + b['count']) - b['target']
        data += loaded_bytes(b)[s:e]
    return bytes(data)


def write_span(dxe, lo, new):
    """Write `new` at lo. A FILL block (Box2D holds one: 32 bytes of a repeated instruction at 0x0193252c) may only be
    met wholly inside the span and only when `new` repeats one 32-bit word there: its fill value becomes that word."""
    hi = lo + len(new)
    for b in _blocks_for(dxe, lo, hi, fill_ok=True):
        s = max(lo, b['target'])
        e = min(hi, b['target'] + b['count'])
        if b['flags'] & 0x0100:
            word = new[s - lo:s - lo + 4]
            if s != b['target'] or e != b['target'] + b['count'] or b['count'] % 4 \
                    or new[s - lo:e - lo] != word * (b['count'] // 4):
                raise LdrError('{:#x}: FILL block at {:#x} not wholly covered by a repeated word'.format(lo, s))
            b['argument'] = int.from_bytes(word, 'little')
            continue
        p = bytearray(b['payload'])
        p[s - b['target']:e - b['target']] = new[s - lo:e - lo]
        b['payload'] = bytes(p)


def _dxe(ldr_bytes):
    dxes = parse(ldr_bytes)
    if len(dxes) != 1:
        raise LdrError('expected a single-DXE app LDR, found {}'.format(len(dxes)))
    return dxes


def check(ldr_bytes, feature=FEATURE, applied=False):
    """[(what, ok, detail)] for every site: stock bytes (applied=False) or the edit (applied=True)."""
    patches, blanks = load_feature(feature)
    dxe = _dxe(ldr_bytes)[0]
    res = []
    for a, old, new, what in patches:
        got = read_span(dxe, a, a + len(old))
        want = new if applied else old
        res.append(('patch {:#010x}'.format(a), got == want, '{} (want {}): {}'.format(got.hex(), want.hex(), what)))
    for lo, hi, sha, what in blanks:
        got = read_span(dxe, lo, hi)
        ok = got == RTS * ((hi - lo) // 2) if applied else hashlib.sha256(got).hexdigest() == sha
        res.append(('blank {:#010x}..{:#010x}'.format(lo, hi), ok, what))
    return res


def apply(ldr_bytes, feature=FEATURE):
    """Check every site, then write every edit. Returns (new LDR bytes, report lines)."""
    bad = [r for r in check(ldr_bytes, feature) if not r[1]]
    if bad:
        raise LdrError('not stock at {} site(s), first: {} {}'.format(len(bad), bad[0][0], bad[0][2]))
    patches, blanks = load_feature(feature)
    dxes = _dxe(ldr_bytes)
    dxe = dxes[0]
    for a, old, new, _ in patches:
        write_span(dxe, a, new)
    for lo, hi, _, _ in blanks:
        write_span(dxe, lo, RTS * ((hi - lo) // 2))
    out = serialize(dxes)
    parse(out)
    if len(out) != len(ldr_bytes):
        raise LdrError('size changed ({} -> {})'.format(len(ldr_bytes), len(out)))
    bad = [r for r in check(out, feature, applied=True) if not r[1]]
    if bad:
        raise LdrError('edit missing after writing: {}'.format(bad[0][0]))
    diff = sum(1 for x, y in zip(ldr_bytes, out) if x != y)
    blanked = sum(hi - lo for lo, hi, _, _ in blanks)
    lines = ['nophysics (op1-sdk, MIT): {} patches, {} ranges blanked with RTS ({:,} bytes), {:,} file bytes changed'
             .format(len(patches), len(blanks), blanked, diff)]
    return out, lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('ldr_in')
    ap.add_argument('ldr_out', nargs='?')
    ap.add_argument('--feature', default=FEATURE)
    ap.add_argument('--check', action='store_true', help='report whether every site holds the stock bytes')
    ap.add_argument('--verify', action='store_true', help='report whether every edit is in the image')
    a = ap.parse_args()
    data = open(a.ldr_in, 'rb').read()
    if a.check or a.verify:
        res = check(data, a.feature, applied=a.verify)
        for what, ok, detail in res:
            if not ok:
                print('NOT {}: {} {}'.format('applied' if a.verify else 'stock', what, detail))
        n = sum(1 for r in res if r[1])
        print('{}: {}/{} sites {}'.format(a.ldr_in, n, len(res), 'applied' if a.verify else 'stock'))
        sys.exit(0 if n == len(res) else 1)
    if not a.ldr_out:
        ap.error('OUT.ldr is required')
    out, lines = apply(data, a.feature)
    with open(a.ldr_out, 'wb') as f:
        f.write(out)
    for l in lines:
        print(l)


if __name__ == '__main__':
    try:
        main()
    except LdrError as e:
        print('ERROR: {}'.format(e))
        sys.exit(1)
