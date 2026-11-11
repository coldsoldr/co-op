// The bitcrusher port's host test (guide/plugin-guidelines.md §7.4 step 7; docs/plugin-performance.md "How a rewrite
// is accepted"): plugins/bitcrusher (the coop effect, its render through FX_bitcrusher) against te-op1-cfw's own
// code, run as the kennel plugin ran it: op1fx::Crush::Process (ports/fx/src/crush/crush.h, its stereo path) on int16
// buffers, with crush_knl.cc's ParamsFromRaw (the knobs normalised by kennel's op1_fx_param_q15 over the filter type's
// range table, rate and bits mirrored, the gain from knl::GainQ14 of word 5, ports/knl/common/knl_fx.h).
//
// The scenario, seeded: 40,000 blocks of 64 in one carried state; random stereo input (full scale, quiet, silence,
// L == R, edge values), knobs that hold, walk, jump and sit at both ends of their ranges (and outside them, as a
// sound may hold), the gain word anywhere 0..32767 and outside. Two runs (the Makefile):
//   1. the host's widened int16 (l << 16): the coop output must be the upstream output << 16, word for word;
//   2. BC_LOW_BITS set: random low 16 bits under the same int16: the top halves must equal upstream's, and the low
//      halves must be the input's (the dry side keeps them).
//
//   [BC_LOW_BITS=1] bitcrusher_compare [BLOCKS]       exit 0 when every word matches
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "coop_plugin.h"
#include "common/knl_fx.h"        // te-op1-cfw ports/knl: knl::GainQ14
#include "fx/src/crush/crush.h"   // te-op1-cfw ports: op1fx::Crush

extern "C" const coop_fx_t FX_bitcrusher;

#ifdef BC_PROFILE   // the Blackfin simulator build (make profile): the render timed with CYCLES (compute only)
static inline uint32_t Cycles() { uint32_t c; __asm__ __volatile__("%0 = CYCLES;" : "=d"(c)); return c; }
#endif

namespace {

// The filter type's range table as kennel read it (0x01DE5C50 + 5 x 0x30 on R246; the same rows coop's host uses,
// docs/device-tests.md e6 probe `filter_ranges`).
const int32_t kLo[4] = { 64, 0, 0, 8192 };
const int32_t kHi[4] = { 14528, 24576, 22528, 32767 };

// third_party/kennel/synth_sdk/op1_plugin.h op1_fx_param_q15, the table passed in.
int32_t ParamQ15(const int16_t* p, int i) {
  int32_t lo = kLo[i], hi = kHi[i], v = p[i];
  v = (v - lo) * 32767 / (hi - lo);
  return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}

// crush_knl.cc ParamsFromRaw.
op1fx::CrushParams ParamsFromRaw(const int16_t* p) {
  op1fx::CrushParams q;
  q.rate = 32767 - ParamQ15(p, 0);
  q.bits = 32767 - ParamQ15(p, 1);
  q.jitter = ParamQ15(p, 2);
  q.mix = ParamQ15(p, 3);
  q.gain_q14 = knl::GainQ14(knl::Word(p, 5));
  return q;
}

uint32_t rng = 0x2545F491u;
uint32_t Rand() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
int32_t Range(int32_t lo, int32_t hi) { return lo + static_cast<int32_t>(Rand() % static_cast<uint32_t>(hi - lo + 1)); }

// One knob: held, walked, jumped, or at an end (sometimes past it).
int16_t NextKnob(int32_t v, int32_t lo, int32_t hi) {
  const uint32_t m = Rand() % 100;
  if (m < 70) return static_cast<int16_t>(v);
  if (m < 85) v += Range(-400, 400);
  else if (m < 93) v = Range(lo, hi);
  else if (m < 96) v = (Rand() & 1) ? lo : hi;
  else v = (Rand() & 1) ? lo - Range(0, 64) : hi + Range(0, 64);
  if (v < -32768) v = -32768;
  if (v > 32767) v = 32767;
  return static_cast<int16_t>(v);
}

int16_t Sample(int kind, int32_t i, int32_t* osc) {
  switch (kind) {
    case 0: return static_cast<int16_t>(Range(-32768, 32767));                 // full-scale noise
    case 1: return static_cast<int16_t>(Range(-300, 300));                     // quiet
    case 2: return 0;                                                          // silence
    case 3: { *osc += 1100; return static_cast<int16_t>(((*osc >> 4) & 0xffff) - 32768); }   // a saw
    default: { static const int16_t e[4] = { -32768, 32767, -1, 1 }; return e[(i + Rand()) & 3]; }   // edges
  }
}

}  // namespace

int main(int argc, char** argv) {
  const long blocks = argc > 1 ? std::atol(argv[1]) : 40000;
  op1fx::Crush ref;
  ref.Init();
  FX_bitcrusher.init(0);
#ifdef BC_PROFILE
  {
    uint32_t s;
    __asm__ __volatile__("%0 = SYSCFG;" : "=d"(s));
    s |= 2;   // CCEN: the cycle counter runs
    __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(s));
  }
#endif
  int16_t params[COOP_FX_PARAMS_MIN] = { 0 };
  params[0] = 14528; params[1] = 24576; params[2] = 0; params[3] = 32767; params[4] = 0x5300 + 32; params[5] = 16384;
  int32_t osc_l = 0, osc_r = 7777;
  long bad = 0, holds_every = 0, wet_blocks = 0;
  unsigned long long cyc_sum = 0;
  uint32_t cyc_max = 0;
  (void)cyc_sum; (void)cyc_max;
  const bool low_bits = std::getenv("BC_LOW_BITS") != nullptr;
  for (long b = 0; b < blocks; ++b) {
    for (int k = 0; k < 4; ++k) params[k] = NextKnob(params[k], kLo[k], kHi[k]);
    params[5] = NextKnob(params[5], 0, 32767);
    const int kind = static_cast<int>(Rand() % 5);
    const bool mono = (Rand() % 4) == 0;
    int16_t l16[64], r16[64];
    int32_t l32[64], r32[64], in_l[64], in_r[64];
    for (int i = 0; i < 64; ++i) {
      l16[i] = Sample(kind, i, &osc_l);
      r16[i] = mono ? l16[i] : Sample(kind, i, &osc_r);
      const int32_t lowl = low_bits ? static_cast<int32_t>(Rand() & 0xffff) : 0;
      const int32_t lowr = low_bits ? static_cast<int32_t>(Rand() & 0xffff) : 0;
      in_l[i] = l32[i] = static_cast<int32_t>(static_cast<uint32_t>(l16[i]) << 16) | lowl;
      in_r[i] = r32[i] = static_cast<int32_t>(static_cast<uint32_t>(r16[i]) << 16) | lowr;
    }
    const op1fx::CrushParams q = ParamsFromRaw(params);
    if (q.rate == 0) holds_every++;
    if (q.mix > 0) wet_blocks++;
    ref.Process(l16, r16, 64, q);
#ifdef BC_PROFILE
    const uint32_t c0 = Cycles();
    FX_bitcrusher.render(0, l32, r32, params, 64);
    const uint32_t c = Cycles() - c0;
    cyc_sum += c;
    if (c > cyc_max) cyc_max = c;
#else
    FX_bitcrusher.render(0, l32, r32, params, 64);
#endif
    for (int i = 0; i < 64; ++i) {
      const bool ok = (l32[i] >> 16) == l16[i] && (r32[i] >> 16) == r16[i] &&
                      (l32[i] & 0xffff) == (in_l[i] & 0xffff) && (r32[i] & 0xffff) == (in_r[i] & 0xffff);
      if (!ok) {
        if (bad < 10)
          std::printf("block %ld sample %d: coop %08x %08x, upstream %04x %04x (in %08x %08x)\n", b, i,
                      static_cast<unsigned>(l32[i]), static_cast<unsigned>(r32[i]), l16[i] & 0xffff, r16[i] & 0xffff,
                      static_cast<unsigned>(in_l[i]), static_cast<unsigned>(in_r[i]));
        bad++;
      }
    }
  }
  std::printf("bitcrusher%s: %ld blocks (%ld at 44.1 kHz holds, %ld with some wet), %ld mismatching samples\n",
              low_bits ? " (random low bits)" : "", blocks, holds_every, wet_blocks, bad);
#ifdef BC_PROFILE
  std::printf("render, simulator cycles a block (64 stereo samples): avg %lu, max %lu\n",
              static_cast<unsigned long>(cyc_sum / static_cast<unsigned long long>(blocks)),
              static_cast<unsigned long>(cyc_max));
#endif
  return bad ? 1 : 0;
}
