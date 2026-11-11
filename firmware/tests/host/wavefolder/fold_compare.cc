// The wavefolder port's host test (guide/plugin-guidelines.md §7.4 step 7; docs/plugin-performance.md "How a rewrite
// is accepted"): plugins/wavefolder (the coop effect: int32 bus, the e6 gain word, the L == R shortcut, the mix-0
// bypass that leaves the bus untouched) against te-op1-cfw's FOLD as its kennel plugin drove it
// (ports/knl/wavefolder/fold_knl.cc: op1_fx_param_q15 on the four range words, knl::GainQ14 on word 5,
// Fold::Process(l, r, 64) on the int16 block, the stereo path). A straight port: bit-identical.
//
// This one source builds twice (Makefile): -DFOLD_REF runs te-op1-cfw's ports/fx/src/fold, without it the coop
// plugin's coop_fx_t. The scenario is the same seeded sequence in both: knob jumps (edge values included) and LFO-rate
// walks of all four turns and the gain, stretches at mix 0 (the bypass), noise, sines, silence, stretches with L == R
// (the coop shortcut) and re-inits. Each build writes every block (int16 l, r) to a file; the Makefile compares them.
// Built natively the C kernels run; built for the Blackfin simulator (make sim) the assembly does, and its output
// must equal the native one too.
//
// The coop build also checks the bus contract: in a block it wrote, the low 16 bits are 0; in a block it left alone
// (mix 0), every word is the input word (the input carries random low bits). -DFOLD_CYCLES (the simulator builds)
// times every call with CYCLES (compute only, ~1 cycle an instruction; no cache misses or SDRAM waits) and prints
// the cost per kind of block.
//
//   fold_ref OUT.raw [BLOCKS]   /   fold_coop OUT.raw [BLOCKS]
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef FOLD_REF
#include "src/fold/fold.h"
#else
#include "coop_plugin.h"
extern "C" const coop_fx_t FX_wavefolder;
#endif

namespace {

uint32_t rng = 0x5eed1234u;
uint32_t Next() {
  rng = rng * 1664525u + 1013904223u;
  return rng;
}
int32_t Range(int32_t lo, int32_t hi) { return lo + static_cast<int32_t>((Next() >> 8) % static_cast<uint32_t>(hi - lo + 1)); }

// The filter type's ranges (sdk/coop_plugin.h): what params[0..3] hold.
const int32_t kLo[4] = { 64, 0, 0, 8192 };
const int32_t kHi[4] = { 14528, 24576, 22528, 32767 };

#ifdef FOLD_CYCLES
void EnableCycles() {   // SYSCFG.CCEN (bit 1): the simulator's cycle counter runs only with it set
  uint32_t syscfg;
  __asm__ __volatile__("%0 = SYSCFG;" : "=d"(syscfg));
  syscfg |= 2;
  __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(syscfg));
}
inline uint32_t Now() {
  uint32_t c;
  __asm__ __volatile__("%0 = CYCLES;" : "=d"(c));
  return c;
}
struct Stat {
  uint32_t n, max;
  uint64_t total;
  void Add(uint32_t c) { ++n; total += c; if (c > max) max = c; }
  void Print(const char* what) const {
    if (n) printf("  %-34s %6u blocks  avg %6u  max %6u cycles\n", what, n, static_cast<unsigned>(total / n), max);
  }
};
Stat st_stereo, st_mono, st_bypass;
#endif

#ifdef FOLD_REF
// fold_knl.cc's Params, with op1_fx_param_q15 on the ranges above and knl::GainQ14 (ports/knl/common/knl_fx.h).
int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }
int32_t ParamQ15(const int16_t* p, int i) {
  int32_t lo = kLo[i], hi = kHi[i], v = p[i];
  v = (v - lo) * 32767 / (hi - lo);
  return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}
int32_t GainQ14(int32_t word) {
  static const int32_t kTable[13] = { 8211, 9215, 10338, 11600, 13016, 14605, 16384, 18382, 20626, 23143, 25966, 29135, 32690 };
  const int32_t pos = Clamp15(word) * 12;
  const int32_t i = pos / 32767;
  const int32_t frac = pos - i * 32767;
  if (i >= 12) return kTable[12];
  return kTable[i] + (((kTable[i + 1] - kTable[i]) * frac) / 32767);
}
op1fx::Fold fold;
#endif

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s OUT.raw [BLOCKS]\n", argv[0]);
    return 2;
  }
  const long blocks = argc > 2 ? atol(argv[2]) : 20000;
  FILE* out = fopen(argv[1], "wb");
  if (!out) return 2;
#ifdef FOLD_CYCLES
  EnableCycles();
#endif

  int16_t p[16] = { 0 };
  int32_t k[5] = { 9830, 16384, 22938, 32767, 16384 };   // Q15 positions, mapped into p[] per block
  int32_t walk[5] = { 0 };                               // per-block steps while walking
  int input = 0, walking = 0, mono = 0;
  uint32_t ph_l = 0, ph_r = 0, inc_l = 0, inc_r = 0;
  int32_t amp = 0;
  long bad_bus = 0, bypass_blocks = 0, mono_blocks = 0;

#ifdef FOLD_REF
  fold.Init();
#else
  FX_wavefolder.init(0);
#endif

  for (long b = 0; b < blocks; ++b) {
    // ---- the scenario ----
    const uint32_t ev = Next() >> 24;
    if (ev < 6) {                                        // a knob jump (edges now and then)
      const int i = Range(0, 4);
      const int32_t e = Range(0, 9);
      k[i] = e == 0 ? 0 : e == 1 ? 32767 : Range(0, 32767);
      walking = 0;
    } else if (ev < 8) {                                 // an LFO-rate walk of every knob for a while
      walking = Range(50, 400);
      for (int i = 0; i < 5; ++i) walk[i] = Range(-300, 300);
    } else if (ev < 10) {                                // the mix to 0 (the bypass), or back up
      k[3] = k[3] ? 0 : Range(8000, 32767);
    } else if (ev < 13) {                                // a new input
      input = Range(0, 3);
      mono = Range(0, 2) == 0;
      { const uint32_t v = Next(); inc_l = v >> Range(6, 10); }
      { const uint32_t v = Next(); inc_r = v >> Range(6, 10); }
      amp = Range(1000, 32767);
    } else if (ev == 13 && Range(0, 7) == 0) {           // the effect starts again (selected, a sound change)
#ifdef FOLD_REF
      fold.Init();
#else
      FX_wavefolder.init(0);
#endif
    }
    if (walking) {
      --walking;
      for (int i = 0; i < 5; ++i) {
        k[i] += walk[i];
        if (k[i] < 0 || k[i] > 32767) { walk[i] = -walk[i]; k[i] = k[i] < 0 ? 0 : 32767; }
      }
    }
    for (int i = 0; i < 4; ++i) p[i] = static_cast<int16_t>(kLo[i] + (k[i] * (kHi[i] - kLo[i])) / 32767);
    p[4] = 0x5300;
    p[5] = static_cast<int16_t>(k[4]);

    int16_t l16[64], r16[64];
    int32_t l32[64], r32[64];
    for (int i = 0; i < 64; ++i) {
      int32_t a = 0, c = 0;
      switch (input) {
        case 0: a = static_cast<int16_t>(Next() >> 16); c = static_cast<int16_t>(Next() >> 16); break;   // noise
        case 1:                                                                                          // sines
          ph_l += inc_l; ph_r += inc_r;
          a = (static_cast<int32_t>(static_cast<int16_t>(ph_l >> 16) < 0 ? ~ph_l : ph_l) >> 15) - 32768;  // triangle-ish
          a = (a * amp) >> 15;
          c = (static_cast<int32_t>(static_cast<int16_t>(ph_r >> 16) < 0 ? ~ph_r : ph_r) >> 15) - 32768;
          c = (c * amp) >> 15;
          break;
        case 2: a = c = 0; break;                                                                        // silence
        default: a = (Next() & 1) ? 32767 : -32768; c = -a; break;                                       // full scale
      }
      a = a > 32767 ? 32767 : (a < -32768 ? -32768 : a);
      c = c > 32767 ? 32767 : (c < -32768 ? -32768 : c);
      if (mono) c = a;
      l16[i] = static_cast<int16_t>(a);
      r16[i] = static_cast<int16_t>(c);
      l32[i] = static_cast<int32_t>(static_cast<uint32_t>(a) << 16 | (Next() >> 16));   // random low bits
      r32[i] = mono ? l32[i] : static_cast<int32_t>(static_cast<uint32_t>(c) << 16 | (Next() >> 16));
    }
    if (mono) ++mono_blocks;

    // ---- one block ----
#ifdef FOLD_REF
    op1fx::FoldParams q;
    q.fold = ParamQ15(p, 0);
    q.symmetry = ParamQ15(p, 1);
    q.tone = ParamQ15(p, 2);
    q.mix = ParamQ15(p, 3);
    q.gain = GainQ14(p[5]);
#ifdef FOLD_CYCLES
    const uint32_t c0 = Now();
#endif
    fold.Process(l16, r16, 64, q);
#ifdef FOLD_CYCLES
    st_stereo.Add(Now() - c0);
#endif
#else
    int32_t in_l[64], in_r[64];
    for (int i = 0; i < 64; ++i) { in_l[i] = l32[i]; in_r[i] = r32[i]; }
#ifdef FOLD_CYCLES
    const uint32_t c0 = Now();
#endif
    FX_wavefolder.render(0, l32, r32, p, 64);
#ifdef FOLD_CYCLES
    const uint32_t dc = Now() - c0;
#endif
    bool same = true, low0 = true;
    for (int i = 0; i < 64; ++i) {
      if (l32[i] != in_l[i] || r32[i] != in_r[i]) same = false;
      if ((l32[i] & 0xffff) || (r32[i] & 0xffff)) low0 = false;
    }
    if (same) ++bypass_blocks;
    else if (!low0) ++bad_bus;
#ifdef FOLD_CYCLES
    (same ? st_bypass : mono ? st_mono : st_stereo).Add(dc);
#endif
    for (int i = 0; i < 64; ++i) { l16[i] = static_cast<int16_t>(l32[i] >> 16); r16[i] = static_cast<int16_t>(r32[i] >> 16); }
#endif
    fwrite(l16, 2, 64, out);
    fwrite(r16, 2, 64, out);
  }
  fclose(out);
#ifdef FOLD_REF
  printf("fold_ref: %ld blocks\n", blocks);
#else
  printf("fold_coop: %ld blocks, %ld with L == R in, %ld left untouched (mix 0), %ld with low bits set (must be 0)\n",
         blocks, mono_blocks, bypass_blocks, bad_bus);
#endif
#ifdef FOLD_CYCLES
  printf("cycles per 64-sample call (simulator: compute only, the device is higher):\n");
#ifdef FOLD_REF
  st_stereo.Print("te-op1-cfw Fold::Process, stereo");
#else
  st_stereo.Print("coop render, stereo");
  st_mono.Print("coop render, L == R in");
  st_bypass.Print("coop render, mix 0 (untouched)");
#endif
#endif
#ifndef FOLD_REF
  if (bad_bus) return 1;
#endif
  return 0;
}
