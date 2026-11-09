/* coop-fw FX slot: a coop stereo insert effect (Phase 2, item 3 "two FX slots").
 *
 * This is the first coop FX and the model for the plugin FX ABI (Phase 4): it processes a stereo
 * Q31 block in place and keeps its state (here a delay line) in a caller-provided buffer, so the
 * firmware wrapper can place that buffer in free SDRAM region B (0x02800000) while the host tests
 * give it an ordinary array. Fixed-point only, no float, no libc, no per-sample divide; the two
 * per-sample multiplies are the verified fractional MAC (coop_mul_q31). A second master FX slot runs
 * this in series after the stock master FX and before the master suite (docs/audio-graph.md).
 *
 * The effect is a feedback delay (echo): a classic insert that exercises a large region-B buffer and
 * per-sample read/modify/write. It is transparent when mix == 0 (early return), so the slot can be
 * present but silent, as milestone A's suite was, before any effect is enabled.
 *
 * STEREO CONTRACT (docs/fx-abi.md): every FX slot is stereo in / stereo out, placed after the
 * per-voice pan so stereo placement survives the chain. coop FX are stereo-native (this delay runs L
 * and R independently). Hosted mono effects are run dual-mono (twice, independent per-channel state).
 * This delay already satisfies the porting rules as a stereo-native effect.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef COOP_FX_H
#define COOP_FX_H
#include <stdint.h>
#include "coop_fixed.h"

/* Samples of the delay line zeroed per block while the line is being cleared, so enabling the effect
 * never clears the whole (region-B) buffer in one block and spikes a single block over budget
 * (the kennel fx_spread idea). The line stays bypassed until fully cleared. */
#define COOP_FX_CLEAR_CHUNK 512

typedef struct {
    int32_t *lineL, *lineR;   /* delay lines, `len` samples each (caller-owned; region B on device) */
    int32_t len;              /* line length in samples (max delay) */
    int32_t delay;            /* current delay in samples, 1..len */
    int32_t fb;               /* feedback, Q31 (|fb| < 1 for stability) */
    int32_t mix;              /* wet level added to the dry signal, Q31; 0 = transparent bypass */
    int32_t pos;              /* write head */
    int32_t cleared;          /* incremental-clear progress, 0..len */
} coop_delay;

/* Bind the lines and set a safe default: transparent (mix = 0), 1/8-second delay, modest feedback.
 * The line is marked uncleared so the first enabled block starts the spread clear. */
static inline void coop_delay_init(coop_delay *d, int32_t *lineL, int32_t *lineR, int32_t len)
{
    d->lineL = lineL; d->lineR = lineR; d->len = len;
    d->delay = len > 1 ? len / 2 : 1;
    d->fb = 0; d->mix = 0; d->pos = 0; d->cleared = 0;
}

/* Set the musical parameters. delay is clamped to [1, len]; fb/mix are Q31. */
static inline void coop_delay_set(coop_delay *d, int32_t delay, int32_t fb_q31, int32_t mix_q31)
{
    if (delay < 1) delay = 1;
    if (delay > d->len) delay = d->len;
    d->delay = delay; d->fb = fb_q31; d->mix = mix_q31;
}

/* Process a stereo Q31 block in place: out = x + mix * delayed; line[pos] = x + fb * delayed. */
static inline void coop_delay_process(coop_delay *d, int32_t *L, int32_t *R, int n)
{
    if (d->mix == 0) return;                      /* transparent bypass */
    if (d->cleared < d->len) {                    /* spread the one-time line clear over blocks */
        int32_t end = d->cleared + COOP_FX_CLEAR_CHUNK;
        if (end > d->len) end = d->len;
        for (int32_t i = d->cleared; i < end; i++) { d->lineL[i] = 0; d->lineR[i] = 0; }
        d->cleared = end;
        return;                                   /* stay dry until the line is clear */
    }
    int32_t pos = d->pos, dl = d->delay, len = d->len, fb = d->fb, mix = d->mix;
    for (int i = 0; i < n; i++) {
        int32_t rp = pos - dl; if (rp < 0) rp += len;
        int32_t dL = d->lineL[rp], dR = d->lineR[rp];
        d->lineL[pos] = coop_add32(L[i], coop_mul_q31(dL, fb));
        d->lineR[pos] = coop_add32(R[i], coop_mul_q31(dR, fb));
        L[i] = coop_add32(L[i], coop_mul_q31(dL, mix));
        R[i] = coop_add32(R[i], coop_mul_q31(dR, mix));
        pos++; if (pos >= len) pos = 0;
    }
    d->pos = pos;
}

#endif
