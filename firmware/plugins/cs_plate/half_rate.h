// The half-rate path of the plate: te-op1-cfw ports/fx/op1/half_rate.h
// (9ced415), as Clouds runs. The 64-sample block is decimated 2:1 by
// Clouds' halfband filters, the plate makes 32 wet samples at 22.05 kHz,
// they are interpolated back and mixed with the full-rate dry input (equal
// power, soft limit, halfband.h).
//
// Mix: Clouds' crossfade tables are 0.707 at their ends, so x sqrt 2: mix 0
// is the dry signal, mix 1 the wet. MixSoftLimit counts the half-scale wet
// twice, so its side x sqrt 2 / 2. Q15.
//
// coop port: kennel's restart on a pause (Begin / End with the cycle
// counter, a 5 ms gap) is gone: coop calls init whenever a position
// (re)starts the effect (selected, a sound change, synth <-> drum), and the
// glue restarts there (cs_plate.cc). The soft-limit table is a literal (no
// build at the first call), and the state starts zeroed (the glue's clear).

#ifndef CS_PLATE_HALF_RATE_H_
#define CS_PLATE_HALF_RATE_H_

#include <stdint.h>

#include "halfband.h"

namespace csplate {

const int32_t kFxBlockSize = 64;
const int32_t kHalfN = kFxBlockSize / 2;

// Clouds' lut_xfade_in / lut_xfade_out, Q31, + 1 guard (te-op1-cfw
// ports/clouds/src/core/clouds_tables.cc).
const int32_t kXfadeInQ31[18] = {
  0, 107247096, 261074382, 412182391, 558997225, 699989703, 833691286, 958709378,
  1073741824, 1177590480, 1269173687, 1347537541, 1411865826, 1461488517, 1495888758, 1514708246,
  1518500250, 1518500250,
};
const int32_t kXfadeOutQ31[18] = {
  1518500250, 1514708246, 1495888758, 1461488517, 1411865826, 1347537541, 1269173687, 1177590480,
  1073741824, 958709378, 833691286, 699989703, 558997225, 412182391, 261074382, 107247096,
  0, 0,
};

// Rings' MulQ31Fast (ports/rings/src/fixed.h): (a x b) >> 31 without the
// low x low product, saturated; the same bits on the host.
inline int32_t MulQ31Fast(int32_t a, int32_t b) {
#if defined(__bfin__)
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

// Clouds' InterpolateQ16 (ports/clouds/src/core/dsp_fixed.h).
inline int32_t InterpolateQ16(const int32_t* table, int32_t x_q16, int bits) {
  const int32_t scaled = x_q16 << bits;
  const int32_t i = scaled >> 16;
  const int32_t frac = (scaled & 0xffff) << 15;
  return table[i] + MulQ31Fast(table[i + 1] - table[i], frac);
}

class HalfRate {
 public:
  // The state after a zero fill (the glue's clear) is the start state; this
  // does the same for a used object (tests).
  void Init() {
    for (int32_t c = 0; c < 2; ++c) {
      decimator_[c].Init();
      interpolator_[c].Init();
    }
    mix_ = 0;
  }

  // The block's input at half rate: in(0), in(1).
  void Decimate(const int16_t* l, const int16_t* r) {
    decimator_[0].Process(l, half_[0], kHalfN);
    decimator_[1].Process(r, half_[1], kHalfN);
  }
  const int16_t* in(int32_t c) const { return half_[c]; }
  // The plate writes its wet output here (at the input's scale).
  int16_t* wet(int32_t c) { return wet_half_[c]; }

  // The wet back to full rate and mixed into l and r, mix 0..32767,
  // smoothed by a quarter per block (~6 ms).
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
  int32_t mix_;   // Q16
};

}  // namespace csplate

#endif  // CS_PLATE_HALF_RATE_H_
