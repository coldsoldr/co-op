#!/usr/bin/env python3
"""Compare two op1emu --audio-dump WAVs (16-bit stereo 44.1 kHz): the A/B gate for coop images.

  wav_compare.py A.wav B.wav [--skip-seconds S] [--max-diff N]

Prints per-channel RMS, the number and span of differing samples, the maximum absolute difference, the
first differing sample, and (for each file) the L/R difference energy as a stereo-width measure.
Exit 0 when the files match within --max-diff (default 0: sample-exact), 1 otherwise. Stdlib only.

Deterministic op1emu runs of the same script produce frame-aligned dumps, so a coop image whose change
is transparent must compare sample-exact against the stock image (docs/stereo-bus.md, the C1 gate).
"""
import argparse
import math
import struct
import sys
import wave


def read(path):
    w = wave.open(path, 'rb')
    ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
    w.close()
    if sw != 2:
        raise SystemExit('{}: {}-byte samples, expected 16-bit'.format(path, sw))
    s = struct.unpack('<{}h'.format(len(raw) // 2), raw)
    chans = [s[c::ch] for c in range(ch)]
    return chans, rate


def rms(x):
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--skip-seconds', type=float, default=0.0, help='ignore the first S seconds (boot)')
    ap.add_argument('--max-diff', type=int, default=0, help='largest tolerated |a-b| per sample')
    ap.add_argument('--align', type=int, metavar='THRESH', default=None,
                    help='align the files on their first frame with |sample| >= THRESH (the note onset): '
                         'op1emu input-script waits are wall time, so the onset block differs run to run')
    args = ap.parse_args()
    A, ra = read(args.a)
    B, rb = read(args.b)
    if ra != rb or len(A) != len(B):
        raise SystemExit('format mismatch: {} Hz x{} vs {} Hz x{}'.format(ra, len(A), rb, len(B)))
    skip = int(args.skip_seconds * ra)
    if args.align is not None:
        def onset(X):
            for i in range(skip, len(X[0])):
                if any(abs(X[c][i]) >= args.align for c in range(len(X))):
                    return i
            raise SystemExit('no sample reaches {} after {:.2f} s'.format(args.align, args.skip_seconds))
        oa, ob = onset(A), onset(B)
        print('onsets: A frame {} ({:.3f} s), B frame {} ({:.3f} s); aligning'.format(oa, oa / ra, ob, ob / rb))
        A = [x[oa:] for x in A]
        B = [x[ob:] for x in B]
        skip = 0
    n = min(len(A[0]), len(B[0]))
    if len(A[0]) != len(B[0]):
        print('length differs: {} vs {} frames (comparing the first {})'.format(len(A[0]), len(B[0]), n))
    worst = 0
    ndiff = 0
    first = None
    last = None
    for c in range(len(A)):
        a, b = A[c][skip:n], B[c][skip:n]
        for i, (x, y) in enumerate(zip(a, b)):
            d = abs(x - y)
            if d:
                ndiff += 1
                last = (c, i + skip)
                if first is None:
                    first = (c, i + skip, x, y)
                if d > worst:
                    worst = d
        print('ch{}: rms A={:.1f} B={:.1f}  peak A={} B={}'.format(
            c, rms(a), rms(b), max(map(abs, a)) if a else 0, max(map(abs, b)) if b else 0))
    for name, X in (('A', A), ('B', B)):
        if len(X) == 2:
            L, R = X[0][skip:n], X[1][skip:n]
            side = [l - r for l, r in zip(L, R)]
            mid = [l + r for l, r in zip(L, R)]
            print('{}: side rms {:.1f}  mid rms {:.1f}  width {:.4f}'.format(
                name, rms(side), rms(mid), rms(side) / rms(mid) if rms(mid) else 0.0))
    if ndiff:
        print('{} differing samples (max |diff| {}); first ch{} frame {} ({:.3f} s): {} vs {}; last ch{} frame {}'.format(
            ndiff, worst, first[0], first[1], first[1] / ra, first[2], first[3], last[0], last[1]))
    else:
        print('identical over {} frames (from {:.2f} s)'.format(n - skip, args.skip_seconds))
    sys.exit(0 if worst <= args.max_diff else 1)


if __name__ == '__main__':
    main()
