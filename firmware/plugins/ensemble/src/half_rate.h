// The half-rate path of the ensemble, as Clouds runs it: the 64-sample block is decimated 2:1 by Clouds' halfband
// filters, the core makes 32 wet samples at 22.05 kHz, they are interpolated back and mixed with the full-rate dry
// input (equal power, soft limit, src/halfband.h).
//
// coop copy of te-op1-cfw ports/fx/op1/half_rate.h: stereo only (coop runs every position in stereo), the soft-limit
// table const (no first-call build), the cycle counter from src/cycles.h, and one addition, Decimate's shared pass:
// when the block's two inputs are equal and the two decimators hold the same history, the right decimator's output
// and history would be the left's, so they're copied instead of computed (the same numbers, a decimator's cost less).
//
// Mix: Clouds' crossfade tables are 0.707 at their ends, so x sqrt 2: mix 0 is the dry signal, mix 1 the wet.
// MixSoftLimit counts the half-scale wet twice, so its side x sqrt 2 / 2. Q15.
//
// Restart: the first block after Stop (the plugin's init), or after a pause over 5 ms (the effect bypassed, the
// position silent), re-initialises the filters and passes the input through; the caller re-initialises its core
// when Begin returns false. The pause is measured from the END of the previous block (End), so a restart that itself
// is slow doesn't read as a pause at the next block.

#ifndef COOP_ENSEMBLE_HALF_RATE_H_
#define COOP_ENSEMBLE_HALF_RATE_H_

#include <cstdint>

#include "src/cycles.h"
#include "src/ensemble_tables.h"
#include "src/halfband.h"

namespace coop_ensemble {

const int32_t kFxBlockSize = 64;
const int32_t kHalfN = kFxBlockSize / 2;

// (a * b) >> 31, Rings' MulQ31Fast (te-op1-cfw ports/rings/src/fixed.h): the low x low term dropped.
inline int32_t MulQ31Fast(int32_t a, int32_t b) {
#ifdef __bfin__
  int32_t r;
  __asm__("A0 = %1.H * %2.H, A1 = %1.H * %2.L (M);\n\t"
          "A1 += %2.H * %1.L (M);\n\t"
          "A1 = A1 >>> 15;\n\t"
          "%0 = (A0 += A1);"
          : "=d"(r) : "d"(a), "d"(b) : "A0", "A1");
  return r;
#else
  int64_t a_hi = a >> 16, b_hi = b >> 16;
  int64_t a_lo = static_cast<uint32_t>(a) & 0xffff;
  int64_t b_lo = static_cast<uint32_t>(b) & 0xffff;
  int64_t a1 = (a_hi * b_lo + b_hi * a_lo) >> 15;
  int64_t r = a_hi * b_hi * 2 + a1;
  if (r > INT32_MAX) r = INT32_MAX;
  if (r < INT32_MIN) r = INT32_MIN;
  return static_cast<int32_t>(r);
#endif
}

// Linear interpolation in a Q-format table with `bits` index bits over [0, 1]: index in Q16.
inline int32_t InterpolateQ16(const int32_t* table, int32_t x_q16, int bits) {
  const int32_t scaled = x_q16 << bits;
  const int32_t i = scaled >> 16;
  const int32_t frac = (scaled & 0xffff) << 15;   // Q31
  return table[i] + MulQ31Fast(table[i + 1] - table[i], frac);
}

class HalfRate {
 public:
  // Once per block, first: false on a restart (the input stays as it is; the caller re-initialises its core and
  // returns).
  bool Begin(uint32_t now) {
    if (running_ && now - last_end_ > kGapCycles) running_ = false;
    last_end_ = now;   // until End
    if (running_) return true;
    for (int32_t c = 0; c < 2; ++c) {
      decimator_[c].Init();
      interpolator_[c].Init();
    }
    mix_ = 0;
    running_ = true;
    return false;
  }

  void Stop() { running_ = false; }

  // Once per block, last (on a restart too): the pause before the next block counts from here.
  void End() { last_end_ = Cycles(); }

  // The block's input at half rate: in(0), in(1). same: l and r hold the same 64 samples.
  void Decimate(const int16_t* l, const int16_t* r, bool same) {
    const bool shared = same && decimator_[1].SameHistory(decimator_[0]);
    decimator_[0].Process(l, half_[0], kHalfN);
    if (shared) {
      decimator_[1].CopyHistory(decimator_[0]);
      for (int32_t i = 0; i < kHalfN; ++i) half_[1][i] = half_[0][i];
    } else {
      decimator_[1].Process(r, half_[1], kHalfN);
    }
  }
  int16_t* in(int32_t c) { return half_[c]; }
  // The core writes its wet output here (at the input's scale).
  int16_t* wet(int32_t c) { return wet_half_[c]; }

  // The wet back to full rate and mixed into l and r, mix 0..32767, smoothed by a quarter per block (~6 ms).
  void Mix(int16_t* l, int16_t* r, int32_t mix) {
    for (int32_t c = 0; c < 2; ++c) {
      int16_t* w = wet_half_[c];
      for (int32_t i = 0; i < kHalfN; ++i) w[i] = static_cast<int16_t>(w[i] >> 1);   // half scale
      interpolator_[c].Process(w, wet_full_[c], kHalfN);
    }
    const int32_t target = (mix << 1) + (mix >> 14);   // Q16
    mix_ += (target - mix_) >> 2;
    const int16_t fade_out = FadeOut(mix_), fade_in = FadeIn(mix_);
    MixSoftLimit(l, wet_full_[0], kFxBlockSize, fade_out, fade_in, &kSoftLimit);
    MixSoftLimit(r, wet_full_[1], kFxBlockSize, fade_out, fade_in, &kSoftLimit);
  }

 private:
  static const uint32_t kGapCycles = 2000000;   // 5 ms at 400 MHz

  static int16_t FadeOut(int32_t mix_q16) {
    const int32_t f = InterpolateQ16(kXfadeOutQ31, mix_q16, 4) >> 16;
    const int32_t g = (f * 46341) >> 15;
    return static_cast<int16_t>(g > 32767 ? 32767 : g);
  }

  static int16_t FadeIn(int32_t mix_q16) {
    const int32_t f = InterpolateQ16(kXfadeInQ31, mix_q16, 4) >> 16;
    return static_cast<int16_t>((f * 23170) >> 15);
  }

  HalfbandDecimator decimator_[2];
  HalfbandInterpolator interpolator_[2];
  int16_t half_[2][kHalfN];
  int16_t wet_half_[2][kHalfN];
  int16_t wet_full_[2][kFxBlockSize];
  bool running_;
  uint32_t last_end_;
  int32_t mix_;   // Q16
};

}  // namespace coop_ensemble

#endif  // COOP_ENSEMBLE_HALF_RATE_H_
