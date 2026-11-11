# Designs SHIFT's Hilbert pair offline (pure Python, no numpy) and prints the
# Q15 coefficient table for shift.h.
#
# Structure (Niemitalo's polyphase IIR Hilbert pair): two chains of
# allpasses in z^-2,
#     A_k(z) = (a_k - z^-2) / (1 - a_k z^-2),    y = a (x + y[n-2]) - x[n-2],
# path I = A_0 A_2 A_4 ..., path Q = A_1 A_3 ... z^-1. Their phase
# difference is ~90 degrees over the band. The unwanted sideband of a
# shifter built on them is tan(|e| / 2) of the wanted one, e = the phase
# difference's error from 90 degrees.
#
# Usage: python design_hilbert.py [sections per path] [f_lo] [f_hi]
# The coefficients are optimised in floating point (minimax, Nelder-Mead on
# a p-norm), then rounded to Q15 and polished by an integer search; the
# figures printed are for the Q15 values the code uses.

import cmath
import math
import sys

FS = 44100.0


def phase_err(coefs, f):
    w = 2 * math.pi * f / FS
    z2 = cmath.exp(-2j * w)
    hi = 1 + 0j
    hq = cmath.exp(-1j * w)
    for k, a in enumerate(coefs):
        h = (a - z2) / (1 - a * z2)
        if k & 1:
            hq *= h
        else:
            hi *= h
    d = cmath.phase(hi / hq)   # I relative to Q
    return abs(abs(d) - math.pi / 2)


def grid(lo, hi, n=160):
    return [lo * (hi / lo) ** (i / (n - 1)) for i in range(n)]


def cost(coefs, fs, p=16):
    for a in coefs:
        if not (0.0 < a < 0.99995):
            return 1e9
    s = sum(phase_err(coefs, f) ** p for f in fs)
    return (s / len(fs)) ** (1.0 / p)


def max_err(coefs, fs):
    return max(phase_err(coefs, f) for f in fs)


def nelder_mead(fn, x0, step=0.02, iters=4000):
    n = len(x0)
    pts = [list(x0)]
    for i in range(n):
        x = list(x0)
        x[i] += step
        pts.append(x)
    vals = [fn(p) for p in pts]
    for _ in range(iters):
        order = sorted(range(n + 1), key=lambda i: vals[i])
        pts = [pts[i] for i in order]
        vals = [vals[i] for i in order]
        c = [sum(p[j] for p in pts[:-1]) / n for j in range(n)]
        xr = [c[j] + (c[j] - pts[-1][j]) for j in range(n)]
        fr = fn(xr)
        if fr < vals[0]:
            xe = [c[j] + 2 * (c[j] - pts[-1][j]) for j in range(n)]
            fe = fn(xe)
            if fe < fr:
                pts[-1], vals[-1] = xe, fe
            else:
                pts[-1], vals[-1] = xr, fr
        elif fr < vals[-2]:
            pts[-1], vals[-1] = xr, fr
        else:
            xc = [c[j] + 0.5 * (pts[-1][j] - c[j]) for j in range(n)]
            fc = fn(xc)
            if fc < vals[-1]:
                pts[-1], vals[-1] = xc, fc
            else:
                for i in range(1, n + 1):
                    pts[i] = [pts[0][j] + 0.5 * (pts[i][j] - pts[0][j]) for j in range(n)]
                    vals[i] = fn(pts[i])
    i = min(range(n + 1), key=lambda i: vals[i])
    return pts[i]


def rejection_db(e):
    return -20 * math.log10(math.tan(e / 2)) if e > 0 else 999.0


def main():
    sections = int(sys.argv[1]) if len(sys.argv) > 1 else 4
    lo = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    hi = float(sys.argv[3]) if len(sys.argv) > 3 else 13000.0
    design = grid(lo, hi)
    check = grid(100.0, 10000.0, 400)
    # Start: Niemitalo's 4 + 4 design (20 Hz-22 kHz at 44.1 kHz), squared
    # (his values are the a of a^2 here), interleaved I/Q; fewer sections
    # take its inner poles.
    ni = [0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278]
    nq = [0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737]
    n = 2 * sections
    x = []
    for k in range(sections):
        x += [ni[k] ** 2, nq[k] ** 2]
    best = None
    for p in (4, 8, 16, 32, 64):
        for _ in range(3):
            x = nelder_mead(lambda c: cost(c, design, p), x)
        e = max_err(x, design)
        if best is None or e < best[0]:
            best = (e, list(x))
    x = best[1]
    print("float: max err %.4f deg design band, %.1f dB worst 100 Hz-10 kHz" %
          (math.degrees(best[0]), rejection_db(max_err(x, check))))
    # Q15, then an integer polish of the worst case over the check band.
    q = [int(round(a * 32768)) for a in x]
    def qerr(qq):
        return max_err([v / 32768.0 for v in qq], check)
    e = qerr(q)
    improved = True
    while improved:
        improved = False
        for i in range(n):
            for d in (-1, 1):
                t = list(q)
                t[i] += d
                et = qerr(t)
                if et < e:
                    q, e, improved = t, et, True
    print("Q15: worst %.1f dB over 100 Hz-10 kHz" % rejection_db(e))
    for f in (20, 30, 50, 70, 100, 200, 500, 1000, 2000, 5000, 8000, 10000, 12000, 15000, 18000):
        print("  %6d Hz  %.1f dB" % (f, rejection_db(phase_err([v / 32768.0 for v in q], f))))
    print("const int16_t kShiftHilbert[%d] = {  // I: even, Q: odd (z^-1)" % n)
    print("  " + ", ".join(str(v) for v in q) + ",")
    print("};")


if __name__ == "__main__":
    main()
