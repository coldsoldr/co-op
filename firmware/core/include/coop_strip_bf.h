/* coop-fw per-tape channel strip, the Blackfin implementation (CS2, docs/channel-strip.md "CS2").
 *
 * The same strip as coop_strip.h (the golden int64 reference: same saved form coop_strip_params, same sections,
 * same rules), in the arithmetic the Blackfin does in one cycle: int16 data between the sections, 16 x 16 MACs on
 * the 40-bit accumulators, truncating products only (no dependence on ASTAT RND_MOD). This header is the C model
 * of exactly that arithmetic: the kernels below (strip_bfc_*) are bit-for-bit what core/sdram/strip_fast.S
 * (strip_bfa_*) computes (tests/sim/run_strip_check.sh compares them on the bfin simulator), and the driver
 * coop_strip_bf_process is shared C (it runs per block, the kernels per sample). tests/host/strip_test.cc runs
 * every property test on the reference and on this model.
 *
 * I/O: int16. A track's block is L[64] / R[64] int16 (the tape lanes are int16), processed in place like the
 * reference; the buses are int16 L / R arrays as the master pair and the master effects use them (d34 / db4),
 * added into with saturation. Every array 4-byte aligned. Inside, a block is 64 stereo frames packed one per
 * 32-bit word (L in the low half, R in the high half) so that one dual-MAC or one vector op does both channels.
 *
 * Sections (what differs from the reference, all inside its host-test tolerances):
 *  - EQ: direct form I with 16-bit inputs and a 31-bit feedback state (Y int16 + E 15 bits: the int16 output and
 *    its rounding remainder), Q29 coefficients split into a 16-bit high and a 15-bit low part, every product exact
 *    over three accumulation scales, and first-order error feedback of the one truncation left (strip_bfc_eq_band
 *    has the arithmetic). A 16-bit feedback state with first-order error feedback (the suggested design) limit-
 *    cycles at 20 Hz: any error the state cannot hold is amplified by 1 / (1 + a1 + a2), about 1.2e5 there.
 *    The b's are scaled by 2^-k (k <= 3) when a boost needs |b| >= 2; the output is then Y 2^k rounded.
 *  - Comp: the reference's detector and gain computer in 32-bit (coop_mul_fr) on the int16 peak; the gain as a
 *    Q15 mantissa and a left shift (0..5) per 16-sample sub-block, ramped linearly within it, ending on the target.
 *    The whole compressor is one kernel (strip_bfc_comp).
 *  - Sat: drive Q11, the driven value clamped to +-4 by a saturating shift (Q29), a 512-step table lerp with a
 *    15-bit fraction, trim Q15; tube's DC blocker in 32 bits; tone and mix on packed pairs in 16 bits.
 *  - Levels: Q15 gains (dry / sends x fader); exact unity (100 % x fader 1 << 15) is a plain saturating add, so a
 *    neutral strip is bit-exact; ramps g_i = g1 - (63 - i) step with step = (g1 - g0) / 64, the last sample at g1.
 *
 * Fixed point only, no libc, no float, no static constructors.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef COOP_STRIP_BF_H
#define COOP_STRIP_BF_H
#include <stdint.h>
#include "coop_strip.h"
#include "coop_fixed.h"

#define STRIP_BF_LPC 6291            /* sat tone one-pole, Q15 (the reference's 412316860 Q31) */

/* ---- packed stereo words ---- */
static inline int16_t strip_bf_lo(uint32_t w) { return (int16_t)(uint16_t)(w & 0xffffu); }
static inline int16_t strip_bf_hi(uint32_t w) { return (int16_t)(uint16_t)(w >> 16); }
static inline uint32_t strip_bf_pk(int32_t l, int32_t r) { return ((uint32_t)(uint16_t)r << 16) | (uint16_t)l; }
static inline int32_t strip_bf_abs16(int32_t v) { return v < 0 ? (v == -32768 ? 32767 : -v) : v; }
static inline int32_t strip_bf_lo_i(uint32_t w) { return strip_bf_lo(w); }
static inline int32_t strip_bf_hi_i(uint32_t w) { return strip_bf_hi(w); }

/* sat kernel constants (one struct, read by the assembly at these offsets) */
typedef struct {
    const int16_t *tab;      /*  0: the shaping table's centre (&table[256]) */
    int32_t drive;           /*  4: Q11 */
    int32_t dcomp;           /*  8: Q15 */
    int32_t tone;            /* 12: Q15 signed, both halves (packed) */
    int32_t mix;             /* 16: packed: low = m, high = 32768 - m (Q15) */
    int32_t lp;              /* 20: tone low-pass state, packed */
    int32_t dc[2];           /* 24: tube DC state, Q31 per channel */
} strip_bf_satk;

/* ---- kernels: the C model (the assembly does exactly this) ---- */

/* pack L / R into frames; returns the peak |x| (int16, |-32768| = 32767) of both channels */
static inline int32_t strip_bfc_pack(const int16_t *L, const int16_t *R, uint32_t *w)
{
    int32_t pk = 0;
    for (int i = 0; i < STRIP_BLOCK; i++) {
        w[i] = strip_bf_pk(L[i], R[i]);
        int32_t a = strip_bf_abs16(L[i]), b = strip_bf_abs16(R[i]);
        if (a > pk) pk = a;
        if (b > pk) pk = b;
    }
    return pk;
}

static inline int32_t strip_bfc_unpack(const uint32_t *w, int16_t *L, int16_t *R)
{
    int32_t pk = 0;
    for (int i = 0; i < STRIP_BLOCK; i++) {
        L[i] = strip_bf_lo(w[i]); R[i] = strip_bf_hi(w[i]);
        int32_t a = strip_bf_abs16(L[i]), b = strip_bf_abs16(R[i]);
        if (a > pk) pk = a;
        if (b > pk) pk = b;
    }
    return pk;
}

static inline int32_t strip_bfc_peak(const int16_t *L, const int16_t *R)
{
    int32_t pk = 0;
    for (int i = 0; i < STRIP_BLOCK; i++) {
        int32_t a = strip_bf_abs16(L[i]), b = strip_bf_abs16(R[i]);
        if (a > pk) pk = a;
        if (b > pk) pk = b;
    }
    return pk;
}

/* One EQ band over a block: DF1 with a 31-bit feedback state y = Y + E / 2^15 (Y int16 = round(y), E signed,
 * |E| <= 2^14, kept as 2E), the coefficients Q29 split c = H 2^15 + L (H signed 16-bit, L 0 .. 32767), the b's
 * scaled by 2^-k (|b / 2^k| < 2). Per channel and sample, every product exact (2 x = the MAC's fractional mode):
 *   A  = L1 2E1 + L2 2E2;  A >>= 15
 *   A += 2 (L1 Y1 + L2 Y2 + Lb0 x0 + Lb1 x1 + Lb2 x2) + H1 2E1 + H2 2E2 + r + 2^29        (2^29: 1/2 LSB)
 *   r  = A & 0x7fff;  A >>= 15                                                            (first-order error feedback)
 *   A += 2 (H1 Y1 + H2 Y2 + Hb0 x0 + Hb1 x1 + Hb2 x2);   v = sat32(A)                    (v = y 2^15 + 2^14)
 *   w = sat32(v << 1): Y = w >> 16, 2E = (int16)((w & 0xffff) ^ 0x8000)
 * The state's only truncation is fed back, so a constant error cannot hold the filter off zero (1 + a1 + a2 is
 * 8e-6 at 20 Hz: a 16-bit state, or any unfed truncation at its level, leaves a DC residue of up to 100+ LSB).
 * The output is Y (k = 0) or round(y 2^k) = sat16(sat16(Y << k) + ((E + 2^(14 - k)) >> (15 - k))).
 * in[0..1] x[-2], x[-1], in[2..65] the block; out[0..1] Y[-2], Y[-1] then out[2..65] Y (the next band's in; for
 * k > 0 rewritten with the scaled output after the state is saved); e = 2E[66], [0..1] history.
 * st = { x[-2], x[-1], Y[-2], Y[-1], 2E[-2], 2E[-1], r } (packed), coef = { -a1, -a2, b2, b1, b0 (H << 16 | L), k }. */
static inline void strip_bfc_eq_band(uint32_t *in, uint32_t *out, const uint32_t *coef, uint32_t *st, uint32_t *e)
{
    in[0] = st[0]; in[1] = st[1]; out[0] = st[2]; out[1] = st[3]; e[0] = st[4]; e[1] = st[5];
    int32_t H[5], Lo[5];
    for (int k = 0; k < 5; k++) { H[k] = strip_bf_hi(coef[k]); Lo[k] = (int32_t)(coef[k] & 0xffffu); }
    const int k = (int)coef[5];
    uint32_t r = st[6];
    for (int n = 0; n < STRIP_BLOCK; n++) {
        uint32_t yn = 0, en = 0, rn = 0;
        for (int c = 0; c < 2; c++) {
            int32_t (*h)(uint32_t) = c ? strip_bf_hi_i : strip_bf_lo_i;
            int32_t Y1 = h(out[n + 1]), Y2 = h(out[n]), E1 = h(e[n + 1]), E2 = h(e[n]);
            int32_t x0 = h(in[n + 2]), x1 = h(in[n + 1]), x2 = h(in[n]);
            int64_t A = (int64_t)Lo[0] * E1 + (int64_t)Lo[1] * E2;
            A >>= 15;
            A += 2 * ((int64_t)Lo[0] * Y1 + (int64_t)Lo[1] * Y2 + (int64_t)Lo[2] * x2 + (int64_t)Lo[3] * x1
                      + (int64_t)Lo[4] * x0) + (int64_t)H[0] * E1 + (int64_t)H[1] * E2 + h(r) + (1LL << 29);
            rn |= (uint32_t)(A & 0x7fff) << (16 * c);
            A >>= 15;
            A += 2 * ((int64_t)H[0] * Y1 + (int64_t)H[1] * Y2 + (int64_t)H[2] * x2 + (int64_t)H[3] * x1 + (int64_t)H[4] * x0);
            uint32_t w = (uint32_t)strip_sat32((int64_t)strip_sat32(A) * 2);
            yn |= (w >> 16) << (16 * c);
            en |= ((w & 0xffffu) ^ 0x8000u) << (16 * c);
        }
        out[n + 2] = yn; e[n + 2] = en;
        r = rn;
    }
    st[0] = in[64]; st[1] = in[65]; st[2] = out[64]; st[3] = out[65]; st[4] = e[64]; st[5] = e[65]; st[6] = r;
    if (k)
        for (int n = 2; n < STRIP_BLOCK + 2; n++) {
            int32_t o[2];
            for (int c = 0; c < 2; c++) {
                int32_t Y = c ? strip_bf_hi(out[n]) : strip_bf_lo(out[n]);
                int32_t E = (c ? strip_bf_hi(e[n]) : strip_bf_lo(e[n])) >> 1;
                o[c] = coop_sat16(coop_sat16(Y * (1 << k)) + ((E + (1 << (14 - k))) >> (15 - k)));
            }
            out[n] = strip_bf_pk(o[0], o[1]);
        }
}

/* the EQ's bands as one call: n > 0 jobs { in, out, coef, st }, e the shared 2E history */
typedef struct { uint32_t *in, *out; const uint32_t *coef; uint32_t *st; } strip_bf_eqjob;    /* 16 bytes */

static inline void strip_bfc_eq(const strip_bf_eqjob *j, int32_t n, uint32_t *e)
{
    for (int i = 0; i < n; i++) strip_bfc_eq_band(j[i].in, j[i].out, j[i].coef, j[i].st, e);
}

/* peak |x| of 16 frames (both channels) */
static inline int32_t strip_bfc_peak16(const uint32_t *w)
{
    int32_t pk = 0;
    for (int i = 0; i < STRIP_COMP_SUB; i++) {
        int32_t a = strip_bf_abs16(strip_bf_lo(w[i])), b = strip_bf_abs16(strip_bf_hi(w[i]));
        if (a > pk) pk = a;
        if (b > pk) pk = b;
    }
    return pk;
}

/* 16 frames x the gain m_i = g + i step (Q15, 0 .. 32767), then << sh with saturation */
static inline void strip_bfc_comp_apply(uint32_t *w, int32_t g, int32_t step, int32_t sh)
{
    for (int i = 0; i < STRIP_COMP_SUB; i++, g += step) {
        int32_t l = strip_bf_lo(w[i]), r = strip_bf_hi(w[i]);
        l = coop_sat16(((l * g) >> 15) * (1 << sh));
        r = coop_sat16(((r * g) >> 15) * (1 << sh));
        w[i] = strip_bf_pk(l, r);
    }
}

static inline int32_t strip_bf_shape1(const strip_bf_satk *k, int32_t x)
{
    int32_t p = x * k->drive;                                   /* Q26 */
    int32_t q = strip_sat32((int64_t)p << 3);                   /* Q29, +-4 */
    int32_t idx = q >> 23, fr = (q >> 8) & 0x7fff;
    int32_t t0 = k->tab[idx], t1 = k->tab[idx + 1];
    int32_t y = coop_sat16(t0 + ((coop_sat16(t1 - t0) * fr) >> 15));
    return (y * k->dcomp) >> 15;
}

static inline int32_t strip_bf_fold1(const strip_bf_satk *k, int32_t x)
{
    /* the triangle of period 4 (Q26 2^28): s = (p << 4) + 2^30 wraps it into 32 bits; |s| - 2^30 = 16 y */
    int32_t p = x * k->drive;                                   /* Q26 */
    int32_t s = (int32_t)(((uint32_t)p << 4) + (1u << 30));
    int32_t m = (s == (int32_t)0x80000000 ? 2147483647 : (s < 0 ? -s : s)) - (1 << 30);   /* ABS saturates */
    int32_t y = m >> 15;
    return (y * k->dcomp) >> 15;
}

static inline void strip_bfc_shape(const uint32_t *x, uint32_t *y, const strip_bf_satk *k)
{
    for (int i = 0; i < STRIP_BLOCK; i++)
        y[i] = strip_bf_pk(strip_bf_shape1(k, strip_bf_lo(x[i])), strip_bf_shape1(k, strip_bf_hi(x[i])));
}

static inline void strip_bfc_fold(const uint32_t *x, uint32_t *y, const strip_bf_satk *k)
{
    for (int i = 0; i < STRIP_BLOCK; i++)
        y[i] = strip_bf_pk(strip_bf_fold1(k, strip_bf_lo(x[i])), strip_bf_fold1(k, strip_bf_hi(x[i])));
}

/* tube DC blocker: z = sat32((y << 16) - dc), dc += z >> 10, y = z >> 16 */
static inline void strip_bfc_tube(uint32_t *y, strip_bf_satk *k)
{
    for (int i = 0; i < STRIP_BLOCK; i++) {
        int32_t o[2];
        for (int c = 0; c < 2; c++) {
            int32_t v = c ? strip_bf_hi(y[i]) : strip_bf_lo(y[i]);
            int32_t z = strip_sat32((int64_t)v * 65536 - k->dc[c]);
            k->dc[c] = (int32_t)((uint32_t)k->dc[c] + (uint32_t)(z >> 10));
            o[c] = z >> 16;
        }
        y[i] = strip_bf_pk(o[0], o[1]);
    }
}

/* tone: lp += (y - lp) c; y += (y - lp) k, all 16-bit saturating, truncating products */
static inline void strip_bfc_tone(uint32_t *y, strip_bf_satk *k)
{
    uint32_t lpw = (uint32_t)k->lp;
    int32_t kk = strip_bf_lo((uint32_t)k->tone);
    for (int i = 0; i < STRIP_BLOCK; i++) {
        int32_t o[2], lp[2] = { strip_bf_lo(lpw), strip_bf_hi(lpw) };
        for (int c = 0; c < 2; c++) {
            int32_t v = c ? strip_bf_hi(y[i]) : strip_bf_lo(y[i]);
            int32_t d = coop_sat16(v - lp[c]);
            lp[c] = coop_sat16(lp[c] + ((d * STRIP_BF_LPC) >> 15));
            int32_t d2 = coop_sat16(v - lp[c]);
            o[c] = coop_sat16(v + ((d2 * kk) >> 15));
        }
        lpw = strip_bf_pk(lp[0], lp[1]);
        y[i] = strip_bf_pk(o[0], o[1]);
    }
    k->lp = (int32_t)lpw;
}

/* mix: x = (x (32768 - m) + y m) >> 15 */
static inline void strip_bfc_mix(uint32_t *x, const uint32_t *y, const strip_bf_satk *k)
{
    int32_t m = strip_bf_lo((uint32_t)k->mix), im = strip_bf_hi((uint32_t)k->mix);
    for (int i = 0; i < STRIP_BLOCK; i++) {
        int32_t l = (strip_bf_lo(x[i]) * im + strip_bf_lo(y[i]) * m) >> 15;
        int32_t r = (strip_bf_hi(x[i]) * im + strip_bf_hi(y[i]) * m) >> 15;
        x[i] = strip_bf_pk(l, r);
    }
}


/* bus[i] = sat16(bus[i] + (x[i] g_i >> 15)), g_i = g + i step (|g_i| <= 32767) */
static inline void strip_bfc_bus(int16_t *bus, const int16_t *x, int32_t g, int32_t step)
{
    for (int i = 0; i < STRIP_BLOCK; i++, g += step) bus[i] = coop_sat16(bus[i] + ((x[i] * g) >> 15));
}

static inline void strip_bfc_bus_unity(int16_t *bus, const int16_t *x)
{
    for (int i = 0; i < STRIP_BLOCK; i++) bus[i] = coop_sat16(bus[i] + x[i]);
}

/* A list of bus additions, each x x the ramp from last block's gain g0 to this block's g1 (Q15, -32767 .. 32768):
 * nothing when both are 0 (a send or the dry path at 0 %), a plain saturating add when both are 32768 (unity),
 * else g_i = g1 - (63 - i) step, step = (g1' - g0') / 64 (toward zero) with g' = min(g, 32767): the last sample on
 * the new gain. */
typedef struct { int16_t *bus; const int16_t *x; int32_t g0, g1; } strip_bf_job;    /* 16 bytes */

static inline void strip_bfc_bus_n(const strip_bf_job *j, int32_t n)
{
    for (int i = 0; i < n; i++) {
        int32_t g0 = j[i].g0, g1 = j[i].g1;
        if (!g0 && !g1) continue;
        if (g0 == 32768 && g1 == 32768) { strip_bfc_bus_unity(j[i].bus, j[i].x); continue; }
        if (g0 > 32767) g0 = 32767;
        if (g1 > 32767) g1 = 32767;
        int32_t step = (g1 - g0) / STRIP_BLOCK;
        strip_bfc_bus(j[i].bus, j[i].x, g1 - (STRIP_BLOCK - 1) * step, step);
    }
}

/* ---- the compressor: constants and state, one struct (read by the assembly at these offsets) ---- */
typedef struct {
    int32_t thr;             /*  0: threshold, log2 Q16 (<= 0) */
    int32_t slope5;          /*  4: (1 - 1/ratio) Q16 << 5 */
    int32_t knee;            /*  8: log2 Q16 */
    int32_t inv2k;           /* 12: 2^34 / 2 knee */
    int32_t makeup;          /* 16: log2 Q16 */
    int32_t att, rel;        /* 20, 24: one-pole coefficients per sub-block, Q31 */
    int32_t gr;              /* 28: gain reduction, log2 Q16 (>= 0) */
    int32_t cg;              /* 32: the last sub-block's gain, Q26 */
    const int32_t *log2t;    /* 36: strip_log2_tab */
    const int32_t *exp2t;    /* 40: strip_exp2_tab */
} strip_bf_comp;

/* 2^g (g log2 Q16) as a Q26 gain: the reference's strip_exp2_q26 with its shift clamped to 0 .. 31 (the same for
 * every g the compressor reaches: makeup <= +24 dB, gr >= 0, so g < 4.0; above it this gives the Q30 lerp, not
 * saturation) */
static inline int32_t strip_bf_exp2(int32_t g)
{
    int32_t n = g >> 16;
    uint32_t f = (uint32_t)g & 0xffffu, idx = f >> 8, fr = f & 255u;
    int32_t m = strip_exp2_tab[idx] + (((strip_exp2_tab[idx + 1] - strip_exp2_tab[idx]) * (int32_t)fr) >> 8);
    int32_t sh = 4 - n;
    sh = sh < 0 ? 0 : (sh > 31 ? 31 : sh);
    return m >> sh;
}

/* the reference's static curve in 32 bits: over x slope >> 16 as coop_mul_fr(over << 10, slope << 5); the knee's
 * d^2 / 2k as coop_mul_fr(d^2 >> 3, 2^34 / 2k) */
static inline int32_t strip_bf_comp_target(const strip_bf_comp *c, int32_t lvl)
{
    int32_t over = lvl - c->thr, k = c->knee;
    if (k <= 0 || 2 * over >= k) return over > 0 ? coop_mul_fr(over << 10, c->slope5) : 0;
    if (2 * over <= -k) return 0;
    int32_t d = over + (k >> 1);
    int32_t q = coop_mul_fr(coop_mul_fr(d << 14, d << 14), c->inv2k);
    return coop_mul_fr(q << 10, c->slope5);
}

/* The compressor over a block, four sub-blocks of 16 frames: the stereo-linked peak, its log2 (strip_log2 of the
 * int16 peak << 16), the target reduction, the one-pole step, the gain exp2 (Q26); the gain as a Q15 mantissa
 * m and a left shift sh (0 .. 5: m <= 32767 for both ends), ramped m_i = m1 - (15 - i) step, step = (m1 - m0) / 16
 * (toward zero), ending on the target; y = sat16(((x m_i) >> 15) << sh). */
static inline void strip_bfc_comp(uint32_t *w, strip_bf_comp *c)
{
    for (int o = 0; o < STRIP_BLOCK; o += STRIP_COMP_SUB) {
        int32_t pk = strip_bfc_peak16(w + o);
        int32_t target = strip_bf_comp_target(c, strip_log2(pk << 16));
        int32_t a = target > c->gr ? c->att : c->rel;
        c->gr += coop_mul_fr(target - c->gr, a);
        int32_t g1 = strip_bf_exp2(c->makeup - c->gr), g0 = c->cg;
        int32_t m1 = g1 >> 11, m0 = g0 >> 11, mx = m1 > m0 ? m1 : m0, sh = 0;
        while ((mx >> sh) > 32767) sh++;
        m1 >>= sh; m0 >>= sh;
        int32_t step = (m1 - m0) / STRIP_COMP_SUB;
        strip_bfc_comp_apply(w + o, m1 - (STRIP_COMP_SUB - 1) * step, step, sh);
        c->cg = g1;
    }
}

/* ---- the kernel set (C model, or the assembly on the Blackfin) ---- */
typedef struct {
    int32_t (*pack)(const int16_t *L, const int16_t *R, uint32_t *w);
    int32_t (*unpack)(const uint32_t *w, int16_t *L, int16_t *R);
    int32_t (*peak)(const int16_t *L, const int16_t *R);
    void (*eq)(const strip_bf_eqjob *j, int32_t n, uint32_t *e);
    void (*comp)(uint32_t *w, strip_bf_comp *c);
    void (*shape)(const uint32_t *x, uint32_t *y, const strip_bf_satk *k);
    void (*fold)(const uint32_t *x, uint32_t *y, const strip_bf_satk *k);
    void (*tube)(uint32_t *y, strip_bf_satk *k);
    void (*tone)(uint32_t *y, strip_bf_satk *k);
    void (*mix)(uint32_t *x, const uint32_t *y, const strip_bf_satk *k);
    void (*bus_n)(const strip_bf_job *j, int32_t n);
} strip_bf_kernels;

static const strip_bf_kernels strip_bf_c_kernels = {
    strip_bfc_pack, strip_bfc_unpack, strip_bfc_peak, strip_bfc_eq, strip_bfc_comp,
    strip_bfc_shape, strip_bfc_fold, strip_bfc_tube, strip_bfc_tone, strip_bfc_mix, strip_bfc_bus_n,
};

#if defined(__bfin__) && !defined(COOP_STRIP_BF_C_ONLY)
/* core/sdram/strip_fast.S */
extern "C" {
int32_t strip_bfa_pack(const int16_t *L, const int16_t *R, uint32_t *w);
int32_t strip_bfa_unpack(const uint32_t *w, int16_t *L, int16_t *R);
int32_t strip_bfa_peak(const int16_t *L, const int16_t *R);
void strip_bfa_eq(const strip_bf_eqjob *j, int32_t n, uint32_t *e);
void strip_bfa_comp(uint32_t *w, strip_bf_comp *c);
void strip_bfa_shape(const uint32_t *x, uint32_t *y, const strip_bf_satk *k);
void strip_bfa_fold(const uint32_t *x, uint32_t *y, const strip_bf_satk *k);
void strip_bfa_tube(uint32_t *y, strip_bf_satk *k);
void strip_bfa_tone(uint32_t *y, strip_bf_satk *k);
void strip_bfa_mix(uint32_t *x, const uint32_t *y, const strip_bf_satk *k);
void strip_bfa_bus_n(const strip_bf_job *j, int32_t n);
}
static const strip_bf_kernels strip_bf_asm_kernels = {
    strip_bfa_pack, strip_bfa_unpack, strip_bfa_peak, strip_bfa_eq, strip_bfa_comp,
    strip_bfa_shape, strip_bfa_fold, strip_bfa_tube, strip_bfa_tone, strip_bfa_mix, strip_bfa_bus_n,
};
#define STRIP_BF_DEFAULT_KERNELS (&strip_bf_asm_kernels)
#else
#define STRIP_BF_DEFAULT_KERNELS (&strip_bf_c_kernels)
#endif

enum {
    STRIP_BF_M_ANY = 1, STRIP_BF_M_EQ = 2, STRIP_BF_M_COMP = 4, STRIP_BF_M_SAT = 8, STRIP_BF_M_PRE = 16,
    STRIP_BF_M_FOLD = 32, STRIP_BF_M_TUBE = 64, STRIP_BF_M_TONE = 128, STRIP_BF_M_MIX = 256,
};

/* ---- state ---- */
typedef struct {
    coop_strip_params p;
    /* derived */
    uint8_t band_on[4], nbands, band_idx[4];
    uint32_t eqc[4][18];                    /* per band: -a1, -a2, b2, b1, b0 (Q29 H << 16 | L), k; the assembly's ring */
    strip_bf_comp comp;                     /* comp constants and state */
    strip_bf_satk sat;                      /* sat constants and state */
    int32_t lvl[3];                         /* dry / fx1 / fx2, Q15 (100 % = 32768) */
    /* state */
    uint32_t eqs[4][7];                     /* per band: x[-2], x[-1], Y[-2], Y[-1], 2E[-2], 2E[-1], r */
    strip_bf_job jobs[6];                   /* the bus additions (s1, s2, dry; L, R), g0 = last block's gain */
    uint8_t dirty, eq_was, comp_was, sat_was, primed;
    uint16_t mode;                          /* STRIP_BF_M_*: what process runs, decided by update */
    strip_bf_eqjob ej[4];                   /* the running bands' EQ jobs (ping-pong between wa and wb) */
    uint32_t *cur, *y;                      /* the block after the EQ; the saturator's output (cur or the other) */
    int32_t peak_in, peak_out;              /* meters for the UI: int16 peaks (0 .. 32767) */
    /* work: two blocks of 2 history + 64 frames, the EQ's 2E history */
    uint32_t wa[STRIP_BLOCK + 2], wb[STRIP_BLOCK + 2];
    uint32_t ye[STRIP_BLOCK + 2];
} coop_strip_bf;

typedef struct {
    int16_t *dryL, *dryR, *s1L, *s1R, *s2L, *s2R;   /* int16, STRIP_BLOCK each, 4-byte aligned */
} coop_strip_bf_bus;

/* The state points into itself (the EQ jobs, the work blocks): a copied coop_strip_bf needs coop_strip_bf_set before
 * its next block. */
static inline void coop_strip_bf_set(coop_strip_bf *s, const coop_strip_params *p) { s->p = *p; s->dirty = 1; }

static inline void coop_strip_bf_init(coop_strip_bf *s)
{
    uint8_t *z = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++) z[i] = 0;
    coop_strip_params_default(&s->p);
    s->comp.cg = 1 << 26;
    s->dirty = 1;
}

static inline uint32_t strip_bf_coef(int64_t c29)    /* Q29, |c| < 2 */
{
    int32_t c = (int32_t)c29;
    return ((uint32_t)(uint16_t)(c >> 15) << 16) | ((uint32_t)c & 0x7fffu);
}

/* coef[6..14]: the assembly's load ring (C2, K = 16384 : 1, C3, C4, mask, C2, C3, C4, xor); [15..17]: k > 0 */
static inline void strip_bf_ring(uint32_t *c, int k)
{
    c[6] = c[2]; c[7] = 0x40000001u; c[8] = c[3]; c[9] = c[4]; c[10] = 0x7fff7fffu;
    c[11] = c[2]; c[12] = c[3]; c[13] = c[4]; c[14] = 0x80008000u;
    c[15] = strip_bf_pk(1 << (14 - k), 1 << (14 - k));
    c[16] = (uint32_t)(k - 15);
    c[17] = (uint32_t)k;
}

static inline int32_t strip_bf_level(int pct)
{
    if (pct <= 0) return 0;
    if (pct >= 100) return 32768;
    return pct * pct * 32768 / 10000;
}

/* One EQ band's coefficients (the expensive part of an update: the reference's five int64 divides) into c[18];
 * returns whether the band runs. Depends only on the band's own fields of p (eq_cut, eq_freq, eq_gain, eq_q). */
static inline int coop_strip_bf_band(const coop_strip_params *p, int b, uint32_t *c)
{
    coop_strip_bq q;
    int on = strip_band_coefs(p, b, &q);
    if (on) {
        /* the b's scaled by 2^-k so that |b| < 2 at Q29 (a boost up to +18 dB has b up to 16: k <= 3) */
        int64_t mb = 0, bb[3] = { q.b2, q.b1, q.b0 };
        for (int i = 0; i < 3; i++) { int64_t v = bb[i] < 0 ? -bb[i] : bb[i]; if (v > mb) mb = v; }
        int k = 0;
        while (((mb * 4) >> k) > (1LL << 30) - (1LL << 16)) k++;
        c[0] = strip_bf_coef(-(int64_t)q.a1 * 4); c[1] = strip_bf_coef(-(int64_t)q.a2 * 4);
        for (int i = 0; i < 3; i++) c[2 + i] = strip_bf_coef((bb[i] * 4) >> k);
        c[5] = (uint32_t)k;
        strip_bf_ring(c, k);
    }
    return on;
}

/* The bands' fields that coop_strip_bf_band reads are the same in a and b */
static inline int coop_strip_bf_band_same(const coop_strip_params *a, const coop_strip_params *b, int k)
{
    return a->eq_cut[k] == b->eq_cut[k] && a->eq_freq[k] == b->eq_freq[k] && a->eq_gain[k] == b->eq_gain[k] &&
           a->eq_q[k] == b->eq_q[k];
}

/* The rest of an update, cheap: s->p is the new form and s->eqc[b] holds the coefficients of every band with
 * on[b] (coop_strip_bf_band). CS3 (core/sdram/coop_strip.cc) computes the bands one a block and commits with this;
 * coop_strip_bf_update does both at once. */
static inline void coop_strip_bf_commit(coop_strip_bf *s, const uint8_t *on)
{
    const coop_strip_params *p = &s->p;
    s->nbands = 0;
    for (int b = 0; b < 4; b++) {
        if (on[b]) {
            if (!s->band_on[b]) for (int i = 0; i < 7; i++) s->eqs[b][i] = 0;
            s->band_idx[s->nbands++] = (uint8_t)b;
        }
        s->band_on[b] = on[b];
    }
    strip_bf_comp *c = &s->comp;
    c->thr = -strip_hdb_l2(p->c_thresh);
    c->slope5 = strip_ratio_slope[p->c_ratio < STRIP_COMP_NRATIO ? p->c_ratio : STRIP_COMP_NRATIO - 1] << 5;
    c->knee = strip_hdb_l2(p->c_knee);
    c->inv2k = c->knee > 0 ? (int32_t)((1LL << 34) / (2LL * c->knee)) : 0;
    c->makeup = strip_hdb_l2(p->c_makeup);
    c->att = strip_att_coef[p->c_attack < STRIP_COMP_NTIME ? p->c_attack : STRIP_COMP_NTIME - 1];
    c->rel = strip_rel_coef[p->c_release < STRIP_COMP_NTIME ? p->c_release : STRIP_COMP_NTIME - 1];
    c->log2t = strip_log2_tab;
    c->exp2t = strip_exp2_tab;
    int d = p->s_drive > 100 ? 100 : p->s_drive;
    s->sat.tab = (p->s_type == STRIP_SAT_TUBE ? strip_sat_tube : p->s_type == STRIP_SAT_DIODE ? strip_sat_diode
                                                                                            : strip_sat_tape) + 256;
    s->sat.drive = strip_drive_gain[d] >> 13;
    s->sat.dcomp = strip_drive_comp[d];
    int t = p->s_tone < -50 ? -50 : (p->s_tone > 50 ? 50 : p->s_tone);
    int32_t tk = t * 32767 / 50;
    s->sat.tone = (int32_t)strip_bf_pk(tk, tk);
    int mx = p->s_mix > 100 ? 100 : p->s_mix;
    int32_t m = mx * 32768 / 100;
    s->sat.mix = (int32_t)strip_bf_pk(m, 32768 - m);
    s->lvl[0] = strip_bf_level(p->dry);
    s->lvl[1] = strip_bf_level(p->fx1);
    s->lvl[2] = strip_bf_level(p->fx2);
    /* what process runs, and where: the EQ's jobs alternate between the two work blocks */
    uint32_t *cur = s->wa + 2, *oth = s->wb + 2;
    for (int k = 0; k < s->nbands; k++) {
        int b = s->band_idx[k];
        s->ej[k].in = cur - 2; s->ej[k].out = oth - 2; s->ej[k].coef = s->eqc[b]; s->ej[k].st = s->eqs[b];
        uint32_t *t = cur; cur = oth; oth = t;
    }
    int eq = p->eq_on && s->nbands, sat = p->sat_on && p->s_mix > 0;
    if (!eq) { cur = s->wa + 2; oth = s->wb + 2; }
    int md = (eq ? STRIP_BF_M_EQ : 0) | (p->comp_on ? STRIP_BF_M_COMP : 0) | (sat ? STRIP_BF_M_SAT : 0);
    if (md) md |= STRIP_BF_M_ANY;
    if (!p->tap_post) md |= STRIP_BF_M_PRE;
    if (p->s_type == STRIP_SAT_FOLD) md |= STRIP_BF_M_FOLD;
    if (p->s_type == STRIP_SAT_TUBE) md |= STRIP_BF_M_TUBE;
    if (tk) md |= STRIP_BF_M_TONE;
    if (p->s_mix < 100) md |= STRIP_BF_M_MIX;
    s->mode = (uint16_t)md;
    s->cur = cur;
    s->y = p->s_mix < 100 ? oth : cur;
    /* a section switched on starts from rest (the reference tracks eq_was but resets a band only when the band
       itself turns on, so the EQ switched back on resumed its old filter state) */
    if (p->eq_on && !s->eq_was) for (int b = 0; b < 4; b++) for (int i = 0; i < 7; i++) s->eqs[b][i] = 0;
    if (p->comp_on && !s->comp_was) { c->gr = 0; c->cg = strip_bf_exp2(c->makeup); }
    if (p->sat_on && !s->sat_was) { s->sat.lp = 0; s->sat.dc[0] = s->sat.dc[1] = 0; }
    s->eq_was = p->eq_on; s->comp_was = p->comp_on; s->sat_was = p->sat_on;
    s->dirty = 0;
}

static inline void coop_strip_bf_update(coop_strip_bf *s)
{
    uint8_t on[4];
    for (int b = 0; b < 4; b++) on[b] = (uint8_t)coop_strip_bf_band(&s->p, b, s->eqc[b]);
    coop_strip_bf_commit(s, on);
}


/* Process one track's block: L / R int16 (unity gain) in place; the results are added into the int16 buses with
 * the track's fader / pan gains gL / gR (Q15, 1 << 15 = unity). */
static inline void coop_strip_bf_process_k(coop_strip_bf *s, int16_t *L, int16_t *R, int32_t gL, int32_t gR,
                                           const coop_strip_bf_bus *bus, const strip_bf_kernels *K)
{
    if (s->dirty) coop_strip_bf_update(s);
    /* the bus jobs (kept in the state: g0 = last block's gain): s1 L / R, s2 L / R, dry L / R; gains Q15 */
    strip_bf_job *j = s->jobs;
    j[0].bus = bus->s1L; j[1].bus = bus->s1R; j[2].bus = bus->s2L; j[3].bus = bus->s2R;
    j[4].bus = bus->dryL; j[5].bus = bus->dryR;
    j[0].x = j[2].x = j[4].x = L; j[1].x = j[3].x = j[5].x = R;
    gL = gL > 32768 ? 32768 : (gL < -32767 ? -32767 : gL);
    gR = gR > 32768 ? 32768 : (gR < -32767 ? -32767 : gR);
    const int32_t *lv = s->lvl;                     /* |lvl|, |g| <= 2^15: the products fit */
    j[0].g0 = j[0].g1; j[0].g1 = (lv[1] * gL) >> 15; j[1].g0 = j[1].g1; j[1].g1 = (lv[1] * gR) >> 15;
    j[2].g0 = j[2].g1; j[2].g1 = (lv[2] * gL) >> 15; j[3].g0 = j[3].g1; j[3].g1 = (lv[2] * gR) >> 15;
    j[4].g0 = j[4].g1; j[4].g1 = (lv[0] * gL) >> 15; j[5].g0 = j[5].g1; j[5].g1 = (lv[0] * gR) >> 15;
    if (!s->primed) { for (int k = 0; k < 6; k++) j[k].g0 = j[k].g1; s->primed = 1; }
    const int md = s->mode;
    if (!(md & STRIP_BF_M_ANY)) {                   /* nothing changes the track: the buses straight from L / R */
        s->peak_in = s->peak_out = K->peak(L, R);
        K->bus_n(j, 6);
        return;
    }
    uint32_t *w = s->wa + 2;
    s->peak_in = K->pack(L, R, w);
    if (md & STRIP_BF_M_PRE) K->bus_n(j, 4);        /* the sends before the strip writes L / R back */
    if (md & STRIP_BF_M_EQ) { K->eq(s->ej, s->nbands, s->ye); w = s->cur; }
    if (md & STRIP_BF_M_COMP) K->comp(w, &s->comp);
    if (md & STRIP_BF_M_SAT) {
        uint32_t *y = s->y;
        if (md & STRIP_BF_M_FOLD) K->fold(w, y, &s->sat); else K->shape(w, y, &s->sat);
        if (md & STRIP_BF_M_TUBE) K->tube(y, &s->sat);
        if (md & STRIP_BF_M_TONE) K->tone(y, &s->sat);
        if (md & STRIP_BF_M_MIX) K->mix(w, y, &s->sat);
    }
    s->peak_out = K->unpack(w, L, R);
    if (md & STRIP_BF_M_PRE) K->bus_n(j + 4, 2); else K->bus_n(j, 6);
}

static inline void coop_strip_bf_process(coop_strip_bf *s, int16_t *L, int16_t *R, int32_t gL, int32_t gR,
                                         const coop_strip_bf_bus *bus)
{
    coop_strip_bf_process_k(s, L, R, gL, gR, bus, STRIP_BF_DEFAULT_KERNELS);
}

#endif
