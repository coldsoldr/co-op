/* coop-fw per-tape channel strip: 4-band parametric EQ -> compressor -> saturator, then dry / FX 1 send /
 * FX 2 send (docs/channel-strip.md). One coop_strip per tape track.
 *
 * Pure DSP on caller-supplied stereo Q31 blocks of STRIP_BLOCK samples. The firmware side (the tape play mix,
 * core/sdram/coop_strip.cc) hands each track's block at unity gain, the track's fader/pan gains (Q15) and three
 * stereo buses to add into: dry (to the master), send 1 (master FX 1), send 2 (master FX 2).
 *
 * Rules (owner, 2026-11-22): a bypassed section costs nothing (it is not entered); an EQ band at 0 dB is not run;
 * a send or the dry path at 0 % is not run. The send tap is PRE (the track as it comes off the tape) or POST
 * (after EQ, comp and sat); sends and dry both follow the track's fader and pan.
 *
 * Fixed point only, no libc, no float. Coefficient divides happen only when a parameter changed (coop_strip_update),
 * never per sample. Parameter steps come from coop_strip_tables.h (tools/gen_strip_tables.py).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef COOP_STRIP_H
#define COOP_STRIP_H
#include <stdint.h>
#include "coop_strip_tables.h"

#define STRIP_BLOCK 64
#define STRIP_BLOCK_SHIFT 6
#define STRIP_EQ_FRAC 27            /* biquad coefficients, Q27 (|c| < 16) */

/* ---- the saved form (per tape project): UI steps, not DSP values ---- */
enum { STRIP_SAT_TAPE = 0, STRIP_SAT_TUBE = 1, STRIP_SAT_DIODE = 2, STRIP_SAT_FOLD = 3, STRIP_SAT_NTYPE = 4 };

typedef struct {
    uint8_t eq_on, comp_on, sat_on, tap_post;   /* section switches; tap_post 1 = sends after the strip */
    uint8_t eq_cut[4];          /* bands 0 and 3: 0 = shelf, 1 = cut (low cut / high cut); bands 1, 2 peak */
    uint8_t eq_freq[4];         /* 0 .. STRIP_EQ_NFREQ-1, 1/12 octave from 20 Hz */
    uint8_t eq_gain[4];         /* 0 .. 72, half dB, STRIP_EQ_GAIN0 = 0 dB (ignored by a cut band) */
    uint8_t eq_q[4];            /* 0 .. STRIP_EQ_NQ-1 */
    uint8_t c_thresh;           /* 0 .. 96: 0 .. -48 dB in half dB */
    uint8_t c_ratio;            /* 0 .. STRIP_COMP_NRATIO-1 */
    uint8_t c_attack, c_release;/* 0 .. STRIP_COMP_NTIME-1 */
    uint8_t c_makeup;           /* 0 .. 48: 0 .. +24 dB in half dB */
    uint8_t c_knee;             /* 0 .. 24: 0 .. 12 dB in half dB */
    uint8_t s_drive;            /* 0 .. 100 */
    uint8_t s_type;             /* STRIP_SAT_* */
    int8_t  s_tone;             /* -50 .. +50 */
    uint8_t s_mix;              /* 0 .. 100 */
    uint8_t dry, fx1, fx2;      /* 0 .. 100 % */
    uint8_t reserved[3];
} coop_strip_params;            /* 40 bytes */

static inline void coop_strip_params_default(coop_strip_params *p)
{
    p->eq_on = p->comp_on = p->sat_on = 0; p->tap_post = 1;
    static const uint8_t f[4] = { 24, 60, 84, 108 };    /* 80 Hz, 640 Hz, 2.5 kHz, 10 kHz */
    for (int b = 0; b < 4; b++) { p->eq_cut[b] = 0; p->eq_freq[b] = f[b]; p->eq_gain[b] = STRIP_EQ_GAIN0; p->eq_q[b] = 7; }
    p->c_thresh = 36; p->c_ratio = 4; p->c_attack = 15; p->c_release = 16; p->c_makeup = 0; p->c_knee = 6;
    p->s_drive = 30; p->s_type = STRIP_SAT_TAPE; p->s_tone = 0; p->s_mix = 100;
    p->dry = 100; p->fx1 = 0; p->fx2 = 0;
    p->reserved[0] = p->reserved[1] = p->reserved[2] = 0;
}

/* A neutral strip changes nothing: every section off, dry 100 %, no sends. The firmware leaves such a track in
 * the stock summed mix (no per-track work at all). */
static inline int coop_strip_neutral(const coop_strip_params *p)
{
    return !p->eq_on && !p->comp_on && !p->sat_on && p->dry == 100 && p->fx1 == 0 && p->fx2 == 0;
}

/* ---- DSP state ---- */
typedef struct { int32_t b0, b1, b2, a1, a2; } coop_strip_bq;
/* Direct form I with first-order error feedback: the rounding remainder of each output is added to the next
 * accumulation, so the filter's own quantisation noise is shaped away from DC. Plain DF2T with 32-bit states
 * left a -66 dBFS residue that never decayed for poles near 20 Hz (host test). */
typedef struct { int32_t x1, x2, y1, y2; int64_t e; } coop_strip_bqs;

typedef struct {
    coop_strip_params p;
    /* derived */
    uint8_t band_on[4], nbands;
    uint8_t band_idx[4];                    /* the running bands, in order */
    coop_strip_bq bq[4];
    int32_t thr_l2, slope, knee_l2, makeup_l2, att, rel;    /* comp, log2 Q16 / Q31 coefs */
    int32_t drive_g, drive_comp, tone_k, mix16;             /* sat */
    int32_t lvl_dry, lvl_fx1, lvl_fx2;                      /* Q30 */
    /* state */
    coop_strip_bqs bs[4][2];
    int32_t gr;                             /* comp gain reduction, log2 Q16 (>= 0) */
    int32_t cg;                             /* comp gain of the last sub-block, Q26 */
    int32_t lp[2], dc[2];                   /* sat tone low-pass, tube DC */
    int32_t g_prev[3][2];                   /* last block's dry/fx1/fx2 x fader gains, Q30, per channel */
    uint8_t dirty, eq_was, comp_was, sat_was, primed;
    /* meters for the UI (read only) */
    int32_t peak_in, peak_out;
} coop_strip;

typedef struct {
    int32_t *dryL, *dryR, *s1L, *s1R, *s2L, *s2R;   /* Q31 accumulators, STRIP_BLOCK each */
} coop_strip_bus;

/* ---- helpers ---- */
static inline int32_t strip_sat32(int64_t v)
{
    if (v > 2147483647LL) return 2147483647;
    if (v < -2147483648LL) return (int32_t)-2147483647 - 1;
    return (int32_t)v;
}
static inline int32_t strip_abs(int32_t v) { return v < 0 ? (v == (int32_t)0x80000000 ? 2147483647 : -v) : v; }

/* log2(x / 2^31) in Q16 for x > 0 (<= 0); -32.0 for x <= 0. */
static inline int32_t strip_log2(int32_t x)
{
    if (x <= 0) return -(32 << 16);
    uint32_t m = (uint32_t)x;
    int p = 30;
    if (m < (1u << 15)) { m <<= 16; p -= 16; }
    if (m < (1u << 23)) { m <<= 8; p -= 8; }
    if (m < (1u << 27)) { m <<= 4; p -= 4; }
    if (m < (1u << 29)) { m <<= 2; p -= 2; }
    if (m < (1u << 30)) { m <<= 1; p -= 1; }
    /* m in [2^30, 2^31): x = m * 2^(p-30) */
    uint32_t idx = (m >> 22) & 255u, fr = (m >> 14) & 255u;
    int32_t lg = strip_log2_tab[idx] + (((strip_log2_tab[idx + 1] - strip_log2_tab[idx]) * (int32_t)fr) >> 8);
    return ((p - 31) << 16) + lg;
}

/* 2^(g) for g in log2 Q16, as a Q26 linear gain (0 .. ~32). */
static inline int32_t strip_exp2_q26(int32_t g)
{
    int32_t n = g >> 16;                       /* floor */
    uint32_t f = (uint32_t)g & 0xffffu, idx = f >> 8, fr = f & 255u;
    int32_t m = strip_exp2_tab[idx] + (((strip_exp2_tab[idx + 1] - strip_exp2_tab[idx]) * (int32_t)fr) >> 8);  /* Q30 */
    if (n >= 4) return n >= 5 ? 2147483647 : m;   /* m << (n-4) with n == 4: m (Q30 [1,2) -> Q26 [16,32)) */
    int sh = 4 - n;
    return sh > 31 ? 0 : m >> sh;
}

static inline int32_t strip_level_q30(int pct)
{
    if (pct <= 0) return 0;
    if (pct >= 100) return 1 << 30;
    return (int32_t)(((int64_t)pct * pct << 30) / 10000);
}

/* half-dB step -> log2 Q16 */
static inline int32_t strip_hdb_l2(int hdb) { return (int32_t)(((int64_t)hdb * 10885 + (hdb >= 0 ? 1 : -1)) / 2); }

static inline int32_t strip_div27(int64_t b, int64_t a0) { return (int32_t)((b << STRIP_EQ_FRAC) / a0); }

/* RBJ cookbook coefficients from the table steps, Q28 intermediates, normalised to Q27. Returns 0 for an
 * identity band (peak/shelf at 0 dB): it is skipped. */
static inline int strip_band_coefs(const coop_strip_params *p, int b, coop_strip_bq *o)
{
    const int64_t one = 1LL << 28;
    int fi = p->eq_freq[b] < STRIP_EQ_NFREQ ? p->eq_freq[b] : STRIP_EQ_NFREQ - 1;
    int gi = p->eq_gain[b] < STRIP_EQ_NGAIN ? p->eq_gain[b] : STRIP_EQ_NGAIN - 1;
    int qi = p->eq_q[b] < STRIP_EQ_NQ ? p->eq_q[b] : STRIP_EQ_NQ - 1;
    int64_t cs = strip_eq_cos[fi] >> 2;                                       /* Q28 */
    int64_t al = (((int64_t)strip_eq_sin[fi] * strip_eq_inv2q[qi]) >> 30) >> 2; /* Q28 */
    int cut = (b == 0 || b == 3) && p->eq_cut[b];
    int64_t b0, b1, b2, a0, a1, a2;
    if (cut) {
        if (b == 0) { b0 = (one + cs) / 2; b1 = -(one + cs); }               /* high-pass = low cut */
        else        { b0 = (one - cs) / 2; b1 = one - cs; }                  /* low-pass = high cut */
        b2 = b0; a0 = one + al; a1 = -2 * cs; a2 = one - al;
    } else {
        if (gi == STRIP_EQ_GAIN0) return 0;
        int64_t A = strip_eq_A[gi];
        if (b == 1 || b == 2) {
            int64_t aA = (al * A) >> 28, aAi = (al * strip_eq_Ainv[gi]) >> 28;
            b0 = one + aA; b1 = -2 * cs; b2 = one - aA; a0 = one + aAi; a1 = -2 * cs; a2 = one - aAi;
        } else {
            int64_t ap1 = A + one, am1 = A - one;
            int64_t t = (am1 * cs) >> 28, apc = (ap1 * cs) >> 28;
            int64_t tsa = (2 * strip_eq_sqrtA[gi] * al) >> 28;
            if (b == 0) {   /* low shelf */
                b0 = (A * (ap1 - t + tsa)) >> 28; b1 = (2 * A * (am1 - apc)) >> 28; b2 = (A * (ap1 - t - tsa)) >> 28;
                a0 = ap1 + t + tsa; a1 = -2 * (am1 + apc); a2 = ap1 + t - tsa;
            } else {        /* high shelf */
                b0 = (A * (ap1 + t + tsa)) >> 28; b1 = (-2 * A * (am1 + apc)) >> 28; b2 = (A * (ap1 + t - tsa)) >> 28;
                a0 = ap1 - t + tsa; a1 = 2 * (am1 - apc); a2 = ap1 - t - tsa;
            }
        }
    }
    o->b0 = strip_div27(b0, a0); o->b1 = strip_div27(b1, a0); o->b2 = strip_div27(b2, a0);
    o->a1 = strip_div27(a1, a0); o->a2 = strip_div27(a2, a0);
    return 1;
}

static inline int32_t strip_bq_sample(coop_strip_bqs *s, const coop_strip_bq *c, int32_t x)
{
    int64_t acc = (int64_t)c->b0 * x + (int64_t)c->b1 * s->x1 + (int64_t)c->b2 * s->x2
                - (int64_t)c->a1 * s->y1 - (int64_t)c->a2 * s->y2 + s->e;
    int64_t q = acc >> STRIP_EQ_FRAC;
    int32_t y;
    if (q > 2147483647LL) { y = 2147483647; s->e = 0; }
    else if (q < -2147483648LL) { y = (int32_t)-2147483647 - 1; s->e = 0; }
    else { y = (int32_t)q; s->e = acc - (q << STRIP_EQ_FRAC); }
    s->x2 = s->x1; s->x1 = x; s->y2 = s->y1; s->y1 = y;
    return y;
}

/* ---- setup ---- */
static inline void coop_strip_set(coop_strip *s, const coop_strip_params *p) { s->p = *p; s->dirty = 1; }

static inline void coop_strip_init(coop_strip *s)
{
    uint8_t *z = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof *s; i++) z[i] = 0;
    coop_strip_params_default(&s->p);
    s->cg = 1 << 26;
    s->dirty = 1;
}

/* Recompute the derived values (call at block start when dirty: a few int64 divides per changed band). */
static inline void coop_strip_update(coop_strip *s)
{
    const coop_strip_params *p = &s->p;
    s->nbands = 0;
    for (int b = 0; b < 4; b++) {
        int on = strip_band_coefs(p, b, &s->bq[b]);
        if (on && !s->band_on[b]) {
            for (int c = 0; c < 2; c++) { coop_strip_bqs *z = &s->bs[b][c]; z->x1 = z->x2 = z->y1 = z->y2 = 0; z->e = 0; }
        }
        s->band_on[b] = (uint8_t)on;
        if (on) s->band_idx[s->nbands++] = (uint8_t)b;
    }
    s->thr_l2 = -strip_hdb_l2(p->c_thresh);
    s->slope = strip_ratio_slope[p->c_ratio < STRIP_COMP_NRATIO ? p->c_ratio : STRIP_COMP_NRATIO - 1];
    s->knee_l2 = strip_hdb_l2(p->c_knee);
    s->makeup_l2 = strip_hdb_l2(p->c_makeup);
    s->att = strip_att_coef[p->c_attack < STRIP_COMP_NTIME ? p->c_attack : STRIP_COMP_NTIME - 1];
    s->rel = strip_rel_coef[p->c_release < STRIP_COMP_NTIME ? p->c_release : STRIP_COMP_NTIME - 1];
    int d = p->s_drive > 100 ? 100 : p->s_drive;
    s->drive_g = strip_drive_gain[d];
    s->drive_comp = strip_drive_comp[d];
    int t = p->s_tone < -50 ? -50 : (p->s_tone > 50 ? 50 : p->s_tone);
    s->tone_k = t * 32767 / 50;
    s->mix16 = (p->s_mix > 100 ? 100 : p->s_mix) * 65536 / 100;
    s->lvl_dry = strip_level_q30(p->dry);
    s->lvl_fx1 = strip_level_q30(p->fx1);
    s->lvl_fx2 = strip_level_q30(p->fx2);
    /* a section switched on starts from rest */
    if (p->comp_on && !s->comp_was) { s->gr = 0; s->cg = strip_exp2_q26(s->makeup_l2); }
    if (p->sat_on && !s->sat_was) { s->lp[0] = s->lp[1] = s->dc[0] = s->dc[1] = 0; }
    s->eq_was = p->eq_on; s->comp_was = p->comp_on; s->sat_was = p->sat_on;
    s->dirty = 0;
}

/* ---- sections ---- */
static inline void strip_eq_block(coop_strip *s, int32_t *L, int32_t *R)
{
    for (int k = 0; k < s->nbands; k++) {
        int b = s->band_idx[k];
        const coop_strip_bq *c = &s->bq[b];
        coop_strip_bqs *sl = &s->bs[b][0], *sr = &s->bs[b][1];
        for (int i = 0; i < STRIP_BLOCK; i++) { L[i] = strip_bq_sample(sl, c, L[i]); R[i] = strip_bq_sample(sr, c, R[i]); }
    }
}

static inline int32_t strip_comp_target(const coop_strip *s, int32_t lvl)
{
    int32_t over = lvl - s->thr_l2, k = s->knee_l2;
    if (k <= 0) return over > 0 ? (int32_t)(((int64_t)over * s->slope) >> 16) : 0;
    if (2 * over <= -k) return 0;
    if (2 * over >= k) return (int32_t)(((int64_t)over * s->slope) >> 16);
    int64_t d = over + k / 2;
    return (int32_t)(((d * d / (2 * k)) * s->slope) >> 16);
}

/* Stereo-linked peak compressor: the gain is computed once per STRIP_COMP_SUB samples in the log2 domain and
 * ramped linearly across the sub-block. */
static inline void strip_comp_block(coop_strip *s, int32_t *L, int32_t *R)
{
    for (int o = 0; o < STRIP_BLOCK; o += STRIP_COMP_SUB) {
        int32_t pk = 0;
        for (int i = o; i < o + STRIP_COMP_SUB; i++) {
            int32_t a = strip_abs(L[i]), b = strip_abs(R[i]);
            if (a > pk) pk = a;
            if (b > pk) pk = b;
        }
        int32_t target = strip_comp_target(s, strip_log2(pk));
        int32_t c = target > s->gr ? s->att : s->rel;
        s->gr += (int32_t)(((int64_t)(target - s->gr) * c) >> 31);
        int32_t g1 = strip_exp2_q26(s->makeup_l2 - s->gr), g0 = s->cg;
        int32_t step = (g1 - g0) / STRIP_COMP_SUB;
        int32_t g = g0;
        for (int i = o; i < o + STRIP_COMP_SUB; i++) {
            g += step;
            if (i == o + STRIP_COMP_SUB - 1) g = g1;
            L[i] = strip_sat32(((int64_t)L[i] * g) >> 26);
            R[i] = strip_sat32(((int64_t)R[i] * g) >> 26);
        }
        s->cg = g1;
    }
}

static inline int32_t strip_shape(const int16_t *t, int64_t v)
{
    const int64_t lim = 4LL << 28;
    if (v >= lim) v = lim - 1;
    if (v < -lim) v = -lim;
    uint32_t pos = (uint32_t)(v + lim);
    uint32_t idx = pos >> 22, fr = (pos >> 6) & 0xffffu;
    int32_t y = t[idx] + (int32_t)((((int64_t)(t[idx + 1] - t[idx])) * fr) >> 16);
    return y << 16;
}

static inline int32_t strip_fold(int64_t v)
{
    const int64_t one = 1LL << 28, per = 4LL << 28;
    int64_t u = v + one;
    while (u < 0) u += per;
    while (u >= per) u -= per;
    int64_t y = u < 2 * one ? u - one : 3 * one - u;     /* triangle, [-1, 1] Q28 */
    return strip_sat32(y << 3);
}

static inline void strip_sat_block(coop_strip *s, int32_t *L, int32_t *R)
{
    const int16_t *tab = s->p.s_type == STRIP_SAT_TUBE ? strip_sat_tube
                       : s->p.s_type == STRIP_SAT_DIODE ? strip_sat_diode : strip_sat_tape;
    int fold = s->p.s_type == STRIP_SAT_FOLD, tube = s->p.s_type == STRIP_SAT_TUBE;
    const int32_t lpc = 412316860;               /* one-pole ~1.5 kHz at 44.1 kHz, Q31 */
    int32_t *ch[2] = { L, R };
    for (int c = 0; c < 2; c++) {
        int32_t *x = ch[c];
        int32_t lp = s->lp[c], dc = s->dc[c];
        for (int i = 0; i < STRIP_BLOCK; i++) {
            int32_t dry = x[i];
            int64_t v = ((int64_t)dry * s->drive_g) >> 27;        /* Q28 */
            int32_t y = fold ? strip_fold(v) : strip_shape(tab, v);
            y = (int32_t)(((int64_t)y * s->drive_comp) >> 15);
            if (tube) { y -= dc; dc += (y) >> 10; }
            if (s->tone_k) {
                lp += (int32_t)(((int64_t)(y - lp) * lpc) >> 31);
                if (s->tone_k < 0) y = strip_sat32(y + ((((int64_t)lp - y) * -s->tone_k) >> 15));
                else               y = strip_sat32(y + ((((int64_t)y - lp) * s->tone_k) >> 15));
            }
            x[i] = strip_sat32(dry + ((((int64_t)y - dry) * s->mix16) >> 16));
        }
        s->lp[c] = lp; s->dc[c] = dc;
    }
}

/* Add x * gain (ramped from the last block's gain) into a bus. Gains Q30. Nothing runs when both are 0. */
static inline void strip_add_bus(int32_t *bus, const int32_t *x, int32_t g0, int32_t g1)
{
    if (!g0 && !g1) return;
    if (g0 == g1) {
        for (int i = 0; i < STRIP_BLOCK; i++) bus[i] = strip_sat32((int64_t)bus[i] + (((int64_t)x[i] * g1) >> 30));
        return;
    }
    int32_t step = (g1 - g0) >> STRIP_BLOCK_SHIFT, g = g0;
    for (int i = 0; i < STRIP_BLOCK; i++) {
        g += step;
        if (i == STRIP_BLOCK - 1) g = g1;
        bus[i] = strip_sat32((int64_t)bus[i] + (((int64_t)x[i] * g) >> 30));
    }
}

static inline int32_t strip_peak(const int32_t *L, const int32_t *R)
{
    int32_t pk = 0;
    for (int i = 0; i < STRIP_BLOCK; i++) {
        int32_t a = strip_abs(L[i]), b = strip_abs(R[i]);
        if (a > pk) pk = a;
        if (b > pk) pk = b;
    }
    return pk;
}

/* Process one track's block. L/R (Q31, unity gain) are processed in place; the results are added into the
 * buses with the track's fader/pan gains gL/gR (Q15, 32767 ~ unity). */
static inline void coop_strip_process(coop_strip *s, int32_t *L, int32_t *R, int32_t gL, int32_t gR,
                                      const coop_strip_bus *bus)
{
    if (s->dirty) coop_strip_update(s);
    const coop_strip_params *p = &s->p;
    int32_t gn[3][2];
    const int32_t lv[3] = { s->lvl_dry, s->lvl_fx1, s->lvl_fx2 };
    for (int k = 0; k < 3; k++) {
        gn[k][0] = (int32_t)(((int64_t)lv[k] * gL) >> 15);
        gn[k][1] = (int32_t)(((int64_t)lv[k] * gR) >> 15);
    }
    if (!s->primed) {                       /* the first block starts at its gains, no fade-in */
        for (int k = 0; k < 3; k++) { s->g_prev[k][0] = gn[k][0]; s->g_prev[k][1] = gn[k][1]; }
        s->primed = 1;
    }
    s->peak_in = strip_peak(L, R);
    if (!p->tap_post) {
        strip_add_bus(bus->s1L, L, s->g_prev[1][0], gn[1][0]); strip_add_bus(bus->s1R, R, s->g_prev[1][1], gn[1][1]);
        strip_add_bus(bus->s2L, L, s->g_prev[2][0], gn[2][0]); strip_add_bus(bus->s2R, R, s->g_prev[2][1], gn[2][1]);
    }
    if (p->eq_on && s->nbands) strip_eq_block(s, L, R);
    if (p->comp_on) strip_comp_block(s, L, R);
    if (p->sat_on) strip_sat_block(s, L, R);
    if (p->tap_post) {
        strip_add_bus(bus->s1L, L, s->g_prev[1][0], gn[1][0]); strip_add_bus(bus->s1R, R, s->g_prev[1][1], gn[1][1]);
        strip_add_bus(bus->s2L, L, s->g_prev[2][0], gn[2][0]); strip_add_bus(bus->s2R, R, s->g_prev[2][1], gn[2][1]);
    }
    strip_add_bus(bus->dryL, L, s->g_prev[0][0], gn[0][0]);
    strip_add_bus(bus->dryR, R, s->g_prev[0][1], gn[0][1]);
    s->peak_out = strip_peak(L, R);
    for (int k = 0; k < 3; k++) { s->g_prev[k][0] = gn[k][0]; s->g_prev[k][1] = gn[k][1]; }
}

#endif
