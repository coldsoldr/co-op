/* freq_shift: the frequency shifter / ring modulator (after Mutable Instruments Warps, MIT) as a coop effect, ported
 * from te-op1-cfw's kennel plugin (ports/knl/freq_shift/shift_knl.cc 0.2, DSP ports/fx/src/shift, at 5aa314b).
 *
 * The DSP is upstream's (src/shift.*, src/shift_bfin.S unchanged but for a Hilbert pair per channel, src/shift.h).
 * This file replaces kennel's glue (common/knl_fx.h): the instance lives in this relocation's .bss (each of coop's
 * five relocations, one per slot, has its own, so per-slot state comes free) instead of op1_fx_scratch; the turns
 * are normalised here as op1_fx_param_q15 did, but from the ranges coop_plugin.h names (no range-table address); the
 * page-2 words are declared controls (plugin.json): gain on fn + blue (shift + fn + blue its fine, given by the core,
 * so kennel's knl::Fine and its shift-byte address are gone), the mode on the green click (a step index, so kennel's
 * ClickMode quantisation is gone). The screen (shift.svg, the "Dial") is not carried: effect screens are not built
 * (docs/plugins.md "Effect controls", Display); the stock filter page shows, the core shows gain and mode.
 *
 * Stereo at every slot: each channel through its own Hilbert pair and shifter (L at shift + spread, R at shift -
 * spread, each with its own barber-pole loop); an L == R block runs one pair (src/shift.h). The bus arrives as Q31
 * (the int16 bus widened by 16 bits): the DSP sees the high halves; a channel the DSP did not write (mix 0 at both
 * ends of the block) is left exactly as it came.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission; the DSP in src/ is MIT
 * (LICENSE-MIT). */
#include <stdint.h>
#include "coop_plugin.h"
#include "src/shift.h"

namespace {

op1fx::FreqShift g_shift;
int16_t g_l[SH_MAX_BLOCK], g_r[SH_MAX_BLOCK];
/* The words the mapped parameters were last computed from (turns 0..3, word 5), and the mapped values. A word is
 * int16; kNoWord is outside it, so the first render after init computes all of them. */
const int32_t kNoWord = 0x10000;
int32_t g_words[5];
int32_t g_mapped[5];   /* plain words: a global ShiftParams would need a static constructor (its gain initialiser) */

inline int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }

/* kennel's op1_fx_param_q15 over the filter type's ranges (sdk/coop_plugin.h: {64..14528}, {0..24576}, {0..22528},
 * {8192..32767}): (v - lo) x 32767 / (hi - lo), clamped. Constant divisors: the compiler multiplies. Per block. */
template <int32_t kLo, int32_t kHi>
inline int32_t KnobQ15(int32_t v) { return Clamp15((v - kLo) * 32767 / (kHi - kLo)); }

/* kennel's knl::GainQ14 (common/knl_fx.h): the input gain word as a Q14 factor, -6 dB at 0, 0 dB at the centre, +6 dB
 * at the top (10^(dB / 20) x 16384, one entry a dB, interpolated). Per block. */
int32_t GainQ14(int32_t word) {
    static const int32_t kTable[13] = { 8211, 9215, 10338, 11600, 13016, 14605, 16384, 18382, 20626, 23143, 25966,
                                        29135, 32690 };
    const int32_t pos = Clamp15(word) * 12;
    const int32_t i = pos / 32767;
    const int32_t frac = pos - i * 32767;
    if (i >= 12) return kTable[12];
    return kTable[i] + (((kTable[i + 1] - kTable[i]) * frac) / 32767);
}

int init(int slot)
{
    (void)slot;
    g_shift.Init();
    for (int i = 0; i < 5; i++) g_words[i] = kNoWord;  /* every mapped value is recomputed at the first render */
    return 0;
}

void render(int slot, int32_t *l, int32_t *r, const int16_t *params, int n)
{
    (void)slot;
    /* Read once for the block (docs/plugins.md "Effect controls"). bfin GCC divides by a constant through
     * __divsi3, so the mapped values are kept and recomputed only for a word that changed. */
    int32_t *m = g_mapped;
    if (params[0] != g_words[0]) { g_words[0] = params[0]; m[0] = KnobQ15<64, 14528>(params[0]); }   /* blue shift */
    if (params[1] != g_words[1]) { g_words[1] = params[1]; m[1] = KnobQ15<0, 24576>(params[1]); }    /* green fb */
    if (params[2] != g_words[2]) { g_words[2] = params[2]; m[2] = KnobQ15<0, 22528>(params[2]); }    /* white spread */
    if (params[3] != g_words[3]) { g_words[3] = params[3]; m[3] = KnobQ15<8192, 32767>(params[3]); } /* orange mix */
    if (params[5] != g_words[4]) { g_words[4] = params[5]; m[4] = GainQ14(params[5]); }   /* fn + blue gain, word 5 */
    op1fx::ShiftParams q;
    q.shift = m[0];                                    /* -2 kHz .. 0 .. +2 kHz, cubic */
    q.feedback = m[1];
    q.spread = m[2];
    q.mix = m[3];
    q.gain = m[4];
    q.ring = params[6] == 1;                           /* green click, word 6: step 0 shift, 1 ring */

    for (int off = 0; off + 2 <= n; ) {                /* the kernels take an even n, 2..64 (the core gives 64) */
        int k = n - off;
        if (k > SH_MAX_BLOCK) k = SH_MAX_BLOCK;
        k &= ~1;
        int32_t *pl = l + off, *pr = r + off;
        uint32_t diff = 0;
        for (int i = 0; i < k; i++) {
            const int16_t a = (int16_t)(pl[i] >> 16), b = (int16_t)(pr[i] >> 16);
            g_l[i] = a;
            g_r[i] = b;
            diff |= (uint32_t)(uint16_t)(a ^ b);
        }
        const uint32_t w = g_shift.Process(g_l, g_r, k, q, diff == 0);
        if (w & 1)
            for (int i = 0; i < k; i++) pl[i] = (int32_t)((uint32_t)(uint16_t)g_l[i] << 16);
        if (w & 2)
            for (int i = 0; i < k; i++) pr[i] = (int32_t)((uint32_t)(uint16_t)g_r[i] << 16);
        off += k;
    }
}

}  // namespace

#ifdef FREQ_SHIFT_TEST                                /* tests/host/freq_shift: the instance's counters */
extern "C" op1fx::FreqShift *freq_shift_instance() { return &g_shift; }
#endif

extern "C" const coop_fx_t FX_freq_shift = { COOP_FX_MAGIC, COOP_PLUGIN_API, 0, init, render };
