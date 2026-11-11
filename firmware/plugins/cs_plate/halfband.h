// 2:1 decimation and interpolation, to run the plate at half the effect
// rate, and the output mix at the full rate: te-op1-cfw's Clouds port
// (ports/clouds/src/core/halfband.h, 5aa314b), in namespace csplate, with
// the soft-limit table as a literal (kSoftLimit, halfband.cc) instead of
// one built at the first call from Rings' table.
//
// The filter is a 19-tap halfband FIR (Blackman-windowed sinc, DC gain 1):
// only the odd taps around the centre (0.5) are non-zero. int16 samples,
// Q15 taps: every product is one 16 x 16 multiply. The mix is Clouds'
// output stage (dry x fade + wet x 1.2 x fade, soft limit of half the sum)
// with the soft limit from a Q15 table, at unity gain.
//
// Each kernel has a C reference (host only) and a Blackfin version
// (halfband_bfin.S, bit-exact, te-op1-cfw ports/clouds/tests/test_kernels.cc).

#ifndef CS_PLATE_HALFBAND_H_
#define CS_PLATE_HALFBAND_H_

#include <stdint.h>

namespace csplate {

// h[+-1], h[+-3], ... h[+-9] in Q15 (they sum to 0.25 = 8192).
const int16_t kHalfbandTaps[5] = { 10019, -2397, 710, -151, 11 };
const int32_t kHalfbandMaxIn = 64;   // input samples per call

// 2 x Clouds' SoftLimit over [0, 4] as int16 output samples, clipped (257
// points), then the steps between them (256). halfband_bfin.S reads it at
// these offsets.
struct SoftLimitTable {
  int16_t value[257];
  int16_t step[256];
};
extern const SoftLimitTable kSoftLimit;

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

}  // namespace csplate

#endif  // CS_PLATE_HALFBAND_H_
