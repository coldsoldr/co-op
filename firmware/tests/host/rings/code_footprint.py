#!/usr/bin/env python3
"""Rings' instruction working set per block, from a GNU simulator instruction trace (`make footprint`):

    bfin-elf-run --trace-insn=on rings_trace.elf OUT.raw BLOCKS MODEL POLY 2>&1 | python3 -I code_footprint.py ELF NM FIRST LAST

The trace build calls RingsBlockMarker at each block's start (rings_compare.cc, -DRINGS_TRACE). For the blocks FIRST
.. LAST-1 it counts the distinct 32-byte lines (the I-cache's line) of plugin code executed in each block (the test
driver and the C library are left out), their average and the union, the 4 KB cache-set spread (how many of the 128
sets the lines fall in, and how many sets hold 3 or more rings lines: the 16 KB cache is 4-way, so those sets leave
the stock code one way or none), and the functions by bytes touched. NM: bfin-elf-nm. Stdlib only, Python 3.8.
"""
import bisect, collections, re, subprocess, sys

DRIVER = re.compile(r'^_(main|_malloc_r|_free_r|memchr|strlen|_fflush_r|_?_?sfvwrite|_?_?swrite|RingsBlockMarker|_?coop_l1|_?Z.*Drive(NoteOn|Render)|.*printf|.*fwrite|.*fopen|.*fclose|.*snprintf|.*atoi|.*getenv|.*exit|_?_?start)')


def symbols(nm, elf):
    out = subprocess.run([nm, '-n', '-S', '-C', elf], capture_output=True, text=True).stdout
    raw = subprocess.run([nm, '-n', elf], capture_output=True, text=True).stdout
    names = []
    for line, rline in zip(out.splitlines(), raw.splitlines()):
        f = line.split(None, 3)
        r = rline.split()
        if len(f) < 4 or f[2].lower() != 't':
            continue
        names.append((int(f[0], 16), int(f[1], 16), f[3], r[-1]))
    return names


def main():
    elf, nm, first, last = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    syms = symbols(nm, elf)
    starts = [s[0] for s in syms]
    marker = next(s[0] for s in syms if s[3] == '_RingsBlockMarker')
    # the plugin's own code: the objects of plugins/rings (everything but the driver, libc and libgcc)
    plugin_ranges = []
    for start, size, name, raw in syms:
        if DRIVER.match(raw) or raw.startswith('___') or 'std::' in name:
            continue
        plugin_ranges.append((start, start + size, name))
    pstarts = [r[0] for r in plugin_ranges]

    def owner(pc):
        i = bisect.bisect_right(pstarts, pc) - 1
        if i >= 0 and plugin_ranges[i][0] <= pc < plugin_ranges[i][1]:
            return plugin_ranges[i][2]
        return None

    block = -1
    per_block, union, func_lines = [], set(), collections.defaultdict(set)
    lines = set()
    cache = {}
    for text in sys.stdin:
        if not text.startswith('insn:'):
            continue
        pc = int(text.split()[1], 16)
        if pc == marker:
            if first <= block < last:
                per_block.append(len(lines))
            block += 1
            lines = set()
            if block >= last:
                break
            continue
        if not first <= block < last:
            continue
        name = cache.get(pc)
        if name is None:
            name = owner(pc) or ''
            cache[pc] = name
        if not name:
            continue
        ln = pc >> 5
        lines.add(ln)
        union.add(ln)
        func_lines[name].add(ln)
    if not per_block:
        sys.exit('no complete block in range')
    sets = collections.Counter(ln & 127 for ln in union)
    print('blocks %d..%d: rings code a block %.0f lines (%.1f KB) avg, %d max; union %d lines (%.1f KB)'
          % (first, last - 1, sum(per_block) / len(per_block), 32 * sum(per_block) / len(per_block) / 1024,
             max(per_block), len(union), 32 * len(union) / 1024))
    print('cache sets used %d of 128; sets with >= 3 rings lines: %d, with 4+: %d'
          % (len(sets), sum(1 for c in sets.values() if c >= 3), sum(1 for c in sets.values() if c >= 4)))
    for name, ls in sorted(func_lines.items(), key=lambda kv: -len(kv[1]))[:25]:
        print('  %5d B  %s' % (32 * len(ls), name[:100]))


if __name__ == '__main__':
    main()
