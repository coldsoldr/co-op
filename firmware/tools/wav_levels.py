#!/usr/bin/env python3
"""Per-second RMS of two op1emu WAVs side by side, plus the best time offset at a few points.

  wav_levels.py A.wav B.wav [--from S] [--seconds N] [--shift-at S1,S2,...]

The A/B check for code that changes the audio callback's speed: op1emu then delivers key events one or more
64-sample blocks later, so a sample-exact compare (wav_compare.py) fails even when the arithmetic is identical.
Equal per-second levels plus a whole-block offset with a residual at the emulator's run-to-run noise (about 0.3 per
sample) is the pass. A level that differs by more than a percent in any second is a fail, whatever the offset.
Stdlib only, Python 3.8.
"""
import argparse
import array
import math
import wave


def read(path):
    w = wave.open(path)
    rate = w.getframerate()
    data = array.array("h", w.readframes(w.getnframes()))
    return rate, data


def rms(data, lo, hi):
    n = max(1, (hi - lo))
    return math.sqrt(sum(x * x for x in data[lo:hi]) / n)


def best_shift(a, b, start, span=2000, reach=2000):
    best = None
    for sh in range(-reach, reach + 1):
        if start + sh < 0 or start + span + sh >= len(b) or start + span >= len(a):
            continue
        e = sum(abs(a[i] - b[i + sh]) for i in range(start, start + span, 4))
        if best is None or e < best[0]:
            best = (e, sh)
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--from", dest="start", type=int, default=0, help="first second to print")
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--shift-at", default="", help="comma-separated seconds at which to search the best offset")
    args = ap.parse_args()
    ra, a = read(args.a)
    rb, b = read(args.b)
    if ra != rb:
        raise SystemExit("different sample rates")
    worst = 0.0
    row = []
    for s in range(args.start, args.start + args.seconds):
        lo, hi = 2 * s * ra, 2 * (s + 1) * ra
        if hi > len(a) or hi > len(b):
            break
        ea, eb = rms(a, lo, hi), rms(b, lo, hi)
        if max(ea, eb) > 100:
            worst = max(worst, abs(ea - eb) / max(ea, eb))
        row.append("%d:%d/%d" % (s, ea, eb))
    print(" ".join(row))
    print("largest level difference in a loud second: %.2f %%" % (100 * worst))
    left_a, left_b = a[0::2], b[0::2]
    for s in [float(x) for x in args.shift_at.split(",") if x]:
        e, sh = best_shift(left_a, left_b, int(s * ra))
        print("at %.1f s: best offset %d samples, residual %.2f per sample" % (s, sh, e / 500.0))


if __name__ == "__main__":
    main()
