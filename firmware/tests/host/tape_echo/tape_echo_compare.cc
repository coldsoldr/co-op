// The tape echo port's host test (guide/plugin-guidelines.md §7.4 step 7; docs/plugin-performance.md "How a rewrite is
// accepted"): plugins/tape_echo (the coop effect) against te-op1-cfw's own DSP (ports/fx/src/tape_echo,
// ports/fx/op1/half_rate.h, ports/clouds/src/core/halfband) driven the way the kennel plugin drove it
// (ports/knl/tape_echo/tape_echo_knl.cc Process: the knobs through op1_fx_param_q15, the gain through knl::GainQ14,
// hold, the pattern). A straight port must be bit-identical.
//
// One source, two builds (Makefile): -DTE_REF is the reference, without it the coop plugin's coop_fx_t. Both run the
// same seeded scenario, one 64-sample stereo block at a time: noise, sines, impulses and silence, L and R different;
// the four turns walking and jumping (in the filter type's range units, as the core passes them); the gain, the head
// pattern and hold changing; restarts (a pause of over 5 ms, or the effect re-initialised as a sound change does).
// Each build writes every output block (int16 L, R) to a file; the Makefile compares the two.
//
// The one designed difference: coop clears the 133 KB tape over its first kClearBlocks (17) blocks after a restart,
// passing the dry signal through, where the original cleared it in one call (in the audio interrupt). The reference
// build models that: after a restart it passes the next 16 blocks through too, and doesn't feed them to its echo.
//
// In the Blackfin simulator (`make sim`): the coop build runs its assembly kernels, the reference the original's C
// (OP1FX_NO_ASM, OP1CLOUDS_NO_ASM), so the comparison also checks the copied .S files; and the coop build prints the
// render's cycles a block (compute only: no cache misses or SDRAM waits, which the device stats give).
//
// The tempo (e10): both builds run at TEMPO (tenths of a BPM, argv[3], default 1200), or with TEMPO 0 walk through
// kWalk (a change every kWalkBlocks, sync on or off). The coop build gets the core's export through ../tempo_host.cc:
// the IEEE single the stock would hold, through the core's own conversion (core/include/coop_tempo.h). The reference gets
// kennel's TempoIncrement for that same tempo: the export's BPM x 1662149.125, truncated (199457895 at 120, kennel's
// kTempo120).
// So the 120 run is the scenario as before the export, and the others show the DSP takes the tempo as kennel did. The
// coop build also checks NinetySixthQ4 against kennel's quotient at every tenth of a BPM from 20.0 to 400.0 (stderr).
//
//   tape_echo_ref OUT.raw [BLOCKS [TEMPO]]   /   tape_echo_coop OUT.raw [BLOCKS [TEMPO]]
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "coop_tempo.h"   // core/include: the export's conversion (both builds)

#ifdef TE_REF
#include "fx/op1/half_rate.h"
#include "src/tape_echo/tape_echo.h"
namespace op1 { uint32_t host_cycles; }
uint32_t measured_cycles;   // fx_host.h
static uint32_t& clock_now = op1::host_cycles;
#else
#include "coop_plugin.h"
#include "src/tape_echo.h"   // TapeEchoTimeWord, for the division table printout
extern "C" const coop_fx_t FX_tape_echo;
#include "state.h"     // coop_tape_echo::NinetySixthQ4
extern "C" long coop_fx_scratch_check(const coop_fx_t* fx, const int16_t* params, long blocks);   /* ../fx_scratch_host.cc */
extern "C" void coop_tempo_host_set(uint32_t f32_bits);                                              /* ../tempo_host.cc */
uint32_t g_tape_echo_host_cycles;
static uint32_t& clock_now = g_tape_echo_host_cycles;
#endif

namespace {

const int kBlock = 64;
const int kClearBlocks = 17;   // ceil(33217 / 2048): plugins/tape_echo/tape_echo.cc kClearPairs
const int16_t kRangeLo[4] = { 64, 0, 0, 8192 };
const int16_t kRangeHi[4] = { 14528, 24576, 22528, 32767 };

// The tempo walk (TEMPO 0): tenths of a BPM, one every kWalkBlocks.
const int32_t kWalk[] = { 900, 1335, 1740, 1200, 600, 2000, 450 };
const int32_t kWalkN = sizeof(kWalk) / sizeof(kWalk[0]);
const int kWalkBlocks = 1300;

// The IEEE single the stock's SetTempo would store for the tempo (tenths / 10.0f, as its callers compute it).
uint32_t TempoF32(int32_t t10) {
  const float f = static_cast<float>(t10) / 10.0f;
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}
// Kennel's tempo increment for the same tempo: the export's BPM (the core's conversion of that single) x 1662149.125
// (= q16 x 13297193 / 2^19), truncated as the stock's float-to-uint conversion does. Exact at 120: 199457895, kennel's
// kTempo120, so the 120 run is the scenario as it was before the export.
uint32_t KennelIncrement(int32_t t10) {
  const uint64_t q16 = coop_tempo_q16_from_f32(TempoF32(t10));
  return static_cast<uint32_t>(q16 * 13297193u >> 19);
}
// Kennel's NinetySixthQ4 (tape_echo_knl.cc) for an increment.
int32_t KennelNinetySixth(uint32_t inc) { return static_cast<int32_t>(1466015528250LL / inc); }

uint32_t rng = 12345;
uint32_t Rand() {
  rng = rng * 1664525u + 1013904223u;
  return rng >> 8;
}
int32_t RandRange(int32_t lo, int32_t hi) { return lo + static_cast<int32_t>(Rand() % static_cast<uint32_t>(hi - lo + 1)); }

#ifdef TE_REF
// The kennel plugin's glue, with the knl:: reads replaced by the scenario's values.
struct Instance {
  op1fx::TapeEcho echo;
  op1fx::HalfRate half;
};
Instance in;

int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }
int32_t ParamQ15(const int16_t* p, int i) {   // op1_fx_param_q15 with the filter row of the stock range table
  int32_t lo = kRangeLo[i], hi = kRangeHi[i], v = p[i];
  v = (v - lo) * 32767 / (hi - lo);
  return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}
int32_t GainQ14(int32_t word) {   // knl::GainQ14
  static const int32_t kTable[13] = { 8211, 9215, 10338, 11600, 13016, 14605, 16384, 18382, 20626, 23143, 25966, 29135, 32690 };
  const int32_t pos = Clamp15(word) * 12;
  const int32_t i = pos / 32767;
  const int32_t frac = pos - i * 32767;
  if (i >= 12) return kTable[12];
  return kTable[i] + (((kTable[i + 1] - kTable[i]) * frac) / 32767);
}

// Kennel's tempo lock (tape_echo_knl.cc TimeLock::Word, DivisionWord, NinetySixthQ4) with the division the scenario
// picks (coop's sync step - 1) and knl::TempoIncrement as the scenario's tempo (g_inc, set every block).
uint32_t g_inc = 199457895u;   // knl::TempoIncrement: the scenario's tempo (kennel's kTempo120 at 120 BPM)
const int32_t kNumDivisions = 11;
const int32_t kDivision96ths[kNumDivisions] = { 4, 6, 8, 9, 12, 16, 18, 24, 32, 36, 48 };
int32_t LockWord(int32_t div, int32_t knob_word) {
  if (div < 0) return knob_word;
  const int32_t s96 = KennelNinetySixth(g_inc);
  const int32_t word = op1fx::TapeEchoTimeWord(s96 * kDivision96ths[div]);
  return word < 0 ? knob_word : word;
}

int skip = 0;   // blocks still passed through after a restart (coop's tape clear)

void RefInit() {
  for (size_t i = 0; i < sizeof(in) / 4; ++i) reinterpret_cast<uint32_t*>(&in)[i] = 0;   // knl::Zero
  in.echo.Init();
  in.half.Stop();
}

void RefProcess(int16_t* l, int16_t* r, const int16_t* p, int32_t pattern, bool hold) {
  // coop runs the half-rate path's Begin and End in its clearing blocks too (a pause or a re-init then restarts the
  // clear), but not the echo or the filters.
  if (!in.half.Begin(op1::Cycles())) {
    in.echo.Init();
    in.half.End();
    skip = kClearBlocks - 1;
    return;
  }
  if (skip > 0) {
    --skip;
    in.half.End();
    return;
  }
  int32_t v[4];
  for (int k = 0; k < 4; ++k) v[k] = ParamQ15(p, k);
  op1fx::TapeEchoParams q;
  q.time = LockWord(p[6] - 1, v[0]);   // coop's sync step 0 = free, 1..11 = kennel's division 0..10
  q.feedback = v[1];
  q.age = v[2];
  q.heads = pattern;
  q.hold = hold;
  in.half.Decimate(l, r);
  const int32_t g = GainQ14(p[5]);
  int16_t gl[op1fx::kHalfN], gr[op1fx::kHalfN];
  for (int32_t c = 0; c < 2; ++c) {
    const int16_t* src = in.half.in(c);
    int16_t* dst = c ? gr : gl;
    for (int32_t i = 0; i < op1fx::kHalfN; ++i) {
      int32_t y = (src[i] * g) >> 14;
      if (y > 32767) y = 32767;
      if (y < -32768) y = -32768;
      dst[i] = static_cast<int16_t>(y);
    }
  }
  in.echo.Process(gl, gr, in.half.wet(0), in.half.wet(1), op1fx::kHalfN, q);
  in.half.Mix(l, r, v[3]);
  in.half.End();
}
#endif

}  // namespace

#if defined(__bfin__) && !defined(TE_REF)
static inline uint32_t RealCycles() {
  uint32_t c;
  __asm__ __volatile__("%0 = CYCLES;" : "=d"(c));
  return c;
}
#endif

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s OUT.raw [BLOCKS]\n", argv[0]);
    return 2;
  }
  const int blocks = argc > 2 ? std::atoi(argv[2]) : 20000;
  const int32_t tempo = argc > 3 ? std::atoi(argv[3]) : 1200;   // tenths of a BPM; 0 = the walk
#if defined(__bfin__) && !defined(TE_REF)
  {
    uint32_t s;
    __asm__ __volatile__("%0 = SYSCFG;" : "=d"(s));
    s |= 2;   // CCEN: the cycle counter runs
    __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(s));
  }
#endif
  std::FILE* out = std::fopen(argv[1], "wb");
  if (!out) return 2;

  // The knob words, in the filter ranges; words 5..7; the latch word.
  int16_t params[16] = { 0 };
  int32_t knob_q15[4] = { 16384, 14746, 9830, 13107 };   // plugin.json defaults
  int32_t gain = 16384, pattern = 0, sync = 0;
  bool hold = false;
  uint32_t phase_l = 0, phase_r = 0, inc_l = 40000000, inc_r = 61000000;
  int source = 1;

#ifdef TE_REF
  RefInit();
#else
  FX_tape_echo.init(0);
#ifdef __bfin__
  uint64_t cyc_sum = 0;
  uint32_t cyc_max = 0, cyc_n = 0;
#endif
#endif
  int wet_blocks = 0;   // blocks whose output differs from the input: the test is not of a pass-through
  int sync_blocks = 0;  // blocks with sync on

  for (int b = 0; b < blocks; ++b) {
    // ---- the scenario ----
    const int32_t t10 = tempo ? tempo : kWalk[(b / kWalkBlocks) % kWalkN];   // the tempo, set every block
#ifdef TE_REF
    g_inc = KennelIncrement(t10);
#else
    coop_tempo_host_set(TempoF32(t10));
#endif
    if (Rand() % 400 == 0) source = RandRange(0, 4);
    for (int k = 0; k < 4; ++k) {
      const uint32_t e = Rand() % 1000;
      if (e < 3) knob_q15[k] = RandRange(0, 32767);                                  // a jump
      else if (e < 60) knob_q15[k] += RandRange(-400, 400);                          // a walk
      if (knob_q15[k] < 0) knob_q15[k] = 0;
      if (knob_q15[k] > 32767) knob_q15[k] = 32767;
    }
    if (b % 3000 < 600) knob_q15[1] = 30500 + (b % 7) * 300;   // stretches of feedback over 1 (self-oscillation)
    if (Rand() % 300 == 0) gain = RandRange(0, 32767);
    if (Rand() % 500 == 0) pattern = (pattern + 1) % 4;
    if (Rand() % 450 == 0) sync = Rand() % 3 == 0 ? 0 : RandRange(1, 11);   // sync: off, or a division
    if (Rand() % 700 == 0) hold = !hold;
    for (int k = 0; k < 4; ++k) {   // the core's knob_map: Q15 into the filter range
      params[k] = static_cast<int16_t>(kRangeLo[k] + (knob_q15[k] * (kRangeHi[k] - kRangeLo[k])) / 32767);
    }
    params[4] = 0x5300;
    params[5] = static_cast<int16_t>(gain);
    params[6] = static_cast<int16_t>(sync);
    if (sync) ++sync_blocks;
    params[7] = static_cast<int16_t>(pattern);
    params[8] = hold ? 1 : 0;

    int16_t l16[kBlock], r16[kBlock];
    for (int i = 0; i < kBlock; ++i) {
      int32_t a = 0, c = 0;
      switch (source) {
        case 0: break;                                                                  // silence
        case 1: a = static_cast<int32_t>(Rand() % 65536) - 32768; c = static_cast<int32_t>(Rand() % 65536) - 32768;
                a >>= 1; c >>= 2; break;                                                // noise, L != R
        case 2: phase_l += inc_l; phase_r += inc_r;                                     // two saws
                a = static_cast<int16_t>(phase_l >> 16) >> 1; c = static_cast<int16_t>(phase_r >> 16) >> 1; break;
        case 3: if (Rand() % 3000 == 0) a = c = 30000; break;                           // impulses
        case 4: a = static_cast<int32_t>(Rand() % 65536) - 32768; c = a; break;         // full-scale noise, L == R
      }
      l16[i] = static_cast<int16_t>(a);
      r16[i] = static_cast<int16_t>(c);
    }

    // Restarts: a pause (bypass) now and then, or a re-init (a sound change).
    const uint32_t ev = Rand() % 2500;
    if (ev == 0) clock_now += 3000000;                     // over 5 ms: both restart
#ifdef TE_REF
    if (ev == 1) RefInit();
#else
    if (ev == 1) FX_tape_echo.init(0);
#endif
    clock_now += 20000;

    int16_t in_l[kBlock], in_r[kBlock];
    for (int i = 0; i < kBlock; ++i) { in_l[i] = l16[i]; in_r[i] = r16[i]; }
    // ---- the effect ----
#ifdef TE_REF
    RefProcess(l16, r16, params, pattern, hold);
#else
    int32_t l[kBlock], r[kBlock];
    for (int i = 0; i < kBlock; ++i) {
      l[i] = static_cast<int32_t>(l16[i]) << 16;
      r[i] = static_cast<int32_t>(r16[i]) << 16;
    }
#ifdef __bfin__
    const uint32_t t0 = RealCycles();
#endif
    FX_tape_echo.render(0, l, r, params, kBlock);
#ifdef __bfin__
    const uint32_t dt = RealCycles() - t0;
    if (b > 100) {
      cyc_sum += dt;
      ++cyc_n;
      if (dt > cyc_max) cyc_max = dt;
    }
#endif
    for (int i = 0; i < kBlock; ++i) {
      l16[i] = static_cast<int16_t>(l[i] >> 16);
      r16[i] = static_cast<int16_t>(r[i] >> 16);
      if ((l[i] & 0xffff) || (r[i] & 0xffff)) {
        std::fprintf(stderr, "block %d: low bits set\n", b);
        return 1;
      }
    }
#endif
    for (int i = 0; i < kBlock; ++i) {
      if (l16[i] != in_l[i] || r16[i] != in_r[i]) { ++wet_blocks; break; }
    }
    int16_t pair[2 * kBlock];
    for (int i = 0; i < kBlock; ++i) {
      pair[2 * i] = l16[i];
      pair[2 * i + 1] = r16[i];
    }
    std::fwrite(pair, sizeof(pair), 1, out);
  }
  std::fclose(out);
  std::printf("%d of %d blocks changed by the effect, %d with sync on\n", wet_blocks, blocks, sync_blocks);
  // The sync divisions' time words at the run's tempo (each walk tempo for TEMPO 0), from each build's own 96th note
  // (the reference kennel's quotient of the increment, coop the export through NinetySixthQ4) and TapeEchoTimeWord:
  // the Makefile compares the two printouts. At 120 BPM a 96th is 7350 Q4 samples at 22050 Hz.
  static const int32_t k96ths[11] = { 4, 6, 8, 9, 12, 16, 18, 24, 32, 36, 48 };   // kennel's kDivision96ths
  for (int w = 0; w < (tempo ? 1 : kWalkN); ++w) {
    const int32_t t10 = tempo ? tempo : kWalk[w];
#ifdef TE_REF
    const int32_t s96 = KennelNinetySixth(KennelIncrement(t10));
#else
    coop_tempo_host_set(TempoF32(t10));
    const int32_t s96 = coop_tape_echo::NinetySixthQ4(coop_tempo_bpm_q16());
#endif
    std::printf("sync words at %d.%d BPM (96th %d):", static_cast<int>(t10 / 10), static_cast<int>(t10 % 10),
                static_cast<int>(s96));
    for (int d = 0; d < 11; ++d) std::printf(" %d", static_cast<int>(op1fx::TapeEchoTimeWord(s96 * k96ths[d])));
    std::printf("\n");
  }
#ifndef TE_REF
  {   // The export's conversion and NinetySixthQ4 against kennel's quotient at every tenth of a BPM, 20.0 .. 400.0
      // (stderr): equal to kennel's with its increment truncated; against the increment as the stock computes it (bpm x
      // 1662149.125 in single precision, truncated), the count that differ (by one Q4 unit: kennel's rounding error).
    int bad = 0, stock_off = 0, n = 0;
    for (int32_t t10 = 200; t10 <= 4000; ++t10, ++n) {
      coop_tempo_host_set(TempoF32(t10));
      const uint32_t q16 = coop_tempo_bpm_q16();
      const int32_t ours = coop_tape_echo::NinetySixthQ4(q16);
      if (ours != KennelNinetySixth(KennelIncrement(t10))) {
        if (bad++ < 5) std::fprintf(stderr, "tempo %d: 96th %d, kennel %d\n", (int)t10, (int)ours,
                                    (int)KennelNinetySixth(KennelIncrement(t10)));
      }
      float f;
      const uint32_t fb = TempoF32(t10);
      std::memcpy(&f, &fb, 4);
      const uint32_t stock_inc = static_cast<uint32_t>(static_cast<float>(f * 1662149.125f));
      if (ours != KennelNinetySixth(stock_inc)) ++stock_off;
    }
    coop_tempo_host_set(0);                                     // unknown: 120
    const bool fallback_ok = coop_tempo_bpm_q16() == (120u << 16);
    coop_tempo_host_set(TempoF32(4010));                        // over 400: 120
    const bool over_ok = coop_tempo_bpm_q16() == (120u << 16);
    coop_tempo_host_set(TempoF32(1335));
    const bool q16_ok = coop_tempo_bpm_q16() == 8749056u;       // 133.5 x 65536
    std::fprintf(stderr, "tempo export: %d of %d tempos (20.0..400.0) give kennel's 96th, %d differ by kennel's single-"
                 "precision increment on the device; fallback %s, over 400 %s, 133.5 = %s\n", n - bad, n, stock_off,
                 fallback_ok ? "120" : "WRONG", over_ok ? "120" : "WRONG", q16_ok ? "8749056" : "WRONG");
    if (bad || !fallback_ok || !over_ok || !q16_ok) return 1;
  }
#endif
#if defined(__bfin__) && !defined(TE_REF)
  std::printf("tape_echo render, simulator compute: avg %lu, max %lu cycles a block over %lu blocks\n",
              static_cast<unsigned long>(cyc_sum / (cyc_n ? cyc_n : 1)), static_cast<unsigned long>(cyc_max),
              static_cast<unsigned long>(cyc_n));
#endif
#ifndef TE_REF
  {   /* e8: two positions at once and a restart on a dirty position buffer (tests/host/fx_scratch_host.cc; stderr) */
    int16_t sp[COOP_FX_PARAMS_MIN] = { 5000, 18000, 6000, 26000, 0x5300 + 32, 20000, 0, 1 };
    if (coop_fx_scratch_check(&FX_tape_echo, sp, 1500)) return 1;
  }
#endif
  return 0;
}
