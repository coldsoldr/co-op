/* wavefolder: te-op1-cfw's FOLD (a sine wavefolder with first-order ADAA, ports/fx/src/fold at 5aa314b) as a coop
 * effect, ported from the kennel plugin ports/knl/wavefolder/fold_knl.cc. The DSP is src/ (fold.h, fold.cc and the
 * Blackfin kernels fold_bfin.S), unchanged apart from what coop needs (src/fold.h says what).
 *
 * Turns (params[0..3], in the filter type's range units: sdk/coop_plugin.h): blue fold, green symmetry, white tone,
 * orange mix, normalised to 0..32767 as kennel's op1_fx_param_q15 did. fn + blue: the input gain (word 5,
 * params[5], Q15: -6 dB at 0, 0 dB at 16384, +6 dB at the top, knl::GainQ14); shift + fn + blue is its fine step
 * (the core's, "fine": true). The gain scales the folder's gain only: the level make-up and the dry side of the mix
 * don't see it, so at mix 0 nothing changes.
 *
 * Stereo: the upstream stereo path (the master's render_stereo on kennel) at every position. When a block has L == R
 * and the two channel states are equal, the mono kernels run once and the result goes to both sides: bit-identical
 * to the stereo run, about a third cheaper. Mix 0 (settled) leaves the int32 bus untouched.
 *
 * State: one Fold per relocation (coop relocates the file once per slot, each with its own .bss).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>

#include "coop_plugin.h"
#include "src/fold.h"

namespace {

op1fx::Fold g_fold;
int32_t g_q[5];                             /* fold, symmetry, tone, mix (0..32767), gain (Q14); a FoldParams
                                               global would need a static constructor (its gain default) */
int16_t g_last[5];                          /* the raw words g_q was made from */
bool g_have;                                /* g_q valid */
alignas(4) int16_t g_l[64];                 /* the block as int16 (the bus is int16 widened by 16 bits); */
alignas(4) int16_t g_r[64];                 /* 4-byte aligned for the pair loads in src/bus_bfin.S */

/* The bus <-> int16 conversions, n even: in, xl = l >> 16, xr = r >> 16, 0 returned when xl == xr; out, l = xl << 16,
 * r = xr << 16. The Blackfin build runs src/bus_bfin.S (two samples a pass, parallel loads), the host these loops. */
#if defined(__bfin__)
extern "C" int32_t WfBusIn(const int32_t* l, const int32_t* r, int16_t* xl, int16_t* xr, int32_t n);
extern "C" void WfBusOut(const int16_t* xl, const int16_t* xr, int32_t* l, int32_t* r, int32_t n);
inline int32_t BusIn(const int32_t* l, const int32_t* r, int16_t* xl, int16_t* xr, int n) { return WfBusIn(l, r, xl, xr, n); }
inline void BusOut(const int16_t* xl, const int16_t* xr, int32_t* l, int32_t* r, int n) { WfBusOut(xl, xr, l, r, n); }
#else
int32_t BusIn(const int32_t* l, const int32_t* r, int16_t* xl, int16_t* xr, int n) {
  int32_t diff = 0;
  for (int i = 0; i < n; i++) {
    const int32_t a = l[i] >> 16, b = r[i] >> 16;
    xl[i] = (int16_t)a;
    xr[i] = (int16_t)b;
    diff |= a ^ b;
  }
  return diff;
}
void BusOut(const int16_t* xl, const int16_t* xr, int32_t* l, int32_t* r, int n) {
  for (int i = 0; i < n; i++) {
    l[i] = (int32_t)((uint32_t)(uint16_t)xl[i] << 16);
    r[i] = (int32_t)((uint32_t)(uint16_t)xr[i] << 16);
  }
}
#endif

/* The filter type's ranges (sdk/coop_plugin.h; plugin_host.cc fx_rng). */
const int32_t kLo[4] = { 64, 0, 0, 8192 };
const int32_t kSpan[4] = { 14528 - 64, 24576, 22528, 32767 - 8192 };

inline int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }

/* kennel's op1_fx_param_q15: a range word -> 0..32767. Per block, only when the word moved. */
inline int32_t Knob(int32_t v, int32_t k) { return Clamp15((v - kLo[k]) * 32767 / kSpan[k]); }

/* kennel's knl::GainQ14 (ports/knl/common/knl_fx.h): word 5 -> Q14, -6 dB at 0, 0 dB at 16384, +6 dB at the top
 * (10^(dB / 20) x 16384, a point a dB, interpolated). */
int32_t GainQ14(int32_t word) {
  static const int32_t kTable[13] = { 8211, 9215, 10338, 11600, 13016, 14605, 16384, 18382, 20626, 23143, 25966,
                                      29135, 32690 };
  const int32_t pos = Clamp15(word) * 12;
  const int32_t i = pos / 32767;
  const int32_t frac = pos - i * 32767;
  if (i >= 12) return kTable[12];
  return kTable[i] + (((kTable[i + 1] - kTable[i]) * frac) / 32767);
}

void UpdateParams(const int16_t* p) {
  const int16_t w[5] = { p[0], p[1], p[2], p[3], p[5] };
  if (g_have && w[0] == g_last[0] && w[1] == g_last[1] && w[2] == g_last[2] && w[3] == g_last[3] &&
      w[4] == g_last[4])
    return;
  for (int k = 0; k < 5; k++) g_last[k] = w[k];
  g_q[0] = Knob(w[0], 0);
  g_q[1] = Knob(w[1], 1);
  g_q[2] = Knob(w[2], 2);
  g_q[3] = Knob(w[3], 3);
  g_q[4] = GainQ14(w[4]);
  g_have = true;
}

int init(int slot) {
  (void)slot;
  g_fold.Init();
  g_have = false;
  return 0;
}

void render(int slot, int32_t* l, int32_t* r, const int16_t* params, int n) {
  (void)slot;
  UpdateParams(params);                     /* read once at the block start */
  op1fx::FoldParams q;
  q.fold = g_q[0];
  q.symmetry = g_q[1];
  q.tone = g_q[2];
  q.mix = g_q[3];
  q.gain = g_q[4];
  while (n >= 2) {
    const int m = n > 64 ? 64 : (n & ~1);
    const int32_t diff = BusIn(l, r, g_l, g_r, m);
    if (diff == 0 && g_fold.ChannelsEqual()) {
      const bool wrote = g_fold.Process(g_l, nullptr, m, q);
      g_fold.MirrorChannels();
      if (wrote) BusOut(g_l, g_l, l, r, m);
    } else if (g_fold.Process(g_l, g_r, m, q)) {
      BusOut(g_l, g_r, l, r, m);
    }
    l += m;
    r += m;
    n -= m;
  }
  /* an odd last sample (coop always passes 64) passes dry */
}

}  // namespace

extern "C" const coop_fx_t FX_wavefolder = { COOP_FX_MAGIC, COOP_PLUGIN_API, 0, init, render };
