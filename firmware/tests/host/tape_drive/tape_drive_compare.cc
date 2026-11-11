// The tape_drive port's host test (guide/plugin-guidelines.md §7.4 step 7; docs/plugin-performance.md "How a rewrite
// is accepted"): plugins/tape_drive (the coop effect: its int32 glue, its copy of the DSP) against te-op1-cfw's
// ports/fx/src/tape driven the way the kennel build drove it (ports/knl/tape_drive/tape_knl.cc RenderStereo: the
// knobs normalised by op1_fx_param_q15, age mirrored, gain_oct from word 5). A straight port: bit-identical.
//
// One source, three builds (Makefile):
//   -DTAPE_REF   te-op1-cfw's TapeSat (host: its C reference kernels), int16 stereo blocks;
//   (default)    the coop plugin's render (host: the copied C kernels), int32 blocks with random low halves;
//   bfin sim     the coop plugin as it ships (the copied assembly kernels), timed with CYCLES per render.
// The scenario is the same seeded sequence in all three: a chord, noise, silence, full-scale bursts; knob jumps and
// walks in the filter type's range units (also past both ends: the clamp), the gain word anywhere in int16, a re-init
// now and then (a sound change), long stretches at mix 0. Each build writes every block's int16 output (the coop
// builds: the high halves) to a file; the Makefile compares them. The coop builds also check, every block, that the
// output's low halves are the input's, and that after 80 blocks at mix 0 the whole int32 output is the input.
//
//   tape_ref OUT.raw / tape_coop OUT.raw   [BLOCKS]
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef TAPE_REF
#include "src/tape/tape.h"   // te-op1-cfw ports/fx
#else
#include "src/tape.h"        // plugins/tape_drive (only for TapeParams' size; the render is the plugin's)
#include "coop_plugin.h"
extern "C" const coop_fx_t FX_tape_drive;
extern "C" long coop_fx_scratch_check(const coop_fx_t* fx, const int16_t* params, long blocks);   /* ../fx_scratch_host.cc */
#endif

namespace {

uint32_t rng = 0x1234567u;
uint32_t Next() {
  rng = rng * 1664525u + 1013904223u;
  return rng;
}
int32_t Range(int32_t lo, int32_t hi) { return lo + static_cast<int32_t>(Next() % static_cast<uint32_t>(hi - lo + 1)); }
int16_t Sat16(int32_t v) { return static_cast<int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

// The filter type's knob ranges (sdk/coop_plugin.h; e6 probe filter_ranges).
const int32_t kLo[4] = { 64, 0, 0, 8192 };
const int32_t kHi[4] = { 14528, 24576, 22528, 32767 };

#ifdef TAPE_REF
// tape_knl.cc's Params(), with kennel's op1_fx_param_q15 and knl::Word.
int32_t ParamQ15(const int16_t* p, int i) {
  int32_t v = (p[i] - kLo[i]) * 32767 / (kHi[i] - kLo[i]);
  return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}
int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }
op1fx::TapeParams Params(const int16_t* p) {
  op1fx::TapeParams q;
  q.saturate = ParamQ15(p, 0);
  q.wobble = ParamQ15(p, 1);
  q.age = 32767 - ParamQ15(p, 2);
  q.mix = ParamQ15(p, 3);
  q.gain_oct = (Clamp15(p[5]) - 16384) * 4;
  return q;
}
op1fx::TapeSat tape;
#endif

inline void EnableCycles() {
#if defined(__bfin__)
  uint32_t s;
  __asm__ volatile("%0 = SYSCFG;" : "=d"(s));
  s |= 2;   // CCEN
  __asm__ volatile("SYSCFG = %0; CSYNC;" : : "d"(s));
#endif
}

inline uint32_t Cycles() {
#if defined(__bfin__)
  uint32_t c;
  __asm__ volatile("%0 = CYCLES;" : "=d"(c));
  return c;
#else
  return 0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s OUT.raw [BLOCKS]\n", argv[0]);
    return 2;
  }
  const int32_t blocks = argc > 2 ? atoi(argv[2]) : 20000;
  FILE* out = fopen(argv[1], "wb");
  if (!out) return 2;

  int16_t params[16] = { 0 };
  int32_t words[4];
  for (int k = 0; k < 4; ++k) words[k] = (kLo[k] + kHi[k]) / 2;
  int32_t gain = 16384;
  uint32_t ph[3] = { 0, 0, 0 };
  const uint32_t inc[3] = { 9739155u, 14608732u, 21913098u };   // ~100, 150, 225 Hz
  int32_t level = 16384, mode = 0, mode_left = 0, mix0_blocks = 0;
  int32_t failures = 0, low_bad = 0, ident_blocks = 0, wet_blocks = 0;
  EnableCycles();
  uint32_t cyc_sum = 0, cyc_max = 0, cyc_n = 0;

#ifdef TAPE_REF
  tape.Init();
#else
  FX_tape_drive.init(2);
#endif

  for (int32_t b = 0; b < blocks; ++b) {
    // The scene: 0 chord, 1 noise, 2 silence, 3 full-scale square bursts; a stretch at mix 0 now and then.
    if (--mode_left <= 0) {
      mode = Range(0, 3);
      mode_left = Range(20, 400);
      level = Range(0, 32767);
    }
    // Knobs: jumps, walks, holds; past both ends sometimes (the core clamps at slots 0-2, coop's knob map at 3-4,
    // but the render clamps too).
    const uint32_t r = Next() & 255;
    if (r < 4) {
      const int k = Range(0, 3);
      words[k] = Range(kLo[k] - 2000, kHi[k] + 2000);
    } else if (r < 40) {
      const int k = Range(0, 3);
      words[k] += Range(-300, 300);
      if (words[k] < kLo[k] - 200) words[k] = kLo[k] + 200;   // walks bounce back (jumps go further)
      if (words[k] > kHi[k] + 200) words[k] = kHi[k] - 200;
    } else if (r < 42) {
      gain = Range(-32768, 32767);
    } else if (r < 60) {
      gain += Range(-400, 400);
      if (gain < -2000) gain = -2000;
      if (gain > 34767) gain = 34767;
    } else if (r == 99) {
      // a sound change: the core runs init again
#ifdef TAPE_REF
      tape.Init();
#else
      FX_tape_drive.init(2);
#endif
    }
    if ((b % 3000) >= 2800) words[3] = kLo[3] - Range(0, 50);   // mix 0 (and below: clamped) for 200 blocks
    for (int k = 0; k < 4; ++k) params[k] = Sat16(words[k]);
    params[4] = 0x5300;
    params[5] = Sat16(gain);
    mix0_blocks = params[3] <= kLo[3] ? mix0_blocks + 1 : 0;

    int32_t l32[64], r32[64];
    for (int i = 0; i < 64; ++i) {
      int32_t s = 0;
      for (int j = 0; j < 3; ++j) {
        ph[j] += inc[j];
        s += static_cast<int32_t>(ph[j] >> 18) - 8192;
      }
      const int32_t noise = static_cast<int32_t>(Next() >> 16) - 32768;
      int32_t a, c;
      switch (mode) {
        case 0: a = (s * level) >> 15; c = ((s - 4096) * level) >> 15; break;
        case 1: a = (noise * level) >> 15; c = ((static_cast<int32_t>(Next() >> 16) - 32768) * level) >> 15; break;
        case 2: a = c = 0; break;
        default: a = (ph[0] >> 31) ? 32767 : -32768; c = (ph[1] >> 31) ? 32767 : -32768; break;
      }
      // int32 bus words: the int16 sample high, random low halves (silence keeps them 0, as the stock bus).
      const uint32_t lo1 = mode == 2 ? 0 : (Next() & 0xffffu), lo2 = mode == 2 ? 0 : (Next() & 0xffffu);
      l32[i] = static_cast<int32_t>((static_cast<uint32_t>(Sat16(a)) << 16) | lo1);
      r32[i] = static_cast<int32_t>((static_cast<uint32_t>(Sat16(c)) << 16) | lo2);
    }

    int16_t o[128];
#ifdef TAPE_REF
    int16_t l16[64], r16[64];
    for (int i = 0; i < 64; ++i) {
      l16[i] = static_cast<int16_t>(l32[i] >> 16);
      r16[i] = static_cast<int16_t>(r32[i] >> 16);
    }
    tape.Process(l16, r16, 64, Params(params));
    for (int i = 0; i < 64; ++i) {
      o[2 * i] = l16[i];
      o[2 * i + 1] = r16[i];
    }
#else
    int32_t inl[64], inr[64];
    for (int i = 0; i < 64; ++i) {
      inl[i] = l32[i];
      inr[i] = r32[i];
    }
    const uint32_t c0 = Cycles();
    FX_tape_drive.render(2, l32, r32, params, 64);
    const uint32_t dc = Cycles() - c0;
    if (b > 0) {
      cyc_sum += dc;
      cyc_n++;
      if (dc > cyc_max) cyc_max = dc;
    }
    bool same = true;
    for (int i = 0; i < 64; ++i) {
      if (((l32[i] ^ inl[i]) & 0xffff) || ((r32[i] ^ inr[i]) & 0xffff)) ++low_bad;
      if (l32[i] != inl[i] || r32[i] != inr[i]) same = false;
      o[2 * i] = static_cast<int16_t>(l32[i] >> 16);
      o[2 * i + 1] = static_cast<int16_t>(r32[i] >> 16);
    }
    if (!same) ++wet_blocks;
    if (mix0_blocks > 80) {
      ++ident_blocks;
      if (!same) ++failures;
    }
#endif
    fwrite(o, sizeof(o), 1, out);
  }
  fclose(out);
#ifndef TAPE_REF
  printf("tape_drive coop: %ld blocks (%ld changed by the effect); low halves kept: %s (%ld bad); "
         "mix 0 identity (int32) in %ld blocks: %s\n",
         static_cast<long>(blocks), static_cast<long>(wet_blocks), low_bad ? "NO" : "yes", static_cast<long>(low_bad),
         static_cast<long>(ident_blocks), failures ? "FAIL" : "ok");
#if defined(__bfin__)
  printf("cycles per render (64 stereo samples, simulator: no cache or SDRAM waits): avg %lu, peak %lu\n",
         static_cast<unsigned long>(cyc_n ? cyc_sum / cyc_n : 0), static_cast<unsigned long>(cyc_max));
#endif
  (void)cyc_sum;
  (void)cyc_max;
  (void)cyc_n;
  {   /* e8: two positions at once and a restart on a dirty position buffer (tests/host/fx_scratch_host.cc) */
    int16_t sp[COOP_FX_PARAMS_MIN] = { 9000, 15000, 6000, 28000, 0x5300 + 32, 20000 };
    if (coop_fx_scratch_check(&FX_tape_drive, sp, 1500)) failures++;
  }
  return (low_bad || failures || !ident_blocks || !wet_blocks) ? 1 : 0;
#else
  printf("tape_drive ref: %ld blocks\n", static_cast<long>(blocks));
  return 0;
#endif
}
