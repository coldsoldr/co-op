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
 *  - EQ: direct form I, 16-bit x / y, coefficients Q26 32-bit split into a signed 16-bit high part (c >> 15) and a
 *    15-bit low part (c & 0x7fff): per channel and sample the low products (plus the error term) accumulate first,
 *    are shifted >> 15 and the high products added: exact c x d for 32-bit c and 16-bit d. First-order error
 *    feedback: the output's 11 dropped bits (kept << 5, a 16-bit word) are added to the next accumulation.
 *  - Comp: the reference's detector and gain computer in 32-bit (coop_mul_fr) on the int16 peak; the gain as a
 *    Q15 mantissa and a left shift (0..5) per 16-sample sub-block, ramped linearly within it, ending on the target.
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

/* One EQ band over a block: DF1 with a 31-bit feedback state. y = Y + E / 2^15 (Y int16, E signed 15-bit), the
 * coefficients Q29 split c = H 2^15 + L (H signed 16-bit, L 0 .. 32767), the b's scaled by 2^-k (|b / 2^k| < 2).
 * Per channel and sample, every product exact, in the MAC's fractional mode (products x 2):
 *   A  = 2 (L1 E1 + L2 E2);  A >>= 15
 *   A += 2 (H1 E1 + H2 E2 + L1 Y1 + L2 Y2 + Lb0 x0 + Lb1 x1 + Lb2 x2) + r + 2^29     (2^29: the 1/2 LSB rounding)
 *   r  = A & 0x7fff;  A >>= 15                                                          (first-order error feedback)
 *   A += 2 (H1 Y1 + H2 Y2 + Hb0 x0 + Hb1 x1 + Hb2 x2);   v = sat32(A)                  (v = y 2^15 + 2^14)
 *   w = sat32(v << 1): Y = w >> 16 (= round(y)), E = (int16)(w ^ 0x8000) >> 1;  out = Y, or for k > 0
 *   out = sat16(sat16(Y << k) + ((E + 2^(14 - k)) >> (15 - k)))                         (round(y 2^k))
 * The state's only truncation is fed back, so a constant error cannot hold the filter off zero (1 + a1 + a2 is
 * 8e-6 at 20 Hz: a 16-bit state, or any unfed truncation at its level, leaves a DC residue of up to 100+ LSB).
 * in[0..1] x[-2], x[-1], in[2..65] the block; out[2..65] the rounded outputs (the next band's in);
 * ye = Y[66] then E[66] scratch, [0..1] history. st = { x[-2], x[-1], Y[-2], Y[-1], E[-2], E[-1], r } (packed),
 * coef = { -a1, -a2, b2, b1, b0 (packed H << 16 | L), k }. */
static inline void strip_bfc_eq_band(uint32_t *in, uint32_t *out, const uint32_t *coef, uint32_t *st, uint32_t *ye)
{
    uint32_t *Yb = ye, *Eb = ye + STRIP_BLOCK + 2;
    in[0] = st[0]; in[1] = st[1]; Yb[0] = st[2]; Yb[1] = st[3]; Eb[0] = st[4]; Eb[1] = st[5];
    int32_t H[5], Lo[5];
    for (int k = 0; k < 5; k++) { H[k] = strip_bf_hi(coef[k]); Lo[k] = (int32_t)(coef[k] & 0xffffu); }
    const int k = (int)coef[5];
    uint32_t r = st[6];
    for (int n = 0; n < STRIP_BLOCK; n++) {
        uint32_t yn = 0, en = 0, on = 0, rn = 0;
        for (int c = 0; c < 2; c++) {
            int32_t (*h)(uint32_t) = c ? strip_bf_hi_i : strip_bf_lo_i;
            int32_t Y1 = h(Yb[n + 1]), Y2 = h(Yb[n]), E1 = h(Eb[n + 1]), E2 = h(Eb[n]);
            int32_t x0 = h(in[n + 2]), x1 = h(in[n + 1]), x2 = h(in[n]);
            int64_t A = 2 * ((int64_t)Lo[0] * E1 + (int64_t)Lo[1] * E2);
            A >>= 15;
            A += 2 * ((int64_t)H[0] * E1 + (int64_t)H[1] * E2 + (int64_t)Lo[0] * Y1 + (int64_t)Lo[1] * Y2
                      + (int64_t)Lo[2] * x2 + (int64_t)Lo[3] * x1 + (int64_t)Lo[4] * x0)
                 + (int64_t)(c ? r >> 16 : r & 0xffffu) + (1LL << 29);
            rn |= (uint32_t)(A & 0x7fff) << (16 * c);
            A >>= 15;
            A += 2 * ((int64_t)H[0] * Y1 + (int64_t)H[1] * Y2 + (int64_t)H[2] * x2 + (int64_t)H[3] * x1 + (int64_t)H[4] * x0);
            int32_t v = strip_sat32(A);
            int32_t w = strip_sat32((int64_t)v * 2);
            int32_t Y = (int16_t)(uint16_t)((uint32_t)w >> 16);
            int32_t E = (int16_t)(uint16_t)(((uint32_t)w & 0xffffu) ^ 0x8000u) >> 1;
            int32_t o = Y;
            if (k) o = coop_sat16(coop_sat16(Y * (1 << k)) + ((E + (1 << (14 - k))) >> (15 - k)));
            yn |= (uint32_t)(uint16_t)Y << (16 * c);
            en |= (uint32_t)(uint16_t)E << (16 * c);
            on |= (uint32_t)(uint16_t)o << (16 * c);
        }
        Yb[n + 2] = yn; Eb[n + 2] = en; out[n + 2] = on;
        r = rn;
    }
    st[0] = in[64]; st[1] = in[65]; st[2] = Yb[64]; st[3] = Yb[65]; st[4] = Eb[64]; st[5] = Eb[65]; st[6] = r;
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
    int32_t p = x * k->drive;                                   /* Q26, period 4 = 2^28 */
    int32_t u = (int32_t)(((uint32_t)p + (1u << 26)) & 0x0fffffffu);
    int32_t t = u - (1 << 27);
    int32_t y = coop_sat16(((1 << 26) - (t < 0 ? -t : t)) >> 11);
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
            k->dc[c] += z >> 10;
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

/* ---- the kernel set (C model, or the assembly on the Blackfin) ---- */
typedef struct {
    int32_t (*pack)(const int16_t *L, const int16_t *R, uint32_t *w);
    int32_t (*unpack)(const uint32_t *w, int16_t *L, int16_t *R);
    int32_t (*peak)(const int16_t *L, const int16_t *R);
    void (*eq_band)(uint32_t *in, uint32_t *out, const uint32_t *coef, uint32_t *st, uint32_t *ye);
    int32_t (*peak16)(const uint32_t *w);
    void (*comp_apply)(uint32_t *w, int32_t g, int32_t step, int32_t sh);
    void (*shape)(const uint32_t *x, uint32_t *y, const strip_bf_satk *k);
    void (*fold)(const uint32_t *x, uint32_t *y, const strip_bf_satk *k);
    void (*tube)(uint32_t *y, strip_bf_satk *k);
    void (*tone)(uint32_t *y, strip_bf_satk *k);
    void (*mix)(uint32_t *x, const uint32_t *y, const strip_bf_satk *k);
    void (*bus)(int16_t *bus, const int16_t *x, int32_t g, int32_t step);
    void (*bus_unity)(int16_t *bus, const int16_t *x);
} strip_bf_kernels;

static const strip_bf_kernels strip_bf_c_kernels = {
    strip_bfc_pack, strip_bfc_unpack, strip_bfc_peak, strip_bfc_eq_band, strip_bfc_peak16, strip_bfc_comp_apply,
    strip_bfc_shape, strip_bfc_fold, strip_bfc_tube, strip_bfc_tone, strip_bfc_mix, strip_bfc_bus, strip_bfc_bus_unity,
};

#if defined(__bfin__) && !defined(COOP_STRIP_BF_C_ONLY)
/* core/sdram/strip_fast.S */
extern "C" {
int32_t strip_bfa_pack(const int16_t *L, const int16_t *R, uint32_t *w);
int32_t strip_bfa_unpack(const uint32_t *w, int16_t *L, int16_t *R);
int32_t strip_bfa_peak(const int16_t *L, const int16_t *R);
void strip_bfa_eq_band(uint32_t *in, uint32_t *out, const uint32_t *coef, uint32_t *st, uint32_t *ye);
int32_t strip_bfa_peak16(const uint32_t *w);
void strip_bfa_comp_apply(uint32_t *w, int32_t g, int32_t step, int32_t sh);
void strip_bfa_shape(const uint32_t *x, uint32_t *y, const strip_bf_satk *k);
void strip_bfa_fold(const uint32_t *x, uint32_t *y, const strip_bf_satk *k);
void strip_bfa_tube(uint32_t *y, strip_bf_satk *k);
void strip_bfa_tone(uint32_t *y, strip_bf_satk *k);
void strip_bfa_mix(uint32_t *x, const uint32_t *y, const strip_bf_satk *k);
void strip_bfa_bus(int16_t *bus, const int16_t *x, int32_t g, int32_t step);
void strip_bfa_bus_unity(int16_t *bus, const int16_t *x);
}
static const strip_bf_kernels strip_bf_asm_kernels = {
    strip_bfa_pack, strip_bfa_unpack, strip_bfa_peak, strip_bfa_eq_band, strip_bfa_peak16, strip_bfa_comp_apply,
    strip_bfa_shape, strip_bfa_fold, strip_bfa_tube, strip_bfa_tone, strip_bfa_mix, strip_bfa_bus, strip_bfa_bus_unity,
};
#define STRIP_BF_DEFAULT_KERNELS (&strip_bf_asm_kernels)
#else
#define STRIP_BF_DEFAULT_KERNELS (&strip_bf_c_kernels)
#endif

/* ---- state ---- */
typedef struct {
    coop_strip_params p;
    /* derived */
    uint8_t band_on[4], nbands, band_idx[4];
    uint32_t eqc[4][6];                     /* per band: -a1, -a2, b2, b1, b0 (Q29 packed H << 16 | L), k */
    int32_t thr_l2, slope, knee_l2, inv2k, makeup_l2, att, rel;    /* comp (as the reference; inv2k = 2^34 / 2k) */
    strip_bf_satk sat;                      /* sat constants and state */
    int32_t lvl[3];                         /* dry / fx1 / fx2, Q15 (100 % = 32768) */
    /* state */
    uint32_t eqs[4][7];                     /* per band: x[-2], x[-1], Y[-2], Y[-1], E[-2], E[-1], r */
    int32_t gr;                             /* comp gain reduction, log2 Q16 (>= 0) */
    int32_t cg;                             /* comp gain of the last sub-block, Q26 */
    int32_t g_prev[3][2];                   /* last block's level x fader gains, Q15 */
    uint8_t dirty, eq_was, comp_was, sat_was, primed;
    int32_t peak_in, peak_out;              /* meters for the UI: int16 peaks (0 .. 32767) */
    /* work: two blocks of 2 history + 64 frames */
    uint32_t wa[STRIP_BLOCK + 2], wb[STRIP_BLOCK + 2];
    uint32_t ye[2 * (STRIP_BLOCK + 2)];     /* the EQ's Y / E histories */
} coop_strip_bf;

typedef struct {
    int16_t *dryL, *dryR, *s1L, *s1R, *s2L, *s2R;   /* int16, STRIP_BLOCK each, 4-byte aligned */
} coop_strip_bf_bus;

static inline void coop_strip_bf_set(coop_strip_bf *s, const coop_strip_params *p) { s->p = *p; s->dirty = 1; }

static inline void coop_strip_bf_init(coop_strip_bf *s)
{
    uint8_t *z = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++) z[i] = 0;
    coop_strip_params_default(&s->p);
    s->cg = 1 << 26;
    s->dirty = 1;
}

static inline uint32_t strip_bf_coef(int64_t c29)    /* Q29, |c| < 2 */
{
    int32_t c = (int32_t)c29;
    return ((uint32_t)(uint16_t)(c >> 15) << 16) | ((uint32_t)c & 0x7fffu);
}

static inline int32_t strip_bf_level(int pct)
{
    if (pct <= 0) return 0;
    if (pct >= 100) return 32768;
    return pct * pct * 32768 / 10000;
}

static inline void coop_strip_bf_update(coop_strip_bf *s)
{
    const coop_strip_params *p = &s->p;
    s->nbands = 0;
    for (int b = 0; b < 4; b++) {
        coop_strip_bq q;
        int on = strip_band_coefs(p, b, &q);
        if (on) {
            /* the b's scaled by 2^-k so that |b| < 2 at Q29 (a boost up to +18 dB has b up to 16: k <= 3) */
            int64_t mb = 0, bb[3] = { q.b2, q.b1, q.b0 };
            for (int i = 0; i < 3; i++) { int64_t v = bb[i] < 0 ? -bb[i] : bb[i]; if (v > mb) mb = v; }
            int k = 0;
            while (((mb * 4) >> k) > (1LL << 30) - (1LL << 16)) k++;
            s->eqc[b][0] = strip_bf_coef(-(int64_t)q.a1 * 4); s->eqc[b][1] = strip_bf_coef(-(int64_t)q.a2 * 4);
            for (int i = 0; i < 3; i++) s->eqc[b][2 + i] = strip_bf_coef((bb[i] * 4) >> k);
            s->eqc[b][5] = (uint32_t)k;
            if (!s->band_on[b]) for (int i = 0; i < 7; i++) s->eqs[b][i] = 0;
            s->band_idx[s->nbands++] = (uint8_t)b;
        }
        s->band_on[b] = (uint8_t)on;
    }
    s->thr_l2 = -strip_hdb_l2(p->c_thresh);
    s->slope = strip_ratio_slope[p->c_ratio < STRIP_COMP_NRATIO ? p->c_ratio : STRIP_COMP_NRATIO - 1];
    s->knee_l2 = strip_hdb_l2(p->c_knee);
    s->inv2k = s->knee_l2 > 0 ? (int32_t)((1LL << 34) / (2LL * s->knee_l2)) : 0;
    s->makeup_l2 = strip_hdb_l2(p->c_makeup);
    s->att = strip_att_coef[p->c_attack < STRIP_COMP_NTIME ? p->c_attack : STRIP_COMP_NTIME - 1];
    s->rel = strip_rel_coef[p->c_release < STRIP_COMP_NTIME ? p->c_release : STRIP_COMP_NTIME - 1];
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
    if (p->comp_on && !s->comp_was) { s->gr = 0; s->cg = strip_exp2_q26(s->makeup_l2); }
    if (p->sat_on && !s->sat_was) { s->sat.lp = 0; s->sat.dc[0] = s->sat.dc[1] = 0; }
    s->eq_was = p->eq_on; s->comp_was = p->comp_on; s->sat_was = p->sat_on;
    s->dirty = 0;
}

/* the reference's static curve in 32 bits: over x slope >> 16 as coop_mul_fr(over << 10, slope << 5); the knee's
 * d^2 / 2k as coop_mul_fr(d^2 >> 3, 2^34 / 2k) */
static inline int32_t strip_bf_comp_target(const coop_strip_bf *s, int32_t lvl)
{
    int32_t over = lvl - s->thr_l2, k = s->knee_l2;
    if (k <= 0 || 2 * over >= k) return over > 0 ? coop_mul_fr(over << 10, s->slope << 5) : 0;
    if (2 * over <= -k) return 0;
    int32_t d = over + k / 2;
    int32_t q = coop_mul_fr(coop_mul_fr(d << 14, d << 14), s->inv2k);
    return coop_mul_fr(q << 10, s->slope << 5);
}

static inline void strip_bf_comp(coop_strip_bf *s, uint32_t *w, const strip_bf_kernels *K)
{
    for (int o = 0; o < STRIP_BLOCK; o += STRIP_COMP_SUB) {
        int32_t pk = K->peak16(w + o);
        int32_t target = strip_bf_comp_target(s, strip_log2(pk << 16));
        int32_t c = target > s->gr ? s->att : s->rel;
        s->gr += coop_mul_fr(target - s->gr, c);
        int32_t g1 = strip_exp2_q26(s->makeup_l2 - s->gr), g0 = s->cg;
        int32_t m1 = g1 >> 11, m0 = g0 >> 11, mx = m1 > m0 ? m1 : m0, sh = 0;
        while ((mx >> sh) > 32767) sh++;
        m1 >>= sh; m0 >>= sh;
        int32_t step = (m1 - m0) / STRIP_COMP_SUB;
        K->comp_apply(w + o, m1 - (STRIP_COMP_SUB - 1) * step, step, sh);
        s->cg = g1;
    }
}

/* add x x the ramp g0 -> g1 (Q15, |g| <= 32768) into a bus; nothing when both are 0 */
static inline void strip_bf_add_bus(int16_t *bus, const int16_t *x, int32_t g0, int32_t g1, const strip_bf_kernels *K)
{
    if (!g0 && !g1) return;
    if (g0 == 32768 && g1 == 32768) { K->bus_unity(bus, x); return; }
    if (g0 > 32767) g0 = 32767;
    if (g1 > 32767) g1 = 32767;
    int32_t step = (g1 - g0) / STRIP_BLOCK;
    K->bus(bus, x, g1 - (STRIP_BLOCK - 1) * step, step);
}

static inline int32_t strip_bf_gain(int32_t lvl, int32_t g)
{
    if (g > 32768) g = 32768;
    if (g < -32767) g = -32767;
    return (lvl * g) >> 15;                 /* |lvl|, |g| <= 2^15: fits */
}

/* Process one track's block: L / R int16 (unity gain) in place; the results are added into the int16 buses with
 * the track's fader / pan gains gL / gR (Q15, 1 << 15 = unity). */
static inline void coop_strip_bf_process_k(coop_strip_bf *s, int16_t *L, int16_t *R, int32_t gL, int32_t gR,
                                           const coop_strip_bf_bus *bus, const strip_bf_kernels *K)
{
    if (s->dirty) coop_strip_bf_update(s);
    const coop_strip_params *p = &s->p;
    int32_t gn[3][2];
    for (int k = 0; k < 3; k++) { gn[k][0] = strip_bf_gain(s->lvl[k], gL); gn[k][1] = strip_bf_gain(s->lvl[k], gR); }
    if (!s->primed) {
        for (int k = 0; k < 3; k++) { s->g_prev[k][0] = gn[k][0]; s->g_prev[k][1] = gn[k][1]; }
        s->primed = 1;
    }
    int eq = p->eq_on && s->nbands, sat = p->sat_on && p->s_mix > 0, any = eq || p->comp_on || sat;
    uint32_t *cur = s->wa + 2, *oth = s->wb + 2;
    if (any) s->peak_in = K->pack(L, R, cur);
    else s->peak_in = K->peak(L, R);
    if (!p->tap_post) {
        strip_bf_add_bus(bus->s1L, L, s->g_prev[1][0], gn[1][0], K); strip_bf_add_bus(bus->s1R, R, s->g_prev[1][1], gn[1][1], K);
        strip_bf_add_bus(bus->s2L, L, s->g_prev[2][0], gn[2][0], K); strip_bf_add_bus(bus->s2R, R, s->g_prev[2][1], gn[2][1], K);
    }
    if (eq) {
        for (int k = 0; k < s->nbands; k++) {
            int b = s->band_idx[k];
            K->eq_band(cur - 2, oth - 2, s->eqc[b], s->eqs[b], s->ye);
            uint32_t *t = cur; cur = oth; oth = t;
        }
    }
    if (p->comp_on) strip_bf_comp(s, cur, K);
    if (sat) {
        int in_place = p->s_mix >= 100;
        uint32_t *y = in_place ? cur : oth;
        if (p->s_type == STRIP_SAT_FOLD) K->fold(cur, y, &s->sat); else K->shape(cur, y, &s->sat);
        if (p->s_type == STRIP_SAT_TUBE) K->tube(y, &s->sat);
        if (strip_bf_lo((uint32_t)s->sat.tone)) K->tone(y, &s->sat);
        if (!in_place) K->mix(cur, y, &s->sat);
    }
    s->peak_out = any ? K->unpack(cur, L, R) : s->peak_in;
    if (p->tap_post) {
        strip_bf_add_bus(bus->s1L, L, s->g_prev[1][0], gn[1][0], K); strip_bf_add_bus(bus->s1R, R, s->g_prev[1][1], gn[1][1], K);
        strip_bf_add_bus(bus->s2L, L, s->g_prev[2][0], gn[2][0], K); strip_bf_add_bus(bus->s2R, R, s->g_prev[2][1], gn[2][1], K);
    }
    strip_bf_add_bus(bus->dryL, L, s->g_prev[0][0], gn[0][0], K);
    strip_bf_add_bus(bus->dryR, R, s->g_prev[0][1], gn[0][1], K);
    for (int k = 0; k < 3; k++) { s->g_prev[k][0] = gn[k][0]; s->g_prev[k][1] = gn[k][1]; }
}

static inline void coop_strip_bf_process(coop_strip_bf *s, int16_t *L, int16_t *R, int32_t gL, int32_t gR,
                                         const coop_strip_bf_bus *bus)
{
    coop_strip_bf_process_k(s, L, R, gL, gR, bus, STRIP_BF_DEFAULT_KERNELS);
}

#endif
