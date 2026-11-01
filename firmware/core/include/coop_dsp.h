/* coop-fw DSP stages (Q15). Copyright (C) 2026 coldsoldr. GPL-3.0-or-later. */
#ifndef COOP_DSP_H
#define COOP_DSP_H
#include <stdint.h>
#include "coop_fixed.h"

/* Pan a mono block to stereo. pan in 0..32767: 0 = hard left, 16384 = centre, 32767 = hard right.
 * v1 law: linear (gainR = pan, gainL = 32767 - pan). Simple and cache-free; a constant-power law is a
 * later refinement (noted in docs/phase2.md). Centre is ~-6 dB per side (linear-pan dip). */
static inline void coop_pan_mono_to_stereo(const int16_t *in, int16_t *outL, int16_t *outR,
                                           int16_t pan, int n)
{
    if (pan < 0) pan = 0;
    int16_t gR = pan;
    int16_t gL = (int16_t)(32767 - pan);
    for (int i = 0; i < n; i++) {
        outL[i] = coop_mul_q15(in[i], gL);
        outR[i] = coop_mul_q15(in[i], gR);
    }
}

#endif
