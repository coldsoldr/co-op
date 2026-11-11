// The ensemble port's test: plugins/ensemble's render (FX_ensemble, the coop ABI) against te-op1-cfw's kennel glue
// (ports/knl/ensemble/ensemble_knl.cc's Process, on its own DSP: ports/fx/src/ensemble, ports/clouds halfband,
// ports/fx/op1/half_rate.h), block for block over random scenarios, outputs compared bit for bit.
//
//   host (make):        coop's C kernels against the upstream C kernels;
//   simulator (make sim): coop's Blackfin kernels (what the device runs) against the upstream C kernels, in
//                       bfin-elf-run, with the cycles of each coop render (no cache or SDRAM model).
//
// Scenarios, the state carried throughout: stereo noise and full-scale edges with the knobs and the gain word
// jumping and sweeping (in the filter type's range units, out-of-range words too); stretches of L == R (coop's
// shared decimator pass); re-inits (selecting the effect again); on the host, pauses over 5 ms (the restart path).
// Also every value of the parameter curves and the soft-limit table against the upstream's.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---- upstream (te-op1-cfw) ----
#include "fx/op1/half_rate.h"
#include "fx/src/ensemble/ensemble.h"

// ---- coop ----
#include "coop_plugin.h"
#include "src/ensemble.h"
#include "src/cycles.h"
#include "src/ensemble_tables.h"

extern "C" const coop_fx_t FX_ensemble;
extern "C" long coop_fx_scratch_check(const coop_fx_t* fx, const int16_t* params, long blocks);   /* ../fx_scratch_host.cc */

#ifndef __bfin__
uint32_t op1::host_cycles;
uint32_t coop_ensemble::host_cycles;
#endif

namespace {

// The kennel glue, as ensemble_knl.cc has it, with knl::Page1 (op1_fx_param_q15 over the filter type's ranges) and
// knl::GainQ14 written out (te-op1-cfw ports/knl/common/knl_fx.h).
const int32_t kLo[4] = { 64, 0, 0, 8192 };
const int32_t kHi[4] = { 14528, 24576, 22528, 32767 };

int32_t ParamQ15(const int16_t* p, int i) {
  int32_t v = p[i];
  v = (v - kLo[i]) * 32767 / (kHi[i] - kLo[i]);
  return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}
int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }
int32_t KnlGainQ14(int32_t word) {
  static const int32_t kTable[13] = { 8211, 9215, 10338, 11600, 13016, 14605, 16384, 18382, 20626, 23143, 25966, 29135, 32690 };
  const int32_t pos = Clamp15(word) * 12;
  const int32_t i = pos / 32767;
  const int32_t frac = pos - i * 32767;
  if (i >= 12) return kTable[12];
  return kTable[i] + (((kTable[i + 1] - kTable[i]) * frac) / 32767);
}
int16_t Sat16(int32_t x) { return static_cast<int16_t>(x > 32767 ? 32767 : (x < -32768 ? -32768 : x)); }

struct RefInstance {
  op1fx::Ensemble ensemble;
  op1fx::HalfRate half;
};
RefInstance ref;

void RefInit() {
  std::memset(&ref, 0, sizeof(ref));   // knl::Zero
  ref.ensemble.Init();
  ref.half.Stop();
}

void RefGain(const int16_t* buf, int32_t gain_q14) {
  if (gain_q14 == 16384) return;
  int16_t* x = const_cast<int16_t*>(buf);
  for (int32_t i = 0; i < op1fx::kHalfN; ++i) x[i] = Sat16((x[i] * gain_q14) >> 14);
}

void RefProcess(int16_t* l, int16_t* r, const int16_t* p) {
  RefInstance* in = &ref;
  if (!in->half.Begin(op1::Cycles())) {
    in->ensemble.Init();
    in->half.End();
    return;
  }
  int32_t v[4];
  for (int k = 0; k < 4; ++k) v[k] = ParamQ15(p, k);
  op1fx::EnsembleParams q;
  q.rate = v[0];
  q.depth = v[1];
  q.voices = v[2];
  const int32_t gain = KnlGainQ14(Clamp15(p[5]));
  in->half.Decimate(l, r);
  RefGain(in->half.in(0), gain);
  if (r) RefGain(in->half.in(1), gain);
  in->ensemble.Process(in->half.in(0), r ? in->half.in(1) : nullptr, in->half.wet(0), r ? in->half.wet(1) : nullptr,
                       op1fx::kHalfN, q);
  in->half.Mix(l, r, v[3]);
  in->half.End();
}

uint32_t rng = 0x2545F491u;
uint32_t Next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
int32_t Range(int32_t lo, int32_t hi) { return lo + static_cast<int32_t>(Next() % static_cast<uint32_t>(hi - lo + 1)); }

#ifdef __bfin__
uint32_t ReadCycles() { uint32_t c; __asm__ __volatile__("%0 = CYCLES;" : "=d"(c)); return c; }
void EnableCycles() {
  uint32_t s;
  __asm__ __volatile__("%0 = SYSCFG;" : "=d"(s));
  s |= 2;
  __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(s));
}
#endif

int Fail(const char* what) { std::printf("FAIL: %s\n", what); return 1; }

int CheckCurves() {
  for (int32_t x = -100; x < 32868; ++x) {
    if (op1fx::EnsembleRateIncrement(x) != coop_ensemble::EnsembleRateIncrement(x)) return Fail("rate increment");
    if (op1fx::EnsembleSlowCap(x) != coop_ensemble::EnsembleSlowCap(x)) return Fail("slow cap");
    int32_t a[3], b[3];
    op1fx::EnsembleGains(x, a);
    coop_ensemble::EnsembleGains(x, b);
    if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) return Fail("gains");
  }
  if (op1fx::kEnsembleFastIncrement != coop_ensemble::kEnsembleFastIncrement) return Fail("fast increment");
  op1clouds::SoftLimitTable t;
  op1clouds::InitSoftLimitTable(&t);
  if (std::memcmp(&t, &coop_ensemble::kSoftLimit, sizeof(t)) != 0) return Fail("soft-limit table");
  if (std::memcmp(op1clouds::kXfadeInQ31, coop_ensemble::kXfadeInQ31, sizeof(coop_ensemble::kXfadeInQ31)) ||
      std::memcmp(op1clouds::kXfadeOutQ31, coop_ensemble::kXfadeOutQ31, sizeof(coop_ensemble::kXfadeOutQ31)))
    return Fail("crossfade tables");
  std::printf("curves, increments and tables: equal\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const int blocks = argc > 1 ? std::atoi(argv[1]) : 40000;
#ifdef __bfin__
  EnableCycles();
  const bool pauses = false;   // the simulator's CYCLES is real: no simulated pause
#else
  const bool pauses = true;
#endif
  if (CheckCurves()) return 1;

  RefInit();
  FX_ensemble.init(0);
  int16_t p[16] = { 9830, 16384, 16384, 16384, 0x5301, 16384, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  // The defaults in range units (what the core hands for the Q15 defaults).
  for (int k = 0; k < 4; ++k) p[k] = static_cast<int16_t>(kLo[k] + (p[k] * (kHi[k] - kLo[k])) / 32767);
  int16_t l[64], r[64];
  int32_t L[64], R[64];
  long bad = 0, restarts = 0, inits = 0, mono_blocks = 0;
  int mode = 0, mode_left = 0, sweep = -1;
  uint64_t cyc_sum = 0;
  uint32_t cyc_max = 0;
  long cyc_n = 0;
  int32_t ph = 0;
  for (int b = 0; b < blocks; ++b) {
    if (mode_left-- <= 0) {          // a new stretch: 0 stereo noise, 1 L == R noise, 2 a stereo saw pair, 3 edges, 4 silence
      mode = Range(0, 9);
      mode = mode < 4 ? 0 : (mode < 7 ? 1 : (mode == 7 ? 2 : (mode == 8 ? 3 : 4)));
      mode_left = Range(20, 400);
      sweep = Range(0, 6) == 0 ? Range(0, 5) : -1;
    }
    // Knobs: jumps now and then (out of range too), a slow sweep of one word sometimes.
    if (Range(0, 60) == 0) {
      const int k = Range(0, 4);
      const int idx = k == 4 ? 5 : k;
      p[idx] = Range(0, 9) == 0 ? static_cast<int16_t>(Next()) : static_cast<int16_t>(k == 4 ? Range(0, 32767) : Range(kLo[k], kHi[k]));
    }
    if (sweep >= 0) {
      const int idx = sweep == 4 ? 5 : (sweep == 5 ? 2 : sweep);
      p[idx] = static_cast<int16_t>(p[idx] + Range(-200, 200));
    }
    for (int i = 0; i < 64; ++i) {
      int16_t a, c;
      switch (mode) {
        case 0: a = static_cast<int16_t>(Next() >> 16); c = static_cast<int16_t>(Next() >> 16); break;
        case 1: a = c = static_cast<int16_t>(Next() >> 17); break;
        case 2: ph += 300; a = static_cast<int16_t>(ph); c = static_cast<int16_t>(ph * 3 / 2); break;
        case 3: { const int16_t e[4] = { -32768, 32767, 0, -1 }; a = e[Next() & 3]; c = e[Next() & 3]; } break;
        default: a = c = 0; break;
      }
      l[i] = a; r[i] = c;
      L[i] = static_cast<int32_t>(a) << 16; R[i] = static_cast<int32_t>(c) << 16;
    }
    if (mode == 1) ++mono_blocks;
    if (Range(0, 2000) == 0) { RefInit(); (void)Range(0, 4); FX_ensemble.init(0); ++inits; }   /* e8: the slot rendered */
#ifndef __bfin__
    const uint32_t step = (pauses && Range(0, 1500) == 0) ? 3000000u : 580499u;
    if (step > 2000000u) ++restarts;
    op1::host_cycles += step;
    coop_ensemble::host_cycles = op1::host_cycles;
#endif
    RefProcess(l, r, p);
#ifdef __bfin__
    const uint32_t c0 = ReadCycles();
#endif
    FX_ensemble.render(0, L, R, p, 64);
#ifdef __bfin__
    const uint32_t c = ReadCycles() - c0;
    if (b > 10) { cyc_sum += c; if (c > cyc_max) cyc_max = c; ++cyc_n; }
#endif
    for (int i = 0; i < 64; ++i) {
      if (L[i] != (static_cast<int32_t>(l[i]) << 16) || R[i] != (static_cast<int32_t>(r[i]) << 16)) {
        if (bad < 5) std::printf("block %d sample %d mode %d: ref %d %d coop %ld %ld\n", b, i, mode, l[i], r[i],
                                 (long)(L[i] >> 16), (long)(R[i] >> 16));
        ++bad;
      }
    }
  }
  (void)pauses; (void)cyc_sum; (void)cyc_max; (void)cyc_n;
  std::printf("ensemble: %d blocks (%ld with L == R, %ld re-inits, %ld pauses): %ld differing samples\n", blocks,
              mono_blocks, inits, restarts, bad);
#ifdef __bfin__
  if (cyc_n) std::printf("coop render (simulator, no cache model): mean %lu, max %lu cycles a block\n",
                         (unsigned long)(cyc_sum / cyc_n), (unsigned long)cyc_max);
#endif
  if (bad) return 1;
  std::printf("ensemble: coop == te-op1-cfw, bit-identical\n");
  {   /* e8: two positions at once and a restart on a dirty position buffer (tests/host/fx_scratch_host.cc) */
    int16_t sp[COOP_FX_PARAMS_MIN] = { 9000, 15000, 6000, 28000, 0x5300 + 32, 20000 };
    if (coop_fx_scratch_check(&FX_ensemble, sp, 1500)) return 1;
  }
  return 0;
}
