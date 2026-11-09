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

static inline coop_pan_delta coop_pan_deltas_tab(int16_t pan_q15, const int16_t *coop_sin_q15)
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
static inline coop_pan_delta coop_pan_deltas(int16_t pan_q15) { return coop_pan_deltas_tab(pan_q15, coop_sin_q15); }

/* ---- per-voice difference accumulate (runs right after the stock render, before the stock sum) ----
 * The stock synth sum (0xffa00d46) adds  sat(out[i] * env[i]) * gain  per sample (16-bit fractional,
 * default rounding); the drum sum (0xffa00922) adds  sat(2 * (out[i] * gain)). We form the identical
 * per-voice value v and accumulate v*(g-1) into the int32 difference buses. */
static inline void coop_side_accum_synth_c(int32_t *dL, int32_t *dR, const int16_t *out,
                                           const int16_t *env, int16_t gain, coop_pan_delta d)
{
    for (int i = 0; i < COOP_VBLK; i++) {
        int16_t v = coop_fr16_mul(coop_fr16_mul(out[i], env[i]), gain);
        dL[i] += ((int32_t)v * d.dL) >> 15;
        dR[i] += ((int32_t)v * d.dR) >> 15;
    }
}

static inline void coop_side_accum_drum_c(int32_t *dL, int32_t *dR, const int16_t *out,
                                          int16_t gain, coop_pan_delta d)
{
    for (int i = 0; i < COOP_VBLK; i++) {
        int16_t v = coop_fr16_shl1(coop_fr16_mul(out[i], gain));
        dL[i] += ((int32_t)v * d.dL) >> 15;
        dR[i] += ((int32_t)v * d.dR) >> 15;
    }
}

/* ---- S3 (stereo bus v2): the full per-voice contribution into the instrument's stereo pair ----
 * L += v + v*(gL-1), R += v + v*(gR-1): the stock per-voice value v placed by the pan law, so a centred voice adds v
 * to both sides (with every voice centred L = R = the stock bus, before saturation). */
static inline void coop_pair_accum_synth_c(int32_t *L, int32_t *R, const int16_t *out,
                                           const int16_t *env, int16_t gain, coop_pan_delta d)
{
    for (int i = 0; i < COOP_VBLK; i++) {
        int16_t v = coop_fr16_mul(coop_fr16_mul(out[i], env[i]), gain);
        L[i] += v + (((int32_t)v * d.dL) >> 15);
        R[i] += v + (((int32_t)v * d.dR) >> 15);
    }
}

static inline void coop_pair_accum_drum_c(int32_t *L, int32_t *R, const int16_t *out,
                                          int16_t gain, coop_pan_delta d)
{
    for (int i = 0; i < COOP_VBLK; i++) {
        int16_t v = coop_fr16_shl1(coop_fr16_mul(out[i], gain));
        L[i] += v + (((int32_t)v * d.dL) >> 15);
        R[i] += v + (((int32_t)v * d.dR) >> 15);
    }
}

#ifdef __bfin__
/* Blackfin kernels (T4). Register budget: the pointers travel in a small argument block whose address is in P5
 * (an explicit register variable, so the asm can use P0-P2 and I0-I2 freely), pairs of 16-bit parameters share
 * one data register's halves, the loop count is preloaded into LC0. The products v*d are integer (IS)
 * 16x16 -> 32 then >>> 15, exactly the C expression; v itself is the same fractional multiply chain as
 * coop_fr16_mul (default rounding). Verified bit-exact against the _c versions in the GNU simulator
 * (tests/sim/kern_check.cc) and by the op1emu gates. */
static inline void coop_side_accum_synth(int32_t *dL, int32_t *dR, const int16_t *out,
                                         const int16_t *env, int16_t gain, coop_pan_delta d)
{
    uint32_t pans = ((uint32_t)(uint16_t)d.dR << 16) | (uint16_t)d.dL;
    const void *args[4] = { out, env, dL, dR };
    register const void **ap __asm__("P5") = args;
    int32_t t0, t1, t2, t4, t6;
    __asm__ volatile(
        "%[t0] = [P5 + 0];\n\t"
        "I0 = %[t0];\n\t"
        "%[t0] = [P5 + 4];\n\t"
        "I1 = %[t0];\n\t"
        "P0 = [P5 + 8];\n\t"
        "P1 = [P5 + 12];\n\t"
        "%[t0] = %[n] (X);\n\t"
        "LC0 = %[t0];\n\t"
        "LSETUP (1f, 2f) LC0;\n\t"
        "1: %[t0].L = W[I0++];\n\t"
        "%[t1].L = W[I1++];\n\t"
        "%[t2].L = %[t0].L * %[t1].L;\n\t"
        "%[t2].L = %[t2].L * %[g].L;\n\t"
        "%[t4] = %[t2].L * %[pans].L (IS);\n\t"
        "%[t6] = %[t2].L * %[pans].H (IS);\n\t"
        "%[t4] >>>= 15;\n\t"
        "%[t6] >>>= 15;\n\t"
        "%[t0] = [P0];\n\t"
        "%[t0] = %[t0] + %[t4];\n\t"
        "[P0++] = %[t0];\n\t"
        "%[t1] = [P1];\n\t"
        "%[t1] = %[t1] + %[t6];\n\t"
        "2: [P1++] = %[t1];"
        : [t0] "=&d"(t0), [t1] "=&d"(t1), [t2] "=&d"(t2), [t4] "=&d"(t4), [t6] "=&d"(t6)
        : "a"(ap), [n] "i"(COOP_VBLK), [g] "d"((int32_t)gain), [pans] "d"(pans)
        : "I0", "I1", "P0", "P1", "LC0", "LT0", "LB0", "memory");
}

static inline void coop_side_accum_drum(int32_t *dL, int32_t *dR, const int16_t *out,
                                        int16_t gain, coop_pan_delta d)
{
    uint32_t pans = ((uint32_t)(uint16_t)d.dR << 16) | (uint16_t)d.dL;
    const void *args[3] = { out, dL, dR };
    register const void **ap __asm__("P5") = args;
    int32_t t0, t1, t2, t4, t6;
    __asm__ volatile(
        "%[t0] = [P5 + 0];\n\t"
        "I0 = %[t0];\n\t"
        "P0 = [P5 + 4];\n\t"
        "P1 = [P5 + 8];\n\t"
        "%[t0] = %[n] (X);\n\t"
        "LC0 = %[t0];\n\t"
        "LSETUP (1f, 2f) LC0;\n\t"
        "1: %[t0].L = W[I0++];\n\t"
        "%[t2].L = %[t0].L * %[g].L;\n\t"
        "%[t2].L = %[t2].L << 1 (S);\n\t"
        "%[t4] = %[t2].L * %[pans].L (IS);\n\t"
        "%[t6] = %[t2].L * %[pans].H (IS);\n\t"
        "%[t4] >>>= 15;\n\t"
        "%[t6] >>>= 15;\n\t"
        "%[t0] = [P0];\n\t"
        "%[t0] = %[t0] + %[t4];\n\t"
        "[P0++] = %[t0];\n\t"
        "%[t1] = [P1];\n\t"
        "%[t1] = %[t1] + %[t6];\n\t"
        "2: [P1++] = %[t1];"
        : [t0] "=&d"(t0), [t1] "=&d"(t1), [t2] "=&d"(t2), [t4] "=&d"(t4), [t6] "=&d"(t6)
        : "a"(ap), [n] "i"(COOP_VBLK), [g] "d"((int32_t)gain), [pans] "d"(pans)
        : "I0", "P0", "P1", "LC0", "LT0", "LB0", "memory");
}
/* S3: the same kernels with the v term added (L += v + v*dL >>> 15): coop_pair_accum_*_c */
static inline void coop_pair_accum_synth(int32_t *dL, int32_t *dR, const int16_t *out,
                                         const int16_t *env, int16_t gain, coop_pan_delta d)
{
    uint32_t pans = ((uint32_t)(uint16_t)d.dR << 16) | (uint16_t)d.dL;
    const void *args[4] = { out, env, dL, dR };
    register const void **ap __asm__("P5") = args;
    int32_t t0, t1, t2, t4, t6;
    __asm__ volatile(
        "%[t0] = [P5 + 0];\n\t"
        "I0 = %[t0];\n\t"
        "%[t0] = [P5 + 4];\n\t"
        "I1 = %[t0];\n\t"
        "P0 = [P5 + 8];\n\t"
        "P1 = [P5 + 12];\n\t"
        "%[t0] = %[n] (X);\n\t"
        "LC0 = %[t0];\n\t"
        "LSETUP (1f, 2f) LC0;\n\t"
        "1: %[t0].L = W[I0++];\n\t"
        "%[t1].L = W[I1++];\n\t"
        "%[t2].L = %[t0].L * %[t1].L;\n\t"
        "%[t2].L = %[t2].L * %[g].L;\n\t"
        "%[t4] = %[t2].L * %[pans].L (IS);\n\t"
        "%[t6] = %[t2].L * %[pans].H (IS);\n\t"
        "%[t4] >>>= 15;\n\t"
        "%[t6] >>>= 15;\n\t"
        "%[t2] = %[t2].L (X);\n\t"
        "%[t4] = %[t4] + %[t2];\n\t"
        "%[t6] = %[t6] + %[t2];\n\t"
        "%[t0] = [P0];\n\t"
        "%[t0] = %[t0] + %[t4];\n\t"
        "[P0++] = %[t0];\n\t"
        "%[t1] = [P1];\n\t"
        "%[t1] = %[t1] + %[t6];\n\t"
        "2: [P1++] = %[t1];"
        : [t0] "=&d"(t0), [t1] "=&d"(t1), [t2] "=&d"(t2), [t4] "=&d"(t4), [t6] "=&d"(t6)
        : "a"(ap), [n] "i"(COOP_VBLK), [g] "d"((int32_t)gain), [pans] "d"(pans)
        : "I0", "I1", "P0", "P1", "LC0", "LT0", "LB0", "memory");
}

static inline void coop_pair_accum_drum(int32_t *dL, int32_t *dR, const int16_t *out,
                                        int16_t gain, coop_pan_delta d)
{
    uint32_t pans = ((uint32_t)(uint16_t)d.dR << 16) | (uint16_t)d.dL;
    const void *args[3] = { out, dL, dR };
    register const void **ap __asm__("P5") = args;
    int32_t t0, t1, t2, t4, t6;
    __asm__ volatile(
        "%[t0] = [P5 + 0];\n\t"
        "I0 = %[t0];\n\t"
        "P0 = [P5 + 4];\n\t"
        "P1 = [P5 + 8];\n\t"
        "%[t0] = %[n] (X);\n\t"
        "LC0 = %[t0];\n\t"
        "LSETUP (1f, 2f) LC0;\n\t"
        "1: %[t0].L = W[I0++];\n\t"
        "%[t2].L = %[t0].L * %[g].L;\n\t"
        "%[t2].L = %[t2].L << 1 (S);\n\t"
        "%[t4] = %[t2].L * %[pans].L (IS);\n\t"
        "%[t6] = %[t2].L * %[pans].H (IS);\n\t"
        "%[t4] >>>= 15;\n\t"
        "%[t6] >>>= 15;\n\t"
        "%[t2] = %[t2].L (X);\n\t"
        "%[t4] = %[t4] + %[t2];\n\t"
        "%[t6] = %[t6] + %[t2];\n\t"
        "%[t0] = [P0];\n\t"
        "%[t0] = %[t0] + %[t4];\n\t"
        "[P0++] = %[t0];\n\t"
        "%[t1] = [P1];\n\t"
        "%[t1] = %[t1] + %[t6];\n\t"
        "2: [P1++] = %[t1];"
        : [t0] "=&d"(t0), [t1] "=&d"(t1), [t2] "=&d"(t2), [t4] "=&d"(t4), [t6] "=&d"(t6)
        : "a"(ap), [n] "i"(COOP_VBLK), [g] "d"((int32_t)gain), [pans] "d"(pans)
        : "I0", "P0", "P1", "LC0", "LT0", "LB0", "memory");
}
#else
#define coop_side_accum_synth coop_side_accum_synth_c
#define coop_side_accum_drum  coop_side_accum_drum_c
#define coop_pair_accum_synth coop_pair_accum_synth_c
#define coop_pair_accum_drum  coop_pair_accum_drum_c
#endif

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

/* ---- both channels' decimators in one pass (T4e): the two rings interleaved (L in the low half of each word,
 * R in the high half) so one 32-bit load feeds a dual MAC: A0 (left) and A1 (right) each do exactly the W32 MAC
 * sequence of coop_hb_fir, so the result is bit-identical to two coop_hb_decimate calls with the same ramp/step.
 * Verified in tests/sim/kern_check.cc. */
typedef struct { uint32_t even[COOP_HB_TAPS], odd[COOP_HB_TAPS]; int w; } coop_hb2_state;

static inline void coop_hb2_init(coop_hb2_state *st)
{
    for (int i = 0; i < COOP_HB_TAPS; i++) st->even[i] = st->odd[i] = 0;
    st->w = 0;
}

static inline void coop_hb_fir2(int32_t acc0, int32_t acc1, const uint32_t *ring, int start, const int16_t *coefs,
                                int32_t *r0, int32_t *r1)
{
#if defined(__bfin__) && !defined(COOP_HB2_NO_ASM)
    int32_t t0, t2, o0, o1;
    __asm__ volatile(
        "A0 = %[a0];\n\t"
        "A1 = %[a1];\n\t"
        "B0 = %[base];\n\t"
        "L0 = 128 (X);\n\t"
        "I0 = %[start];\n\t"
        "I2 = %[coefs];\n\t"
        "P2 = %[cnt];\n\t"
        "%[t0] = [I0++];\n\t"
        "%[t2].L = W[I2++];\n\t"
        "LSETUP (1f, 1f) LC0 = P2;\n\t"
        "1: A0 += %[t0].L * %[t2].L, A1 += %[t0].H * %[t2].L (W32) || %[t0] = [I0++] || %[t2].L = W[I2++];\n\t"
        "A0 += %[t0].L * %[t2].L, A1 += %[t0].H * %[t2].L (W32);\n\t"
        "L0 = 0 (X);\n\t"
        "%[o0] = A0.W;\n\t"
        "%[o1] = A1.W;"
        : [o0] "=d"(o0), [o1] "=d"(o1), [t0] "=&d"(t0), [t2] "=&d"(t2)
        : [a0] "d"(acc0), [a1] "d"(acc1), [base] "d"(ring), [start] "d"(ring + start), [coefs] "d"(coefs),
          [cnt] "d"(COOP_HB_TAPS - 1)
        : "A0", "A1", "I0", "I2", "B0", "L0", "P2", "LC0", "LT0", "LB0", "memory");
    *r0 = o0; *r1 = o1;
#else
    for (int k = 0; k < COOP_HB_TAPS; k++) {
        uint32_t v = ring[(start + k) & (COOP_HB_TAPS - 1)];
        acc0 = coop_mac_w32(acc0, coefs[k], (int16_t)(v & 0xffff));
        acc1 = coop_mac_w32(acc1, coefs[k], (int16_t)(v >> 16));
    }
    *r0 = acc0; *r1 = acc1;
#endif
}

static inline int16_t coop_hb_decimate2(coop_hb2_state *st, const int32_t *inL, const int32_t *inR,
                                        int16_t *outL, int16_t *outR, const int16_t *coefs, int16_t ramp, int16_t step)
{
    int w = st->w;
    for (int n = 0; n < COOP_MBLK; n++) {
        st->even[w] = (uint16_t)coop_sat16(inL[2 * n]) | ((uint32_t)(uint16_t)coop_sat16(inR[2 * n]) << 16);
        st->odd[w] = (uint16_t)coop_sat16(inL[2 * n + 1]) | ((uint32_t)(uint16_t)coop_sat16(inR[2 * n + 1]) << 16);
        w = (w + 1) & (COOP_HB_TAPS - 1);                   /* now the oldest slot */
        uint32_t o = st->odd[(w + 15) & (COOP_HB_TAPS - 1)];
        int32_t acc0 = (int32_t)(int16_t)(o & 0xffff) << 15;   /* centre tap * 0.5 */
        int32_t acc1 = (int32_t)(int16_t)(o >> 16) << 15;
        int32_t r0, r1;
        coop_hb_fir2(acc0, acc1, st->even, w, coefs, &r0, &r1);
        outL[n] = coop_fr16_mul((int16_t)(r0 >> 16), ramp);
        outR[n] = coop_fr16_mul((int16_t)(r1 >> 16), ramp);
        ramp = coop_fr16_add(step, ramp);
    }
    st->w = w;
    return ramp;                                            /* the ramp the stock decimator writes back (0xffa0067e) */
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
static inline int16_t coop_mix_a_c(volatile int16_t *L, volatile int16_t *R, const int16_t *M,
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

static inline int16_t coop_mix_a_mono_c(volatile int16_t *L, volatile int16_t *R, const int16_t *M,
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

static inline void coop_mix_b_c(volatile int16_t *L, volatile int16_t *R, const int16_t *M,
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
static inline void coop_mix_b_mono_c(volatile int16_t *L, volatile int16_t *R, const int16_t *M,
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

#ifdef __bfin__
/* Blackfin kernels (T4), same conventions as the accumulate above. The mono bus and the difference buses stream
 * through I0-I2, the stereo bus is read-modify-written through P0/P1 (saturating 16-bit adds), level through P2;
 * gL/gR share a register (L/H), as do ramp/step (L = ramp advances, H = step). Loop A keeps the meter as a
 * saturating ABS + MAX (V), reloading the level word every sample as the stock loop does. */
static inline int16_t coop_mix_a(volatile int16_t *L, volatile int16_t *R, const int16_t *M,
                                 const int16_t *dL, const int16_t *dR, int16_t gL, int16_t gR, int16_t step,
                                 volatile int16_t *rec, const volatile int16_t *level)
{
    const void *args[6] = { M, dL, dR, (const void *)L, (const void *)R, (const void *)level };
    register const void **ap __asm__("P5") = args;
    uint32_t rs = ((uint32_t)(uint16_t)step << 16) | (uint16_t)rec[0];
    uint32_t g = ((uint32_t)(uint16_t)gR << 16) | (uint16_t)gL;
    int32_t meter = rec[1];
    int32_t m, a, b, t;
    __asm__ volatile(
        "%[t] = [P5 + 0];\n\t"
        "I0 = %[t];\n\t"
        "%[t] = [P5 + 4];\n\t"
        "I1 = %[t];\n\t"
        "%[t] = [P5 + 8];\n\t"
        "I2 = %[t];\n\t"
        "P0 = [P5 + 12];\n\t"
        "P1 = [P5 + 16];\n\t"
        "P2 = [P5 + 20];\n\t"
        "%[t] = %[n] (X);\n\t"
        "LC0 = %[t];\n\t"
        "LSETUP (1f, 2f) LC0;\n\t"
        "1: %[m].L = W[I0++];\n\t"
        "%[t] = W[P2] (X);\n\t"
        "%[t].L = %[m].L * %[t].L;\n\t"
        "%[t] = ABS %[t] (V);\n\t"
        "%[meter] = MAX (%[meter], %[t]) (V);\n\t"
        "%[a].L = W[I1++];\n\t"
        "%[b].L = W[I2++];\n\t"
        "%[a].L = %[m].L + %[a].L (S);\n\t"
        "%[b].L = %[m].L + %[b].L (S);\n\t"
        "%[a].L = %[a].L * %[g].L;\n\t"
        "%[b].L = %[b].L * %[g].H;\n\t"
        "%[a].L = %[a].L * %[rs].L;\n\t"
        "%[b].L = %[b].L * %[rs].L;\n\t"
        "%[t] = W[P0] (X);\n\t"
        "%[t].L = %[a].L + %[t].L (S);\n\t"
        "W[P0++] = %[t];\n\t"
        "%[t] = W[P1] (X);\n\t"
        "%[t].L = %[b].L + %[t].L (S);\n\t"
        "W[P1++] = %[t];\n\t"
        "2: %[rs].L = %[rs].H + %[rs].L (S);"
        : [rs] "+d"(rs), [meter] "+d"(meter), [m] "=&d"(m), [a] "=&d"(a), [b] "=&d"(b), [t] "=&d"(t)
        : "a"(ap), [n] "i"(COOP_MBLK), [g] "d"(g)
        : "I0", "I1", "I2", "P0", "P1", "P2", "LC0", "LT0", "LB0", "memory");
    rec[0] = (int16_t)rs; rec[1] = (int16_t)meter;
    return (int16_t)rs;
}

static inline int16_t coop_mix_a_mono(volatile int16_t *L, volatile int16_t *R, const int16_t *M,
                                      int16_t gL, int16_t gR, int16_t step, volatile int16_t *rec,
                                      const volatile int16_t *level)
{
    const void *args[4] = { M, (const void *)L, (const void *)R, (const void *)level };
    register const void **ap __asm__("P5") = args;
    uint32_t rs = ((uint32_t)(uint16_t)step << 16) | (uint16_t)rec[0];
    uint32_t g = ((uint32_t)(uint16_t)gR << 16) | (uint16_t)gL;
    int32_t meter = rec[1];
    int32_t m, a, b, t;
    __asm__ volatile(
        "%[t] = [P5 + 0];\n\t"
        "I0 = %[t];\n\t"
        "P0 = [P5 + 4];\n\t"
        "P1 = [P5 + 8];\n\t"
        "P2 = [P5 + 12];\n\t"
        "%[t] = %[n] (X);\n\t"
        "LC0 = %[t];\n\t"
        "LSETUP (1f, 2f) LC0;\n\t"
        "1: %[m].L = W[I0++];\n\t"
        "%[t] = W[P2] (X);\n\t"
        "%[t].L = %[m].L * %[t].L;\n\t"
        "%[t] = ABS %[t] (V);\n\t"
        "%[meter] = MAX (%[meter], %[t]) (V);\n\t"
        "%[a].L = %[m].L * %[g].L;\n\t"
        "%[b].L = %[m].L * %[g].H;\n\t"
        "%[a].L = %[a].L * %[rs].L;\n\t"
        "%[b].L = %[b].L * %[rs].L;\n\t"
        "%[t] = W[P0] (X);\n\t"
        "%[t].L = %[a].L + %[t].L (S);\n\t"
        "W[P0++] = %[t];\n\t"
        "%[t] = W[P1] (X);\n\t"
        "%[t].L = %[b].L + %[t].L (S);\n\t"
        "W[P1++] = %[t];\n\t"
        "2: %[rs].L = %[rs].H + %[rs].L (S);"
        : [rs] "+d"(rs), [meter] "+d"(meter), [m] "=&d"(m), [a] "=&d"(a), [b] "=&d"(b), [t] "=&d"(t)
        : "a"(ap), [n] "i"(COOP_MBLK), [g] "d"(g)
        : "I0", "P0", "P1", "P2", "LC0", "LT0", "LB0", "memory");
    rec[0] = (int16_t)rs; rec[1] = (int16_t)meter;
    return (int16_t)rs;
}

static inline void coop_mix_b(volatile int16_t *L, volatile int16_t *R, const int16_t *M,
                              const int16_t *dL, const int16_t *dR, int16_t gain,
                              int16_t *ramp_state, int16_t step)
{
    const void *args[5] = { M, dL, dR, (const void *)L, (const void *)R };
    register const void **ap __asm__("P5") = args;
    uint32_t rs = ((uint32_t)(uint16_t)step << 16) | (uint16_t)*ramp_state;
    int32_t m, a, b, t;
    __asm__ volatile(
        "%[t] = [P5 + 0];\n\t"
        "I0 = %[t];\n\t"
        "%[t] = [P5 + 4];\n\t"
        "I1 = %[t];\n\t"
        "%[t] = [P5 + 8];\n\t"
        "I2 = %[t];\n\t"
        "P0 = [P5 + 12];\n\t"
        "P1 = [P5 + 16];\n\t"
        "%[t] = %[n] (X);\n\t"
        "LC0 = %[t];\n\t"
        "LSETUP (1f, 2f) LC0;\n\t"
        "1: %[m].L = W[I0++];\n\t"
        "%[a].L = W[I1++];\n\t"
        "%[b].L = W[I2++];\n\t"
        "%[a].L = %[m].L + %[a].L (S);\n\t"
        "%[b].L = %[m].L + %[b].L (S);\n\t"
        "%[a].L = %[a].L * %[g].L;\n\t"
        "%[b].L = %[b].L * %[g].L;\n\t"
        "%[a].L = %[a].L * %[rs].L;\n\t"
        "%[b].L = %[b].L * %[rs].L;\n\t"
        "%[t] = W[P0] (X);\n\t"
        "%[t].L = %[a].L + %[t].L (S);\n\t"
        "W[P0++] = %[t];\n\t"
        "%[t] = W[P1] (X);\n\t"
        "%[t].L = %[b].L + %[t].L (S);\n\t"
        "W[P1++] = %[t];\n\t"
        "2: %[rs].L = %[rs].H + %[rs].L (S);"
        : [rs] "+d"(rs), [m] "=&d"(m), [a] "=&d"(a), [b] "=&d"(b), [t] "=&d"(t)
        : "a"(ap), [n] "i"(COOP_MBLK), [g] "d"((int32_t)gain)
        : "I0", "I1", "I2", "P0", "P1", "LC0", "LT0", "LB0", "memory");
    *ramp_state = (int16_t)rs;
}

static inline void coop_mix_b_mono(volatile int16_t *L, volatile int16_t *R, const int16_t *M,
                                   int16_t gain, int16_t *ramp_state, int16_t step)
{
    const void *args[3] = { M, (const void *)L, (const void *)R };
    register const void **ap __asm__("P5") = args;
    uint32_t rs = ((uint32_t)(uint16_t)step << 16) | (uint16_t)*ramp_state;
    int32_t m, a, t;
    __asm__ volatile(
        "%[t] = [P5 + 0];\n\t"
        "I0 = %[t];\n\t"
        "P0 = [P5 + 4];\n\t"
        "P1 = [P5 + 8];\n\t"
        "%[t] = %[n] (X);\n\t"
        "LC0 = %[t];\n\t"
        "LSETUP (1f, 2f) LC0;\n\t"
        "1: %[m].L = W[I0++];\n\t"
        "%[a].L = %[m].L * %[g].L;\n\t"
        "%[a].L = %[a].L * %[rs].L;\n\t"
        "%[t] = W[P0] (X);\n\t"
        "%[t].L = %[a].L + %[t].L (S);\n\t"
        "W[P0++] = %[t];\n\t"
        "%[t] = W[P1] (X);\n\t"
        "%[t].L = %[a].L + %[t].L (S);\n\t"
        "W[P1++] = %[t];\n\t"
        "2: %[rs].L = %[rs].H + %[rs].L (S);"
        : [rs] "+d"(rs), [m] "=&d"(m), [a] "=&d"(a), [t] "=&d"(t)
        : "a"(ap), [n] "i"(COOP_MBLK), [g] "d"((int32_t)gain)
        : "I0", "P0", "P1", "LC0", "LT0", "LB0", "memory");
    *ramp_state = (int16_t)rs;
}
#else
#define coop_mix_a      coop_mix_a_c
#define coop_mix_a_mono coop_mix_a_mono_c
#define coop_mix_b      coop_mix_b_c
#define coop_mix_b_mono coop_mix_b_mono_c
#endif

#endif
