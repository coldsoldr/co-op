#!/usr/bin/env python3
"""What coop's bridge would make of the foreign plugin files it is shown (docs/bridge.md "Survey").

    python3 tools/bridge/survey.py [PATH ...] [--tsv] [--json] [--imports] [--budget BYTES]

PATH: .knl / .opmod / .coop files or folders (searched recursively). Without one: the local, gitignored copies this
checkout may have (incoming/op1-workshop/variants/7ff, incoming/opus, ../../te-op1-cfw/incoming/hundekraft/*/**/opmods).
Nothing is run or relocated: headers, tables and the text's immediates are read (formats.py).

The verdict is core/bridge/bridge_knl.cc and bridge_opmod.cc's, rule for rule, from the same table
(core/bridge/bridge_imports.def); tests/host/bridge compares the two on every real file it finds (--tsv is that
comparison's format: file, dialect, kind, phase, needs, known addresses, unlisted addresses).

Stdlib only, Python 3.8. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later.
"""
import argparse
import collections
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import formats  # noqa: E402

FW = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEF = os.path.join(FW, 'core', 'bridge', 'bridge_imports.def')
BUDGET = 1536 * 1024                      # plugin_host.cc PLUG_BUDGET_DEFAULT
KN_STUB = 12                              # plugin_load.h KN_STUB_SIZE: a call stub per import, heap 0 is far from the core
PHASES = {2: 'B2 kennel effects', 3: 'B3 kennel screens', 4: 'B4 kennel engines', 5: 'B5 controls', 6: 'B6 Opus effects',
          7: 'B7 Opus engines', 8: 'B8 big buffers, engine extensions', 9: 'B9 Opus pages'}
ENGINE_EXT = 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800
COMMON = 1 | 2 | 4 | 8 | 0x10 | formats.COARSE_MASK

R_IMPORT = 'needs an import the bridge does not provide'
R_LATER = 'needs an import a later bridge phase provides'


def load_def(path=DEF):
    imports, addrs = {}, {}
    for line in open(path, encoding='utf-8'):
        m = re.match(r'BR_IMPORT\((\w+), "([^"]+)", (\d+), (\d+), (\w+), (\w+), "(.*)"\)\s*$', line)
        if m:
            imports[(m.group(1), m.group(2))] = {'loads': int(m.group(3)), 'full': int(m.group(4)), 'kind': m.group(5),
                                                 'shim': m.group(6) != '0', 'note': m.group(7)}
            continue
        m = re.match(r'BR_ADDR\((0x[0-9A-Fa-f]+), "([^"]*)", "(.*)"\)\s*$', line)
        if m:
            addrs[int(m.group(1), 16)] = m.group(2)
        elif line.startswith(('BR_IMPORT(', 'BR_ADDR(')):
            raise SystemExit('bridge_imports.def: a line survey.py cannot read: ' + line.strip())
    return imports, addrs


class Verdict:
    """bridge_knl.cc later() / bridge_opmod.cc set(): the latest phase wins, the first 'never' wins over all."""

    def __init__(self):
        self.phase, self.reason, self.needs = 0, None, ''

    def later(self, phase, reason, needs):
        if self.phase == 0 and self.reason:
            return
        if phase == 0 or phase > self.phase:
            self.phase, self.reason, self.needs = phase, reason, needs[:39]


def verdict_knl(i, table):
    v = Verdict()
    kind, flags = i['kind'], i['flags']
    if kind == 'feature':
        v.later(0, "a kennel feature: it hooks kennel's own core, which coop does not have", 'kind feature')
    elif kind == 'fx':
        v.later(2, None, '')
        if flags & 0x10:
            v.later(8, 'a declared pool buffer', 'flag bigbuf')
        if flags & ~(2 | 4 | 0x10):
            v.later(0, 'flags this bridge does not know', 'flags')
    else:
        v.later(4, None, '')
        if flags & 1:
            v.later(8, "an engine that plays the slot's sample", 'flag uses_sample')
        if flags & ENGINE_EXT:
            v.later(8, 'kennel engine extension (own envelope, voice-sum block, own pages, input, service)',
                    'flag engine extension')
        if flags & 0x10:
            v.later(8, 'a declared pool buffer', 'flag bigbuf')
        if flags & 8:
            v.later(0, 'an input engine: coop has no input engine path', 'flag input')
        if flags & ~(1 | 2 | 8 | 0x10 | ENGINE_EXT | formats.COARSE_MASK):
            v.later(0, 'flags this bridge does not know', 'flags')
    if flags & ~COMMON or v.phase == 0:
        return v                                             # br_knl_normalise returns 1: the imports are not read
    for name in i['imports']:
        e = table.get(('KNL', name))
        if not e or e['loads'] == 0:
            v.later(0, R_IMPORT, name)
        elif e['loads'] > v.phase:
            v.later(e['loads'], R_LATER, name)
    return v


def verdict_opmod(i, table):
    v = Verdict()

    def never(reason, needs):
        v.later(0, reason, needs)
        return v
    if not i['core']:
        return never('its manifest has no core line', 'core')
    if i['places']:
        return never("it patches the firmware's code (places)", 'place')
    if i['exports']:
        return never('a library other files link against', 'exports')
    if i['needs']:
        return never('needs another plugin file (a library)', i['needs'][0])
    if not i['adds']:
        return never("adds no effect or engine (a feature of Opus's own core)", 'adds')
    for a in i['adds']:
        word = a.split()[0] if a.split() else ''
        if word == 'effect':
            v.later(6, None, '')
        elif word == 'engine':
            v.later(7, None, '')
        else:
            return never('adds something coop has no place for', a)
    for name in i['imports']:
        e = table.get(('OPMOD', name))
        if not e or e['loads'] == 0:
            return never(R_IMPORT, name)
        if e['loads'] > v.phase:
            v.later(e['loads'], R_LATER, name)
    return v


def opmod_kind(i):
    words = [a.split()[0] for a in i['adds'] if a.split()]
    return 'synth' if 'engine' in words else 'fx' if 'effect' in words else 'feature'


def find(paths):
    if not paths:
        te = os.path.join(FW, '..', '..', 'te-op1-cfw', 'incoming', 'hundekraft')
        paths = [os.path.join(FW, 'incoming', 'op1-workshop', 'variants', '7ff'), os.path.join(FW, 'incoming', 'opus')]
        paths += sorted(glob.glob(os.path.join(te, '*', '*', 'opmods')))[-1:]
    out = []
    for p in paths:
        if os.path.isdir(p):
            for root, _dirs, names in os.walk(p):
                out += [os.path.join(root, n) for n in sorted(names) if n.lower().endswith(('.knl', '.opmod', '.coop'))]
        elif os.path.isfile(p):
            out.append(p)
    return out


def survey(files, table, addrs):
    rows = []
    for path in files:
        d = open(path, 'rb').read()
        row = {'file': os.path.basename(path), 'path': path}
        try:
            i = formats.read_any(d)
        except formats.BadFile as e:
            row.update(dialect='?', kind='?', phase=0, reason=str(e), needs='', error=True)
            rows.append(row)
            continue
        if i['format'] == 'opmod':
            v = verdict_opmod(i, table)
            row.update(dialect='opmod', kind=opmod_kind(i), name=i['id'], known=0, unlisted=0, unlisted_addrs=[],
                       cost=i['load'], flags=[])
        else:
            v = verdict_knl(i, table) if i['format'] == 'knl' else Verdict()
            ab, st = formats.kn_abs(d, i, addrs)
            row.update(dialect=i['format'], kind=i['kind'], name=i['name'], flags=i['flag_names'],
                       known=sum(c for a, c in ab.items() if a in addrs),
                       unlisted=sum(c for a, c in ab.items() if a not in addrs),
                       unlisted_addrs=sorted(a for a in ab if a not in addrs),
                       scan_aligned=st['relocated_seen'] == st['relocated_text'],
                       # two relocations of an effect (a channel each), one of an engine; a call stub an import
                       cost=(2 if i['kind'] == 'fx' else 1) * (i['load'] + 3 + KN_STUB * len(i['imports'])),
                       scratch=0x01AB8BA0 in ab, bigbuf=i['bigbuf'])
            # past br_knl_normalise (common flags only, a phase) and a synth: kn_parse wants 36 bytes at the entry
            row['entry_short'] = bool(i['format'] == 'knl' and i['kind'] == 'synth' and v.phase and
                                      not (i['flags'] & ~COMMON) and i['entry'] + 36 > i['image'])
        row.update(phase=v.phase, reason=v.reason, needs=v.needs, load=i['load'], imports=i['imports'],
                   author=i.get('author', ''), version=i.get('version', ''))
        rows.append(row)
    return rows


def tsv_line(r, exact=True):
    """exact: the loader has kn_parse_as. Without it a .knl engine whose entry object lies in the image's last 36
    bytes does not get past kn_parse (it wants a coop_engine_t there)."""
    if not exact and r.get('entry_short'):
        return chr(9).join((r['file'], 'knl', r['kind'], 'PARSE', 'entry object truncated', '0', '0'))
    return chr(9).join(str(x) for x in (r['file'], r['dialect'], r['kind'] if r['dialect'] == 'knl' else '-', r['phase'],
                                      r['needs'], r['known'], r['unlisted']))


def check(binary, rows):
    """The core's code (bridge_test --tsv) and this script must say the same of every real file. No file: skip."""
    import subprocess
    if not rows:
        print('SKIP bridge real-file check: no local .knl / .opmod files (they are never in the repository)')
        return 0
    bad = 0
    exact = subprocess.run([binary, '--loader'], stdout=subprocess.PIPE, universal_newlines=True).stdout.strip() == 'kn_parse_as'
    for at in range(0, len(rows), 40):
        part = rows[at:at + 40]
        out = subprocess.run([binary, '--tsv'] + [r['path'] for r in part], stdout=subprocess.PIPE, universal_newlines=True)
        if out.returncode:
            print('bridge_test --tsv failed (%d)' % out.returncode)
            return 1
        theirs = out.stdout.splitlines()
        for r, line in zip(part, theirs):
            if tsv_line(r, exact) != line:
                bad += 1
                print('MISMATCH')
                print('  survey.py : ' + tsv_line(r, exact))
                print('  core code : ' + line)
        bad += abs(len(theirs) - len(part))
    by = collections.Counter((r['dialect'], 'never' if not r['phase'] else 'B%d' % r['phase']) for r in rows)
    short = [r['file'] for r in rows if r.get('entry_short')]
    if short and not exact:
        print('this loader has no kn_parse_as yet: refused as "entry object truncated": ' + ', '.join(short))
    print('bridge real-file check: %d files, %d mismatches; %s' % (
        len(rows), bad, ', '.join('%s %s %d' % (d, p, n) for (d, p), n in sorted(by.items()))))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('paths', nargs='*')
    ap.add_argument('--tsv', action='store_true', help='one line a file, the host test\'s comparison format')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--imports', action='store_true', help='the import histogram by dialect and kind')
    ap.add_argument('--budget', type=int, default=BUDGET)
    ap.add_argument('--check', metavar='BIN', help='compare with bridge_test --tsv (tests/host/bridge) on the same files')
    a = ap.parse_args()
    table, addrs = load_def()
    rows = survey(find(a.paths), table, addrs)
    if a.check:
        return check(a.check, [r for r in rows if r['dialect'] in ('knl', 'opmod')])
    if a.tsv:
        for r in rows:
            if r['dialect'] in ('knl', 'opmod'):
                print(tsv_line(r))
        return 0
    if a.json:
        json.dump([{k: v for k, v in r.items() if k != 'path'} for r in rows], sys.stdout, indent=1)
        return 0
    if not rows:
        print('no .knl / .opmod files found (give a path; the defaults are local, gitignored copies)')
        return 0
    print('%-22s %-6s %-8s %-7s %8s  %s' % ('file', 'format', 'kind', 'loads', 'bytes', 'why not before / never'))
    for r in rows:
        when = 'never' if r['phase'] == 0 else 'B%d' % r['phase']
        why = (r['reason'] or '') + (': ' + r['needs'] if r.get('needs') and r['reason'] else '')
        extra = ' [%d unlisted address%s]' % (r['unlisted'], '' if r['unlisted'] == 1 else 'es') if r.get('unlisted') else ''
        print('%-22s %-6s %-8s %-7s %8d  %s%s' % (r['file'][:22], r['dialect'], r['kind'], when, r.get('cost', 0), why, extra))
    print()
    for dialect in ('knl', 'opmod'):
        mine = [r for r in rows if r['dialect'] == dialect]
        if not mine:
            continue
        print('%s: %d files' % (dialect, len(mine)))
        by = collections.Counter((r['kind'], r['phase']) for r in mine)
        for kind in ('fx', 'synth', 'feature'):
            parts = ['%s %d' % ('never' if p == 0 else 'B%d' % p, n) for (k, p), n in sorted(by.items()) if k == kind]
            if parts:
                print('  %-8s %s' % (kind, ', '.join(parts)))
        total = 0
        for phase in sorted(set(r['phase'] for r in mine) - {0}):
            got = [r for r in mine if r['phase'] == phase]
            total += sum(r['cost'] for r in got)
            print('  from %-34s +%2d files, %7d bytes of heap 0 (running total %d = %d %% of the %d KiB budget)' % (
                PHASES.get(phase, 'B%d' % phase), len(got), sum(r['cost'] for r in got), total, 100 * total // a.budget,
                a.budget >> 10))
        if dialect == 'knl':
            scr = sum(1 for r in mine if r.get('scratch'))
            bad = [r['file'] for r in mine if not r.get('scan_aligned', True)]
            unl = collections.Counter(x for r in mine if r['phase'] for x in r['unlisted_addrs'])
            print('  %d files reach R246\'s position buffer table (0x01AB8BA0)' % scr)
            print('  address walk misaligned in: %s' % (', '.join(bad) or 'none'))
            print('  unlisted firmware addresses in files that would load: %s' % (
                ', '.join('%08X x%d' % (x, n) for x, n in sorted(unl.items())) or 'none'))
    if a.imports:
        hist = collections.defaultdict(collections.Counter)
        for r in rows:
            for n in r.get('imports', []):
                hist[(r['dialect'], r['kind'])][n] += 1
        for key in sorted(hist):
            print('\n%s %s imports:' % key)
            for n, c in hist[key].most_common():
                e = table.get(('KNL' if key[0] == 'knl' else 'OPMOD', n))
                print('  %3d  %-28s %s' % (c, n, 'not in the table' if not e else 'never' if not e['loads'] else
                                           'B%d%s' % (e['loads'], '' if e['full'] == e['loads'] else ' (full B%d)' % e['full'])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
