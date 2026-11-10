#!/usr/bin/env python3
"""Compare two rings_compare outputs (s16le, the same scenario's voice blocks in the same order): the overall SNR of B
against A, the level ratio, and the SNR of the worst 1 % of 128-sample blocks (by error energy, among blocks with any
signal). For a build that is close but not bit-identical (OP1RINGS_CONTROL_HALVED). Stdlib only, Python 3.8.

    python3 -I compare_raw.py ref.raw halved.raw
"""
import array, math, sys


def load(path):
    a = array.array('h')
    with open(path, 'rb') as f:
        a.frombytes(f.read())
    return a


def db(num, den):
    return 99.0 if den == 0 else (-99.0 if num == 0 else 10 * math.log10(num / den))


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    if len(a) != len(b):
        sys.exit('lengths differ: %d, %d' % (len(a), len(b)))
    sig = err = sb = 0
    blocks = []
    for i in range(0, len(a), 128):
        s = e = 0
        for x, y in zip(a[i:i + 128], b[i:i + 128]):
            s += x * x
            e += (x - y) * (x - y)
            sb += y * y
        sig += s
        err += e
        if s:
            blocks.append((e / s, s, e))
    blocks.sort(reverse=True)
    worst = blocks[:max(1, len(blocks) // 100)]
    ws, we = sum(w[1] for w in worst), sum(w[2] for w in worst)
    print('blocks %d  SNR %.1f dB  level B/A %+.2f dB  worst 1%% of blocks SNR %.1f dB'
          % (len(blocks), db(sig, err), db(sb, sig), db(ws, we)))


if __name__ == '__main__':
    main()
