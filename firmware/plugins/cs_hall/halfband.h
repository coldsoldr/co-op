// 2:1 decimation and interpolation, so the hall runs at half the OP-1's
// effect rate (22.05 kHz), and the output mix at the full rate.
//
// coop port (plugins/cs_hall): the parts of te-op1-cfw
// ports/clouds/src/core/halfband.h the hall's half-rate path uses, with
// Rings' soft-limit table and Clouds' crossfade tables as constant data
// (halfband.cc; the upstream built its soft-limit table at run time). The
// kernels are halfband_bfin.S, unchanged; their C references are host only.
//
// The filter is a 19-tap halfband FIR (Blackman-windowed sinc, DC gain 1):
// only the odd taps around the centre (0.5) are non-zero. int16 samples,
// Q15 taps. The mix is Clouds' output stage (dry x fade + wet x fade, soft
// limit of half the sum) with the soft limit from a Q15 table, at unity.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware
// permission (the Clouds and Rings DSP behind it: Emilie Gillet, MIT).

#ifndef COOP_CS_HALL_HALFBAND_H_
#define COOP_CS_HALL_HALFBAND_H_

#include <cstdint>

namespace op1clouds {

// h[+-1], h[+-3], ... h[+-9] in Q15 (they sum to 0.25 = 8192).
const int16_t kHalfbandTaps[5] = { 10019, -2397, 710, -151, 11 };
const int32_t kHalfbandMaxIn = 64;   // input samples per call

// 2 x upstream's SoftLimit over [0, 4] as int16 output samples, clipped
// (257 points), then the steps between them (256). halfband_bfin.S reads
// step at +514 bytes.
struct SoftLimitTable {
  int16_t value[257];
  int16_t step[256];
};
extern const SoftLimitTable kSoftLimit;
extern const int32_t kXfadeInQ31[18];    // lut_xfade_in, Q31, + 1 guard
extern const int32_t kXfadeOutQ31[18];   // lut_xfade_out, Q31, + 1 guard

extern "C" void HalfbandDecimateAsm(const int16_t* x, int16_t* out, int32_t n);
extern "C" void HalfbandInterpolateAsm(const int16_t* x, int16_t* out, int32_t n);
// gains = fade_out | wet_gain << 16.
extern "C" void MixSoftLimitAsm(int16_t* io, const int16_t* wet, int32_t n, int32_t gains,
                                const SoftLimitTable* t);

#if !defined(__bfin__)
// x: 18 samples of history then 2n new ones; n outputs.
void HalfbandDecimateC(const int16_t* x, int16_t* out, int32_t n);
// x: 9 samples of history then n new ones; 2n outputs.
void HalfbandInterpolateC(const int16_t* x, int16_t* out, int32_t n);
// io = 2 x SoftLimit(((io x fade_out) >> 4) + ((wet x wet_gain) >> 2)).
void MixSoftLimitC(int16_t* io, const int16_t* wet, int32_t n, int32_t fade_out, int32_t wet_gain,
                   const SoftLimitTable* t);
#endif

inline void HalfbandDecimate(const int16_t* x, int16_t* out, int32_t n) {
#if defined(__bfin__)
  HalfbandDecimateAsm(x, out, n);
#else
  HalfbandDecimateC(x, out, n);
#endif
}

inline void HalfbandInterpolate(const int16_t* x, int16_t* out, int32_t n) {
#if defined(__bfin__)
  HalfbandInterpolateAsm(x, out, n);
#else
  HalfbandInterpolateC(x, out, n);
#endif
}

inline void MixSoftLimit(int16_t* io, const int16_t* wet, int32_t n, int32_t fade_out,
                         int32_t wet_gain, const SoftLimitTable* t) {
#if defined(__bfin__)
  MixSoftLimitAsm(io, wet, n, (fade_out & 0xffff) | (wet_gain << 16), t);
#else
  MixSoftLimitC(io, wet, n, fade_out, wet_gain, t);
#endif
}

class HalfbandDecimator {
 public:
  void Init() {
    for (int32_t i = 0; i < kHistory; ++i) x_[i] = 0;
  }

  // 2n samples in, n samples out (out may be in).
  void Process(const int16_t* in, int16_t* out, int32_t n) {
    for (int32_t i = 0; i < 2 * n; ++i) x_[kHistory + i] = in[i];
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

// stmlib's (a x b) >> 31 the Blackfin way (te-op1-cfw ports/rings/src/fixed.h
// MulQ31Fast): the low x low term is dropped.
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

// Linear interpolation in a Q-format table with `bits` index bits over
// [0, 1]: index in Q16 (te-op1-cfw ports/clouds/src/core/dsp_fixed.h).
inline int32_t InterpolateQ16(const int32_t* table, int32_t x_q16, int bits) {
  const int32_t scaled = x_q16 << bits;
  const int32_t i = scaled >> 16;
  const int32_t frac = (scaled & 0xffff) << 15;
  return table[i] + MulQ31Fast(table[i + 1] - table[i], frac);
}

}  // namespace op1clouds

#endif  // COOP_CS_HALL_HALFBAND_H_
