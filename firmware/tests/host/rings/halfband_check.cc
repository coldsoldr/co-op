// The rings upsampler's half-band kernel (plugins/rings/src/halfband_bfin.S) against its C loop (rings.cc
// Upsampler), in the GNU simulator: 20,000 random blocks, every fourth one built from the edge values (-32768, 32767,
// 0, +-1), and the mid buffer compared word for word. `make halfband-check`.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <cstdint>
#include <cstdio>

#include "src/halfband_taps.h"

extern "C" void HalfbandAsm(const int16_t* x, const int16_t* xs, int32_t* mid3);

namespace {
const int kN = 56;   // 24 history + 32
uint32_t rng = 0x9E3779B9u;
uint32_t Next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

void Reference(const int16_t* x, int32_t* mid) {
  for (int m = 0; m < 32; ++m) {
    const int16_t* c = x + m + 12;
    int32_t acc = 1 << 13;
    for (int j = 0; j < 12; ++j) {
      const int16_t tap = static_cast<int16_t>(op1rings::kHalfbandTaps[j]);
      acc += static_cast<int32_t>(tap) * c[-j];
      acc += static_cast<int32_t>(tap) * c[1 + j];
    }
    mid[3 + 2 * m] = c[0];
    mid[3 + 2 * m + 1] = acc >> 14;
  }
}
}  // namespace

int main() {
  alignas(4) static int16_t x[kN], xs[kN];
  static int32_t a[3 + 64], b[3 + 64];
  static const int16_t kEdges[6] = { -32768, 32767, 0, 1, -1, -32767 };
  long bad = 0;
  for (int block = 0; block < 20000; ++block) {
    for (int i = 0; i < kN; ++i) {
      x[i] = block % 4 == 3 ? kEdges[Next() % 6] : static_cast<int16_t>(Next());
    }
    // the worst case for the sum: alternating full-scale signs matched to the taps' signs
    if (block == 0) for (int i = 0; i < kN; ++i) x[i] = (i % 2) ? -32768 : 32767;
    for (int i = 0; i + 1 < kN; ++i) xs[i] = x[i + 1];
    for (int i = 0; i < 67; ++i) a[i] = b[i] = 0x5A5A5A5A;
    Reference(x, a);
    HalfbandAsm(x, xs, b + 3);
    for (int i = 3; i < 67; ++i) {
      if (a[i] != b[i]) {
        if (bad < 5) std::printf("block %d mid[%d]: C %ld asm %ld\n", block, i, (long)a[i], (long)b[i]);
        ++bad;
      }
    }
  }
  std::printf("halfband: %ld mismatching words in 20000 blocks\n", bad);
  return bad != 0;
}
