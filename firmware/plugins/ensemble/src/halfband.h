// 2:1 decimation and interpolation, to run the ensemble at half the OP-1's effect rate (22050 Hz), and the output mix
// at the full rate. coop copy of te-op1-cfw ports/clouds/src/core/halfband.h, cut to what the ensemble uses: the
// soft-limit table is a const table (src/ensemble_tables.cc) instead of one built at the first call, the kernels'
// Blackfin symbols are prefixed Ens, and the C references are built for the host only (the device runs the assembly).
//
// The filter is a 19-tap halfband FIR (Blackman-windowed sinc, DC gain 1): only the odd taps around the centre (0.5)
// are non-zero. int16 samples, Q15 taps: every product is one 16 x 16 multiply. The mix is Clouds' output stage
// (dry x fade + wet x 1.2 x fade, soft limit of half the sum) with the soft limit from a Q15 table, at unity gain.

#ifndef COOP_ENSEMBLE_HALFBAND_H_
#define COOP_ENSEMBLE_HALFBAND_H_

#include <cstdint>

#include "src/ensemble_tables.h"

namespace coop_ensemble {

// h[+-1], h[+-3], ... h[+-9] in Q15 (they sum to 0.25 = 8192).
const int16_t kHalfbandTaps[5] = { 10019, -2397, 710, -151, 11 };
const int32_t kHalfbandMaxIn = 64;   // input samples per call

#ifndef __bfin__
// x: 18 samples of history then 2n new ones; n outputs. Output k is centred on x[2k + 10].
void HalfbandDecimateC(const int16_t* x, int16_t* out, int32_t n);
// x: 9 samples of history then n new ones; 2n outputs.
void HalfbandInterpolateC(const int16_t* x, int16_t* out, int32_t n);
// io = 2 x SoftLimit(((io x fade_out) >> 4) + ((wet x wet_gain) >> 2)), half the sum in Q27; fades in Q15 (wet_gain
// includes the 0.6), wet at half scale.
void MixSoftLimitC(int16_t* io, const int16_t* wet, int32_t n, int32_t fade_out, int32_t wet_gain,
                   const SoftLimitTable* t);
#endif

extern "C" void EnsHalfbandDecimateAsm(const int16_t* x, int16_t* out, int32_t n);
extern "C" void EnsHalfbandInterpolateAsm(const int16_t* x, int16_t* out, int32_t n);
// gains = fade_out | wet_gain << 16.
extern "C" void EnsMixSoftLimitAsm(int16_t* io, const int16_t* wet, int32_t n, int32_t gains,
                                   const SoftLimitTable* t);

inline void HalfbandDecimate(const int16_t* x, int16_t* out, int32_t n) {
#if defined(__bfin__)
  EnsHalfbandDecimateAsm(x, out, n);
#else
  HalfbandDecimateC(x, out, n);
#endif
}

inline void HalfbandInterpolate(const int16_t* x, int16_t* out, int32_t n) {
#if defined(__bfin__)
  EnsHalfbandInterpolateAsm(x, out, n);
#else
  HalfbandInterpolateC(x, out, n);
#endif
}

inline void MixSoftLimit(int16_t* io, const int16_t* wet, int32_t n, int32_t fade_out, int32_t wet_gain,
                         const SoftLimitTable* t) {
#if defined(__bfin__)
  EnsMixSoftLimitAsm(io, wet, n, (fade_out & 0xffff) | (wet_gain << 16), t);
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

  // coop: the two decimators of a stereo pair hold the same history (so the same input gives the same output).
  bool SameHistory(const HalfbandDecimator& o) const {
    for (int32_t i = 0; i < kHistory; ++i) {
      if (x_[i] != o.x_[i]) return false;
    }
    return true;
  }
  // coop: take the other's history, as Process would have left it had it run on the same input.
  void CopyHistory(const HalfbandDecimator& o) {
    for (int32_t i = 0; i < kHistory; ++i) x_[i] = o.x_[i];
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

}  // namespace coop_ensemble

#endif  // COOP_ENSEMBLE_HALFBAND_H_
