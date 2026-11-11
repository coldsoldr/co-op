/* cs_plate: coldsoldr's plate reverb (Dattorro's figure-of-eight plate, te-op1-cfw ports/fx/src/plate) as a coop
 * effect. The kennel build (te-op1-cfw ports/knl/cs_plate/plate_knl.cc, 9ced415, never released) ran the same core
 * behind fx/op1/half_rate.h; this is that glue on coop's ABI (sdk/coop_plugin.h), true stereo at every position.
 *
 *   turns (params[0..3], the filter type's range units, mapped to Q15 as kennel's op1_fx_param_q15 did):
 *     blue decay, green damp, white pre-delay, orange mix
 *   fn + blue   size (params[5]: step 0..7, the plate's 8 tank lengths)
 *   fn + green  diffusion (params[6], Q15; shift + fn + green fine)
 *   fn + white  motion (params[7], Q15; shift + fn + white fine)
 *   orange click freeze (params[8] bit 0, a latch: not saved)
 *   fixed: low cut 3277 (kennel's choice: its orange press couldn't be a page knob)
 *
 * Memory: the plate and the half-rate path (~76 KB) in this relocation's .bss, one per position. init only arms a
 * clear: the next renders zero the state 8 KB a block (10 blocks, ~15 ms, dry passed through), then reset it and run
 * (guide/plugin-guidelines.md §2.3: no 76 KB clear on the audio thread in one go).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. The half-rate kernels and tables
 * come from te-op1-cfw's Clouds port (MIT, LICENSE-MIT). */
#include <stdint.h>
#include "coop_plugin.h"
#include "half_rate.h"
#include "plate.h"

namespace {

using namespace csplate;

const int32_t kLowCut = 3277;                          /* kennel's fixed low cut */
const int32_t kClearChunk = 2048;                      /* words a block: 8 KB (~2.2 k cycles in the GNU sim) */

struct Instance {
    PlateReverb plate;
    HalfRate half;
};
static_assert(sizeof(Instance) % 4 == 0, "cleared in words");
const int32_t kWords = sizeof(Instance) / 4;

/* Plain storage (trivial types: no constructor, zeroed by the loader). One per relocation, so one per position. */
Instance g_in;
int16_t g_l[kFxBlockSize], g_r[kFxBlockSize];
int32_t g_cleared;                                     /* words zeroed since init; kWords + 1 = running */

/* The four turns' ranges (sdk/coop_plugin.h): what kennel's op1_fx_param_q15 read from the firmware's table. */
const int16_t kLo[4] = { 64, 0, 0, 8192 };
const int16_t kHi[4] = { 14528, 24576, 22528, 32767 };

inline int32_t Clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* op1_fx_param_q15 with the constant ranges: (v - lo) x 32767 / (hi - lo), clamped. Four divides a block. */
inline int32_t Turn(const int16_t* p, int k) {
    return Clamp((p[k] - kLo[k]) * 32767 / (kHi[k] - kLo[k]), 0, 32767);
}

int init(int slot) {
    (void)slot;
    g_cleared = 0;
    return 0;
}

void render(int slot, int32_t* l, int32_t* r, const int16_t* params, int n) {
    (void)slot;
    if (n != kFxBlockSize) return;                     /* coop's block is 64 (§2.2); anything else passes dry */
    if (g_cleared < kWords) {                          /* restarting: clear a chunk, the input passes */
        uint32_t* w = reinterpret_cast<uint32_t*>(&g_in) + g_cleared;
        const int32_t m = kWords - g_cleared < kClearChunk ? kWords - g_cleared : kClearChunk;
        for (int32_t i = 0; i < m; ++i) w[i] = 0;
        g_cleared += m;
        return;
    }
    if (g_cleared == kWords) {                         /* cleared: the start state (HalfRate's is all zero) */
        g_in.plate.Reset();
        g_cleared = kWords + 1;
    }

    PlateParams q;                                     /* read once a block, clamped (docs/plugins.md e6) */
    q.decay = Turn(params, 0);
    q.damp = Turn(params, 1);
    q.predelay = Turn(params, 2);
    const int32_t mix = Turn(params, 3);
    q.size = Clamp(params[5], 0, kPlateNumSizes - 1);
    q.diffusion = Clamp(params[6], 0, 32767);
    q.motion = Clamp(params[7], 0, 32767);
    q.lowcut = kLowCut;
    q.freeze = (params[COOP_FX_LATCH] & 1) != 0;

    for (int32_t i = 0; i < kFxBlockSize; ++i) {       /* the bus is int16 << 16 */
        g_l[i] = static_cast<int16_t>(l[i] >> 16);
        g_r[i] = static_cast<int16_t>(r[i] >> 16);
    }
    HalfRate& h = g_in.half;
    h.Decimate(g_l, g_r);
    g_in.plate.Process(h.in(0), h.in(1), h.wet(0), h.wet(1), kHalfN, q);
    h.Mix(g_l, g_r, mix);
    for (int32_t i = 0; i < kFxBlockSize; ++i) {
        l[i] = static_cast<int32_t>(g_l[i]) << 16;
        r[i] = static_cast<int32_t>(g_r[i]) << 16;
    }
}

}  // namespace

extern "C" const coop_fx_t FX_cs_plate = { COOP_FX_MAGIC, COOP_PLUGIN_API, 0, init, render };
