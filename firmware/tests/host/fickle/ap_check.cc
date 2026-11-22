// fickle's diffuser kernel (plugins/fickle/fickle_bfin.S, FickleApAsm) against ApPass's C (fickle.cc, the loop
// copied here on plain arrays), in the Blackfin GNU simulator: random rings and blocks, full-scale edges, v past 16
// bits (a later diffuser's input), every gain from 0 to the top (23592) and 0..64 clock samples, every ring size and
// head position fickle uses. Checks v, the whole ring (nothing written outside the pass's run) and the guards; prints
// the cycles a call.
//   wsl -e bash -lc 'cd /mnt/i/GitHub/coldsoldr/co-op/firmware && make -C tests/host/fickle ap'
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" void FickleApAsm(uint32_t* ring, int32_t ring_bytes, uint32_t ri, uint32_t wi, int32_t* vl, int32_t* vr,
                            int32_t g, int32_t ticks);

namespace {

uint32_t g_seed = 0x9e3779b9u;
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

inline int32_t Sat16(int32_t x) { return x > 32767 ? 32767 : (x < -32768 ? -32768 : x); }
inline int32_t M16(int32_t a, int32_t b) { return static_cast<int16_t>(a) * static_cast<int32_t>(static_cast<int16_t>(b)); }
inline uint32_t PackRaw(int32_t l, int32_t r) { return (static_cast<uint32_t>(l) & 0xffffu) | (static_cast<uint32_t>(r) << 16); }
inline int32_t Lo(uint32_t w) { return static_cast<int16_t>(w); }
inline int32_t Hi(uint32_t w) { return static_cast<int32_t>(w) >> 16; }

// fickle.cc ApPass, its ring and block as arguments
void ApPassC(uint32_t* ring, uint32_t mask, uint32_t m, uint32_t n0, int32_t g, int32_t* vl, int32_t* vr, int32_t ticks) {
  uint32_t ri = (n0 - m) & mask, wi = n0 & mask;
  int32_t k = 0;
  while (k < ticks) {
    int32_t run = ticks - k;
    if (run > static_cast<int32_t>(mask + 1 - ri)) run = static_cast<int32_t>(mask + 1 - ri);
    if (run > static_cast<int32_t>(mask + 1 - wi)) run = static_cast<int32_t>(mask + 1 - wi);
    const uint32_t* rp = ring + ri;
    uint32_t* wp = ring + wi;
    for (int32_t j = 0; j < run; ++j, ++k) {
      const uint32_t z = rp[j];
      const int32_t zl = Lo(z), zr = Hi(z);
      const int32_t ul = Sat16(vl[k] - (M16(zl, g) >> 15)), ur = Sat16(vr[k] - (M16(zr, g) >> 15));
      wp[j] = PackRaw(ul, ur);
      vl[k] = zl + (M16(ul, g) >> 15);
      vr[k] = zr + (M16(ur, g) >> 15);
    }
    ri = (ri + run) & mask;
    wi = (wi + run) & mask;
  }
}

const int kG = 4;                                       // guard words around every array
uint32_t g_ring_c[kG + 512 + kG], g_ring_a[kG + 512 + kG];
int32_t g_v_c[2][kG + 65 + kG], g_v_a[2][kG + 65 + kG];

int16_t S16(int kind) {
  switch (kind) {
    case 1: return (Rand() & 1) ? 32767 : -32768;
    case 2: return static_cast<int16_t>(static_cast<int32_t>(Rand() % 5) - 2);
    default: return static_cast<int16_t>(Rand() >> 16);
  }
}

}  // namespace

int main() {
  uint32_t sc;
  __asm__ __volatile__("%0 = SYSCFG;" : "=d"(sc));
  sc |= 2;
  __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(sc));
  const uint32_t kMask[4] = { 127, 255, 255, 511 };
  const uint32_t kDelay[4] = { 113, 167, 229, 311 };
  int fail = 0;
  long calls = 0;
  uint64_t cyc = 0;
  long cyc_n = 0;
  uint64_t cyc_c = 0;
  for (int round = 0; round < 20000; ++round) {
    const int which = round & 3, kind = (round >> 2) % 3;
    const uint32_t mask = kMask[which], m = kDelay[which];
    const int32_t ticks = round < 4000 ? static_cast<int32_t>(round % 66) : 30 + static_cast<int32_t>(Rand() % 35);
    const int32_t g = (round % 7 == 0) ? 23592 : (round % 7 == 1) ? 0 : static_cast<int32_t>(Rand() % 23593);
    const uint32_t n0 = Rand();
    for (uint32_t i = 0; i < kG + 512 + kG; ++i) g_ring_c[i] = PackRaw(S16(kind), S16(kind));
    memcpy(g_ring_a, g_ring_c, sizeof(g_ring_c));
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < kG + 65 + kG; ++i) {
        // a first diffuser's input is int16; a later one's v reaches 32767 + 23592
        const int32_t big = (round & 8) ? static_cast<int32_t>(Rand() % 112721) - 56360 : S16(kind);
        g_v_c[c][i] = big;
      }
      memcpy(g_v_a[c], g_v_c[c], sizeof(g_v_c[c]));
    }
    uint32_t t = Cycles();
    ApPassC(g_ring_c + kG, mask, m, n0, g, g_v_c[0] + kG, g_v_c[1] + kG, ticks);
    if (ticks == 48) cyc_c += Cycles() - t;
    t = Cycles();
    FickleApAsm(g_ring_a + kG, static_cast<int32_t>((mask + 1) * 4), (n0 - m) & mask, n0 & mask, g_v_a[0] + kG,
                g_v_a[1] + kG, g, ticks);
    if (ticks == 48) { cyc += Cycles() - t; ++cyc_n; }
    ++calls;
    if (memcmp(g_ring_c, g_ring_a, sizeof(g_ring_c)) || memcmp(g_v_c, g_v_a, sizeof(g_v_c))) {
      if (++fail < 10) printf("FAIL round %d: ring %lu delay %lu ticks %ld g %ld n0 %lu\n", round,
                              static_cast<unsigned long>(mask + 1), static_cast<unsigned long>(m), static_cast<long>(ticks), static_cast<long>(g), static_cast<unsigned long>(n0));
    }
  }
  printf("fickle ap: %ld calls; cycles a call at 48 clock samples: %lu (ApPass in C, -O2: %lu)\n", calls,
         static_cast<unsigned long>(cyc_n ? cyc / cyc_n : 0), static_cast<unsigned long>(cyc_n ? cyc_c / cyc_n : 0));
  if (fail) { printf("fickle ap: %d FAILED\n", fail); return 1; }
  printf("fickle ap: FickleApAsm == ApPass (C), bit for bit\n");
  return 0;
}
