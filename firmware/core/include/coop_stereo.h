/* coop-fw stereo instrument bus, milestone C1: the DSP behind the L1 takeover (docs/stereo-bus.md).
 *
 * Architecture ("difference bus"): the stock mono path is left running untouched -- per-voice sum into
 * the mono bus, L1 half-band decimation, SDRAM DC blocker, insert FX, meters. coop adds, per panned
 * voice, only the DIFFERENCE between its stereo contribution and its mono one:
 *     dL += v * (gL - 1),  dR += v * (gR - 1)       (v = the exact stock per-voice contribution)
 * runs dL/dR through replicas of the stock decimator and DC blocker (same taps, same arithmetic), and
 * the replaced mix loop writes  L = M + dL,  R = M + dR  into the master pair with the stock gain and
 * ramp. With every voice centred (gL = gR = 1) dL = dR = 0 and the output is the stock output bit for
 * bit, FX included: that is the C1 regression gate, by construction. The pan law is equal power with
 * unity at centre: gL = sqrt2*cos(u*pi/2), gR = sqrt2*sin(u*pi/2), u = (pan+1)/2, so gL^2 + gR^2 = 2
 * everywhere (the power of the stock L = R = M pair).
 *
 * Everything here is pure, host-testable code (tests/host/stereo_test.cc). The device-only parts are
 * the hooks and thunks in core/sdram/stereo_bus.cc + stereo_thunks.S. Fixed point only, no libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef COOP_STEREO_H
#define COOP_STEREO_H
#include <stdint.h>
#include "coop_fixed.h"
#include "coop_voice.h"          /* coop_sin_q15, coop_spread */

#define COOP_VBLK 128            /* voice render block: 128 samples at 88.2 kHz */
#define COOP_MBLK 64             /* master block: 64 samples at 44.1 kHz */
#define COOP_HB_TAPS 32          /* even-phase taps of the stock half-band decimator (0xffa005d4) */
#define COOP_SYNTH_VOICES 6
#define COOP_DRUM_VOICES 12

/* ---- pan law: the per-voice deltas gL-1, gR-1 in Q15 ([-1, +0.414]) ----
 * Centre returns exactly (0, 0) (the table's sin(pi/4) times sqrt2 rounds to 1.0), which is what makes
 * the centred mix identical to stock. */
typedef struct { int16_t dL, dR; } coop_pan_delta;

static inline coop_pan_delta coop_pan_deltas(int16_t pan_q15)
{
    int32_t u = (int32_t)pan_q15 + 32768;        /* 0..65535 */
    int32_t pos = u >> 10, frac = u & 1023;
    if (pos > 63) { pos = 63; frac = 1023; }
    int32_t s = coop_sin_q15[pos] + ((coop_sin_q15[pos + 1] - coop_sin_q15[pos]) * frac >> 10);
    int32_t hi = 64 - pos;
    int32_t c = coop_sin_q15[hi] + ((coop_sin_q15[hi - 1] - coop_sin_q15[hi]) * frac >> 10);
    /* times sqrt2, rounded, minus 1.0. The table's sin(pi/4) is 23170 (true 23170.47), so sqrt2 is
     * taken as 46342/32768 rather than 46341: 23170 * 46342 rounds to exactly 32768 and the centre
     * delta is exactly 0, which the regression gate depends on; the hard sides still clamp to the
     * same values and the law stays monotone. */
    int32_t gR = ((s * 46342 + 16384) >> 15) - 32768;
    int32_t gL = ((c * 46342 + 16384) >> 15) - 32768;
    if (gR > 32767) gR = 32767;
    if (gL > 32767) gL = 32767;
    if (gR < -32768) gR = -32768;
    if (gL < -32768) gL = -32768;
    coop_pan_delta d; d.dL = (int16_t)gL; d.dR = (int16_t)gR;
    return d;
}

/* ---- per-voice difference accumulate (runs right after the stock render, before the stock sum) ----
 * The stock synth sum (0xffa00d46) adds  sat(out[i] * env[i]) * gain  per sample (16-bit fractional,
 * default rounding); the drum sum (0xffa00922) adds  sat(2 * (out[i] * gain)). We form the identical
 * per-voice value v and accumulate v*(g-1) into the int32 difference buses. */
static inline void coop_side_accum_synth(int32_t *dL, int32_t *dR, const int16_t *out,
                                         const int16_t *env, int16_t gain, coop_pan_delta d)
{
    for (int i = 0; i < COOP_VBLK; i++) {
        int16_t v = coop_fr16_mul(coop_fr16_mul(out[i], env[i]), gain);
        dL[i] += ((int32_t)v * d.dL) >> 15;
        dR[i] += ((int32_t)v * d.dR) >> 15;
    }
}

static inline void coop_side_accum_drum(int32_t *dL, int32_t *dR, const int16_t *out,
                                        int16_t gain, coop_pan_delta d)
{
    for (int i = 0; i < COOP_VBLK; i++) {
        int16_t v = coop_fr16_shl1(coop_fr16_mul(out[i], gain));
        dL[i] += ((int32_t)v * d.dL) >> 15;
        dR[i] += ((int32_t)v * d.dR) >> 15;
    }
}

/* ---- replica of the stock L1 half-band decimator (0xffa005d4..0xffa00682) ----
 * Per output: the two new input samples go into 32-entry rings (even into one, odd into the other);
 * acc = odd[16 back] * 0.5 + sum_k coef[k] * even[oldest + k]   (A0 += (W32): 32-bit saturating MACs)
 * out = (acc >> 16) * ramp;  ramp = sat(ramp + step)   -- the volume ramp word at 0xff80107c.
 * The 32 taps are the live stock table (0x0106a6f0, read at run time). */
typedef struct { int16_t even[COOP_HB_TAPS], odd[COOP_HB_TAPS]; int w; } coop_hb_state;

static inline void coop_hb_init(coop_hb_state *st)
{
    for (int i = 0; i < COOP_HB_TAPS; i++) st->even[i] = st->odd[i] = 0;
    st->w = 0;
}

/* the 32-tap even-phase MAC: acc += sum_k coef[k] * ring[(start + k) & 31], W32 semantics */
static inline int32_t coop_hb_fir(int32_t acc, const int16_t *ring, int start, const int16_t *coefs)
{
#ifdef __bfin__
    /* circular I0 over the 64-byte ring, linear I1 over the taps; hardware loop; A0 in W32 mode.
     * Operands arrive in D registers and are moved into the DAG registers here, and the loop count
     * goes through P2 (clobbered) so the LSETUP always names a P0-P5 register (gcc would otherwise
     * hand it FP). B0/L0/I0/I1/LC0 are declared clobbered; L0 is reset to 0 for compiled code. */
    int32_t r, t1, t2;
    __asm__ volatile(
        "A0 = %[acc];\n\t"
        "B0 = %[base];\n\t"
        "L0 = 64 (X);\n\t"
        "I0 = %[start];\n\t"
        "I1 = %[coefs];\n\t"
        "P2 = %[cnt];\n\t"
        "%[t1].L = W[I0++];\n\t"
        "%[t2].L = W[I1++];\n\t"
        "LSETUP (1f, 1f) LC0 = P2;\n\t"
        "1: A0 += %[t1].L * %[t2].L (W32) || %[t1].L = W[I0++] || %[t2].L = W[I1++];\n\t"
        "A0 += %[t1].L * %[t2].L (W32);\n\t"
        "L0 = 0 (X);\n\t"
        "%[r] = A0.W;"
        : [r] "=d"(r), [t1] "=&d"(t1), [t2] "=&d"(t2)
        : [acc] "d"(acc), [base] "d"(ring), [start] "d"(ring + start), [coefs] "d"(coefs),
          [cnt] "d"(COOP_HB_TAPS - 1)
        : "A0", "I0", "I1", "B0", "L0", "P2", "LC0", "LT0", "LB0", "memory");
    return r;
#else
    for (int k = 0; k < COOP_HB_TAPS; k++)
        acc = coop_mac_w32(acc, coefs[k], ring[(start + k) & (COOP_HB_TAPS - 1)]);
    return acc;
#endif
}

/* in: 128 int32 difference samples (saturated to int16 on the way in, like the stock int16 bus);
 * out: 64 int16. ramp/step: the stock volume ramp word's halves as they were before the stock
 * decimator ran this block. */
static inline void coop_hb_decimate(coop_hb_state *st, const int32_t *in, int16_t *out,
                                    const int16_t *coefs, int16_t ramp, int16_t step)
{
    int w = st->w;
    for (int n = 0; n < COOP_MBLK; n++) {
        st->even[w] = coop_sat16(in[2 * n]);
        st->odd[w] = coop_sat16(in[2 * n + 1]);
        w = (w + 1) & (COOP_HB_TAPS - 1);                   /* now the oldest slot */
        int32_t acc = (int32_t)st->odd[(w + 15) & (COOP_HB_TAPS - 1)] << 15;   /* centre tap * 0.5 */
        acc = coop_hb_fir(acc, st->even, w, coefs);
        out[n] = coop_fr16_mul((int16_t)(acc >> 16), ramp);
        ramp = coop_fr16_add(step, ramp);
    }
    st->w = w;
}

/* ---- replica of the stock SDRAM DC blocker (0x019674f8..0x01967612) ----
 * y[n] = sat(sat(y[n-1] - X[n-1]) + X[n]) - c*Yq[n-1],  out = y >> 16,  X = in << 16, Yq = out << 16,
 * c = the live coefficient word at 0x01bab53c, products via coop_fr32_mul_stock. */
typedef struct { int32_t xprev, yq, y; } coop_dc_state;

static inline void coop_dc_init(coop_dc_state *st) { st->xprev = st->yq = st->y = 0; }

static inline void coop_dc_block(coop_dc_state *st, const int16_t *in, int16_t *out, int32_t c, int n)
{
    int32_t xp = st->xprev, yq = st->yq, y = st->y;
    for (int i = 0; i < n; i++) {
        int32_t x = (int32_t)in[i] << 16;
        int32_t t = coop_ssub32(y, xp);
        t = coop_sadd32(t, x);
        y = coop_ssub32(t, coop_fr32_mul_stock(c, yq));
        int16_t o = (int16_t)(y >> 16);
        out[i] = o;
        xp = x;
        yq = (int32_t)o << 16;
    }
    st->xprev = xp; st->yq = yq; st->y = y;
}

/* ---- replica of the stock mix loop B (0xffa00fae..0xffa00fdc) with the difference buses added ----
 * stock:  v = (M[i] * gain) * ramp;  L[i] = sat(L[i] + v);  R[i] = sat(R[i] + v);  ramp = sat(ramp + step)
 * coop:   vL from sat(M[i] + dL[i]), vR from sat(M[i] + dR[i]).  dL = dR = 0 reproduces stock exactly. */
/* Stock mix loop A (0xffa00f2c..0xffa00f86: the instrument monitor on the tape page / while the tape runs):
 * per sample  L[i] += ((m*gL)*ramp),  R[i] += ((m*gR)*ramp),  meter = max(meter, |m*level|),  ramp += step
 * with ramp = W[rec], meter = W[rec+1], level read from 0x01b4ef6e every sample, all fr16 ops. The coop version
 * adds the difference buses to m before the gains. Returns the final ramp. */
static inline int16_t coop_mix_a(volatile int16_t *L, volatile int16_t *R, const int16_t *M,
                                 const int16_t *dL, const int16_t *dR, int16_t gL, int16_t gR, int16_t step,
                                 volatile int16_t *rec, const volatile int16_t *level)
{
    int16_t ramp = rec[0], meter = rec[1];
    for (int i = 0; i < COOP_MBLK; i++) {
        int16_t m = M[i];
        int16_t lv = coop_fr16_mul(m, *level);
        if (lv < 0) lv = (int16_t)(lv == -32768 ? 32767 : -lv);
        if (lv > meter) meter = lv;
        int16_t vl = coop_fr16_mul(coop_fr16_mul(coop_fr16_add(m, dL[i]), gL), ramp);
        int16_t vr = coop_fr16_mul(coop_fr16_mul(coop_fr16_add(m, dR[i]), gR), ramp);
        L[i] = coop_fr16_add(vl, L[i]);
        R[i] = coop_fr16_add(vr, R[i]);
        ramp = coop_fr16_add(step, ramp);
    }
    rec[0] = ramp; rec[1] = meter;
    return ramp;
}

static inline int16_t coop_mix_a_mono(volatile int16_t *L, volatile int16_t *R, const int16_t *M,
                                      int16_t gL, int16_t gR, int16_t step, volatile int16_t *rec,
                                      const volatile int16_t *level)
{
    int16_t ramp = rec[0], meter = rec[1];
    for (int i = 0; i < COOP_MBLK; i++) {
        int16_t m = M[i];
        int16_t lv = coop_fr16_mul(m, *level);
        if (lv < 0) lv = (int16_t)(lv == -32768 ? 32767 : -lv);
        if (lv > meter) meter = lv;
        int16_t vl = coop_fr16_mul(coop_fr16_mul(m, gL), ramp);
        int16_t vr = coop_fr16_mul(coop_fr16_mul(m, gR), ramp);
        L[i] = coop_fr16_add(vl, L[i]);
        R[i] = coop_fr16_add(vr, R[i]);
        ramp = coop_fr16_add(step, ramp);
    }
    rec[0] = ramp; rec[1] = meter;
    return ramp;
}

static inline void coop_mix_b(volatile int16_t *L, volatile int16_t *R, const int16_t *M,
                              const int16_t *dL, const int16_t *dR, int16_t gain,
                              int16_t *ramp_state, int16_t step)
{
    int16_t ramp = *ramp_state;
    for (int i = 0; i < COOP_MBLK; i++) {
        int16_t m = M[i];
        int16_t vl = coop_fr16_mul(coop_fr16_mul(coop_fr16_add(m, dL[i]), gain), ramp);
        int16_t vr = coop_fr16_mul(coop_fr16_mul(coop_fr16_add(m, dR[i]), gain), ramp);
        L[i] = coop_fr16_add(vl, L[i]);
        R[i] = coop_fr16_add(vr, R[i]);
        ramp = coop_fr16_add(step, ramp);
    }
    *ramp_state = ramp;
}

/* the mono fast path (no panned voice sounding): exactly loop B */
static inline void coop_mix_b_mono(volatile int16_t *L, volatile int16_t *R, const int16_t *M,
                                   int16_t gain, int16_t *ramp_state, int16_t step)
{
    int16_t ramp = *ramp_state;
    for (int i = 0; i < COOP_MBLK; i++) {
        int16_t v = coop_fr16_mul(coop_fr16_mul(M[i], gain), ramp);
        L[i] = coop_fr16_add(v, L[i]);
        R[i] = coop_fr16_add(v, R[i]);
        ramp = coop_fr16_add(step, ramp);
    }
    *ramp_state = ramp;
}

#endif
