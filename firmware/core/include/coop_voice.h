/* coop-fw stereo instrument bus (Phase 2 milestone C, item 2): per-voice equal-power panning into a
 * stereo bus, plus the voice-spread control (random per-voice pan scaled by an amount).
 *
 * The stock voice path sums every voice into a mono bus (0xff800bb4) and then applies a single global
 * pan while downmixing to the master (docs/stereo-bus.md). To get true stereo we pan each voice as it
 * is mixed, into a stereo bus. This header is the pure, host-testable DSP; the L1 takeover that feeds
 * it the stock per-voice render output is the next step (milestone C1).
 *
 * Fixed-point only (Q15 pan/gain), no float, no libc. Pan is equal-power (constant perceived loudness
 * across the field): at centre both gains are ~0.707, so a centred mix matches the mono sum in power.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef COOP_VOICE_H
#define COOP_VOICE_H
#include <stdint.h>
#include "coop_fixed.h"

#define COOP_MAX_VOICES 12        /* 6 synth + up to 12 drum; sized for the larger */

/* quarter-wave sine table, Q15: coop_sin_q15[k] = sin(k * pi/128), k = 0..64 (0 -> 0, 64 -> ~1.0). */
static const int16_t coop_sin_q15[65] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,
};

typedef struct { int16_t gL, gR; } coop_pan;   /* Q15 channel gains */

/* Equal-power pan gains for pan in Q15 [-1,1] (-1 = hard left, 0 = centre, +1 = hard right).
 * gR = sin(u*pi/2), gL = cos(u*pi/2) with u = (pan+1)/2, from the quarter-sine table, interpolated. */
static inline coop_pan coop_pan_gains(int16_t pan_q15)
{
    int32_t u = (int32_t)pan_q15 + 32768;        /* 0..65535 */
    int32_t pos = u >> 10;                        /* 0..63 (64 only at the very top) */
    int32_t frac = u & 1023;                      /* 0..1023 */
    if (pos > 63) { pos = 63; frac = 1023; }
    int32_t gR = coop_sin_q15[pos] + ((coop_sin_q15[pos + 1] - coop_sin_q15[pos]) * frac >> 10);
    int32_t hi = 64 - pos;                         /* cos(u*pi/2) = sin((1-u)*pi/2) */
    int32_t gL = coop_sin_q15[hi] + ((coop_sin_q15[hi - 1] - coop_sin_q15[hi]) * frac >> 10);
    coop_pan p; p.gL = (int16_t)gL; p.gR = (int16_t)gR;
    return p;
}

/* ---- per-voice spread ---- */
typedef struct {
    int16_t pan[COOP_MAX_VOICES];    /* current pan per voice, Q15 */
    int16_t spread;                  /* 0 = all centred, 32767 = full field */
    uint32_t rng;                    /* deterministic PRNG state */
} coop_spread;

static inline uint32_t coop_xorshift(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x1u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

static inline void coop_spread_init(coop_spread *s, int16_t spread_q15)
{
    for (int i = 0; i < COOP_MAX_VOICES; i++) s->pan[i] = 0;
    s->spread = spread_q15 < 0 ? 0 : spread_q15;
    s->rng = 0x9E3779B9u;
}

static inline void coop_spread_set(coop_spread *s, int16_t spread_q15)
{
    s->spread = spread_q15 < 0 ? 0 : spread_q15;
}

/* Assign a voice a fresh pan at note-on: a random position in [-1,1] scaled by the spread amount.
 * spread 0 -> always centre; spread full -> anywhere hard-left to hard-right. Deterministic for a
 * given RNG seed (no per-note jitter once assigned). */
static inline void coop_spread_note_on(coop_spread *s, int voice)
{
    if (voice < 0 || voice >= COOP_MAX_VOICES) return;
    int32_t r = (int32_t)(coop_xorshift(&s->rng) & 0xffff) - 32768;   /* [-32768, 32767] */
    s->pan[voice] = (int16_t)(((int32_t)r * s->spread) >> 15);        /* scale by spread */
}

/* ---- stereo mix ---- */
/* Mix one mono voice (int16 samples) into the stereo int32 bus (accumulate), applying the voice gain
 * (Q15) and the pan gains (Q15). n samples. The bus is int32 so several voices sum without clipping;
 * the decimate stage saturates to int16 at the master. */
static inline void coop_voice_mix(int32_t *busL, int32_t *busR, const int16_t *v, int n,
                                  coop_pan pan, int16_t gain)
{
    int32_t cl = ((int32_t)gain * pan.gL) >> 15;    /* combined left gain, Q15 */
    int32_t cr = ((int32_t)gain * pan.gR) >> 15;    /* combined right gain, Q15 */
    for (int i = 0; i < n; i++) {
        busL[i] += ((int32_t)v[i] * cl) >> 15;
        busR[i] += ((int32_t)v[i] * cr) >> 15;
    }
}

#endif
