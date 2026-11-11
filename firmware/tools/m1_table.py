#!/usr/bin/env python3
"""The m1 table (docs/request-rings-collateral.md): where the block goes per section, from the run files of the
m1 scenarios (tests/sim/run_m1_mode*.txt, an image built with COOP_M1=1 and g_coop_gap_prof=1).

    python3 -I tools/m1_table.py m1_mode0_run.txt [m1_mode2_run.txt m1_mode4_run.txt]

Per section, cycles per block (the `stages` line: every span summed over the block, then averaged over the blocks
that had it, so a section where it ran every block reads as per block):
  cb          the whole callback (CoopAudioCb: coop's block start and the stock callback)
  dsp         the stock 0xffa017ec: synth voice loop, drum loop, buses, FX, master stage (twice), the rest of it
  voices      the synth voice loop 0xffa009a0, of which
    render    the voice renders as the thunk times them (a plugin: the dispatcher + the plugin)
    plugin    the plugin's own render (inside the dispatcher); dispatch = render - plugin
    hook      coop's voice hook; thunk = voice_coop - render - hook (LFO 2 stage 0, the counters, the bookkeeping)
    v_stock   voices - voice_coop: the stock's own per-voice work (LFO, envelope, gain, the loop)
  drums, master (a + b), dsp_rest = dsp - voices - drums - master
  push        l1v_push_lr; cb_tail the callback's last call 0xffa036c4; cb_rest = cb - dsp - push - cb_tail
Then collateral = (cb - render - hook) - (the same in the idle section): what a playing engine costs outside its
renders and coop's hook, and the stages it lands in (each stage's increase over idle, render and hook taken out of
voices). With p0 / p1 per stage when the counters ran (the device; op1emu has none), labelled by the file's pf_mode.
"""
import re
import sys

MODES = {0: ('code stall', 'data stall'), 1: ('I fills', 'D fills'), 2: ('code stall', 'data stall 0x98'),
         3: ('code stall', 'data 0x96'), 4: ('code stall', 'fill buffer 0x95')}


def parse(path):
    secs, cur, mode = [], None, None
    for line in open(path, encoding='utf-8', errors='replace'):
        w = line.split()
        if not w:
            continue
        if w[0] == 'section':
            cur = {'name': w[1], 'kv': dict(zip(w[2::2], w[3::2])), 'st': {}, 'eng': {}}
            secs.append(cur)
        elif cur and w[0] == 'cost' and 'pf_mode' in w:
            mode = int(w[w.index('pf_mode') + 1])
        elif cur and w[0] in ('stages', 'engines'):
            for t in w[1:]:
                m = re.match(r'([a-z0-9_]+)=([0-9/]+)$', t)
                if m:
                    v = [int(x) for x in m.group(2).split('/')]
                    (cur['st'] if w[0] == 'stages' else cur['eng'])[m.group(1)] = v
    return secs, mode


def g(sec, k, i=0):
    v = sec['st'].get(k)
    return v[i] if v and len(v) > i else 0


def row(sec):
    r = {'cb': g(sec, 'cb'), 'dsp': g(sec, 'dsp'), 'voices': g(sec, 'voices'), 'render': g(sec, 'render'),
         'plugin': g(sec, 'plugin'), 'hook': g(sec, 'hook'), 'drums': g(sec, 'drums'), 'push': g(sec, 'push'),
         'cb_tail': g(sec, 'cb_tail'), 'master': g(sec, 'master_a') + g(sec, 'master_b')}
    vc = g(sec, 'voice_coop')
    r['dispatch'] = r['render'] - r['plugin'] if r['plugin'] else 0
    r['thunk'] = vc - r['render'] - r['hook'] if vc else 0
    r['v_stock'] = r['voices'] - vc if r['voices'] else 0
    r['dsp_rest'] = r['dsp'] - r['voices'] - r['drums'] - r['master']
    r['cb_rest'] = r['cb'] - r['dsp'] - r['push'] - r['cb_tail']
    for k in ('cb', 'dsp', 'voices', 'render', 'plugin', 'hook', 'drums', 'push', 'cb_tail'):
        r[k + '_p'] = (g(sec, k, 3), g(sec, k, 4))
    r['master_p'] = (g(sec, 'master_a', 3) + g(sec, 'master_b', 3), g(sec, 'master_a', 4) + g(sec, 'master_b', 4))
    return r


COLS = ['cb', 'dsp', 'voices', 'render', 'plugin', 'dispatch', 'hook', 'thunk', 'v_stock', 'drums', 'master',
        'dsp_rest', 'push', 'cb_tail', 'cb_rest']


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        secs, mode = parse(path)
        secs = [s for s in secs if s['name'] not in ('-', 'lead-in')]
        idle = next((row(s) for s in secs if s['name'] == 'idle'), None)
        names = MODES.get(mode, ('p0', 'p1'))
        print('## %s (pf_mode %s: p0 = %s, p1 = %s)\n' % (path, mode, names[0], names[1]))
        print('| section | ' + ' | '.join(COLS) + ' | collateral |')
        print('|' + '---|' * (len(COLS) + 2))
        for s in secs:
            r = row(s)
            coll = ''
            if idle:
                coll = (r['cb'] - r['render'] - r['hook']) - (idle['cb'] - idle['render'] - idle['hook'])
            print('| %s | ' % s['name'] + ' | '.join('{:,}'.format(r[c]) for c in COLS) + ' | %s |' % ('{:,}'.format(coll) if coll != '' else ''))
        print()
        if any(row(s)['cb_p'] != (0, 0) for s in secs):
            pc = ['cb', 'dsp', 'voices', 'render', 'plugin', 'hook', 'drums', 'master', 'push', 'cb_tail']
            print('Stalls per block, %s / %s:\n' % names)
            print('| section | ' + ' | '.join(pc) + ' |')
            print('|' + '---|' * (len(pc) + 1))
            for s in secs:
                r = row(s)
                print('| %s | ' % s['name'] + ' | '.join('{:,} / {:,}'.format(*r[c + '_p']) for c in pc) + ' |')
            print()
        if idle:
            print('Collateral by stage (increase over idle; voices without the renders and the hook):\n')
            print('| section | v_stock | thunk | dispatch | drums | master | dsp_rest | push | cb_tail | cb_rest |')
            print('|---|---|---|---|---|---|---|---|---|---|')
            for s in secs:
                r = row(s)
                print('| %s | ' % s['name'] + ' | '.join('{:,}'.format(r[c] - idle[c]) for c in
                      ('v_stock', 'thunk', 'dispatch', 'drums', 'master', 'dsp_rest', 'push', 'cb_tail', 'cb_rest')) + ' |')
            print()


if __name__ == '__main__':
    main()
