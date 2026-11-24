#!/usr/bin/env python3
"""Prove that nothing in a coop image can reach the code nophysics blanks (tools/nophysics.py).

A port of op1-sdk's features/nophysics/prove.py (BertBrandstifter, MIT; third_party/op1-sdk/nophysics/) to coop's
tools, run against coop's own image rather than the stock one: the image is everything the OP-1 runs, so the
argument covers TE's code as coop leaves it (coop's SDRAM site patches), coop's code (region A .text, the region-C
trampolines and .near thunks), the whole L1 instruction SRAM decrypted (stock audio core plus coop's L1 inserts)
and every loaded data word, coop's included.

    python3 -I tools/nophysics_prove.py IMAGE.ldr [--objdump PATH] [--keep-dis DIR]

IMAGE.ldr is a plaintext coop image *without* nophysics (tools/build_image.sh with NOPHYSICS=0): the stock bytes at
the sites nophysics changes are still there, and the references they make are taken out as feature.json says
(the redirected pointers, the patched switch entry, the code the patched JUMP.S skips). The disassembly is made
here from the image with bfin-elf-objdump and kept only in a temporary directory (it is TE code: never commit it).

Method (greatest fixpoint, as op1-sdk's): the candidate code (Box2D, the Lander and Tombola regions) is cut into
chunks at instruction boundaries nothing can fall through (previous instruction RTS / RTI / JUMP); every chunk
starts dead and becomes live when anything outside the dead set refers to any byte of it: a CALL / JUMP, a constant
built with X.H + X.L of one register within 10 instructions (every pairing counts), or any 32-bit word, at any
halfword offset, of the loaded image outside the L1 instruction SRAM. Repeat until stable; every blanked range must
lie inside dead chunks. An X.H with no X.L near it whose 64 KB window touches blanked code fails the proof unless
it is listed below with its evidence.

Assumptions taken over from op1-sdk, each with its evidence there and re-checked here where the image allows:
NOT_POINTERS (words in data that only look like code addresses), BOX2D_TABLES (Box2D's own vtables and switch
tables, checked: only Box2D code loads them), EXIT_RECORDS (TE's destructor-at-exit records, run only by exit()).
op1-sdk's L1 windows (calls whose low half sat in 8-byte blocks it could not decrypt) do not apply: coop's L1 is
decrypted whole, so every L1 call is resolved or fails the proof here.
"""

import argparse
import bisect
import collections
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ldr_dump import parse, memory_runs, write_elf  # noqa: E402
import nophysics  # noqa: E402

STOCK_CODE = (0x0191209E, 0x01AA8000)    # TE's SDRAM code (before it: ctor table, rodata, vtables)
L1_CODE = (0xFFA00000, 0xFFA0C000)       # L1 instruction SRAM, decrypted (stock + coop's L1 inserts)
COOP_C = (0x01E00000, 0x01E01000)        # region C: coop's SDRAM trampolines + the .near thunks
COOP_A = 0x02C00000                      # region A: coop's .text from here (its end read from the image)
CANDIDATES = [(0x019C3000, 0x019C9A00),  # LanderScreen.cpp and its neighbours
              (0x01A63E00, 0x01A668F4)]  # Tombola*.cpp and neighbours
BOX2D = (0x0191FE8C, 0x0193BFF0)         # ext/Box2D (b2BlockAllocator .. b2World), blanked
BOX2D_ALL = (0x0191FE8C, 0x0193C12C)     # + b2WorldCallbacks.cpp, kept (TombolaScreen's boot init uses it)
NOT_POINTERS = {0x018B0942, 0x018B66C6, 0x018BA646, 0x018D1604, 0x018D17F8, 0x018DB832, 0x018EB014,
                0x018EB02E, 0x018EB0DA, 0x018EB188, 0x018EB2E6, 0x018ECD42, 0x018ECE10, 0x018ECF2C,
                0x018ECF98, 0x018ECF9C, 0x018ECFAA, 0x019B2206, 0x01A214C4, 0x01A22BB6,
                0xFF90141E, 0xFF905B14}
BOX2D_TABLES = [(0x0190EB00, 0x0190F178), (0x018C58C0, 0x018C5940)]
EXIT_RECORDS = [(0x018C18F0, 0x018C1980), (0xFF907930, 0xFF907AB0), (0xFF9003D4, 0xFF9003DC)]
TABLES = [(0x018E1078, 0x018E1088),      # switch in 0x019C5E2C (Lander)
          (0x018E1144, 0x018E1164),      # switch in 0x019C8D38 (Lander draw)
          (0x018FDDA4, 0x018FDDB8),      # switch in 0x01A63E50 (Tombola knobs)
          (0x01911E0C, 0x01911E44)]      # TombolaPlayer vtable (only its constructor loads it)
SWITCH_CASES = {0x018F2F00: 0x01A0D150}  # patched switch entry -> end of the case block it led to
# coop data objects (by ELF symbol, so a rebuild that moves them keeps the exemption) whose words only look like
# pointers into TE's code; each checked by hand. Anything else of coop's that looks like one fails the proof.
COOP_NUMBER_TABLES = {
    '__ZL5ms_of': 'coop_bend.cc: uint16_t ms_of[129], the bend ramp times in ms (0x0180, 0x0192 side by side '
                  'read as 0x01920180)',
}

LINE = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$')
BRANCH = re.compile(r'(CALL|JUMP(?:\.[SLX])?) 0x0x([0-9a-f]+)')
HALF = re.compile(r'\b([PRIBLM][0-7])\.([LH]) = (0x[0-9a-f]+);')
PAIR = 10
LOOSE = False     # --loose-pairs
TERM = re.compile(r'^(RTS|RTI|RTX|RTN|JUMP \(P[0-5]\)|JUMP(\.[SL])? 0x)')


def overwritten_unread(insns, halves, j, reg):
    """True when the X.L at instruction j is followed by an X.H of the same register before anything else names the
    register: the value (this X.H):(that X.L) then never exists where code could use it. Only that one case is
    dropped (the false pairing of an X.L with the X.H before it in `X.H = a; CALL (X); X.L = b; X.H = c`)."""
    mention = re.compile(r'\b%s\b' % reg)
    for k in range(j + 1, min(len(insns), j + PAIR + 1)):
        if any(r2 == reg and p2 == 'H' for r2, p2, _ in halves[k]):
            rest = re.sub(r'\b%s\.H = 0x[0-9a-f]+;' % reg, '', insns[k][1].split('/*')[0])
            return not mention.search(rest)
        if mention.search(insns[k][1].split('/*')[0]):
            return False
    return False


def disasm(path):
    """(instructions, references, windows) as op1-sdk's prove.py."""
    insns, refs, windows = [], [], []
    for l in open(path, errors='replace'):
        m = LINE.match(l)
        if m:
            insns.append((int(m.group(1), 16), m.group(3)))
    halves = [HALF.findall(t) for a, t in insns]
    for i, (a, t) in enumerate(insns):
        for k, v in BRANCH.findall(t):
            refs.append((int(v, 16), a, k))
        for reg, part, v in halves[i]:
            if part != 'H':
                continue
            lows = set()
            for fwd, rng in ((False, range(i - 1, max(-1, i - PAIR - 1), -1)),
                             (True, range(i + 1, min(len(insns), i + PAIR + 1)))):
                found = False
                for j in rng:
                    for r2, p2, v2 in halves[j]:
                        if r2 == reg and p2 == 'L' and (LOOSE or not (fwd and overwritten_unread(insns, halves, j, reg))):
                            lows.add(int(v2, 16))
                            found = True
                    if not LOOSE and found and any(r2 == reg and p2 == 'H' for r2, p2, _ in halves[j]):
                        break        # an X.L beyond the next X.H of the register pairs with that one, not this
            h = int(v, 16) << 16
            refs += [(h | x, a, 'const') for x in lows]
            if not lows:
                windows.append((h, a))
    return insns, refs, windows


def jump_s(addr, raw):
    w = int.from_bytes(raw[:2], 'little')
    if w >> 12 != 2:
        return None
    off = w & 0xFFF
    off -= 0x1000 if off & 0x800 else 0
    return addr + 2 * off


def elf_text(path):
    """(start, end) of the coop ELF's .text section."""
    data = open(path, 'rb').read()
    (shoff,) = struct.unpack_from('<I', data, 32)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 46)
    sh = [struct.unpack_from('<10I', data, shoff + i * shentsize) for i in range(shnum)]
    names = data[sh[shstrndx][4]:sh[shstrndx][4] + sh[shstrndx][5]]
    for h in sh:
        if names[h[0]:names.index(b'\0', h[0])] == b'.text':
            return h[3], h[3] + h[5]
    raise SystemExit('{}: no .text'.format(path))


def make_dis(image, objdump, tmp, text):
    """Disassemble the image's code ranges into tmp; returns the DXEs, the memory runs and {name: (path, lo, hi)}."""
    dxes = parse(open(image, 'rb').read())
    elf = os.path.join(tmp, 'image.elf')
    with open(os.devnull, 'w') as null:
        old, sys.stdout = sys.stdout, null
        try:
            write_elf(dxes, elf, len(dxes) - 1)
        finally:
            sys.stdout = old
    runs = memory_runs(dxes[-1])
    out = {}
    for name, (lo, hi) in (('stock', STOCK_CODE), ('l1', L1_CODE), ('coop_c', COOP_C), ('coop_a', text)):
        path = os.path.join(tmp, name + '.dis')
        with open(path, 'w') as f:
            subprocess.check_call([objdump, '-D', '--start-address=%#x' % lo, '--stop-address=%#x' % hi, elf], stdout=f)
        out[name] = (path, lo, hi)
    return dxes, runs, out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('image')
    ap.add_argument('--objdump', default=os.path.expanduser('~/op1-toolchain/bfin/bin/bfin-elf-objdump'))
    ap.add_argument('--elf', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'core',
                                                  'coop_combined.elf'),
                    help="the coop ELF the image was built from (for its .text range)")
    ap.add_argument('--keep-dis', help='keep the disassembly in this directory (local only: TE code, never commit)')
    ap.add_argument('--loose-pairs', action='store_true',
                    help="op1-sdk's pairing (every X.L within 10 instructions of an X.H): shows the false pairings the "
                         "default rule drops")
    a = ap.parse_args()
    global LOOSE
    LOOSE = a.loose_pairs
    patches, blanks_t = nophysics.load_feature()
    blanks = [(lo, hi) for lo, hi, _, _ in blanks_t]
    neut_words = {addr for addr, _, _, _ in patches}
    neut_code = []
    for addr, old, new, _ in patches:
        if addr in SWITCH_CASES:
            neut_code.append((int.from_bytes(old, 'little'), SWITCH_CASES[addr]))
        t = jump_s(addr, new)
        if t:
            neut_code.append((addr + 2, t))

    tmp = a.keep_dis or tempfile.mkdtemp(prefix='np_prove_')
    os.makedirs(tmp, exist_ok=True)
    try:
        dxes, runs, dis = make_dis(a.image, a.objdump, tmp, elf_text(a.elf))
        insns, refs_all, windows = [], [], []
        stock_insns = None
        for name in ('stock', 'l1', 'coop_c', 'coop_a'):
            path, lo, hi = dis[name]
            ins, r, w = disasm(path)
            if name == 'stock':
                stock_insns, stock_refs = ins, r
            insns += ins
            refs_all += r
            windows += w
            print('disassembled {:<7} {:#010x}..{:#010x}: {:,} instructions, {:,} references'.format(name, lo, hi, len(ins), len(r)))
    finally:
        if not a.keep_dis:
            shutil.rmtree(tmp, ignore_errors=True)
    idx = {addr: i for i, (addr, _) in enumerate(stock_insns)}
    refs = [(t, s, k) for t, s, k in refs_all if not any(lo <= s < hi for lo, hi in neut_code)]

    def in_code(v):
        return STOCK_CODE[0] <= v < STOCK_CODE[1]
    from inject_app import read_elf_symbols
    syms = read_elf_symbols(a.elf)
    exempt = []
    for name, why_not in COOP_NUMBER_TABLES.items():
        if name not in syms:
            sys.exit('FAIL: {} ({}) not in {}: update COOP_NUMBER_TABLES'.format(name, why_not, a.elf))
        v, n = syms[name]
        exempt.append((v, v + n))
    pat = re.compile(rb'(?=..[\x91-\xaa]\x01)', re.S)    # any halfword-aligned word in 0x0191xxxx..0x01aaxxxx
    data_refs = 0
    for base, raw in runs:
        if L1_CODE[0] <= base < L1_CODE[1]:
            continue                                 # L1 instruction SRAM: handled as code
        raw = bytes(raw)
        for m in pat.finditer(raw):
            o = m.start()
            if o & 1 or o + 4 > len(raw):
                continue
            v = struct.unpack_from('<I', raw, o)[0]
            at = base + o
            if not in_code(v) or at in neut_words or at in NOT_POINTERS:
                continue
            if any(lo <= at and at + 4 <= hi for lo, hi in exempt):
                continue
            if any(lo <= at < hi for lo, hi in EXIT_RECORDS + BOX2D_TABLES):
                continue
            refs.append((v, at, 'data'))
            data_refs += 1
    print('data words that look like code addresses: {:,}'.format(data_refs))

    fails = 0
    for h, at in windows:                           # X.H with no X.L: could be any hi:xxxx
        if any(lo < h + 0x10000 and h < hi for lo, hi in blanks) and not any(lo <= at < hi for lo, hi in blanks):
            print('FAIL: {:#x} loads {:#x}xxxx with no low half nearby; check by hand'.format(at, h >> 16))
            fails += 1
    for lo, hi in BOX2D_TABLES:                     # those tables: only Box2D code loads them
        users = {s for t, s, k in refs_all if lo <= t < hi and not (0x01000000 <= s < STOCK_CODE[0])}
        if not all(BOX2D_ALL[0] <= s < BOX2D_ALL[1] for s in users):
            print('FAIL: table {:#x} used outside Box2D: {}'.format(lo, sorted(map(hex, users))))
            fails += 1

    calls = {t for t, s, k in stock_refs if k == 'CALL'} | {s for s, t in stock_insns if t.startswith('LINK')}
    targets = {t for t, s, k in refs}

    def splittable(s):
        i = idx[s] - 1
        while stock_insns[i][1].startswith('NOP'):
            i -= 1
        return bool(TERM.match(stock_insns[i][1]))
    chunks = [BOX2D] + TABLES
    for lo, hi in CANDIDATES:
        cut = sorted(s for s in (calls | targets) if lo < s < hi and s in idx and splittable(s))
        chunks += list(zip([lo] + cut, cut + [hi]))
    starts = sorted(c[0] for c in chunks)
    by_start = dict(chunks)
    dead = set(chunks)

    def owner(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0:
            c = (starts[i], by_start[starts[i]])
            if c[0] <= addr < c[1] and c in dead:
                return c
        return None
    by_target = collections.defaultdict(list)
    for t, s, k in refs:
        by_target[t].append((s, k))
    tlist = sorted(by_target)
    why, changed = {}, True
    while changed:
        changed = False
        for c in sorted(dead):
            i, j = bisect.bisect_left(tlist, c[0]), bisect.bisect_left(tlist, c[1])
            for t in tlist[i:j]:
                ext = [(s, k) for s, k in by_target[t] if not c[0] <= s < c[1] and owner(s) is None]
                if ext:
                    why[c] = (t,) + ext[0]
                    dead.discard(c)
                    changed = True
                    break
    dead_ranges = sorted(c for c in dead if c[0] >= STOCK_CODE[0])
    for lo, hi in blanks:
        at = lo
        for s, e in dead_ranges:
            if s <= at < e:
                at = e
        if at < hi:
            fails += 1
            c = next((c for c in chunks if c[0] <= at < c[1]), None)
            t, s, k = why.get(c, (0, 0, '?'))
            print('FAIL: blank {:#x}..{:#x}: {:#x} is reachable ({} at {:#x} -> {:#x})'.format(lo, hi, at, k, s, t))
        elif lo not in (CANDIDATES[0][0], CANDIDATES[1][0]) and lo in idx and not splittable(lo):
            fails += 1
            print('FAIL: blank {:#x}: code before it can fall through'.format(lo))
    # coop's own code: no reference at all from it into any blanked range or the 36 redirected words' old targets
    coop_hits = [(t, s, k) for t, s, k in refs_all
                 if (COOP_C[0] <= s < COOP_C[1] or s >= COOP_A and s < 0x03000000 or L1_CODE[0] <= s < L1_CODE[1])
                 and any(lo <= t < hi for lo, hi in blanks)]
    for t, s, k in coop_hits:
        print('FAIL: coop / L1 code at {:#x} refers to blanked {:#x} ({})'.format(s, t, k))
        fails += 1
    total = sum(e - s for s, e in blanks)
    print('{} blank ranges, {:,} bytes; dead code found: {:,} bytes'.format(
        len(blanks), total, sum(e - s for s, e in dead_ranges)))
    print('PASS' if not fails else 'FAIL ({})'.format(fails))
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
