// The shared half-band kernels (plugins/shared/halfband_bfin.S) against their C references (tape_echo's
// src/halfband.cc, the same arithmetic in all five plugins' copies), in the Blackfin GNU simulator: random blocks,
// full-scale and alternating edges, every even n from 2 to 64 and the plugins' 32. Checks the outputs bit for bit, that nothing past out[n - 1] (out[2n - 1]) is written and that
// the input is left as it was; prints the cycles a call.
//   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/halfband'
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/half_rate.h"

using namespace op1clouds;

namespace {

uint32_t g_seed = 0x12345678u;
uint32_t Rand() {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return g_seed;
}

inline uint32_t Cycles() {
  uint32_t c;
  __asm__ __volatile__("%0 = CYCLES;" : "=d"(c));
  return c;
}

// One input sample of a kind: 0 random, 1 full scale +, 2 full scale -, 3 alternating extremes, 4 small, 5 mixed
int16_t Sample(int kind, int i) {
  switch (kind) {
    case 1: return 32767;
    case 2: return -32768;
    case 3: return (i & 1) ? -32768 : 32767;
    case 4: return static_cast<int16_t>(static_cast<int32_t>(Rand() % 7) - 3);
    case 5: { const uint32_t r = Rand(); return (r & 3) == 0 ? ((r & 4) ? 32767 : -32768) : static_cast<int16_t>(r >> 16); }
    default: return static_cast<int16_t>(Rand() >> 16);
  }
}

const int kGuard = 8;
alignas(4) int16_t g_x[18 + 128 + kGuard], g_x_copy[18 + 128 + kGuard];
alignas(4) int16_t g_out_c[128 + kGuard], g_out_a[128 + kGuard];
int g_fail = 0;

bool Check(const char* what, int kind, int n, int outs) {
  if (memcmp(g_out_c, g_out_a, sizeof(g_out_c)) != 0) {
    for (int i = 0; i < outs + kGuard; ++i) {
      if (g_out_c[i] != g_out_a[i]) {
        if (++g_fail < 10) printf("FAIL %s kind %d n %d: out[%d] C %d asm %d%s\n", what, kind, n, i, g_out_c[i],
                                  g_out_a[i], i >= outs ? " (past the end)" : "");
        return false;
      }
    }
  }
  if (memcmp(g_x, g_x_copy, sizeof(g_x)) != 0) {
    if (++g_fail < 10) printf("FAIL %s kind %d n %d: the input changed\n", what, kind, n);
    return false;
  }
  return true;
}

}  // namespace

#ifdef HB_UPSTREAM
// te-op1-cfw's kernels (the ones the plugins carried before), renamed by the Makefile: their cycles for comparison
extern "C" void UpHalfbandDecimateAsm(const int16_t* x, int16_t* out, int32_t n);
extern "C" void UpHalfbandInterpolateAsm(const int16_t* x, int16_t* out, int32_t n);
#endif

int main() {
  uint32_t sc;
  __asm__ __volatile__("%0 = SYSCFG;" : "=d"(sc));
  sc |= 2;   // CCEN: CYCLES counts
  __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(sc));
  long calls = 0;
#ifdef HB_UPSTREAM
  uint32_t up_dec = 0, up_int = 0;
#endif
  uint32_t cyc_dec = 0, cyc_int = 0, n_dec = 0, n_int = 0;
  for (int round = 0; round < 3000; ++round) {
    const int kind = round % 6;
    const int n = round < 200 ? 2 * (1 + round % 32) : 32;            // every even n, then the plugins' 32
    // the decimator: 18 + 2n in, n out
    for (int i = 0; i < 18 + 2 * n + kGuard; ++i) g_x[i] = Sample(kind, i);
    memcpy(g_x_copy, g_x, sizeof(g_x));
    for (int i = 0; i < 128 + kGuard; ++i) g_out_c[i] = g_out_a[i] = static_cast<int16_t>(0x5a5a + i);
    HalfbandDecimateC(g_x, g_out_c, n);
    uint32_t t = Cycles();
    HalfbandDecimateAsm(g_x, g_out_a, n);
    t = Cycles() - t;
    if (n == 32) { cyc_dec += t; ++n_dec; }
#ifdef HB_UPSTREAM
    if (n == 32) { t = Cycles(); UpHalfbandDecimateAsm(g_x, g_out_a, n); up_dec += Cycles() - t; }
#endif
    Check("decimate", kind, n, n);
    // the interpolator: 9 + n in, 2n out
    for (int i = 0; i < 9 + n + kGuard; ++i) g_x[i] = Sample(kind, i);
    memcpy(g_x_copy, g_x, sizeof(g_x));
    for (int i = 0; i < 128 + kGuard; ++i) g_out_c[i] = g_out_a[i] = static_cast<int16_t>(0x2c2c + i);
    HalfbandInterpolateC(g_x, g_out_c, n);
    t = Cycles();
    HalfbandInterpolateAsm(g_x, g_out_a, n);
    t = Cycles() - t;
    if (n == 32) { cyc_int += t; ++n_int; }
#ifdef HB_UPSTREAM
    if (n == 32) { t = Cycles(); UpHalfbandInterpolateAsm(g_x, g_out_a, n); up_int += Cycles() - t; }
#endif
    Check("interpolate", kind, n, 2 * n);
    calls += 2;
  }
  printf("halfband: %ld calls; cycles a call at n = 32: decimate %lu, interpolate %lu\n", calls,
         static_cast<unsigned long>(n_dec ? cyc_dec / n_dec : 0), static_cast<unsigned long>(n_int ? cyc_int / n_int : 0));
#ifdef HB_UPSTREAM
  printf("halfband: te-op1-cfw's kernels (before): decimate %lu, interpolate %lu\n", static_cast<unsigned long>(up_dec / n_dec),
         static_cast<unsigned long>(up_int / n_int));
#endif
  if (g_fail) { printf("halfband: %d FAILED\n", g_fail); return 1; }
  printf("halfband: the assembly == the C references, bit for bit\n");
  return 0;
}
