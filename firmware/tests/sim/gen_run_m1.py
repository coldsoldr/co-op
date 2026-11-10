#!/usr/bin/env python3
"""Write the m1 runner scenarios (docs/request-rings-collateral.md section 4): tests/sim/run_m1_mode{0,2,4}.txt, the
same session three times, each with the performance counters in one mode (#@pfmode, coop_stats.cc):
  0 = code fetch stall cycles / data stall cycles, 2 = code fetch stalls / data stalls (0x98),
  4 = code fetch stalls / data memory fill buffer stalls (0x95).

Every engine is picked from the engine list (synth page, shift + T1; 8 encoder steps a row; SEQ then SYNTH close
it). The first pick turns 20 rows anticlockwise (the list stops at its first row, checked in op1emu), then ROWS[name]
rows clockwise; the list opens on the sound's engine, so every later pick moves from the row of the one before. So the
run does not depend on the sound the OP-1 starts with; it needs exactly the engines in ROWS at those rows: the stock
ones plus the plugins named in docs/device-tests.md "m1". The runner holds 2,048 events (one a Gray step).

Sections (#@mark, 10 s of a held six-note chord each, the transitions in sections named "-"): idle, voltage, rings
modal and sympathetic strings, probe_alu, probe_code 4 / 8 / 12 KB, probe_sdram's eight loads, rings modal with all
four knobs turning. A probe's variant is its word 5 (fn + blue, one step = one detent = 2 Gray steps); the selection
starts every probe at step 0 (the engine row's default).

    python3 tests/sim/gen_run_m1.py        # rewrites the three files
"""
import os

# Rows below the list's first row (op1emu, a NAND with saw, rings and the three probes; tests/sim/run_m1_mode0.txt
# sections name what played): the stock list's order with the plugins sorted in by name.
ROWS = {'voltage': 15, 'rings': 11, 'probe_alu': 7, 'probe_code': 8, 'probe_sdram': 9}
CHORD = ['c4', 'd4', 'e4', 'f4', 'g4', 'a4']
HOLD = 10000                 # ms a section's chord is held before the next mark
SDRAM = ['r16k', 'r64k', 'r128k', 'w16k', 'w64k', 'w128k', 'rs128k', 'ws128k']


class S:
    def __init__(self, mode):
        self.t = 26000
        self.lines = ['# m1 (docs/request-rings-collateral.md): written by tests/sim/gen_run_m1.py, do not edit.',
                      '# pfmode %d; docs/device-tests.md "m1" says what to install and what to send back.' % mode,
                      '#@name m1_mode%d' % mode, '#@tail 3000', 'accel 64 -64 512', 'wait-frames 1 300000']
        self.at(0)
        self.lines.append('#@pfmode %d' % mode)
        self.row = None                               # the list's row of the sound's engine, once one was picked

    def at(self, dt):
        self.t += dt
        self.lines.append('at %d' % self.t)

    def do(self, *cmds, dt=0):
        if dt:
            self.at(dt)
        self.lines.extend(cmds)

    def mark(self, name):
        self.lines.append('#@mark %s' % name)

    def select(self, engine):
        self.mark('-')
        self.do('press shift', dt=1500)
        self.do('tap t1 200', dt=200)
        self.do('release shift', dt=500)
        if self.row is None:
            self.do('turn enc1 ccw 160', dt=1200)      # 20 rows: 160 steps, 20 ms apart = 3.2 s
            self.row, t = 0, 3400
        else:
            t = 1200
        d = 8 * (ROWS[engine] - self.row)
        if d:
            self.do('turn enc1 %s %d' % ('cw' if d > 0 else 'ccw', abs(d)), dt=t)
        self.row = ROWS[engine]
        self.do('tap seq 200', dt=200 + 20 * abs(d) + 1000)
        self.do('tap synth 200', dt=1000)

    def fn_blue(self, steps):                       # fn + blue: `steps` Gray transitions (+cw / -ccw)
        self.do('press help', dt=1200)
        self.do('turn enc1 %s %d' % ('cw' if steps > 0 else 'ccw', abs(steps)), dt=300)
        self.do('release help', dt=300 + 20 * abs(steps) + 200)

    def variant(self, k, gray):                     # m1c: word 5 = step k from the bottom, absolutely
        # m1c on the device lost a step (rings modal -> symp and probe_code 8 -> 12 KB each played the same as the
        # section before; ctl_turns 21 of 22). Relative steps carried such a loss into every later section; now each
        # section goes down to the bottom (the list clamps) and up k steps in whole detents (2 transitions each, so
        # the stock's decoder and coop's remainder stay aligned): a lost transition costs that section only.
        self.fn_blue(-(8 * gray + 8))
        if k:
            self.fn_blue(k * gray)

    def chord(self, name, knobs=False):
        self.do(*['press %s' % k for k in CHORD], dt=1000)
        self.at(800)                                  # the attack settles outside the section
        self.mark(name)
        if knobs:                                     # all four encoders back and forth, a step every 20 ms
            for i in range(HOLD // 1000):
                for e in range(4):
                    self.do('turn enc%d %s 6' % (e + 1, 'cw' if i % 2 == 0 else 'ccw'), dt=250)
        else:
            self.at(HOLD)
        if knobs:
            self.at(HOLD - (HOLD // 1000) * 1000 + 200)
        self.mark('-')
        self.do(*['release %s' % k for k in CHORD])


def scenario(mode):
    s = S(mode)
    s.do('tap synth 200', dt=1500)
    s.at(1500)
    s.mark('idle')
    s.at(HOLD)
    s.select('voltage'); s.chord('voltage')
    s.select('rings'); s.variant(0, 4); s.chord('rings_modal')      # rings' model: 2 detents a step, 4 transitions
    s.variant(1, 4); s.chord('rings_symp')
    s.select('probe_alu'); s.chord('alu')
    s.select('probe_code'); s.variant(0, 2); s.chord('code4k')     # the probes: 1 detent a step, 2 transitions
    s.variant(1, 2); s.chord('code8k')
    s.variant(2, 2); s.chord('code12k')
    s.select('probe_sdram')
    for i, name in enumerate(SDRAM):
        s.variant(i, 2)
        s.chord('sdram_' + name)
    s.select('rings'); s.variant(0, 4); s.chord('rings_knobs', knobs=True)
    s.at(2000)
    s.lines += ['wait %d' % (s.t + 6000), 'quit']
    return '\n'.join(s.lines) + '\n'


if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    for m in (0, 2, 4):
        p = os.path.join(here, 'run_m1_mode%d.txt' % m)
        open(p, 'w', newline='\n').write(scenario(m))
        print(p)
