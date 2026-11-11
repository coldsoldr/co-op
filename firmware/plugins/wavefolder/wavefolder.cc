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
 * State (e8): one Fold and the block's buffers in the slot's position buffer (sdk/coop_plugin.h coop_fx_scratch,
 * plugin.json "scratch"); the file is relocated once and its .bss holds only the five buffer pointers. init binds the
 * slot's buffer and resets the state (the buffer holds whatever the effect before left).
 * Screen (e9): wavefolder_screen.cc, wavefolder.svg; it reads the turns and the gain word only (no display data).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>

#include "coop_plugin.h"
#include "src/fold.h"

extern "C" const coop_fx_screen_t SCREEN_wavefolder;   /* wavefolder_screen.cc (e9) */

namespace {

struct Instance {
  op1fx::Fold fold;
  int32_t q[5];                             /* fold, symmetry, tone, mix (0..32767), gain (Q14); a FoldParams
                                               would need a constructor (its gain default) */
  int16_t last[5];                          /* the raw words q was made from */
  bool have;                                /* q valid */
  alignas(4) int16_t l[64];                 /* the block as int16 (the bus is int16 widened by 16 bits); */
  alignas(4) int16_t r[64];                 /* 4-byte aligned for the pair loads in src/bus_bfin.S */
};
#ifdef COOP_FX_SCRATCH_BYTES
static_assert(sizeof(Instance) <= COOP_FX_SCRATCH_BYTES, "plugin.json scratch: the state must fit");
#endif
Instance* g_inst[COOP_FX_SLOTS];            /* .bss: each slot's state, in its position buffer (bound by init) */

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

void UpdateParams(Instance& g, const int16_t* p) {
  const int16_t w[5] = { p[0], p[1], p[2], p[3], p[5] };
  if (g.have && w[0] == g.last[0] && w[1] == g.last[1] && w[2] == g.last[2] && w[3] == g.last[3] &&
      w[4] == g.last[4])
    return;
  for (int k = 0; k < 5; k++) g.last[k] = w[k];
  g.q[0] = Knob(w[0], 0);
  g.q[1] = Knob(w[1], 1);
  g.q[2] = Knob(w[2], 2);
  g.q[3] = Knob(w[3], 3);
  g.q[4] = GainQ14(w[4]);
  g.have = true;
}

int init(int slot) {
  if (slot < 0 || slot >= COOP_FX_SLOTS) return -1;
  uint32_t bytes = 0;
  Instance* p = static_cast<Instance*>(coop_fx_scratch(slot, &bytes));
  g_inst[slot] = (p && bytes >= sizeof(Instance)) ? p : nullptr;
  if (!g_inst[slot]) return -1;
  Instance& g = *p;
  g.fold.Init();
  g.have = false;
  return 0;
}

void render(int slot, int32_t* l, int32_t* r, const int16_t* params, int n) {
  if (slot < 0 || slot >= COOP_FX_SLOTS || !g_inst[slot]) return;   /* not bound: dry */
  Instance& g = *g_inst[slot];
  UpdateParams(g, params);                     /* read once at the block start */
  op1fx::FoldParams q;
  q.fold = g.q[0];
  q.symmetry = g.q[1];
  q.tone = g.q[2];
  q.mix = g.q[3];
  q.gain = g.q[4];
  while (n >= 2) {
    const int m = n > 64 ? 64 : (n & ~1);
    const int32_t diff = BusIn(l, r, g.l, g.r, m);
    if (diff == 0 && g.fold.ChannelsEqual()) {
      const bool wrote = g.fold.Process(g.l, nullptr, m, q);
      g.fold.MirrorChannels();
      if (wrote) BusOut(g.l, g.l, l, r, m);
    } else if (g.fold.Process(g.l, g.r, m, q)) {
      BusOut(g.l, g.r, l, r, m);
    }
    l += m;
    r += m;
    n -= m;
  }
  /* an odd last sample (coop always passes 64) passes dry */
}

}  // namespace

extern "C" const coop_fx_t FX_wavefolder = { COOP_FX_MAGIC, COOP_PLUGIN_API, COOP_FX_F_SCREEN, init, render, &SCREEN_wavefolder };
