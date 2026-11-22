// The saw engine in the Blackfin GNU simulator (bfin-elf-run): six voices, 3000 renders each, the knobs and the
// shape moving (out-of-range shape words too), CYCLES around each render, an FNV-1a digest of every output sample.
// The digest is the bit-exactness check: 0.3 (no unswitching) and 0.4 (-funswitch-loops on saw.cc) both print
// 967d6f58 (docs/plugin-performance.md "Loop unswitching"). Compute only: no cache or L1 / SDRAM model.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <stdint.h>
#include <stdio.h>
#include "coop_plugin.h"
#include "coop_screen.h"
extern "C" const coop_engine_t ENGINE_saw;
extern "C" const coop_screen_t SCREEN_saw = { 0, 0 };
extern "C" void* coop_l1_lease(uint32_t) { return 0; }
static inline uint32_t Cycles() { uint32_t c; __asm__ __volatile__("%0 = CYCLES;" : "=d"(c)); return c; }
static uint32_t rng = 1;
static uint32_t Rand() { rng = rng * 1664525u + 1013904223u; return rng; }
int main() {
  uint32_t s; __asm__ __volatile__("%0 = SYSCFG;" : "=d"(s)); s |= 2; __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(s));
  ENGINE_saw.init();
  int16_t params[16] = { 20000, 9000, 12000, 15000, 0, 0, 0, 0 };
  int32_t pitch[COOP_VOICE_BLOCK];
  int16_t out[COOP_VOICE_BLOCK];
  uint32_t h = 2166136261u;
  unsigned long long cyc[3] = {0, 0, 0}; unsigned long cnt[3] = {0, 0, 0};
  for (int v = 0; v < COOP_VOICES; ++v) ENGINE_saw.note_on(v, 0, 20000 + v * 1000, params);
  for (int b = 0; b < 3000; ++b) {
    if (b % 50 == 0) { for (int k = 0; k < 4; ++k) params[k] = (int16_t)(Rand() >> 17); params[5] = (b / 50) % 3; params[7] = (Rand() >> 31); }
    if (b % 333 == 0) params[5] = (int16_t)(Rand() >> 16);   // out-of-range shapes too
    for (int v = 0; v < COOP_VOICES; ++v) {
      uint32_t inc = 0x00400000u + (Rand() >> 8);
      for (int i = 0; i < COOP_VOICE_BLOCK; ++i) pitch[i] = (int32_t)(inc + i * 37);
      uint32_t c0 = Cycles();
      ENGINE_saw.render(v, 0, out, pitch, params);
      uint32_t c = Cycles() - c0;
      int sh = params[5] < 0 ? 0 : params[5] > 2 ? 2 : params[5];
      cyc[sh] += c; cnt[sh]++;
      for (int i = 0; i < COOP_VOICE_BLOCK; ++i) { h ^= (uint16_t)out[i]; h *= 16777619u; }
    }
  }
  for (int k = 0; k < 3; ++k) printf("shape %d: %lu renders, avg %lu cycles a voice render\n", k, cnt[k], (unsigned long)(cyc[k] / (cnt[k] ? cnt[k] : 1)));
  printf("all: avg %lu cycles a voice render\n", (unsigned long)((cyc[0] + cyc[1] + cyc[2]) / (cnt[0] + cnt[1] + cnt[2])));
  printf("digest %08lx\n", (unsigned long)h);
  return 0;
}
