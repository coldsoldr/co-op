// C references of the half-rate kernels (src/halfband.h), for host builds: the device runs halfband_bfin.S.
// coop copy of te-op1-cfw ports/clouds/src/core/halfband.cc (InitSoftLimitTable is gen_tables.py's now).

#include "src/halfband.h"

#ifndef __bfin__

namespace coop_ensemble {

namespace {
inline int32_t Sat16(int32_t x) { return x > 32767 ? 32767 : (x < -32768 ? -32768 : x); }
}  // namespace

void HalfbandDecimateC(const int16_t* x, int16_t* out, int32_t n) {
  for (int32_t k = 0; k < n; ++k) {
    const int16_t* c = &x[2 * k + 10];
    int32_t y = c[0] * 16384 + 16384;
    for (int32_t t = 0; t < 5; ++t) {
      y += c[2 * t + 1] * kHalfbandTaps[t] + c[-2 * t - 1] * kHalfbandTaps[t];
    }
    out[k] = static_cast<int16_t>(Sat16(y >> 15));
  }
}

void HalfbandInterpolateC(const int16_t* x, int16_t* out, int32_t n) {
  for (int32_t m = 0; m < n; ++m) {
    // Offsets +-(2t + 1) from the centre, between x[m + 4] and x[m + 5].
    const int16_t* c = &x[m];
    int32_t y = 8192;
    for (int32_t t = 0; t < 5; ++t) {
      y += c[5 + t] * kHalfbandTaps[t] + c[4 - t] * kHalfbandTaps[t];
    }
    out[2 * m] = static_cast<int16_t>(Sat16(y >> 14));
    out[2 * m + 1] = c[5];
  }
}

void MixSoftLimitC(int16_t* io, const int16_t* wet, int32_t n, int32_t fade_out, int32_t wet_gain,
                   const SoftLimitTable* t) {
  for (int32_t i = 0; i < n; ++i) {
    const int32_t u = ((io[i] * fade_out) >> 4) + ((wet[i] * wet_gain) >> 2);
    int32_t a = u < 0 ? -u : u;
    if (a > (4 << 27) - 1) a = (4 << 27) - 1;
    const int32_t k = a >> 21;
    const int32_t y = t->value[k] + ((t->step[k] * ((a >> 6) & 0x7fff)) >> 15);
    io[i] = static_cast<int16_t>(u < 0 ? -y : y);
  }
}

}  // namespace coop_ensemble

#endif
