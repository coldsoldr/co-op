/* ENSEMBLE (chorus -> string ensemble) as a coop effect: te-op1-cfw's kennel effect (ports/knl/ensemble/
 * ensemble_knl.cc, version 0.2) on the coop effect ABI (sdk/coop_plugin.h), stereo at every position.
 *
 * The DSP is te-op1-cfw's, copied into src/ (ports/fx/src/ensemble: the core at 22.05 kHz; ports/clouds/src/core
 * halfband: the 2:1 decimation, the interpolation back and the soft-limited mix; ports/fx/op1/half_rate.h: the path
 * that joins them), with the float-built tables as integers (gen_tables.py). tests/host/ensemble checks this file's
 * render against the upstream glue bit for bit.
 *
 * Controls (plugin.json, README.md): blue rate, green depth, white voices, orange mix on the four turns (params[0..3],
 * in the filter type's range units: normalised here as kennel's op1_fx_param_q15 did); fn + blue the input gain
 * (params[5], the kennel page-2 word 5: -6 dB at 0, 0 dB at 16384, +6 dB at 32767; shift + fn + blue is its fine).
 * The gain scales the wet path's input only (the decimated signal before the core): at mix 0 it changes nothing.
 *
 * Kennel -> coop: the render is stereo at every position (kennel's synth and drum positions ran it mono: one swept
 * copy set, the left side only); the instance is in the slot's position buffer as on kennel (e8: coop_fx_scratch,
 * plugin.json "scratch"; the file is relocated once, its .bss holds the five buffer pointers, init resets it all); the
 * fine step on the gain is the core's (kennel's knl::Fine read the shift byte at a firmware address); the screen is
 * the stock filter page until effect screens exist (README.md has the brief).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. The DSP in src/ follows
 * Mutable Instruments' Rings, Plaits and Clouds (Emilie Gillet, MIT): LICENSE-MIT. */
#include <stdint.h>

#include "coop_plugin.h"
#include "src/cycles.h"
#include "src/ensemble.h"
#include "src/half_rate.h"

namespace coop_ensemble {

namespace {

/* The filter type's ranges for the four turns (sdk/coop_plugin.h): what the core hands in params[0..3]. */
const int32_t kRangeLo[4] = { 64, 0, 0, 8192 };
const int32_t kRangeHi[4] = { 14528, 24576, 22528, 32767 };

struct Instance {
  Ensemble ensemble;
  HalfRate half;
  int16_t raw[5];        /* params[0..3] and params[5] the knobs below came from */
  bool raw_valid;
  int32_t knob[4];       /* the four turns, 0..32767 */
  int32_t gain_q14;      /* the input gain, Q14 */
};

#ifdef COOP_FX_SCRATCH_BYTES
static_assert(sizeof(Instance) <= COOP_FX_SCRATCH_BYTES, "plugin.json scratch: the state must fit");
#endif
Instance* g_inst[COOP_FX_SLOTS];   /* .bss: each slot's state, in its position buffer (bound by init) */

inline int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }

inline int16_t Sat16(int32_t x) { return static_cast<int16_t>(x > 32767 ? 32767 : (x < -32768 ? -32768 : x)); }

/* An input gain word as a Q14 factor: -6 dB at 0, 0 dB at the centre, +6 dB at the top (10^(dB / 20) x 16384, one
 * table entry per dB, interpolated). te-op1-cfw ports/knl/common/knl_fx.h GainQ14, unchanged. */
int32_t GainQ14(int32_t word) {
  static const int32_t kTable[13] = { 8211, 9215, 10338, 11600, 13016, 14605, 16384, 18382, 20626, 23143, 25966,
                                      29135, 32690 };
  const int32_t pos = Clamp15(word) * 12;      /* 0 .. 12 x 32767 */
  const int32_t i = pos / 32767;
  const int32_t frac = pos - i * 32767;
  if (i >= 12) return kTable[12];
  return kTable[i] + (((kTable[i + 1] - kTable[i]) * frac) / 32767);
}

/* The knobs and the gain from the block's params, recomputed only when a word moved (the divisions stay out of a
 * steady block). Kennel's op1_fx_param_q15: (v - lo) x 32767 / (hi - lo), clamped. */
void ReadParams(Instance* in, const int16_t* p) {
  const int16_t w[5] = { p[0], p[1], p[2], p[3], p[5] };
  if (in->raw_valid && w[0] == in->raw[0] && w[1] == in->raw[1] && w[2] == in->raw[2] && w[3] == in->raw[3] &&
      w[4] == in->raw[4])
    return;
  for (int32_t k = 0; k < 4; ++k) {
    in->knob[k] = Clamp15((static_cast<int32_t>(w[k]) - kRangeLo[k]) * 32767 / (kRangeHi[k] - kRangeLo[k]));
    in->raw[k] = w[k];
  }
  in->gain_q14 = GainQ14(w[4]);
  in->raw[4] = w[4];
  in->raw_valid = true;
}

/* The input gain on the wet path: the decimated input, in place. */
void Gain(int16_t* x, int32_t gain_q14) {
  if (gain_q14 == 16384) return;
  for (int32_t i = 0; i < kHalfN; ++i) x[i] = Sat16((x[i] * gain_q14) >> 14);
}

int Init(int slot) {
  if (slot < 0 || slot >= COOP_FX_SLOTS) return -1;
  uint32_t bytes = 0;
  Instance* g = static_cast<Instance*>(coop_fx_scratch(slot, &bytes));
  g_inst[slot] = (g && bytes >= sizeof(Instance)) ? g : nullptr;
  if (!g_inst[slot]) return -1;
  g->half.Stop();        /* the first render restarts: filters and core cleared there, the input passed through */
  g->raw_valid = false;
  return 0;
}

void Render(int slot, int32_t* l, int32_t* r, const int16_t* params, int n) {
  if (slot < 0 || slot >= COOP_FX_SLOTS || !g_inst[slot]) return;   /* not bound: dry */
  if (n != kFxBlockSize) return;                      /* the core always hands 64; anything else passes through */
  Instance* in = g_inst[slot];
  if (!in->half.Begin(Cycles())) {                    /* a restart: the input passes through */
    in->ensemble.Init();
    in->half.End();
    return;
  }
  ReadParams(in, params);
  int16_t xl[kFxBlockSize], xr[kFxBlockSize];
  int32_t diff = 0;
  for (int32_t i = 0; i < kFxBlockSize; ++i) {         /* Q31 (the int16 bus widened by 16 bits) -> int16 */
    xl[i] = static_cast<int16_t>(l[i] >> 16);
    xr[i] = static_cast<int16_t>(r[i] >> 16);
    diff |= xl[i] ^ xr[i];
  }
  EnsembleParams q;
  q.rate = in->knob[0];
  q.depth = in->knob[1];
  q.voices = in->knob[2];
  in->half.Decimate(xl, xr, diff == 0);
  Gain(in->half.in(0), in->gain_q14);
  Gain(in->half.in(1), in->gain_q14);
  in->ensemble.Process(in->half.in(0), in->half.in(1), in->half.wet(0), in->half.wet(1), kHalfN, q);
  in->half.Mix(xl, xr, in->knob[3]);
  for (int32_t i = 0; i < kFxBlockSize; ++i) {
    l[i] = static_cast<int32_t>(xl[i]) << 16;
    r[i] = static_cast<int32_t>(xr[i]) << 16;
  }
  in->half.End();
}

}  // namespace

}  // namespace coop_ensemble

extern "C" const coop_fx_t FX_ensemble = { COOP_FX_MAGIC, COOP_PLUGIN_API, 0, coop_ensemble::Init,
                                           coop_ensemble::Render };
