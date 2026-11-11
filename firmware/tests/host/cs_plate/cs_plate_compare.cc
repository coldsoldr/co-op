// cs_plate's host test: plugins/cs_plate against te-op1-cfw's plate (ports/fx/src/plate) behind its kennel glue's
// half-rate path (ports/fx/op1/half_rate.h), bit for bit.
//
// 1. Tables: the plugin's integer tables against what the upstream builds (constexpr double, Clouds' tables, the
//    soft-limit table its first Begin builds): the size table entry by entry, the damping and low-cut tables through
//    PlateControl at every Q15 input (0..32767), the crossfade and soft-limit tables.
// 2. The chain: FX_cs_plate.render against the upstream PlateReverb + HalfRate driven as plate_knl.cc does, over
//    random segments (each starts with an init: the plugin's chunked clear against the upstream's Init), random
//    blocks of noise, saw, bursts and silence (L != R), the turns in range units moving at random, size steps,
//    diffusion, motion, freeze toggles. The plugin's int32 output >> 16 must equal the upstream's int16, its low half 0;
//    during the clear the plugin must leave the block untouched.
//
// Host (C kernels on both sides) and the GNU Blackfin simulator (the plugin's assembly against the upstream's C:
// OP1FX_NO_ASM, OP1CLOUDS_NO_ASM), see Makefile. Argument: the number of segments (default 40).
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "coop_plugin.h"
// The plugin's own headers (plugins/cs_plate, namespace csplate).
#include "half_rate.h"
#include "plate.h"
// The upstream (te-op1-cfw ports, namespaces op1fx / op1clouds).
#include "fx/op1/half_rate.h"
#include "src/core/clouds_tables.h"
#include "src/core/halfband.h"
#include "src/plate/plate.h"

extern "C" const coop_fx_t FX_cs_plate;

namespace op1 {
uint32_t host_cycles;   // OP1_HOST_SIM: half_rate.h's clock; never moves, so the upstream never sees a pause
}
namespace op1fx {
uint32_t measured_cycles;
}

namespace {

int failures;

#define CHECK(cond, ...)                 \
  do {                                   \
    if (!(cond)) {                       \
      if (failures < 20) {               \
        printf("FAIL: " __VA_ARGS__);    \
        printf("\n");                    \
      }                                  \
      ++failures;                        \
    }                                    \
  } while (0)

uint32_t rng = 0x12345678u;
uint32_t Rand() {
  rng = rng * 1664525u + 1013904223u;
  return rng;
}
int32_t RandRange(int32_t lo, int32_t hi) {   // inclusive
  return lo + static_cast<int32_t>((Rand() >> 8) % static_cast<uint32_t>(hi - lo + 1));
}

#if defined(__bfin__)
inline uint32_t Cycles() {
  uint32_t c;
  __asm__ __volatile__("%0 = CYCLES;" : "=d"(c));
  return c;
}
#endif

// --- 1. Tables ---

void CheckTables() {
  for (int s = 0; s < 8; ++s) {
    const csplate::PlateSizeTable& a = csplate::kPlateSizes[s];
    const op1fx::PlateSizeTable& b = op1fx::kPlateSizes[s];
    bool same = a.a1 == b.a1 && a.a3 == b.a3;
    for (int j = 0; j < 6; ++j) same = same && a.tail[j] == b.tail[j];
    for (int j = 0; j < 7; ++j) same = same && a.tap_l[j] == b.tap_l[j] && a.tap_r[j] == b.tap_r[j];
    CHECK(same, "size table %d", s);
  }
  for (int j = 0; j < 7; ++j) CHECK(csplate::kPlateTapSign[j] == op1fx::kPlateTapSign[j], "tap sign %d", j);

  // Damping and low cut through the control's first call (the targets are taken as they are), every input.
  int coef_diffs = 0;
  for (int32_t x = 0; x <= 32767; ++x) {
    csplate::PlateControl a;
    op1fx::PlateControl b;
    a.Init();
    b.Init();
    csplate::PlateParams pa = {x, x, x, static_cast<int32_t>(x & 7), x, x, x, (x & 1) != 0};
    op1fx::PlateParams pb = {x, x, x, static_cast<int32_t>(x & 7), x, x, x, (x & 1) != 0};
    csplate::PlateCoefs ca;
    op1fx::PlateCoefs cb;
    a.Update(pa, 32, &ca);
    b.Update(pb, 32, &cb);
    const bool same = ca.decay == cb.decay && ca.dd1 == cb.dd1 && ca.dd2 == cb.dd2 && ca.klp == cb.klp &&
                      ca.g1 == cb.g1 && ca.g2 == cb.g2 && ca.khp == cb.khp && ca.gain0 == cb.gain0 &&
                      ca.gain_step == cb.gain_step && ca.pre0 == cb.pre0 && ca.pre_step == cb.pre_step &&
                      ca.mod0[0] == cb.mod0[0] && ca.mod0[1] == cb.mod0[1] &&
                      ca.mod_step[0] == cb.mod_step[0] && ca.mod_step[1] == cb.mod_step[1] &&
                      ca.size == cb.size;
    if (!same) ++coef_diffs;
  }
  CHECK(coef_diffs == 0, "control coefficients differ at %d of 32768 inputs", coef_diffs);
  // Damping off at freeze hides klp; run the damp sweep without freeze too.
  int klp_diffs = 0;
  for (int32_t x = 0; x <= 32767; ++x) {
    csplate::PlateControl a;
    op1fx::PlateControl b;
    a.Init();
    b.Init();
    csplate::PlateParams pa = {0, x, 0, 7, 0, 0, x, false};
    op1fx::PlateParams pb = {0, x, 0, 7, 0, 0, x, false};
    csplate::PlateCoefs ca;
    op1fx::PlateCoefs cb;
    a.Update(pa, 32, &ca);
    b.Update(pb, 32, &cb);
    if (ca.klp != cb.klp || ca.khp != cb.khp) ++klp_diffs;
  }
  CHECK(klp_diffs == 0, "damping / low cut coefficients differ at %d inputs", klp_diffs);

  for (int i = 0; i < 18; ++i) {
    CHECK(csplate::kXfadeInQ31[i] == op1clouds::kXfadeInQ31[i], "xfade in %d", i);
    CHECK(csplate::kXfadeOutQ31[i] == op1clouds::kXfadeOutQ31[i], "xfade out %d", i);
  }
  static op1clouds::SoftLimitTable sl;
  op1clouds::InitSoftLimitTable(&sl);
  for (int i = 0; i < 257; ++i) CHECK(csplate::kSoftLimit.value[i] == sl.value[i], "soft limit value %d", i);
  for (int i = 0; i < 256; ++i) CHECK(csplate::kSoftLimit.step[i] == sl.step[i], "soft limit step %d", i);
  printf("tables: size 8 x 22, control at 2 x 32768 inputs, crossfade 2 x 18, soft limit 513: %s\n",
         failures ? "DIFFER" : "identical");
}

// --- 2. The chain ---

// The upstream as plate_knl.cc runs it (Init, Process), at one position, stereo.
struct Ref {
  op1fx::PlateReverb plate;
  op1fx::HalfRate half;
};
Ref ref;

const int16_t kLo[4] = { 64, 0, 0, 8192 };
const int16_t kHi[4] = { 14528, 24576, 22528, 32767 };

// kennel's op1_fx_param_q15 with the filter type's table (the upstream side's input).
int32_t Q15(int32_t v, int k) {
  v = (v - kLo[k]) * 32767 / (kHi[k] - kLo[k]);
  return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}

struct Knobs {
  int16_t p[16];
};

void RandomKnobs(Knobs* k, bool all) {
  for (int i = 0; i < 4; ++i) {
    if (all || ((Rand() >> 16) & 3) == 0) {
      // Mostly in range; sometimes outside (the plugin clamps as the upstream's mapping does).
      k->p[i] = static_cast<int16_t>(((Rand() >> 16) & 15) == 0 ? RandRange(-32768, 32767) : RandRange(kLo[i], kHi[i]));
    }
  }
  if (all || ((Rand() >> 16) & 7) == 0) k->p[5] = static_cast<int16_t>(((Rand() >> 16) & 31) == 0 ? RandRange(-5, 20) : RandRange(0, 7));
  if (all || ((Rand() >> 16) & 3) == 0) k->p[6] = static_cast<int16_t>(RandRange(0, 32767));
  if (all || ((Rand() >> 16) & 3) == 0) k->p[7] = static_cast<int16_t>(RandRange(0, 32767));
  if (all || ((Rand() >> 16) & 15) == 0) k->p[COOP_FX_LATCH] = static_cast<int16_t>((Rand() >> 16) & 1);
}

int32_t Clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The upstream side of one block (plate_knl.cc Process), from the same knobs.
void RefBlock(int16_t* l, int16_t* r, const Knobs& k) {
  if (!ref.half.Begin(op1::host_cycles)) {
    ref.plate.Init();
    ref.half.End();
    return;
  }
  op1fx::PlateParams q;
  q.decay = Q15(k.p[0], 0);
  q.damp = Q15(k.p[1], 1);
  q.predelay = Q15(k.p[2], 2);
  q.size = Clamp(k.p[5], 0, 7);   // kennel quantised word 5 into this; coop hands the step
  q.diffusion = Clamp(k.p[6], 0, 32767);
  q.motion = Clamp(k.p[7], 0, 32767);
  q.lowcut = 3277;
  q.freeze = (k.p[COOP_FX_LATCH] & 1) != 0;
  ref.half.Decimate(l, r);
  ref.plate.Process(ref.half.in(0), ref.half.in(1), ref.half.wet(0), ref.half.wet(1), op1fx::kHalfN, q);
  ref.half.Mix(l, r, Q15(k.p[3], 3));
  ref.half.End();
}

struct Source {
  int kind;            // 0 noise, 1 saw, 2 bursts, 3 silence
  int32_t phase_l, phase_r, inc_l, inc_r, amp;
  int32_t t;
  void Pick() {
    kind = RandRange(0, 3);
    inc_l = RandRange(200000, 30000000);
    inc_r = RandRange(200000, 30000000);
    amp = RandRange(500, 32767);
    t = 0;
  }
  void Fill(int16_t* l, int16_t* r) {
    for (int i = 0; i < 64; ++i, ++t) {
      int32_t a = 0, b = 0;
      switch (kind) {
        case 0:
          a = static_cast<int16_t>(Rand() >> 16);
          b = static_cast<int16_t>(Rand() >> 16);
          break;
        case 1:
          phase_l += inc_l;
          phase_r += inc_r;
          a = phase_l >> 16;
          b = phase_r >> 16;
          break;
        case 2:
          if ((t & 4095) < 300) {
            a = static_cast<int16_t>(Rand() >> 16);
            b = static_cast<int16_t>(Rand() >> 16);
          }
          break;
        default:
          break;
      }
      l[i] = static_cast<int16_t>((a * amp) >> 15);
      r[i] = static_cast<int16_t>((b * amp) >> 15);
    }
  }
};

}  // namespace

int main(int argc, char** argv) {
  const int segments = argc > 1 ? atoi(argv[1]) : 40;
  CheckTables();

  static int16_t in_l[64], in_r[64], ref_l[64], ref_r[64];
  static int32_t co_l[64], co_r[64];
  Knobs k = {};
  Source src = {};
  src.Pick();
  long compared = 0, clearing = 0, wet_blocks = 0, size_changes = 0, freezes = 0;
  int clear_blocks = 0;
#if defined(__bfin__)
  uint64_t cyc_sum = 0;
  uint32_t cyc_max = 0, clr_max = 0, first_max = 0;
  long cyc_n = 0;
  uint32_t syscfg;   // the cycle counter on (CCEN), as te-op1-cfw's harness/cycles.h
  __asm__ __volatile__("%0 = SYSCFG;" : "=d"(syscfg));
  syscfg |= 2;
  __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(syscfg));
#endif
  for (int seg = 0; seg < segments; ++seg) {
    // A restart at both: init at the plugin, the host's stop at the upstream.
    CHECK(FX_cs_plate.init(seg % 5) == 0, "init");
    ref.half.Stop();
    RandomKnobs(&k, true);
    // The upstream's restart block passes its input through, as do the plugin's clear blocks.
    src.Fill(ref_l, ref_r);
    RefBlock(ref_l, ref_r, k);
    int n_clear = 0;
    for (;;) {
      src.Fill(in_l, in_r);
      for (int i = 0; i < 64; ++i) {
        // Low bits set: a processed block has them 0, so it can't look untouched (silence in, silence out).
        co_l[i] = in_l[i] * 65536 + static_cast<int32_t>((Rand() & 0xffff) | 1);
        co_r[i] = in_r[i] * 65536;
      }
      int32_t keep_l[64], keep_r[64];
      for (int i = 0; i < 64; ++i) {
        keep_l[i] = co_l[i];
        keep_r[i] = co_r[i];
      }
#if defined(__bfin__)
      const uint32_t c0 = Cycles();
      FX_cs_plate.render(seg % 5, co_l, co_r, k.p, 64);
      const uint32_t dc = Cycles() - c0;
#else
      FX_cs_plate.render(seg % 5, co_l, co_r, k.p, 64);
#endif
      bool untouched = true;
      for (int i = 0; i < 64; ++i) untouched = untouched && co_l[i] == keep_l[i] && co_r[i] == keep_r[i];
      if (!untouched) {   // the first processed block (the reset and a render): compare it below
#if defined(__bfin__)
        if (dc > first_max) first_max = dc;
#endif
        break;
      }
#if defined(__bfin__)
      if (dc > clr_max) clr_max = dc;
#endif
      ++n_clear;
      ++clearing;
      CHECK(n_clear < 64, "the clear never ends");
      if (n_clear >= 64) return 1;
    }
    clear_blocks = n_clear;
    // That first processed block, now on the upstream side from the same input (the plugin's input had low bits on
    // odd segments: the plugin takes >> 16, as the int16 bus would be).
    const int blocks = RandRange(100, 3000);
    for (int b = 0; b < blocks; ++b) {
      if (b > 0) {
        if (((Rand() >> 8) % 97) == 0) src.Pick();
        const int16_t size_before = k.p[5], freeze_before = k.p[COOP_FX_LATCH];
        if (((Rand() >> 8) % 23) == 0) RandomKnobs(&k, false);
        if (Clamp(k.p[5], 0, 7) != Clamp(size_before, 0, 7)) ++size_changes;
        if (k.p[COOP_FX_LATCH] != freeze_before) ++freezes;
        src.Fill(in_l, in_r);
        for (int i = 0; i < 64; ++i) {
          co_l[i] = in_l[i] * 65536 + static_cast<int32_t>(Rand() & 0xffff) * (seg & 1);
          co_r[i] = in_r[i] * 65536;
        }
#if defined(__bfin__)
        const uint32_t c0 = Cycles();
        FX_cs_plate.render(seg % 5, co_l, co_r, k.p, 64);
        const uint32_t dc = Cycles() - c0;
        cyc_sum += dc;
        if (dc > cyc_max) cyc_max = dc;
        ++cyc_n;
#else
        FX_cs_plate.render(seg % 5, co_l, co_r, k.p, 64);
#endif
      }
      for (int i = 0; i < 64; ++i) {
        ref_l[i] = in_l[i];
        ref_r[i] = in_r[i];
      }
      RefBlock(ref_l, ref_r, k);
      bool same = true, wet = false;
      for (int i = 0; i < 64; ++i) {
        same = same && (co_l[i] >> 16) == ref_l[i] && (co_r[i] >> 16) == ref_r[i] && (co_l[i] & 0xffff) == 0 &&
               (co_r[i] & 0xffff) == 0;
        wet = wet || ref_l[i] != in_l[i] || ref_r[i] != in_r[i];
      }
      CHECK(same, "segment %d block %d differs", seg, b);
      if (!same) break;
      ++compared;
      if (wet) ++wet_blocks;
    }
  }
  printf("chain: %d segments, %ld blocks compared (%ld changed by the effect), %ld clear blocks (%d a restart), "
         "%ld size changes, %ld freeze toggles: %s\n",
         segments, compared, wet_blocks, clearing, clear_blocks, size_changes, freezes,
         failures ? "DIFFER" : "bit-identical");
#if defined(__bfin__)
  printf("render cycles (GNU simulator, no cache or SDRAM model): avg %lu, max %lu over %ld blocks; a clear block "
         "max %lu, the first processed block (reset + render) max %lu\n",
         static_cast<unsigned long>(cyc_n ? cyc_sum / cyc_n : 0), static_cast<unsigned long>(cyc_max), cyc_n,
         static_cast<unsigned long>(clr_max), static_cast<unsigned long>(first_max));
#endif
#if defined(__bfin__)
  // Steady-state profile: still knobs (the control's fast path), noise in, each stage on its own.
  {
    static csplate::PlateReverb pr;
    static csplate::HalfRate hr;
    pr.ClearBuffers();
    pr.Reset();
    hr.Init();
    csplate::PlateParams q = {16384, 9830, 3277, 4, 22938, 9830, 3277, false};
    uint64_t t_render = 0, t_dec = 0, t_plate = 0, t_mix = 0, t_conv = 0;
    const int kBlocks = 1500, kWarm = 100;
    for (int i = 0; i < 16; ++i) k.p[i] = 0;
    k.p[0] = 7296; k.p[1] = 7372; k.p[2] = 2253; k.p[3] = 16982;   // ~ the defaults in range units
    k.p[5] = 4; k.p[6] = 22938; k.p[7] = 9830; k.p[COOP_FX_LATCH] = 0;
    FX_cs_plate.init(0);
    for (int b = 0; b < kBlocks + 20; ++b) {
      src.kind = 0;
      src.amp = 12000;
      src.Fill(in_l, in_r);
      for (int i = 0; i < 64; ++i) {
        co_l[i] = in_l[i] * 65536;
        co_r[i] = in_r[i] * 65536;
      }
      uint32_t c0 = Cycles();
      FX_cs_plate.render(0, co_l, co_r, k.p, 64);
      uint32_t c1 = Cycles();
      if (b >= 20 + kWarm) t_render += c1 - c0;
      // The stages on a second instance.
      for (int i = 0; i < 64; ++i) {
        ref_l[i] = in_l[i];
        ref_r[i] = in_r[i];
      }
      c0 = Cycles();
      hr.Decimate(ref_l, ref_r);
      c1 = Cycles();
      pr.Process(hr.in(0), hr.in(1), hr.wet(0), hr.wet(1), 32, q);
      const uint32_t c2 = Cycles();
      hr.Mix(ref_l, ref_r, 11469);
      const uint32_t c3 = Cycles();
      for (int i = 0; i < 64; ++i) {
        co_l[i] = static_cast<int32_t>(ref_l[i]) << 16;
        co_r[i] = static_cast<int32_t>(ref_r[i]) << 16;
      }
      const uint32_t c4 = Cycles();
      if (b >= 20 + kWarm) {
        t_dec += c1 - c0;
        t_plate += c2 - c1;
        t_mix += c3 - c2;
        t_conv += c4 - c3;
      }
    }
    const int n = kBlocks - kWarm;
    printf("steady profile (GNU simulator, %d blocks): render %lu; decimate %lu, plate %lu, interpolate + mix %lu, "
           "one int16 -> int32 pass %lu\n",
           n, static_cast<unsigned long>(t_render / n), static_cast<unsigned long>(t_dec / n),
           static_cast<unsigned long>(t_plate / n), static_cast<unsigned long>(t_mix / n),
           static_cast<unsigned long>(t_conv / n));
  }
#endif
  printf("%s: %d failure(s)\n", failures ? "FAIL" : "ok", failures);
  return failures ? 1 : 0;
}
