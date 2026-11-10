/*
 * coop-fw v1: the 44.1 kHz voice (docs/engines.md "The 44.1 kHz voice"). An engine with COOP_ENGINE_F_44K renders
 * 64 samples at 44.1 kHz into out[0..63]; the stock voice loop wants 128 at 88.2 kHz, so the dispatcher
 * (engine_host.cc coop_eng_render) calls coop_voice_44k_to_88k right after the render: every sample twice, in place.
 *
 * Why the plain repeat is enough: the voice goes on through the stock envelope and gain and coop's voice hook at
 * 88.2 kHz, then through the half-band decimator back to 44.1 kHz (coop_hb_decimate2: 32 taps on the even phase,
 * one tap of 0.5 on the odd). Repeat-then-decimate is a linear time-invariant filter at 44.1 kHz, so it makes no
 * image or alias at all; its response is the decimator's times cos(pi f / 88,200): -0.14 dB at 5 kHz, -0.56 dB at
 * 10 kHz, -2.4 dB at 20 kHz (the envelope, which changes slowly, aside) [I: derived; op1emu tone gate in the doc].
 * An engine that interpolates 44.1 -> 88.2 itself (rings' 4-point stage: most of the ~1.6 k a render its README
 * gives for the 2x stage and the copies) buys a flatter top octave; this costs ~0.2 k.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_cost.h"

volatile uint32_t g_eng_44k_renders;                                     /* stats: renders that went through here */

/* out[2i] = out[2i + 1] = out[i], i = 63 .. 0 (backwards: no sample is overwritten before it is read). The voice
 * buffer is the stock's 0xFF80113C (L1, 4-aligned): a hardware loop of one load, one PACK and one 32-bit store per
 * input sample (the load of the next sample issued with the PACK: 2 instructions a sample, ~140 cycles a render;
 * the C loop compiled to a branch loop of ~10 cycles a sample). Any other alignment takes the 16-bit stores. */
extern "C" COOP_HOT_VOICE void coop_voice_44k_to_88k(int16_t *out)
{
    g_eng_44k_renders++;
    if (!((uint32_t)(uintptr_t)out & 3u)) {
        const int16_t *src = out + 63;
        uint32_t *dst = (uint32_t *)(void *)out + 63;
        int32_t s, w;
        __asm__ __volatile__(
            "%2 = W[%0--] (X);\n\t"                     /* out[63] */
            "LSETUP (1f, 2f) LC1 = %4;\n"
            "1: %3 = PACK(%2.L, %2.L) || %2 = W[%0--] (X);\n"   /* out[i] twice; load out[i - 1] */
            "2: [%1--] = %3;\n\t"
            "%3 = PACK(%2.L, %2.L);\n\t"                /* out[0] */
            "[%1] = %3;"
            : "+a"(src), "+a"(dst), "=&d"(s), "=&d"(w)
            : "a"(63)
            : "LC1", "LT1", "LB1", "memory");
        return;
    }
    for (int i = 63; i >= 0; i--) { int16_t s = out[i]; out[2 * i + 1] = s; out[2 * i] = s; }
}
