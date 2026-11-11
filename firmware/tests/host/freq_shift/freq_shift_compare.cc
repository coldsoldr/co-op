// The freq_shift port's host test (guide/plugin-guidelines.md §7.4 step 7; docs/plugin-performance.md "How a rewrite
// is accepted"): plugins/freq_shift (the coop effect: its render, its knob and gain mapping, its Hilbert pair per
// channel with the L == R shortcut) against te-op1-cfw's ports/fx/src/shift (the kennel plugin's DSP, C kernels;
// upstream's own tests show its Blackfin assembly equal to them bit for bit) and the kennel glue's mappings
// (common/knl_fx.h GainQ14; op1_plugin.h op1_fx_param_q15's formula).
//
// One source, built three ways (Makefile):
//   -DFS_REF     te-op1-cfw, two upstream MONO FreqShift instances: L on l with the block's settings, R on r with the
//                shift moved to (shift - spread) and no spread (upstream mono runs the left channel at shift +
//                spread; the remapped word gives exactly R's d - s). This is what "a Hilbert pair and a shifter per
//                channel" must equal.
//   -DFS_REF_ST  te-op1-cfw, one upstream STEREO FreqShift (the kennel master's path). Only the scenario with
//                L == R throughout is run this way: there the upstream's (l + r) / 2 input equals each channel's, so
//                coop must equal the kennel master bit for bit.
//   (neither)    coop: FX_freq_shift.render on int32 buffers, params as the core hands them (turns in the filter
//                type's range units, words 5..7, latch word).
// Each build writes every output block (int32, L then R) to a file; the Makefile compares them.
//
//   freq_shift_* OUT.raw SCENARIO       SCENARIO: 0 mixed (L == R, L != R, back, silence; the sync paths), 1 L == R
//
// The coop build also checks mix 0 (any other settings): both channels come back bit for bit, low 16 bits included.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(FS_REF) || defined(FS_REF_ST)
#include "src/shift/shift.h"
#include "common/knl_fx.h"
#else
#include "coop_plugin.h"
#include "src/shift.h"
extern "C" const coop_fx_t FX_freq_shift;
extern "C" op1fx::FreqShift* freq_shift_instance();
#endif

namespace {

const int kN = 64;
uint32_t rng = 0x5eed1234u;
uint32_t Next() { rng = rng * 1664525u + 1013904223u; return rng; }
int32_t Range(int32_t lo, int32_t hi) { return lo + (int32_t)(Next() % (uint32_t)(hi - lo + 1)); }

// The filter type's ranges (sdk/coop_plugin.h) and the core's Q15 -> range map (plugin_host.cc knob_map).
const int32_t kLo[4] = {64, 0, 0, 8192}, kHi[4] = {14528, 24576, 22528, 32767};
int16_t KnobMap(int i, int32_t q15) { return (int16_t)(kLo[i] + (int32_t)(((uint32_t)(kHi[i] - kLo[i]) * (uint32_t)q15) >> 15)); }

#if defined(FS_REF) || defined(FS_REF_ST)
// kennel op1_plugin.h op1_fx_param_q15, the range table as above (filled).
int32_t ParamQ15(const int16_t* p, int i) {
  int32_t v = (p[i] - kLo[i]) * 32767 / (kHi[i] - kLo[i]);
  return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}
#endif

// A test signal: a few sines (phase accumulators, a small integer sine) plus noise, per channel.
struct Voice { uint32_t ph, inc; int32_t amp; };
int16_t Sine(uint32_t ph) {   // a triangle (integer only; its harmonics give the shifter a spectrum to move)
  int32_t t = (int32_t)(ph >> 16);   // 0..65535
  int32_t tri = t < 32768 ? t - 16384 : 49151 - t;   // -16384..16383
  return (int16_t)(tri * 2);
}

struct Signal {
  Voice v[2][3];
  void Init() {
    for (int c = 0; c < 2; c++)
      for (int k = 0; k < 3; k++) { v[c][k].ph = Next(); v[c][k].inc = (uint32_t)Range(1000000, 90000000); v[c][k].amp = Range(1000, 9000); }
  }
  int16_t Sample(int c, int32_t noise) {
    int32_t s = 0;
    for (int k = 0; k < 3; k++) { s += (Sine(v[c][k].ph) * v[c][k].amp) >> 15; v[c][k].ph += v[c][k].inc; }
    s += noise ? (int32_t)((int16_t)(Next() >> 16)) * noise >> 15 : 0;
    if (Next() % 997 == 0) s = (Next() & 1) ? 32767 : -32768;   // the odd full-scale sample
    return (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
  }
};

// The scenario's settings for a block, as Q15 knobs and step / word values.
struct Settings { int32_t knob[4]; int32_t gain; int32_t ring; };

void Walk(Settings* s, int block) {
  for (int i = 0; i < 4; i++) {
    if (Next() % 61 == 0) s->knob[i] = Range(0, 32767);            // a jump
    else if (Next() % 3 == 0) s->knob[i] += Range(-600, 600);        // a turn
    s->knob[i] = s->knob[i] < 0 ? 0 : (s->knob[i] > 32767 ? 32767 : s->knob[i]);
  }
  if (Next() % 97 == 0) s->gain = Range(0, 32767);
  else if (Next() % 7 == 0) s->gain += Range(-328, 328);
  s->gain = s->gain < 0 ? 0 : (s->gain > 32767 ? 32767 : s->gain);
  if (Next() % 151 == 0) s->ring ^= 1;
  if (block % 500 < 40) s->knob[3] = 0;                              // mix 0 stretches (the no-write path)
  if (block % 900 > 860) s->knob[1] = 32767;                         // the top feedback
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s OUT.raw SCENARIO\n", argv[0]); return 2; }
  FILE* out = fopen(argv[1], "wb");
  if (!out) return 2;
  const int scenario = atoi(argv[2]);
  const int kBlocks = 12000;   // ~17 s

#ifdef FS_REF
  static op1fx::FreqShift refl, refr;
  refl.Init();
  refr.Init();
#elif defined(FS_REF_ST)
  static op1fx::FreqShift ref;
  ref.Init();
#else
  if (FX_freq_shift.magic != COOP_FX_MAGIC || FX_freq_shift.init(0) != 0) return 2;
  long one = 0, two = 0;
#ifdef FS_SIM
  // The Blackfin build: argv[3] "c" runs the C kernels, else the assembly (the plugin's path); the cycles of every
  // render (the simulator counts instructions: no cache misses or SDRAM waits).
  const bool use_c = argc > 3 && argv[3][0] == 'c';
  freq_shift_instance()->set_use_asm(!use_c);
  uint32_t cyc_max[2] = {0, 0};
  uint64_t cyc_sum[2] = {0, 0};
  long cyc_n[2] = {0, 0};
  {
    uint32_t syscfg;   // the cycle counter on (SYSCFG CCEN), as te-op1-cfw's rings/harness/cycles.h
    __asm__ volatile("%0 = SYSCFG;" : "=d"(syscfg));
    syscfg |= 2;
    __asm__ volatile("SYSCFG = %0; CSYNC;" : : "d"(syscfg));
  }
#endif
#endif

  Signal sig;
  sig.Init();
  Settings set = {{19000, 8192, 0, 16384}, 16384, 0};
  static int32_t l[kN], r[kN];
  for (int b = 0; b < kBlocks; b++) {
    Walk(&set, b);
    // Scenario 0: L == R (0-1999), L != R (2000-5999), L == R (6000-8999: the sync after stereo), silence
    // (9000-9999: the decay to an exact sync), L != R then L == R again.
    bool same = scenario == 1 || b < 2000 || (b >= 6000 && b < 10000) || b >= 11000;
    bool silent = scenario == 0 && b >= 9000 && b < 10000;
    for (int i = 0; i < kN; i++) {
      int16_t a = silent ? 0 : sig.Sample(0, 3000);
      int16_t c = same ? a : (silent ? 0 : sig.Sample(1, 3000));
      l[i] = (int32_t)((uint32_t)(uint16_t)a << 16);
      r[i] = (int32_t)((uint32_t)(uint16_t)c << 16);
    }
    int16_t params[16] = {0};
    for (int i = 0; i < 4; i++) params[i] = KnobMap(i, set.knob[i]);
    params[4] = 0x5320;
    params[5] = (int16_t)set.gain;
    params[6] = (int16_t)set.ring;

#if defined(FS_REF) || defined(FS_REF_ST)
    op1fx::ShiftParams q;
    q.shift = ParamQ15(params, 0);
    q.feedback = ParamQ15(params, 1);
    q.spread = ParamQ15(params, 2);
    q.mix = ParamQ15(params, 3);
    q.gain = knl::GainQ14(params[5]);
    q.ring = params[6] == 1;
    int16_t l16[kN], r16[kN];
    for (int i = 0; i < kN; i++) { l16[i] = (int16_t)(l[i] >> 16); r16[i] = (int16_t)(r[i] >> 16); }
#ifdef FS_REF
    refl.Process(l16, nullptr, kN, q);
    // R: upstream mono runs at d + s, d = 2 (shift - 16384), s = spread x 10923 >> 15. R wants d - s: the same d'
    // from a spread of 0 when d - s is even, else from d - s - 1 and a spread of 3 (s' = 1). Below -32768 upstream
    // clamps: shift 0, no spread.
    {
      op1fx::ShiftParams qr = q;
      const int32_t d = op1fx::ShiftKnobToD(q.shift), s = op1fx::ShiftSpreadToD(q.spread);
      int32_t want = d - s;
      if (want <= -32768) { qr.shift = 0; qr.spread = 0; }
      else if ((want & 1) == 0) { qr.shift = 16384 + want / 2; qr.spread = 0; }
      else { qr.shift = 16384 + (want - 1) / 2; qr.spread = 3; }
      refr.Process(r16, nullptr, kN, qr);
    }
#else
    ref.Process(l16, r16, kN, q);
#endif
    for (int i = 0; i < kN; i++) {
      l[i] = (int32_t)((uint32_t)(uint16_t)l16[i] << 16);
      r[i] = (int32_t)((uint32_t)(uint16_t)r16[i] << 16);
    }
#else
#ifdef FS_SIM
    uint32_t c0, c1;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c0));
#endif
    FX_freq_shift.render(b % 5 == 0 ? 2 : 0, l, r, params, kN);
    const int pairs = freq_shift_instance()->pairs_run();
    if (pairs == 1) one++; else two++;
#ifdef FS_SIM
    __asm__ volatile("%0 = CYCLES;" : "=d"(c1));
    {
      const uint32_t d = c1 - c0;
      const int k = pairs - 1;
      if (b > 0 && d > cyc_max[k]) cyc_max[k] = d;
      cyc_sum[k] += d;
      cyc_n[k]++;
    }
#endif
#endif
    fwrite(l, sizeof(l), 1, out);
    fwrite(r, sizeof(r), 1, out);
  }
  fclose(out);

#if !defined(FS_REF) && !defined(FS_REF_ST)
  const op1fx::FreqShift* fs = freq_shift_instance();
  printf("freq_shift coop: %ld blocks one pair, %ld two pairs; syncs exact %ld, forced %ld\n", one, two,
         (long)fs->syncs_exact(), (long)fs->syncs_forced());
#ifdef FS_SIM
  for (int k = 0; k < 2; k++)
    if (cyc_n[k])
      printf("freq_shift %s, %s: cycles a render avg %lu, max %lu (%ld renders)\n", use_c ? "C kernels" : "assembly",
             k ? "two pairs (L != R)" : "one pair (L == R)", (unsigned long)(cyc_sum[k] / cyc_n[k]),
             (unsigned long)cyc_max[k], cyc_n[k]);
#endif
  // Mix 0, after wet blocks, any other settings, inputs with low bits: returned bit for bit.
  int bad = 0;
  for (int b = 0; b < 2000; b++) {
    int16_t params[16] = {0};
    for (int i = 0; i < 3; i++) params[i] = KnobMap(i, Range(0, 32767));
    params[3] = b < 4 ? KnobMap(3, 32767) : 8192;                     // wet first, then mix 0 (ramps down in a block)
    params[5] = (int16_t)Range(0, 32767);
    params[6] = (int16_t)(Next() & 1);
    int32_t in[2][kN];
    for (int i = 0; i < kN; i++) { in[0][i] = l[i] = (int32_t)Next(); in[1][i] = r[i] = (b & 1) ? l[i] : (int32_t)Next(); }
    FX_freq_shift.render(4, l, r, params, kN);
    if (b >= 5 && (memcmp(in[0], l, sizeof(l)) || memcmp(in[1], r, sizeof(r)))) bad++;
  }
  printf("freq_shift coop: mix 0 bit for bit: %s (%d blocks differ)\n", bad ? "FAIL" : "ok", bad);
  return bad ? 1 : 0;
#else
  return 0;
#endif
}
