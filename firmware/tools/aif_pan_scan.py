#!/usr/bin/env python3
"""Where is the audio in an OP-1 tape track file, and how is each partial placed?

Reads a track file copied in disk mode (AIFC, sowt, stereo), lists the 0.25 s segments with signal and prints,
for a 32768-frame window at the loudest one, every spectral peak with its left / right magnitude and R/L ratio:
partials of one voice share a ratio, so distinct ratio families mean per-voice placement survived the record
path (docs/tape-stereo.md section 12). Stdlib only (array, audioop, a radix-2 FFT in Python); takes a minute.

    python3 -I tools/aif_pan_scan.py ~/Downloads/track_1.aif
"""
import sys, struct, audioop, array, math, cmath
path = sys.argv[1]
data = open(path, "rb").read()
# chunk walk
pos = 12; ssnd = None; frames = None
while pos + 8 <= len(data):
    cid = data[pos:pos+4]; ln = struct.unpack(">I", data[pos+4:pos+8])[0]
    if cid == b"COMM": frames = struct.unpack(">I", data[pos+10:pos+14])[0]
    if cid == b"SSND":
        off = struct.unpack(">I", data[pos+8:pos+12])[0]; ssnd = pos + 16 + off; break
    pos += 8 + ln + (ln & 1)
print("frames", frames, "ssnd at", hex(ssnd))
R = 44100
seg = R // 4 * 4  # bytes per 0.25 s
loud = []
for i in range(0, frames * 4, seg):
    chunk = data[ssnd + i: ssnd + i + seg]
    if len(chunk) < 8: break
    v = audioop.rms(chunk, 2)
    if v > 20: loud.append((i // 4 / R, v))
print("loud 0.25s segments (t, rms):")
for t, v in loud[:200]: print("  %7.2f %6d" % (t, v))
if not loud: sys.exit()
# FFT on 32768 frames from the loudest segment start
best_t = max(loud, key=lambda x: x[1])[0]
n = 32768
start = ssnd + int(best_t * R) * 4
a = array.array("h"); a.frombytes(data[start:start + n * 4])
if sys.byteorder != "little": a.byteswap()
L = a[0::2]; Rr = a[1::2]
def fft(x):
    N = len(x)
    if N == 1: return x
    e = fft(x[0::2]); o = fft(x[1::2])
    out = [0] * N
    for k in range(N // 2):
        t = cmath.exp(-2j * math.pi * k / N) * o[k]
        out[k] = e[k] + t; out[k + N // 2] = e[k] - t
    return out
w = [0.5 - 0.5 * math.cos(2 * math.pi * i / n) for i in range(n)]
FL = fft([L[i] * w[i] for i in range(n)]); FR = fft([Rr[i] * w[i] for i in range(n)])
mag = [(abs(FL[k]), abs(FR[k])) for k in range(n // 2)]
tot = [m[0] + m[1] for m in mag]
mx = max(tot)
peaks = []
for k in range(3, n // 2 - 3):
    if tot[k] > 0.02 * mx and tot[k] >= max(tot[k-3:k+4]):
        peaks.append((k * R / n, mag[k][0], mag[k][1]))
print("window at %.2f s, %d peaks (freq, L, R, R/L):" % (best_t, len(peaks)))
for f, l, r in peaks[:60]:
    print("  %8.1f Hz  L %9.0f  R %9.0f  R/L %.2f" % (f, l, r, r / l if l else 0))
