// The half-rate path of the tape echo, from te-op1-cfw ports/fx/op1/half_rate.h and ports/clouds/src/core/halfband.h
// (5aa314b; Clouds' output stage, Mutable Instruments, MIT, ../LICENSE-MIT): the 64-sample call is decimated 2:1 by
// a 19-tap halfband FIR, the core makes 32 wet samples at 22.05 kHz, they are interpolated back and mixed with the
// full-rate dry input (equal power crossfade, soft limit).
//
// What changed for coop: one stereo instance; the soft-limit table is a const table (src/tape_echo_tables.h, the
// numbers InitSoftLimitTable built) rather than built in the audio thread; the crossfade tables are copied in; the
// pause-restart takes the clock from the caller (Begin(now), End(now)). Each kernel has a C reference (halfband.cc,
// host builds) and a Blackfin version (halfband_bfin.S, bit-exact).
//
// Mix: Clouds' crossfade tables are 0.707 at their ends, so x sqrt 2: mix 0 is the dry signal, mix 1 the wet.
// MixSoftLimit counts the half-scale wet twice, so its side x sqrt 2 / 2. Q15.
//
// Restart: the first call after Stop, or after a pause over 5 ms (measured from the end of the previous call),
// re-initialises the filters and passes the input through; the caller then restarts its core.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#ifndef COOP_TAPE_ECHO_HALF_RATE_H_
#define COOP_TAPE_ECHO_HALF_RATE_H_

#include <stdint.h>

#include "src/tape_echo_tables.h"

namespace op1clouds {

const int32_t kFxBlockSize = 64;
const int32_t kHalfN = kFxBlockSize / 2;

// h[+-1], h[+-3], ... h[+-9] in Q15 (they sum to 0.25 = 8192).
const int16_t kHalfbandTaps[5] = { 10019, -2397, 710, -151, 11 };
const int32_t kHalfbandMaxIn = 64;

// 2 x stmlib SoftLimit over [0, 4] as int16 output samples, clipped (257 points), then the steps (256). The layout
// the mix kernel reads: value at 0, step at +514 bytes.
struct SoftLimitTable {
  int16_t value[257];
  int16_t step[256];
};
extern const SoftLimitTable kSoftLimit;

// Clouds' lut_xfade_in / lut_xfade_out, Q31, + 1 guard.
extern const int32_t kXfadeInQ31[18];
extern const int32_t kXfadeOutQ31[18];

#if !defined(__bfin__) || defined(COOP_TAPE_ECHO_C)
void HalfbandDecimateC(const int16_t* x, int16_t* out, int32_t n);
void HalfbandInterpolateC(const int16_t* x, int16_t* out, int32_t n);
void MixSoftLimitC(int16_t* io, const int16_t* wet, int32_t n, int32_t fade_out, int32_t wet_gain,
                   const SoftLimitTable* t);
#endif
extern "C" void HalfbandDecimateAsm(const int16_t* x, int16_t* out, int32_t n);
extern "C" void HalfbandInterpolateAsm(const int16_t* x, int16_t* out, int32_t n);
extern "C" void MixSoftLimitAsm(int16_t* io, const int16_t* wet, int32_t n, int32_t gains, const SoftLimitTable* t);

inline void HalfbandDecimate(const int16_t* x, int16_t* out, int32_t n) {
#if defined(__bfin__) && !defined(COOP_TAPE_ECHO_C)
  HalfbandDecimateAsm(x, out, n);
#else
  HalfbandDecimateC(x, out, n);
#endif
}

inline void HalfbandInterpolate(const int16_t* x, int16_t* out, int32_t n) {
#if defined(__bfin__) && !defined(COOP_TAPE_ECHO_C)
  HalfbandInterpolateAsm(x, out, n);
#else
  HalfbandInterpolateC(x, out, n);
#endif
}

inline void MixSoftLimit(int16_t* io, const int16_t* wet, int32_t n, int32_t fade_out, int32_t wet_gain,
                         const SoftLimitTable* t) {
#if defined(__bfin__) && !defined(COOP_TAPE_ECHO_C)
  MixSoftLimitAsm(io, wet, n, (fade_out & 0xffff) | (wet_gain << 16), t);
#else
  MixSoftLimitC(io, wet, n, fade_out, wet_gain, t);
#endif
}

// rings' MulQ31Fast: (a * b) >> 31 from three 16 x 16 products (the low x low term dropped), saturated.
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

// Linear interpolation in a Q31 table with `bits` index bits over [0, 1]: index in Q16.
inline int32_t InterpolateQ16(const int32_t* table, int32_t x_q16, int bits) {
  const int32_t scaled = x_q16 << bits;
  const int32_t i = scaled >> 16;
  const int32_t frac = (scaled & 0xffff) << 15;
  return table[i] + MulQ31Fast(table[i + 1] - table[i], frac);
}

class HalfbandDecimator {
 public:
  void Init() {
    for (int32_t i = 0; i < kHistory; ++i) x_[i] = 0;
  }
  // The 2n new input samples go to in() first; n samples out.
  int16_t* in() { return x_ + kHistory; }
  void Process(int16_t* out, int32_t n) {
    HalfbandDecimate(x_, out, n);
    for (int32_t i = 0; i < kHistory; ++i) x_[i] = x_[2 * n + i];
  }

 private:
  static const int32_t kHistory = 18;
  int16_t x_[kHistory + kHalfbandMaxIn];
};

class HalfbandInterpolator {
 public:
  void Init() {
    for (int32_t i = 0; i < kHistory; ++i) x_[i] = 0;
  }
  // n samples in, 2n samples out.
  void Process(const int16_t* in, int16_t* out, int32_t n) {
    for (int32_t i = 0; i < n; ++i) x_[kHistory + i] = in[i];
    HalfbandInterpolate(x_, out, n);
    for (int32_t i = 0; i < kHistory; ++i) x_[i] = x_[n + i];
  }

 private:
  static const int32_t kHistory = 9;
  int16_t x_[kHistory + kHalfbandMaxIn / 2];
};

// The stereo half-rate path: Begin, then the input in in_full(c), Decimate, the core reads in(c) and writes wet(c),
// Mix writes the result over the dry in dry(c), End.
class HalfRate {
 public:
  // Once per call, first: false on a restart (the filters start again; the caller passes the input through and
  // restarts its core).
  bool Begin(uint32_t now) {
    if (running_ && now - last_end_ > kGapCycles) running_ = false;
    last_end_ = now;
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

  // Once per call, last (on a restart too): the pause before the next call counts from here.
  void End(uint32_t now) { last_end_ = now; }

  // Where the call's 64 new input samples go (the decimator's own buffer: no copy).
  int16_t* in_full(int32_t c) { return decimator_[c].in(); }
  void Decimate() {
    decimator_[0].Process(half_[0], kHalfN);
    decimator_[1].Process(half_[1], kHalfN);
  }
  const int16_t* in(int32_t c) const { return half_[c]; }
  int16_t* wet(int32_t c) { return wet_half_[c]; }

  // The wet back to full rate and mixed into l and r (the dry, in place), mix 0..32767, smoothed by a quarter per
  // call (~6 ms).
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

}  // namespace op1clouds

#endif  // COOP_TAPE_ECHO_HALF_RATE_H_
