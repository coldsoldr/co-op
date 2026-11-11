/* drive: the oversampled soft-clip overdrive (te-op1-cfw ports/fx/src/drive, the kennel plugin ports/knl/drive) as a
 * coop effect, true stereo at every position. The DSP is upstream's packed-stereo path (src/drive_dsp.cc, the six
 * passes in src/drive_bfin.S, L in the low halves and R in the high, one dual MAC for both channels), unchanged in
 * its arithmetic; this file is the glue kennel's drive_knl.cc was:
 *   - the four turns: blue drive, green tone (centre flat), white bias, orange mix. They arrive in the filter type's
 *     range units (sdk/coop_plugin.h) and are normalised exactly as kennel's op1_fx_param_q15 did (the same ranges:
 *     the filter type's row);
 *   - fn + blue: the input gain (word 5, Q15, 16384 = 0 dB, -6..+6 dB, kennel's blue page-2 word): GainQ14 below,
 *     kennel common/knl_fx.h's, into DriveParams::gain. It scales the clipper's gain only: the dry side of the mix
 *     and the auto-level don't see it. Its fine step is the core's (shift + fn + blue, "fine": true); kennel's
 *     knl::Fine, which read the stock shift byte at a firmware address, is gone.
 * State: one Drive per relocation (coop relocates the file once per position, each with its own .bss): 1.2 KB, plus
 * the block's int16 copies. init only resets it (a few hundred stores).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission; the halfband's coefficients are
 * Clouds' (MIT, LICENSE-MIT). */
#include <stdint.h>
#include "coop_plugin.h"
#include "src/drive_dsp.h"

namespace {

op1fx::Drive g_drive;                                      /* trivially constructible: .bss, no constructor */
int16_t g_l[op1fx::kDriveMaxBlock], g_r[op1fx::kDriveMaxBlock];

/* The filter type's knob ranges (the core's fx_rng, plugin_host.cc; kennel read the same row, 0x01DE5C50 + 5 x 0x30) */
const int16_t kRange[4][2] = { { 64, 14528 }, { 0, 24576 }, { 0, 22528 }, { 8192, 32767 } };

inline int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }

/* kennel's op1_fx_param_q15: a knob word in range units -> 0..32767 */
inline int32_t KnobQ15(const int16_t* p, int i) {
  const int32_t lo = kRange[i][0], hi = kRange[i][1];
  return Clamp15((p[i] - lo) * 32767 / (hi - lo));
}

/* kennel common/knl_fx.h GainQ14: the gain word as a Q14 factor, -6 dB at 0, 0 dB at the centre, +6 dB at the top
 * (10^(dB / 20) x 16384, one table entry per dB, interpolated). Once a block. */
int32_t GainQ14(int32_t word) {
  static const int32_t kTable[13] = { 8211, 9215, 10338, 11600, 13016, 14605, 16384, 18382, 20626, 23143, 25966, 29135, 32690 };
  const int32_t pos = Clamp15(word) * 12;                  /* 0 .. 12 x 32767 */
  const int32_t i = pos / 32767;
  const int32_t frac = pos - i * 32767;
  if (i >= 12) return kTable[12];
  return kTable[i] + (((kTable[i + 1] - kTable[i]) * frac) / 32767);
}

/* The last block's words and what they gave: a still block skips the six divides (a cache of the inputs, cleared by
 * init; not a copy of the saved word) */
int16_t g_words[5];
op1fx::DriveParams g_q;
bool g_have;

int init(int slot) {
  (void)slot;
  g_drive.Init();
  g_have = false;
  return 0;
}

void render(int slot, int32_t* l, int32_t* r, const int16_t* params, int n) {
  (void)slot;
  const int16_t w0 = params[0], w1 = params[1], w2 = params[2], w3 = params[3], w5 = params[5];   /* once a block */
  if (!g_have || w0 != g_words[0] || w1 != g_words[1] || w2 != g_words[2] || w3 != g_words[3] || w5 != g_words[4]) {
    g_words[0] = w0; g_words[1] = w1; g_words[2] = w2; g_words[3] = w3; g_words[4] = w5;
    g_q.drive = KnobQ15(g_words, 0);
    g_q.tone = KnobQ15(g_words, 1);
    g_q.bias = KnobQ15(g_words, 2);
    g_q.mix = KnobQ15(g_words, 3);
    g_q.gain = GainQ14(w5);                                /* clamped in GainQ14 (a sound may hold anything) */
    g_have = true;
  }
  const op1fx::DriveParams q = g_q;
  for (int at = 0; at + 2 <= n;) {                         /* coop hands 64; any even run of 2..64 works */
    int m = n - at;
    if (m > op1fx::kDriveMaxBlock) m = op1fx::kDriveMaxBlock;
    m &= ~1;
    int32_t* bl = l + at;
    int32_t* br = r + at;
    for (int i = 0; i < m; i++) {                          /* the bus is int16 widened by 16 bits */
      g_l[i] = static_cast<int16_t>(bl[i] >> 16);
      g_r[i] = static_cast<int16_t>(br[i] >> 16);
    }
    g_drive.Process(g_l, g_r, m, q);
    for (int i = 0; i < m; i++) {
      bl[i] = static_cast<int32_t>(g_l[i]) * 65536;
      br[i] = static_cast<int32_t>(g_r[i]) * 65536;
    }
    at += m;
  }
}

}  // namespace

extern "C" const coop_fx_t FX_drive = { COOP_FX_MAGIC, COOP_PLUGIN_API, 0, init, render };
