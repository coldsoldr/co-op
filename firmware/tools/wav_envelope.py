#!/usr/bin/env python3
"""Amplitude envelope of an op1emu --audio-dump WAV over a time range: RMS per window, plus the
modulation depth and dominant period of that envelope. For checking an LFO/tremolo on a held chord.

  wav_envelope.py FILE.wav --from S --to S [--window MS] [--channel 0]

Prints one line per window (time, rms) when --verbose, and always a summary: mean rms, min/max,
depth = (max-min)/mean, and the period of the envelope's strongest oscillation (zero-crossing based).
Stdlib only.
"""
import argparse
import math
import struct
import wave


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('wav')
    ap.add_argument('--from', dest='t0', type=float, required=True)
    ap.add_argument('--to', dest='t1', type=float, required=True)
    ap.add_argument('--window', type=float, default=10.0, help='window in ms (default 10)')
    ap.add_argument('--channel', type=int, default=0)
    ap.add_argument('--verbose', action='store_true')
    a = ap.parse_args()
    w = wave.open(a.wav, 'rb')
    ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
    w.close()
    s = struct.unpack('<{}h'.format(len(raw) // 2), raw)[a.channel::ch]
    i0, i1 = int(a.t0 * rate), min(int(a.t1 * rate), len(s))
    win = max(1, int(a.window * rate / 1000))
    env = []
    for i in range(i0, i1 - win, win):
        seg = s[i:i + win]
        env.append(math.sqrt(sum(v * v for v in seg) / len(seg)))
    if not env:
        raise SystemExit('empty range')
    mean = sum(env) / len(env)
    lo, hi = min(env), max(env)
    if a.verbose:
        for k, e in enumerate(env):
            print('{:.3f}s rms {:.1f}'.format(a.t0 + k * win / rate, e))
    # period: average distance between upward crossings of the mean
    crossings = [k for k in range(1, len(env)) if env[k - 1] < mean <= env[k]]
    period = None
    if len(crossings) >= 2:
        period = (crossings[-1] - crossings[0]) / (len(crossings) - 1) * win / rate
    print('{}: {:.2f}-{:.2f}s, {} windows of {} ms: mean rms {:.1f}, min {:.1f}, max {:.1f}, depth {:.3f}{}'.format(
        a.wav, a.t0, a.t1, len(env), a.window, mean, lo, hi, (hi - lo) / mean if mean else 0.0,
        ', envelope period {:.3f} s ({:.2f} Hz)'.format(period, 1 / period) if period else ''))


if __name__ == '__main__':
    main()
