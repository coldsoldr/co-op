/* e6 test effect: the declared effect controls (sdk/coop_plugin.h "Effect controls"), as the eleven kennel ports will
 * use them. Not a real plugin. Per block: gain = params[5] (Q15, 16384 = unity, so 0..2x), polarity = params[6]
 * (step 0 normal, 1 inverted), params[7] depth (read and clamped, not used), mute = bit 0 of params[8] (the latch
 * word). Everything is read and clamped once at the block start. The turns params[0..3] are ignored.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>
#include "coop_plugin.h"

static int init(int slot) { (void)slot; return 0; }

static void render(int slot, int32_t *l, int32_t *r, const int16_t *params, int n)
{
    (void)slot;
    int32_t g = params[5];                                 /* clamp what you read: a sound may hold anything */
    if (g < 0) g = 0; else if (g > 32767) g = 32767;
    int inv = params[6] == 1;
    int mute = (params[COOP_FX_LATCH] & 1) != 0;
    if (mute) g = 0;
    if (inv) g = -g;
    for (int i = 0; i < n; i++) {                          /* (x >> 16) * g * 2: |x >> 16| <= 32768, fits 32 bits */
        l[i] = (l[i] >> 16) * g * 2;
        r[i] = (r[i] >> 16) * g * 2;
    }
}

extern "C" const coop_fx_t FX_test_fx = { COOP_FX_MAGIC, COOP_PLUGIN_API, 0, init, render };
