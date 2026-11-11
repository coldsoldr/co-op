/* bitcrusher: te-op1-cfw's kennel bitcrusher (ports/knl/bitcrusher/crush_knl.cc, coldsoldr, 5aa314b) as a coop
 * effect. The DSP is src/crush.h (upstream's ports/fx/src/crush/crush.h); its stereo path runs at every position
 * (guidelines §2.2: one clock, a held sample per channel).
 *
 * Controls (plugins/bitcrusher/README.md):
 *   turns params[0..3]: blue rate, green bits, white jitter, orange mix, in the filter type's range units
 *     (sdk/coop_plugin.h). Normalised to Q15 as kennel's op1_fx_param_q15 did ((v - lo) * 32767 / (hi - lo),
 *     clamped). Blue and green turn the stock way, clockwise = more (a higher rate, more bits): the core's rate and
 *     bits are the amount of crushing, so both are the knob mirrored (32767 - v), as upstream.
 *   params[5]: the input gain (fn + blue, shift + fn + blue fine; Q15, 16384 = 0 dB, -6..+6 dB), kennel's blue page
 *     word 5. Drives the crusher only.
 * Everything is read once a block. One relocation per slot (five, each its own .bss), so one state per file: the
 * slot argument is not needed.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>
#include "coop_plugin.h"
#include "src/crush.h"

namespace {

crush::Crush g_crush;   /* plain data in .bss: no constructor runs */

/* The filter type's ranges (sdk/coop_plugin.h; plugin_host.cc fx_rng; the device table, docs/device-tests.md e6
 * probe `filter_ranges`). */
const int32_t kLo[4] = { 64, 0, 0, 8192 };
const int32_t kHi[4] = { 14528, 24576, 22528, 32767 };

inline int32_t Q15(const int16_t* p, int i) {
  int32_t v = (p[i] - kLo[i]) * 32767 / (kHi[i] - kLo[i]);   /* per block, four divisions */
  return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}

int init(int slot) {
  (void)slot;
  g_crush.Init();
  return 0;
}

void render(int slot, int32_t* l, int32_t* r, const int16_t* params, int n) {
  (void)slot;
  crush::Params q;
  q.rate = 32767 - Q15(params, 0);   /* clockwise = a higher rate */
  q.bits = 32767 - Q15(params, 1);   /* clockwise = more bits */
  q.jitter = Q15(params, 2);
  q.mix = Q15(params, 3);
  q.gain_q14 = crush::GainQ14(params[5]);   /* clamped inside */
  g_crush.Process(l, r, n, q);
}

}  // namespace

extern "C" const coop_fx_t FX_bitcrusher = { COOP_FX_MAGIC, COOP_PLUGIN_API, 0, init, render };
