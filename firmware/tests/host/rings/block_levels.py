#!/usr/bin/env python3
"""Per-voice-block level difference of two rings_compare outputs (s16le voice blocks in the same order): |dB| of each
128-sample block's energy, over the blocks above about -50 dBFS RMS. Stdlib only.

    python3 -I block_levels.py a.raw b.raw
"""
import array, math, sys
def load(p):
    a = array.array('h'); a.frombytes(open(p,'rb').read()); return a
a, b = load(sys.argv[1]), load(sys.argv[2])
d = []
for i in range(0, len(a), 128):
    sa = sum(x*x for x in a[i:i+128]); sb = sum(y*y for y in b[i:i+128])
    if sa > 128 * 100**2 and sb > 0:   # blocks above ~ -50 dBFS RMS
        d.append(abs(10*math.log10(sb/sa)))
d.sort()
print('voice blocks %d: |level diff| mean %.2f dB, median %.2f, p95 %.2f, p99 %.2f' % (len(d), sum(d)/len(d), d[len(d)//2], d[int(len(d)*.95)], d[int(len(d)*.99)]))
