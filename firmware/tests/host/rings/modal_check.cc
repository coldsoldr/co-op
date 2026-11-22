// rings 0.14: the modal coefficient kernels (src/modal_bfin.S ModalTranslateAsm, ModalCoeffsAsm) against their C
// (src/modal_resonator.cc ModalTranslateC, ModalCoeffsC) in the GNU simulator, on random modes: every output word.
//   make modal-check
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <cstdio>
#include <cstring>

#include "src/modal_resonator.cc"
#include "src/scratch.h"
namespace op1rings { Scratch* g_scratch; }

namespace {
uint32_t rng = 0x1234567u;
uint32_t Next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
int32_t Range(int32_t lo, int32_t hi) { return lo + static_cast<int32_t>(Next() % static_cast<uint32_t>(hi - lo)); }
}  // namespace

int main() {
  using namespace op1rings;
  const int kModes = 64;
  static Mode a[kModes], b[kModes];
  static int32_t ga[kModes * 6], gb[kModes * 6], log2_pf[kModes], gaina[kModes], gainb[kModes];
  long bad = 0, words = 0;
  for (int t = 0; t < 4000; ++t) {
    const int n = 2 * Range(1, 33);
    for (int i = 0; i < n; ++i) {
      int32_t pf = Range(1 << 12, 1 << 30);
      if (t % 7 == 0) pf = Range(1, 1 << 16);
      if (t % 11 == 0 && i > n / 2) pf = 1052266987;   // 0.49
      int32_t* g = ga + 6 * i;
      g[0] = InterpPf(kPhiQ, pf); g[1] = InterpPf(kPhiQ + 1, pf); g[2] = InterpPf(kPhiQ + 2, pf);
      g[3] = InterpPf(kPhiQ + 3, pf); g[4] = Range(0, 1 << 31 - 1); g[5] = Range(0, 1 << 30);
      log2_pf[i] = Log2Q24(pf);
      a[i].lp = Range(-(1 << 28), 1 << 28); a[i].bp = Range(-(1 << 28), 1 << 28);
      a[i].half_f = Range(0, 1 << 30); a[i].half_q = Range(0, 1 << 30);
      a[i].weight = Range(-1000, 1000); a[i].weight_increment = 7;
    }
    const int32_t u0 = Range(-(1 << 26), 1 << 26);
    std::memcpy(b, a, sizeof(a));
    std::memcpy(gb, ga, sizeof(ga));
    ModalTranslateC(a, ga, n, u0);
    ModalTranslateAsm(b, gb, n, u0);
    for (int i = 0; i < n; ++i) if (a[i].lp != b[i].lp || a[i].bp != b[i].bp) { if (bad < 10) std::printf("translate t %d mode %d: %ld %ld / %ld %ld\n", t, i, (long)a[i].lp, (long)b[i].lp, (long)a[i].bp, (long)b[i].bp); ++bad; }
    ModalCoeffs ka = { a, ga, log2_pf, gaina, n, u0, Range(-(8 << 24), 23 << 24), Range(322122547, 2147483647),
                       Range(0, 214748365), kExp2NegQ31, kRecip1pQ31, kRhoQ31, kInvRhoQ30, kRecip1mQ30,
                       kLog2MantissaQ30 };
    if (t % 13 == 0) ka.log2_q = Range(-(70 << 24), -(60 << 24));
    ModalCoeffs kb = ka;
    kb.modes = b; kb.geometry = gb; kb.gain = gainb;
    ModalCoeffsC(&ka);
    ModalCoeffsAsm(&kb);
    for (int i = 0; i < n; ++i) {
      const int32_t* x = reinterpret_cast<const int32_t*>(&a[i]);
      const int32_t* y = reinterpret_cast<const int32_t*>(&b[i]);
      for (int w = 0; w < 6; ++w, ++words) if (x[w] != y[w]) { if (bad < 10) std::printf("t %d mode %d word %d: %ld %ld\n", t, i, w, (long)x[w], (long)y[w]); ++bad; }
      for (int w = 0; w < 6; ++w, ++words) if (ga[6 * i + w] != gb[6 * i + w]) { if (bad < 10) std::printf("t %d mode %d geometry %d: %ld %ld\n", t, i, w, (long)ga[6*i+w], (long)gb[6*i+w]); ++bad; }
      ++words;
      if (gaina[i] != gainb[i]) { if (bad < 10) std::printf("t %d gain %d\n", t, i); ++bad; }
    }
    words += 2;
    if (ka.log2_q != kb.log2_q || ka.q_loss != kb.q_loss) { if (bad < 10) std::printf("t %d chain %ld %ld %ld %ld\n", t, (long)ka.log2_q, (long)kb.log2_q, (long)ka.q_loss, (long)kb.q_loss); ++bad; }
  }
  std::printf("modal kernels against the C: %ld words, %ld differ\n", words, bad);
  return bad != 0;
}
