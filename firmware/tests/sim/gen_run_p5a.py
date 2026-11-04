#!/usr/bin/env python3
"""Writes tests/sim/run_p5a.txt, the p5a device test as a test-runner scenario (docs/test-runner.md)."""
L = []
t = 26000
def at(ms): L.append('at %d' % ms)
def c(s): L.append('# ' + s)
CHORD6 = ['c4', 'd4', 'e4', 'f4', 'g4', 'a4']
CHORD3 = ['c4', 'e4', 'g4']
def chord(notes, start, hold):
    at(start)
    for n in notes: L.append('press ' + n)
    at(start + hold)
    for n in notes: L.append('release ' + n)
L += ['# The p5a device test (docs/device-tests.md "Performance 5") as a test-runner scenario (p5b, docs/test-runner.md):',
      '# tools/coop_sysex.html "Run a test" plays it on the OP-1 and records the audio; tests/sim/run_emu.sh plays it in',
      '# op1emu. Factory sound slots assumed (s1 digital, s2 voltage, s3 pulse, s4 drwave, s5 dsynth, s6 fm, s7 phase,',
      '# s8 sampler: the engines line names what really played). Each #@mark starts a section of the run file. The six',
      '# effects are picked on s5 through the stock FX list, which clamps at its top: 56 steps ccw reach the first row',
      '# from anywhere, then 8 steps per row (cwo delay grid nitro phone punch spring); the synth key closes it (p5b used',
      '# SEQ + synth, which starts the sequencer on the device). The test leaves s5 on spring.',
      '#@name p5a', '#@tail 6000',
      'accel 64 -64 512', 'wait-frames 1 300000']
at(t); L.append('tap synth 200')
# Voltage and Phase: six voices, a knob sweep while held
for name, slot in (('voltage6', 's2'), ('phase6', 's7')):
    t += 1500; at(t); L.append('#@mark ' + name); L.append('tap %s 200' % slot)
    t += 1500; chord(CHORD6, t, 7000)
    at(t + 2000); L.append('turn enc1 cw 16')
    at(t + 4000); L.append('turn enc1 ccw 16')
    t += 9000
# DSynth dry
t += 1000; at(t); L.append('#@mark dsynth6'); L.append('tap s5 200')
t += 1500; chord(CHORD6, t, 6000); t += 8000
# DSynth through each rewritten effect, knobs turned while the chord sounds
for row, fx in ((6, 'punch'), (5, 'phone'), (4, 'nitro'), (3, 'grid'), (7, 'spring'), (2, 'delay')):
    at(t); L.append('#@mark ' + fx); L.append('tap t1 200')
    at(t + 500); L.append('tap t3 200')
    at(t + 1500); L.append('press shift')
    at(t + 1700); L.append('tap t3 200')
    at(t + 2200); L.append('release shift')
    at(t + 3000); L.append('turn enc1 ccw 56')
    at(t + 4400); L.append('turn enc1 cw %d' % (8 * (row - 1)))
    at(t + 6000); L.append('tap synth 200')                  # closes the list (T1-T4 do too); SEQ would start the sequencer
    s = t + 8500
    chord(CHORD3, s, 6000)
    for k, enc in enumerate(('enc1', 'enc2', 'enc3', 'enc4')):
        at(s + 500 + 600 * k); L.append('turn %s cw 16' % enc)
        at(s + 3000 + 600 * k); L.append('turn %s ccw 16' % enc)
    t = s + 6000 + (5000 if fx in ('spring', 'delay') else 2500)   # the tails ring out
# The other engines, three voices each, for the ranking
for name, slot in (('digital', 's1'), ('pulse', 's3'), ('drwave', 's4'), ('fm', 's6'), ('sampler', 's8')):
    at(t); L.append('#@mark ' + name); L.append('tap %s 200' % slot)
    chord(CHORD3, t + 1500, 3500); t += 6500
# Drums: whatever kit the drum mode holds, eight hits
at(t); L.append('#@mark drum'); L.append('tap drum 200')
for i, k in enumerate(['c4', 'e4', 'd4', 'e4', 'c4', 'e4', 'd4', 'f4']):
    at(t + 1500 + 400 * i); L.append('tap %s 100' % k)
t += 1500 + 400 * 8 + 1500
at(t); L.append('tap synth 200')
L += ['wait 200000', 'quit']
open(__import__('os').path.join(__import__('os').path.dirname(__file__), 'run_p5a.txt'), 'w').write('\n'.join(L) + '\n')
print('ends at', t)
