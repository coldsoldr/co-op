// The half-rate path's tables and its kernels' C references (src/half_rate.h), from te-op1-cfw
// ports/clouds/src/core/halfband.cc and clouds_tables.cc (5aa314b; Mutable Instruments Clouds, MIT, ../LICENSE-MIT).
// The C references are built off the Blackfin only (the plugin runs halfband_bfin.S).
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#include "src/half_rate.h"

namespace op1clouds {

#define V(i) op1fx::tables::kSoftLimitValue[i]
#define V8(i) V(i), V(i + 1), V(i + 2), V(i + 3), V(i + 4), V(i + 5), V(i + 6), V(i + 7)
#define V64(i) V8(i), V8(i + 8), V8(i + 16), V8(i + 24), V8(i + 32), V8(i + 40), V8(i + 48), V8(i + 56)
#define T(i) op1fx::tables::kSoftLimitStep[i]
#define T8(i) T(i), T(i + 1), T(i + 2), T(i + 3), T(i + 4), T(i + 5), T(i + 6), T(i + 7)
#define T64(i) T8(i), T8(i + 8), T8(i + 16), T8(i + 24), T8(i + 32), T8(i + 40), T8(i + 48), T8(i + 56)
const SoftLimitTable kSoftLimit = {
  { V64(0), V64(64), V64(128), V64(192), V(256) },
  { T64(0), T64(64), T64(128), T64(192) },
};
#undef V
#undef V8
#undef V64
#undef T
#undef T8
#undef T64
static_assert(sizeof(SoftLimitTable) == 1026, "the mix kernel reads the steps at +514");

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

#if !defined(__bfin__) || defined(COOP_TAPE_ECHO_C)
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
#endif

}  // namespace op1clouds
