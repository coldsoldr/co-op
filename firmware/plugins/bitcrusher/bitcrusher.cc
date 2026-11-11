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
 * Everything is read once a block.
 * Memory (e8): the state lives in the slot's position buffer (sdk/coop_plugin.h coop_fx_scratch, plugin.json
 * "scratch"); the file is relocated once and its .bss holds only the five buffer pointers. init(slot) binds the slot's
 * buffer and resets the state there (the buffer holds whatever the effect before left).
 * Screen (e9): bitcrusher_screen.cc, bitcrusher.svg; the render publishes the input level in the state (state.h).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>
#include "coop_plugin.h"
#include "state.h"

extern "C" const coop_fx_screen_t SCREEN_bitcrusher;   /* bitcrusher_screen.cc (e9) */

namespace {

typedef coop_bitcrusher::State Instance;
#ifdef COOP_FX_SCRATCH_BYTES
static_assert(sizeof(Instance) <= COOP_FX_SCRATCH_BYTES, "plugin.json scratch: the state must fit");
#endif
Instance* g_inst[COOP_FX_SLOTS];   /* .bss: each slot's state, in its position buffer (bound by init) */

/* The filter type's ranges (sdk/coop_plugin.h; plugin_host.cc fx_rng; the device table, docs/device-tests.md e6
 * probe `filter_ranges`). */
const int32_t kLo[4] = { 64, 0, 0, 8192 };
const int32_t kHi[4] = { 14528, 24576, 22528, 32767 };

inline int32_t Q15(const int16_t* p, int i) {
  int32_t v = (p[i] - kLo[i]) * 32767 / (kHi[i] - kLo[i]);   /* per block, four divisions */
  return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}

int init(int slot) {
  if (slot < 0 || slot >= COOP_FX_SLOTS) return -1;
  uint32_t bytes = 0;
  Instance* g = static_cast<Instance*>(coop_fx_scratch(slot, &bytes));
  g_inst[slot] = (g && bytes >= sizeof(Instance)) ? g : nullptr;
  if (!g_inst[slot]) return -1;
  g->crush.Init();
  g->disp.level = 0;
  return 0;
}

void render(int slot, int32_t* l, int32_t* r, const int16_t* params, int n) {
  if (slot < 0 || slot >= COOP_FX_SLOTS || !g_inst[slot]) return;   /* not bound: dry */
  crush::Params q;
  q.rate = 32767 - Q15(params, 0);   /* clockwise = a higher rate */
  q.bits = 32767 - Q15(params, 1);   /* clockwise = more bits */
  q.jitter = Q15(params, 2);
  q.mix = Q15(params, 3);
  q.gain_q14 = crush::GainQ14(params[5]);   /* clamped inside */
  Instance* g = g_inst[slot];
  /* e9: the input level for the screen (kennel's meter: the block's peak of the top 16 bits, decaying by an eighth a
   * block), published as one word after the block; the DSP never reads it */
  int32_t peak = 0;
  for (int i = 0; i < n; ++i) {
    int32_t a = l[i] >> 16, b = r[i] >> 16;
    a = a < 0 ? -a : a; b = b < 0 ? -b : b;
    if (a > peak) peak = a;
    if (b > peak) peak = b;
  }
  g->crush.Process(l, r, n, q);
  const int32_t held = g->disp.level - (g->disp.level >> 3);
  g->disp.level = peak > held ? (peak > 32767 ? 32767 : peak) : held;
}

}  // namespace

extern "C" const coop_fx_t FX_bitcrusher = { COOP_FX_MAGIC, COOP_PLUGIN_API, COOP_FX_F_SCREEN, init, render, &SCREEN_bitcrusher };
