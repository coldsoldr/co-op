/* coop-fw master bus suite: compressor -> drive -> EQ, on the stereo master pair (int32/Q31).
 *
 * The master pair is 0xff800e34 (L) / 0xff800f34 (R), 64 x int32 Q31 (docs/audio-graph.md). The suite
 * runs comp first (so the drive's character is level-independent), then drive, then EQ. Fixed-point,
 * no FPU, no libc, no per-sample divides on the hot path (the one divide is in the compressor's gain
 * and can be replaced by a reciprocal table; kept explicit here for the host-tested reference).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef COOP_MASTER_H
#define COOP_MASTER_H
#include <stdint.h>
#include "coop_fixed.h"

#define COOP_EQ_FRAC 28   /* biquad coefficients are Q28 (a1 can reach ~2) */

/* ---- soft-clip drive ---- */
/* Unity-small-signal soft clip after a pre-gain. drive_q31 in [0,1): 0 = clean bypass, higher = more
 * pre-gain + saturation. pre-gain (Q29) = 1 + 3*drive. The shaper y = g - g^3/3 has derivative 1 at 0
 * (so no level jump) and saturates; g is clamped to [-1,1] first (outside that the cubic folds back). */
static inline int32_t coop_drive_sample(int32_t x, int32_t drive_q31)
{
    if (drive_q31 <= 0) return x;                               /* clean bypass */
    int32_t pre = (1 << 29) + 3 * (drive_q31 >> 2);             /* Q29: 1 + 3*drive */
    int32_t g = coop_mul_qn(x, pre, 29);
    if (g > 2147483647 - 1) g = 2147483647;                     /* clamp to [-1,1] before the cubic */
    if (g < -2147483647) g = -2147483647;
    int32_t g3 = coop_mul_q31(coop_mul_q31(g, g), g);
    return coop_sat32((int64_t)g - g3 / 3);                     /* g - g^3/3 */
}

/* ---- peak compressor (per channel) ---- */
typedef struct { int32_t env; } coop_comp_state;

/* thresh_q31: level above which gain is reduced. ratio_inv_q31: 1/ratio (e.g. 1/4 for 4:1).
 * makeup_qn / makeup_frac: output make-up gain. Attack/release as right-shift smoothing of the peak
 * envelope (atk_shift fast, rel_shift slow). Feed-forward peak detection. */
static inline int32_t coop_comp_sample(coop_comp_state *st, int32_t x, int32_t thresh_q31,
                                       int32_t ratio_inv_q31, int32_t makeup, int makeup_frac,
                                       int atk_shift, int rel_shift)
{
    int32_t a = coop_abs32(x);
    if (a > st->env) st->env += (a - st->env) >> atk_shift;        /* attack */
    else             st->env -= (st->env - a) >> rel_shift;        /* release */
    int32_t gained = x;
    if (st->env > thresh_q31 && st->env > 0) {
        /* target level = thresh + (env-thresh)*ratio_inv ; gain = target/env */
        int64_t over = (int64_t)(st->env - thresh_q31);
        int64_t target = (int64_t)thresh_q31 + ((over * ratio_inv_q31) >> 31);
        int32_t gain_q31 = (int32_t)((target << 31) / st->env);     /* <= 1.0 */
        gained = coop_mul_q31(x, gain_q31);
    }
    return coop_mul_qn(gained, makeup, makeup_frac);
}

/* ---- biquad EQ (transposed direct form II, per channel) ---- */
typedef struct { int32_t s1, s2; } coop_biquad_state;

/* coef (Q COOP_EQ_FRAC) * sample (Q31) -> Q31, as int64 (coef can exceed 1, so the product can
 * exceed full-scale before the state sums settle). The fractional MAC gives (coef*x) >> 31; shifting
 * back up by 31-COOP_EQ_FRAC restores the COOP_EQ_FRAC scaling. |coef| must stay < 2^(31-FRAC) = 8. */
static inline int64_t coop_eq_prod(int32_t coef, int32_t x)
{
    return (int64_t)coop_mul_fr(coef, x) << (31 - COOP_EQ_FRAC);
}

static inline int32_t coop_biquad_sample(coop_biquad_state *st, int32_t x,
                                         int32_t b0, int32_t b1, int32_t b2, int32_t a1, int32_t a2)
{
    int64_t y = coop_eq_prod(b0, x) + st->s1;
    int32_t yo = coop_sat32(y);
    st->s1 = (int32_t)(coop_eq_prod(b1, x) - coop_eq_prod(a1, yo) + st->s2);
    st->s2 = (int32_t)(coop_eq_prod(b2, x) - coop_eq_prod(a2, yo));
    return yo;
}

/* ---- the suite ---- */
typedef struct {
    int32_t comp_thresh, comp_ratio_inv, comp_makeup;
    int comp_makeup_frac, comp_atk_shift, comp_rel_shift;
    int32_t drive;
    int32_t b0, b1, b2, a1, a2;        /* EQ, Q28 */
    coop_comp_state cL, cR;
    coop_biquad_state eqL, eqR;
} coop_master;

/* identity-ish defaults: no compression (threshold at full scale), no drive, bypass EQ (b0=1). */
static inline void coop_master_init(coop_master *m)
{
    m->comp_thresh = 2147483647; m->comp_ratio_inv = 2147483647; m->comp_makeup = 1 << 20;
    m->comp_makeup_frac = 20; m->comp_atk_shift = 2; m->comp_rel_shift = 7;
    m->drive = 0;
    m->b0 = 1 << COOP_EQ_FRAC; m->b1 = m->b2 = m->a1 = m->a2 = 0;
    m->cL.env = m->cR.env = 0; m->eqL.s1 = m->eqL.s2 = 0; m->eqR.s1 = m->eqR.s2 = 0;
}

/* process a stereo block in place: comp -> drive -> EQ, per channel */
static inline void coop_master_process(coop_master *m, int32_t *L, int32_t *R, int n)
{
    for (int i = 0; i < n; i++) {
        int32_t l = coop_comp_sample(&m->cL, L[i], m->comp_thresh, m->comp_ratio_inv,
                                     m->comp_makeup, m->comp_makeup_frac, m->comp_atk_shift, m->comp_rel_shift);
        int32_t r = coop_comp_sample(&m->cR, R[i], m->comp_thresh, m->comp_ratio_inv,
                                     m->comp_makeup, m->comp_makeup_frac, m->comp_atk_shift, m->comp_rel_shift);
        l = coop_drive_sample(l, m->drive);
        r = coop_drive_sample(r, m->drive);
        L[i] = coop_biquad_sample(&m->eqL, l, m->b0, m->b1, m->b2, m->a1, m->a2);
        R[i] = coop_biquad_sample(&m->eqR, r, m->b0, m->b1, m->b2, m->a1, m->a2);
    }
}

#endif
