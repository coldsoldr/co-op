// tape_echo 0.5, the tail that settles (plugins/tape_echo/src/tape_echo_core.cc SettleTail): the echo's loop rounded
// to nearest at every stage, so at higher feedback a repeat of a few LSB came round unchanged for ever (0.4's "ghost":
// 9 LSB of the 16-bit output at feedback 0.87 on a new tape), and such an echo never reached its idle state. 0.5 takes
// what is written to the tape a step towards zero while the input is silent.
//
// The plugin at two positions in lockstep (built with COOP_IDLE_PROBE): position 0 as it ships, position 1 with
// g_coop_te_ghost set around its render, which is 0.4's echo sample for sample (the Makefile's reference build proves
// that build against te-op1-cfw's echo). Per setting (feedback x age x time, and sync): two notes, then silence.
//   - where the two first differ (seconds after the input, and how loud 0.4's echo still is there);
//   - the largest and the RMS difference from there on, and over the first 3 s of the tail (the audible repeats);
//   - when 0.5's output is exact zero for good, and when it goes idle;
//   - what 0.4 still gives in the run's last 10 s (its ghost).
// Checks: no difference while the input plays; 0.5 reaches exact zero and its idle state in every setting under the
// "never idle" feedback (0.97); the difference never exceeds kMaxDiff.
//
//   tape_echo_tail [SECONDS]      the silence each setting gets (default 600)
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "coop_plugin.h"

extern "C" const coop_fx_t FX_tape_echo;
extern "C" void coop_fx_scratch_dirty(int slot);
extern "C" {
int g_coop_idle_off;
uint32_t g_coop_idle_blocks;
int32_t g_coop_idle_level;
int g_coop_te_ghost;            // 1: the render keeps 0.4's tail (no settling)
}
uint32_t g_tape_echo_host_cycles;

namespace {

const int kBlock = 64;
const int kMaxDiff = 128;       // LSB: the largest difference allowed anywhere (settling starts under 127 on the tape)
const int16_t kLo[4] = { 64, 0, 0, 8192 };
const int16_t kHi[4] = { 14528, 24576, 22528, 32767 };
const double kSec = 64.0 / 44100.0;

int32_t Tri(uint32_t ph) {
  const int32_t x = static_cast<int32_t>(ph >> 16) - 32768;
  return (x < 0 ? -x : x) * 2 - 32768;
}

void Note(long t, long len, int32_t peak, uint32_t inc, int16_t* l, int16_t* r) {
  if (t < 0 || t >= len) { *l = *r = 0; return; }
  int32_t env = 32767 - static_cast<int32_t>((t * 24000) / len);
  if (len - t <= 64) env = static_cast<int32_t>((env * (len - 1 - t)) / 64);
  const uint32_t ph = inc * static_cast<uint32_t>(t);
  const int32_t a = (Tri(ph) + (Tri(ph * 2 + 0x30000000u) >> 1)) >> 1, b = (Tri(ph + ph / 173) + (Tri(ph * 3) >> 2)) >> 1;
  *l = static_cast<int16_t>((((a * env) >> 15) * peak) >> 15);
  *r = static_cast<int16_t>((((b * env) >> 15) * peak) >> 15);
}

int g_fail = 0;

struct Out {
  int diff;       // the largest |new - old|
  int old_peak;   // the largest |old|
  int new_peak;
  double sq;      // the sum of the differences' squares
  bool idle;
};

Out Both(const int16_t* il, const int16_t* ir, const int16_t* p) {
  int32_t l0[kBlock], r0[kBlock], l1[kBlock], r1[kBlock];
  for (int i = 0; i < kBlock; ++i) {
    l0[i] = l1[i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(il[i])) << 16);
    r0[i] = r1[i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(ir[i])) << 16);
  }
  g_tape_echo_host_cycles += 20000;
  Out o;
  const uint32_t before = g_coop_idle_blocks;
  g_coop_te_ghost = 0;
  FX_tape_echo.render(0, l0, r0, p, kBlock);
  o.idle = g_coop_idle_blocks != before;
  g_coop_te_ghost = 1;
  FX_tape_echo.render(1, l1, r1, p, kBlock);
  g_coop_te_ghost = 0;
  o.diff = o.old_peak = o.new_peak = 0;
  o.sq = 0;
  for (int i = 0; i < kBlock; ++i) {
    const int a[2] = { l0[i] >> 16, r0[i] >> 16 }, b[2] = { l1[i] >> 16, r1[i] >> 16 };
    for (int c = 0; c < 2; ++c) {
      const int d = a[c] > b[c] ? a[c] - b[c] : b[c] - a[c];
      if (d > o.diff) o.diff = d;
      o.sq += static_cast<double>(d) * d;
      const int pa = a[c] < 0 ? -a[c] : a[c], pb = b[c] < 0 ? -b[c] : b[c];
      if (pa > o.new_peak) o.new_peak = pa;
      if (pb > o.old_peak) o.old_peak = pb;
    }
  }
  return o;
}

double Root(double v) {
  if (v <= 0) return 0;
  double x = v;
  for (int i = 0; i < 80; ++i) x = 0.5 * (x + v / x);
  return x;
}

struct Set {
  const char* name;
  int32_t k[4];   // time, feedback, age, mix (Q15)
  int16_t sync, pattern;
};

void Run(const Set& s, long silence_blocks, bool must_idle) {
  coop_fx_scratch_dirty(0);
  coop_fx_scratch_dirty(1);
  FX_tape_echo.init(0);
  FX_tape_echo.init(1);
  int16_t p[16] = { 0 };
  for (int k = 0; k < 4; ++k) p[k] = static_cast<int16_t>(kLo[k] + (s.k[k] * (kHi[k] - kLo[k])) / 32767);
  p[4] = 0x5300;
  p[5] = 16384;
  p[6] = s.sync;
  p[7] = s.pattern;
  int16_t l[kBlock], r[kBlock];
  int playing_diff = 0;
  for (long b = 0; b < 60 + 520; ++b) {
    for (int i = 0; i < kBlock; ++i) {
      const long t = (b - 60) * kBlock + i;
      if (t < 260 * 64) Note(t, 260 * 64, 20000, 21474836u, &l[i], &r[i]);
      else Note(t - 260 * 64, 260 * 64, 14000, 32212254u, &l[i], &r[i]);
    }
    const Out o = Both(l, r, p);
    if (o.diff > playing_diff) playing_diff = o.diff;
  }
  for (int i = 0; i < kBlock; ++i) l[i] = r[i] = 0;
  long first_diff = -1, last_sound = -1, idle_at = -1, n_tail = 0;
  int level_at_diff = 0, recent_old = 0, worst = 0, worst3 = 0, ghost = 0, worst_old_level = 0;
  double sq = 0, sq3 = 0;
  int ring[690] = { 0 };          // 0.4's peak over the last second
  for (long b = 0; b < silence_blocks; ++b) {
    const Out o = Both(l, r, p);
    ring[b % 690] = o.old_peak;
    if (o.diff && first_diff < 0) {
      first_diff = b;
      for (int k = 0; k < 690; ++k) if (ring[k] > recent_old) recent_old = ring[k];
      level_at_diff = recent_old;
    }
    if (o.diff > worst) { worst = o.diff; worst_old_level = o.old_peak; }
    sq += o.sq;
    ++n_tail;
    if (b < 2067) {               // the first 3 s: the repeats one hears
      if (o.diff > worst3) worst3 = o.diff;
      sq3 += o.sq;
    }
    if (o.new_peak) last_sound = b;
    if (o.idle && idle_at < 0) idle_at = b;
    if (b >= silence_blocks - 6890 && o.old_peak > ghost) ghost = o.old_peak;
    if (idle_at >= 0 && b > idle_at + 13780 && b < silence_blocks - 6891) {   // 20 s idle: skip on to the last 10 s
      // (0.4's side must still be rendered for its ghost: keep going, the idle side is cheap)
    }
  }
  const double rms = Root(sq / (n_tail * 128.0)), rms3 = Root(sq3 / (2067 * 128.0));
  printf("  %-34s parts %6.2f s after the input (0.4 at %4d LSB there); difference: first 3 s max %2d rms %.3f, whole "
         "tail max %2d (0.4 at %3d LSB) rms %.3f; 0.5 exact zero from %7.2f s, idle at %7.2f s; 0.4's last 10 s: %2d LSB\n",
         s.name, first_diff < 0 ? -1.0 : first_diff * kSec, level_at_diff, worst3, rms3, worst, worst_old_level, rms,
         (last_sound + 1) * kSec, idle_at < 0 ? -1.0 : idle_at * kSec, ghost);
  if (playing_diff) { ++g_fail; printf("FAIL: %s: a difference of %d LSB while the input played\n", s.name, playing_diff); }
  if (worst > kMaxDiff) { ++g_fail; printf("FAIL: %s: a difference of %d LSB\n", s.name, worst); }
  if (must_idle && (idle_at < 0 || last_sound >= idle_at)) { ++g_fail; printf("FAIL: %s: no exact zero and idle state\n", s.name); }
  if (!must_idle && idle_at >= 0) { ++g_fail; printf("FAIL: %s: idle over the never-idle feedback\n", s.name); }
}

}  // namespace

int main(int argc, char** argv) {
  const long seconds = argc > 1 ? atol(argv[1]) : 600;
  const long blocks = static_cast<long>(seconds / kSec);
  printf("tape_echo 0.5 against 0.4 (the same build with its tail kept), %ld s of silence after two notes, full wet:\n",
         seconds);
  static const int32_t kFb[4] = { 18000, 26000, 28300, 28900 };          // 0.60, 0.87, 0.95, 0.97 (just under the bound)
  static const char* const kFbName[4] = { "0.60", "0.87", "0.95", "0.97" };
  static const int32_t kAge[3] = { 0, 8000, 32767 };
  static const int32_t kTime[4] = { 32767, 25000, 16384, 0 };
  static const char* const kTimeName[4] = { "longest", "0.6 s", "0.21 s", "shortest" };
  char name[96];
  for (int t = 0; t < 4; ++t) {
    for (int f = 0; f < 4; ++f) {
      for (int a = 0; a < 3; ++a) {
        snprintf(name, sizeof(name), "%s, fb %s, age %d", kTimeName[t], kFbName[f], static_cast<int>(kAge[a]));
        const Set s = { name, { kTime[t], kFb[f], kAge[a], 32767 }, 0, static_cast<int16_t>((t + f + a) & 3) };
        Run(s, blocks, true);
      }
    }
  }
  {
    const Set s1 = { "defaults (mix 40 %)", { 16384, 14746, 9830, 13107 }, 0, 0 };
    Run(s1, blocks, true);
    const Set s2 = { "sync 1/4, fb 0.95, age 8000", { 16384, 28300, 8000, 32767 }, 8, 1 };
    Run(s2, blocks, true);
    const Set s3 = { "sync 1/16T, fb 0.87, age 0", { 16384, 26000, 0, 32767 }, 1, 2 };
    Run(s3, blocks, true);
    const Set s4 = { "sync 1/2, fb 0.87, age 0", { 16384, 26000, 0, 32767 }, 11, 3 };
    Run(s4, blocks, true);
    const Set s5 = { "longest, fb 1.0 (never idle)", { 32767, 29789, 0, 32767 }, 0, 0 };
    Run(s5, blocks < 124032 ? blocks : 124032, false);
  }
  if (g_fail) { printf("tape_echo tail: %d checks FAILED\n", g_fail); return 1; }
  printf("tape_echo tail: all checks passed\n");
  return 0;
}
