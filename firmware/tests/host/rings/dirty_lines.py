#!/usr/bin/env python3
"""Rings' store footprint per block, from the store-footprint build (`make dirty`):

    bfin-elf-run rings_dirty.elf OUT.raw BLOCKS MODEL POLY | python3 -I dirty_lines.py ELF BFIN_BIN [DEPTH]

The build (rings_compare.cc -DRINGS_DIRTY) compares the program's .bss block to block in 32-byte lines (the data
cache's line) and prints each line's count of blocks in which it changed. This tool names each line: the symbol it
is in (bfin-elf-nm) and, inside the voice states, the member (bfin-elf-gdb `ptype /o`, one RingsVoice's layout,
arrays of classes expanded), cut to DEPTH levels (default 3); then prints the lines written per block by owner, most
written first. A lower bound: a store of the value already there is not seen. Stdlib only, Python 3.8.
"""
import bisect, collections, re, subprocess, sys

FIELD = re.compile(r'^/\*\s*(\d+)(?::\s*\d+)?\s*\|\s*(\d+)\s*\*/\s*(.*?)\s*$')
CLOSE = re.compile(r'^}\s*(\w+)(\[\d+\])?;')


def symbols(nm, elf):
    out = subprocess.run([nm, '-n', '-S', '-C', elf], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        f = line.split(None, 3)
        if len(f) == 4 and f[2] in 'bBdD':
            syms.append((int(f[0], 16), int(f[1], 16), f[3]))
    return syms


class Layouts:
    def __init__(self, gdb, elf):
        self.gdb, self.elf, self.cache = gdb, elf, {}

    def get(self, typename):
        """[(offset from the type's start, size, name, element type or None, count)] of the type's leaves."""
        if typename in self.cache:
            return self.cache[typename]
        out = subprocess.run([self.gdb, '-batch', '-ex', 'set max-value-size unlimited', '-ex',
                              'ptype /o ' + typename, self.elf], capture_output=True, text=True).stdout
        leaves, opens, base = [], [], None
        for raw in out.splitlines():
            line = raw.strip()
            m = FIELD.match(line)
            if m:
                off, size, decl = int(m.group(1)), int(m.group(2)), m.group(3)
                if base is None:   # the type line itself
                    base = off
                    continue
                if decl.endswith('{'):
                    opens.append(len(leaves))
                    continue
                decl = decl.rstrip(';')
                words = decl.split()
                name = words[-1].lstrip('*&')
                count = 1
                am = re.match(r'(\w+)\[(\d+)\]', name)
                if am:
                    name, count = am.group(1), int(am.group(2))
                etype = ' '.join(words[:-1])
                etype = etype if '::' in etype and '*' not in decl else None
                leaves.append([off - base, size, name, etype, count])
                continue
            c = CLOSE.match(line)
            if c and opens:
                start = opens.pop()
                for leaf in leaves[start:]:
                    leaf[2] = c.group(1) + '.' + leaf[2]
        leaves.sort()
        self.cache[typename] = leaves
        return leaves

    def path(self, typename, off, depth):
        leaves = self.get(typename)
        offs = [l[0] for l in leaves]
        i = bisect.bisect_right(offs, off) - 1
        if i < 0 or off >= leaves[i][0] + leaves[i][1]:
            return '(pad)'
        lo, size, name, etype, count = leaves[i]
        parts = name.split('.')
        if len(parts) >= depth:
            return '.'.join(parts[:depth])
        if etype and count >= 1:
            esize = size // count
            k, inner = divmod(off - lo, esize)
            sub = self.path(etype, inner, depth - len(parts))
            return name + '[%d].' % k + sub if count > 1 else name + '.' + sub
        return name


def main():
    elf, binp = sys.argv[1], sys.argv[2]
    depth = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    syms = symbols(binp + '/bfin-elf-nm', elf)
    starts = [s[0] for s in syms]
    blocks, counts = 0, []
    for text in sys.stdin:
        if text.startswith('dirty base'):
            blocks = int(text.split()[-1])
        elif text.startswith('dirty 0x'):
            _, a, n = text.split()
            counts.append((int(a, 16), int(n)))
        elif text.strip():
            print(text.rstrip())
    lay = Layouts(binp + '/bfin-elf-gdb', elf)
    vs = next((s for s in syms if s[2].endswith('states_memory')), None)
    vsize = vs[1] // 6 if vs else 0

    def owner(addr):
        i = bisect.bisect_right(starts, addr) - 1
        while i >= 0 and not (syms[i][0] <= addr < syms[i][0] + max(syms[i][1], 1)):
            i -= 1
        if i < 0:
            return '?', -1
        s = syms[i]
        if s is vs:
            v, off = divmod(addr - s[0], vsize)
            # a 32-byte line: name it by its first word that holds a member
            names = []
            for o in range(off, min(off + 32, vsize), 4):
                p = lay.path('op1rings::RingsVoice', o, depth)
                if p != '(pad)' and p not in names:
                    names.append(p)
            return 'voice.' + re.sub(r'\[\d+\]', '[]', names[0] if names else '(pad)'), v
        return re.sub(r'^\(anonymous namespace\)::', '', s[2]), -1

    by = collections.defaultdict(float)
    lines_by = collections.defaultdict(int)
    voices_by = collections.defaultdict(set)
    total = 0.0
    for a, n in counts:
        name, v = owner(a)
        rate = n / blocks if blocks else 0
        by[name] += rate
        lines_by[name] += 1
        if v >= 0:
            voices_by[name].add(v)
        total += rate
    print('lines written per block (a lower bound): %.1f; %d lines written at least once in %d blocks'
          % (total, len(counts), blocks))
    # On the device the host stubs l1_lease / l1_scratch are L1 (no cache line, no write-back); dirty:: is this scan.
    l1 = sum(r for name, r in by.items() if name in ('l1_lease', 'l1_scratch'))
    own = sum(r for name, r in by.items() if name.startswith('dirty::'))
    print('SDRAM lines written per block on the device: %.1f (L1: %.1f)' % (total - l1 - own, l1))
    print('%8s %6s %7s  %s' % ('per blk', 'lines', 'voices', 'owner'))
    for name, r in sorted(by.items(), key=lambda kv: -kv[1]):
        if r >= 0.05:
            print('%8.2f %6d %7s  %s' % (r, lines_by[name], len(voices_by[name]) or '', name))


if __name__ == '__main__':
    main()
