#!/usr/bin/env python3
"""Spectral comparison of two rings_compare mixes (OUT.raw.mix: int32, the voices summed, 88.2 kHz): for a build that
moves control updates in time (OP1RINGS_CONTROL_HALVED), the waveforms decorrelate in phase while the sound stays the
same, so compare levels per octave band instead. Frames of 4096 samples (46 ms), Hann window; per frame and band
(62.5 Hz .. 22 kHz, 9 octaves) the level difference in dB where A has signal (above -80 dB of the loudest frame),
then the mean and 95th percentile of |difference| per band and over all. Stdlib only (an iterative FFT).

    python3 -I compare_spectra.py ref.raw.mix halved.raw.mix [max_frames]
"""
import array, cmath, math, sys

N = 4096
RATE = 88200


def fft(x):
    n = len(x)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            x[i], x[j] = x[j], x[i]
    size = 2
    while size <= n:
        w = cmath.exp(-2j * math.pi / size)
        for start in range(0, n, size):
            wk = 1
            for k in range(size // 2):
                a, b = x[start + k], x[start + k + size // 2] * wk
                x[start + k], x[start + k + size // 2] = a + b, a - b
                wk *= w
        size <<= 1
    return x


def bands(frame, window):
    spec = fft([complex(s * w) for s, w in zip(frame, window)])
    out = []
    lo = 62.5
    for _ in range(9):
        hi = lo * 2
        a, b = int(lo * N / RATE), int(hi * N / RATE)
        out.append(sum(abs(spec[k]) ** 2 for k in range(max(a, 1), b)))
        lo = hi
    return out


def load(path):
    a = array.array('i')
    with open(path, 'rb') as f:
        a.frombytes(f.read())
    return a


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 10 ** 9
    window = [0.5 - 0.5 * math.cos(2 * math.pi * i / (N - 1)) for i in range(N)]
    frames = []
    for i in range(0, min(len(a), len(b)) - N + 1, N):
        if len(frames) >= limit:
            break
        frames.append((bands(a[i:i + N], window), bands(b[i:i + N], window)))
    peak = max(max(fa) for fa, _ in frames)
    diffs = [[] for _ in range(9)]
    for fa, fb in frames:
        for k in range(9):
            if fa[k] > peak * 1e-8 and fb[k] > 0:
                diffs[k].append(abs(10 * math.log10(fb[k] / fa[k])))
    every = sorted(d for ds in diffs for d in ds)
    lo = 62.5
    for k in range(9):
        ds = sorted(diffs[k])
        if ds:
            print('  %6.0f-%-6.0f Hz  frames %5d  mean |diff| %5.2f dB  p95 %5.2f dB'
                  % (lo, lo * 2, len(ds), sum(ds) / len(ds), ds[int(len(ds) * 0.95)]))
        lo *= 2
    print('frames %d  all bands: mean |diff| %.2f dB, p95 %.2f dB'
          % (len(frames), sum(every) / len(every), every[int(len(every) * 0.95)]))


if __name__ == '__main__':
    main()
