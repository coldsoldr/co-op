// The idle paths' host test (docs/efficiency-audit.md finding 6): an effect that is on with silent input and a tail
// that has died away stops computing. One source for the five effects (Makefile: -DFXSYM=FX_<id> -DFXID=n and the
// effect's own sources, built with -DCOOP_IDLE_PROBE: the plugin then reads g_coop_idle_off and counts its idle
// blocks in g_coop_idle_blocks).
//
// The lockstep: the same effect at two positions, the same input and controls block for block; position 0 as it
// ships, position 1 with the idle path switched off (g_coop_idle_off around its render), which is the effect as it was
// before (the reference tests in tests/host/<id> prove that build against te-op1-cfw or its own digest). Every output
// sample of the two is compared.
//
//   1. never silent: noise and saws, every control walking and jumping, the latches toggling, re-inits. The two
//      outputs must be the same bit for bit and no block may idle.
//   2. gaps: notes with 150 ms of exact silence between them (shorter than any hold): the same.
//   3. a note, then silence, per setting: no difference before the idle state; the idle state reached (after how
//      long); while idle the largest difference (16-bit output LSB) and where; then a soft note (40 LSB peak) and a
//      loud one: the difference on the wake's first block and the largest after; then silence again: idle again.
//      Beside them a third position, never idle either, that heard the notes before the silence 1 % quieter (and
//      the same notes after it): what the old effect's output after a silence owes to what it played before it. A
//      reverb whose rounding leaves its last LSBs going round (the allpasses of cs_hall and cs_plate), and the more
//      one that rounds with a dither (cs_hall), is not a fixed function of the input since the silence; the idle
//      build's difference after the wake is to be read against this figure.
//   4. the states that hold a sound (freeze, hold, a playing loop, feedback at or over 1, infinite decay): a note,
//      the state engaged, a minute of silence: never idle, no difference. And engaged while idle: the idle state ends
//      on that block and does not come back.
//   4b. where the DSP's own rounding holds a tail over the idle level for ever (tape_echo at high feedback on a new
//      tape): three minutes of silence, never idle, no difference.
//   5. the controls moving every block while idle, then a note: the difference stays under the bound.
//
// In the Blackfin simulator (`make sim`): the render's cycles a block playing, silent before the idle state, idle, and
// on the block that wakes (one position, compute only; the plugin as it ships, without the probe: an idle block is
// told by its cost, under half a playing one).
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_plugin.h"

extern "C" const coop_fx_t FXSYM;
extern "C" void coop_fx_scratch_dirty(int slot);

// The plugins' probe (COOP_IDLE_PROBE).
extern "C" {
int g_coop_idle_off;            // 1: the render never enters (or stays in) the idle state
uint32_t g_coop_idle_blocks;    // blocks rendered through the idle path
int32_t g_coop_idle_level;      // the level the last silent block's check saw (the effect's own unit)
}
// The effects' clocks (their half-rate paths restart after a pause): stepped here.
uint32_t g_tape_echo_host_cycles;
uint32_t g_petri_host_cycles;
extern "C" { uint32_t cs_hall_host_cycles; }

namespace {

const int kBlock = 64;
const int16_t kLo[4] = { 64, 0, 0, 8192 };
const int16_t kHi[4] = { 14528, 24576, 22528, 32767 };

struct Set {
  const char* name;
  int32_t k[4];                 // the four turns, Q15
  int16_t w5, w6, w7, latch;
};

struct Cfg {
  const char* name;
  int bound;                    // the largest difference allowed, 16-bit output LSB
  long idle_max_blocks;         // the idle state must come within this of the input's end
  int nsets;
  Set sets[16];
  int nblock;
  Set block[8];                 // the states that must never idle (a latch is engaged during the note's tail)
  int nawake;
  Set awake[4];                 // settings whose tail never falls under the idle level (the DSP's own limit cycle):
};                              // they must stay awake, with no difference

#define E(n, e) { n, { 16384, 13107, 13107, 16384 }, e, 30000, 8000, 0 }
Cfg kCfg =
#if FXID == 1
{ "tape_echo", 0, 60000, 4, {
    { "defaults", { 16384, 14746, 9830, 13107 }, 16384, 0, 0, 0 },
    { "time 0.6 s, feedback 0.6, wet", { 25000, 18000, 16000, 32767 }, 16384, 0, 0, 0 },
    { "short, worn tape, three heads", { 3000, 20000, 32767, 20000 }, 20000, 0, 2, 0 },
    { "sync 1/4, feedback 0.95, two heads", { 16384, 28300, 16000, 26000 }, 16384, 8, 1, 0 } },
  3, {
    { "hold", { 16384, 14746, 9830, 32767 }, 16384, 0, 0, 1 },
    { "feedback at 1.0", { 16384, 29789, 9830, 32767 }, 16384, 0, 0, 0 },
    { "feedback at the top (1.1)", { 16384, 32767, 9830, 32767 }, 16384, 0, 0, 0 } },
  2, {
    { "longest time, feedback 0.6, age 8000, wet", { 32767, 18000, 8000, 32767 }, 16384, 0, 0, 0 },
    { "longest time, feedback 0.87, new tape, wet", { 32767, 26000, 0, 32767 }, 16384, 0, 0, 0 } } };
#elif FXID == 2
{ "cs_hall", 0, 90000, 3, {
    { "defaults", { 19661, 16384, 9830, 11469 }, 3277, 16384, 32767, 0 },
    { "largest, decay 30000, pre-delay 200 ms, wet", { 32767, 30000, 0, 32767 }, 32767, 0, 32767, 0 },
    { "smallest, short, wet", { 0, 2000, 32767, 32767 }, 0, 32767, 0, 0 } },
  2, {
    { "freeze", { 19661, 16384, 9830, 32767 }, 3277, 16384, 32767, 1 },
    { "infinite decay", { 19661, 32767, 9830, 32767 }, 3277, 16384, 32767, 0 } }, 0, {} };
#elif FXID == 3
{ "cs_plate", 0, 90000, 3, {
    { "defaults", { 16384, 9830, 3277, 11469 }, 4, 22938, 9830, 0 },
    { "longest decay, largest, wet", { 32767, 0, 32767, 32767 }, 7, 32767, 32767, 0 },
    { "shortest, smallest, wet", { 0, 32767, 0, 32767 }, 0, 0, 0, 0 } },
  1, {
    { "freeze", { 16384, 9830, 3277, 32767 }, 4, 22938, 9830, 1 } }, 0, {} };
#elif FXID == 4
{ "fickle", 0, 120000, 5, {
    { "defaults (reverb)", { 14000, 12000, 29372, 16384 }, 0, 1, 0, 0 },
    { "delay, wet", { 14000, 20000, 29372, 32767 }, 1, 1, 0, 0 },
    { "slip, wet", { 14000, 12000, 29372, 32767 }, 2, 1, 0, 0 },
    { "reverb, long, clock 32 kHz, wet", { 24000, 30000, 32767, 32767 }, 0, 1, 0, 0 },
    { "delay, clock 6 kHz, wet", { 20000, 24000, 12000, 32767 }, 1, 1, 0, 0 } },
  6, {
    { "freeze (reverb)", { 14000, 12000, 29372, 32767 }, 0, 1, 0, 1 },
    { "freeze (delay)", { 14000, 20000, 29372, 32767 }, 1, 1, 0, 1 },
    { "freeze (slip)", { 14000, 12000, 29372, 32767 }, 2, 1, 0, 1 },
    { "a loop playing", { 14000, 12000, 29372, 32767 }, 0, 1, 0, 2 },
    { "a loop playing, dub on", { 14000, 12000, 29372, 32767 }, 1, 2, 4, 6 },
    { "delay with modify at the top (it holds)", { 14000, 32767, 29372, 32767 }, 1, 1, 0, 0 } }, 0, {} };
#elif FXID == 5
{ "petri", 0, 60000, 14, {
    E("mosaic", 0), E("seq", 1), E("glide", 2), E("haze", 3), E("tunnel", 4), E("strum", 5), E("blocks", 6),
    E("cut", 7), E("arp", 8), E("pattern", 9), E("warp", 10),
    { "tunnel, repeats and space at the top, wet", { 16384, 26000, 32767, 32767 }, 4, 30000, 32767, 0 },
    { "pattern, repeats at the top, reverse, wet", { 16384, 26000, 32767, 32767 }, 9, 12000, 20000, 2 },
    { "warp, repeats at the top, wet", { 24000, 20000, 32767, 32767 }, 10, 30000, 16000, 0 } },
  4, {
    { "hold (mosaic)", { 16384, 13107, 13107, 32767 }, 0, 30000, 8000, 1 },
    { "hold (tunnel)", { 16384, 13107, 13107, 32767 }, 4, 30000, 8000, 1 },
    { "hold (pattern)", { 16384, 13107, 13107, 32767 }, 9, 30000, 8000, 1 },
    { "hold (warp, reverse)", { 16384, 13107, 13107, 32767 }, 10, 30000, 8000, 3 } }, 0, {} };
#else
#error "FXID: 1 tape_echo, 2 cs_hall, 3 cs_plate, 4 fickle, 5 petri"
#endif
#undef E

void Params(const Set& s, int16_t* p) {
  for (int k = 0; k < 16; ++k) p[k] = 0;
  for (int k = 0; k < 4; ++k) p[k] = static_cast<int16_t>(kLo[k] + (s.k[k] * (kHi[k] - kLo[k])) / 32767);
  p[4] = 0x5300;
  p[5] = s.w5;
  p[6] = s.w6;
  p[7] = s.w7;
  p[8] = s.latch;
}

uint32_t g_rng = 0x1234567u;
uint32_t Rand() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
int32_t RandIn(int32_t lo, int32_t hi) { return lo + static_cast<int32_t>(Rand() % static_cast<uint32_t>(hi - lo + 1)); }

// A triangle, +-32767.
int32_t Tri(uint32_t ph) {
  const int32_t x = static_cast<int32_t>(ph >> 16) - 32768;
  return (x < 0 ? -x : x) * 2 - 32768;
}

// A plucked note of `len` samples at `peak` (two detuned triangles, L != R), ending in exact zeros (the last 64
// samples fade): sample t of it.
void Note(long t, long len, int32_t peak, uint32_t inc, int16_t* l, int16_t* r) {
  if (t < 0 || t >= len) { *l = *r = 0; return; }
  int32_t env = 32767 - static_cast<int32_t>((t * 24000) / len);      // falls to a quarter over the note
  if (len - t <= 64) env = static_cast<int32_t>((env * (len - 1 - t)) / 64);
  const uint32_t ph = inc * static_cast<uint32_t>(t);
  const int32_t a = (Tri(ph) + (Tri(ph * 2 + 0x30000000u) >> 1)) >> 1, b = (Tri(ph + ph / 173) + (Tri(ph * 3) >> 2)) >> 1;
  *l = static_cast<int16_t>((((a * env) >> 15) * peak) >> 15);
  *r = static_cast<int16_t>((((b * env) >> 15) * peak) >> 15);
}

#ifdef __bfin__
inline uint32_t RealCycles() { uint32_t c; __asm__ __volatile__("%0 = CYCLES;" : "=d"(c)); return c; }
#endif

// One block at both positions: position 0 as it ships, position 1 with the idle path off. Returns the largest
// difference (16-bit LSB) and where; *idle: position 0 took the idle path.
struct Step {
  int diff, at;                 // the largest |out0 - out1| and its sample (L even, R odd)
  int ref_peak;                 // the largest |out1| (what the effect gave without the idle path)
  bool idle;
  int32_t level;                // the idle check's level at position 1 (g_coop_idle_level)
  int ctl;                      // the largest |out1 - out2| (the third position, when it runs)
};

bool g_ctl;                     // the third position runs (never idle)
int g_ctl_nudge;                // while not 0 its input is 1 % quieter (the notes before the silence)
int g_ctl_worst;                // the largest Step.ctl since it was cleared

int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; if (g_fail < 30) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

Step Both(const int16_t* il, const int16_t* ir, const int16_t* p) {
  int32_t l0[kBlock], r0[kBlock], l1[kBlock], r1[kBlock];
  for (int i = 0; i < kBlock; ++i) {
    l0[i] = l1[i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(il[i])) << 16);
    r0[i] = r1[i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(ir[i])) << 16);
  }
  g_tape_echo_host_cycles += 20000;
  g_petri_host_cycles += 20000;
  cs_hall_host_cycles += 20000;
  Step s;
  const uint32_t before = g_coop_idle_blocks;
  g_coop_idle_off = 0;
  FXSYM.render(0, l0, r0, p, kBlock);
  s.idle = g_coop_idle_blocks != before;
  g_coop_idle_off = 1;
  const uint32_t mid = g_coop_idle_blocks;
  FXSYM.render(1, l1, r1, p, kBlock);
  g_coop_idle_off = 0;
  CHECK(g_coop_idle_blocks == mid, "the position with the idle path off idled");
  s.level = g_coop_idle_level;
  s.ctl = 0;
  if (g_ctl) {
    int32_t l2[kBlock], r2[kBlock];
    for (int i = 0; i < kBlock; ++i) {
      const int16_t a = g_ctl_nudge ? static_cast<int16_t>(il[i] - il[i] / 100) : il[i];
      const int16_t c = g_ctl_nudge ? static_cast<int16_t>(ir[i] - ir[i] / 100) : ir[i];
      l2[i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(a)) << 16);
      r2[i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(c)) << 16);
    }
    g_coop_idle_off = 1;
    FXSYM.render(2, l2, r2, p, kBlock);
    g_coop_idle_off = 0;
    for (int i = 0; i < kBlock; ++i) {
      const int a1 = l1[i] >> 16, a2 = l2[i] >> 16, b1 = r1[i] >> 16, b2 = r2[i] >> 16;
      const int dl = a1 > a2 ? a1 - a2 : a2 - a1, dr = b1 > b2 ? b1 - b2 : b2 - b1;
      if (dl > s.ctl) s.ctl = dl;
      if (dr > s.ctl) s.ctl = dr;
    }
    if (s.ctl > g_ctl_worst) g_ctl_worst = s.ctl;
  }
  s.diff = 0;
  s.at = 0;
  s.ref_peak = 0;
  for (int i = 0; i < kBlock; ++i) {
    CHECK((l0[i] & 0xffff) == 0 && (r0[i] & 0xffff) == 0, "low bits set");
    const int a0 = l0[i] >> 16, a1 = l1[i] >> 16, b0 = r0[i] >> 16, b1 = r1[i] >> 16;
    const int dl = a0 > a1 ? a0 - a1 : a1 - a0, dr = b0 > b1 ? b0 - b1 : b1 - b0;
    if (dl > s.diff) { s.diff = dl; s.at = 2 * i; }
    if (dr > s.diff) { s.diff = dr; s.at = 2 * i + 1; }
    const int pl = a1 < 0 ? -a1 : a1, pr = b1 < 0 ? -b1 : b1;
    if (pl > s.ref_peak) s.ref_peak = pl;
    if (pr > s.ref_peak) s.ref_peak = pr;
  }
  return s;
}

void InitBoth() {
  coop_fx_scratch_dirty(0);
  coop_fx_scratch_dirty(1);
  CHECK(FXSYM.init(0) == 0 && FXSYM.init(1) == 0, "init");
}

void Silence(int16_t* l, int16_t* r) { for (int i = 0; i < kBlock; ++i) l[i] = r[i] = 0; }

// `blocks` of a note that starts at block 0 of the call (len samples), both positions; the largest difference.
int PlayNote(long blocks, long len, int32_t peak, uint32_t inc, const int16_t* p, int* first_diff, long* idle_blocks) {
  int16_t l[kBlock], r[kBlock];
  int worst = 0;
  for (long b = 0; b < blocks; ++b) {
    for (int i = 0; i < kBlock; ++i) Note(b * kBlock + i, len, peak, inc, &l[i], &r[i]);
    const Step s = Both(l, r, p);
    if (b == 0 && first_diff) *first_diff = s.diff;
    if (s.diff > worst) worst = s.diff;
    if (s.idle && idle_blocks) ++*idle_blocks;
  }
  return worst;
}

const double kSec = 64.0 / 44100.0;

// ---- 1, 2: never silent; gaps ----
void NeverSilent(long blocks, bool gaps) {
  InitBoth();
  Set s = kCfg.sets[0];
  int16_t p[16];
  const uint32_t idle0 = g_coop_idle_blocks;
  long differ = 0, worst = 0, inits = 0, moves = 0;
  uint32_t ph_l = 0, ph_r = 0, seed = 99;
  int16_t l[kBlock], r[kBlock];
  int src = 0;
  for (long b = 0; b < blocks; ++b) {
    if (Rand() % 331 == 0) { s = kCfg.sets[Rand() % kCfg.nsets]; ++moves; }
    if (Rand() % 257 == 0) { s = kCfg.block[Rand() % kCfg.nblock]; ++moves; }
    if (b % 1000 >= 500) {                       // half the time every turn and word walks every block
      for (int k = 0; k < 4; ++k) {
        s.k[k] += RandIn(40, 300) * ((Rand() & 1) ? 1 : -1);
        s.k[k] = s.k[k] < 0 ? 200 : (s.k[k] > 32767 ? 32500 : s.k[k]);
      }
      ++moves;
    } else if (Rand() % 40 == 0) {
      s.k[Rand() & 3] = RandIn(0, 32767);         // a jump
      ++moves;
    }
    if (Rand() % 400 == 0) s.latch = static_cast<int16_t>(s.latch ^ (1 << (Rand() % 3)));
    if (Rand() % 1500 == 0) { CHECK(FXSYM.init(0) == 0 && FXSYM.init(1) == 0, "init"); s.latch = 0; ++inits; }
    if (Rand() % 200 == 0) src = static_cast<int>(Rand() % 3);
    Params(s, p);
    for (int i = 0; i < kBlock; ++i) {
      const long t = b * kBlock + i;
      int32_t a, c;
      if (gaps) {                                 // 0.55 s notes, 0.15 s of exact silence between them
        int16_t nl, nr;
        Note(t % 30870, 24255, 18000, 21000000u + static_cast<uint32_t>((t / 30870) % 7) * 3100000u, &nl, &nr);
        a = nl; c = nr;
      } else if (src == 0) {                      // noise, never 0 on both sides for a whole block
        seed = seed * 1664525u + 1013904223u; a = static_cast<int32_t>(seed >> 16) - 32768;
        seed = seed * 1664525u + 1013904223u; c = static_cast<int32_t>(seed >> 16) - 32768;
        a >>= 2; c >>= 3;
      } else if (src == 1) {                      // saws
        ph_l += 40000000u; ph_r += 61000000u;
        a = static_cast<int16_t>(ph_l >> 16) >> 1; c = static_cast<int16_t>(ph_r >> 16) >> 2;
      } else {                                    // a quiet signal: +-1 and +-2 LSB (not silence)
        seed = seed * 1664525u + 1013904223u; a = static_cast<int32_t>((seed >> 20) & 3) - 2; c = (i & 1) ? 1 : -1;
      }
      l[i] = static_cast<int16_t>(a);
      r[i] = static_cast<int16_t>(c);
    }
    const Step st = Both(l, r, p);
    if (st.diff) { ++differ; if (st.diff > worst) worst = st.diff; }
  }
  const long idled = static_cast<long>(g_coop_idle_blocks - idle0);
  printf("  %s: %ld blocks, %ld control moves, %ld re-inits: %ld blocks differ (largest %ld LSB), %ld idle blocks\n",
         gaps ? "notes with 150 ms gaps" : "never silent", blocks, moves, inits, differ, worst, idled);
  CHECK(differ == 0 && idled == 0, "%s: the idle build differs or idled", gaps ? "gaps" : "never silent");
}

// ---- 3: a note, silence, the wake ----
void NoteThenSilence(const Set& set) {
  InitBoth();
  coop_fx_scratch_dirty(2);
  CHECK(FXSYM.init(2) == 0, "init");
  g_ctl = true;
  g_ctl_nudge = 1;                                         // the notes before the silence: 1 % quieter there
  int16_t p[16];
  Params(set, p);
  int16_t l[kBlock], r[kBlock];
  // 60 blocks for the start (the buffers' clear), then two notes (0.75 s), ending in exact zeros
  Silence(l, r);
  for (int b = 0; b < 60; ++b) Both(l, r, p);
  int before = PlayNote(260, 260 * 64, 20000, 21474836u, p, 0, 0);
  const int b2 = PlayNote(260, 260 * 64, 14000, 32212254u, p, 0, 0);
  if (b2 > before) before = b2;
  g_ctl_nudge = 0;
  // silence until the idle state
  long idle_at = -1, tail_end = -1, b = 0;
  int level_max_late = 0;
  for (; b < kCfg.idle_max_blocks; ++b) {
    const Step s = Both(l, r, p);
    if (s.diff > before) before = s.diff;
    if (s.ref_peak > kCfg.bound) tail_end = b;              // the last block the old build still sounded over the bound
    if (getenv("IDLE_TRACE") && b % 2000 == 0) printf("    %7.1f s: output %d LSB, check level %d\n", b * kSec, s.ref_peak, (int)s.level);   // IDLE_TRACE: the tail every 2.9 s
    if (s.idle) { idle_at = b; break; }
  }
  if (idle_at < 0) {
    g_ctl = false;
    printf("  %-44s NOT idle after %.1f s of silence (the tail over %d LSB until %.1f s)\n", set.name, b * kSec,
           kCfg.bound, (tail_end + 1) * kSec);
    CHECK(false, "%s: the idle state never came", set.name);
    return;
  }
  // idle: 3037 more blocks (4.4 s; an odd count, so every free-running phase is somewhere new)
  int idle_diff = 0, idle_ref = 0;
  long idle_diff_at = 0, not_idle = 0;
  for (long k = 0; k < 3037; ++k) {
    const Step s = Both(l, r, p);
    if (!s.idle) ++not_idle;
    if (s.diff > idle_diff) { idle_diff = s.diff; idle_diff_at = k; }
    if (s.ref_peak > idle_ref) idle_ref = s.ref_peak;
    if (s.level > level_max_late) level_max_late = s.level;
  }
  if (getenv("IDLE_STATE")) {      // explore: where the two positions' states differ just before the wake
    uint32_t bytes = 0;
    const uint8_t* a = static_cast<const uint8_t*>(coop_fx_scratch(0, &bytes));
    const uint8_t* c = static_cast<const uint8_t*>(coop_fx_scratch(1, &bytes));
    long n = 0, nz0 = 0, nz1 = 0, runs = 0;
    for (uint32_t i = 0; i < bytes; ++i) {
      if (a[i]) ++nz0;
      if (c[i]) ++nz1;
      if (a[i] != c[i]) {
        if (i == 0 || a[i - 1] == c[i - 1]) {                 // a run of differing bytes starts
          uint32_t j = i;
          while (j < bytes && a[j] != c[j]) ++j;
          if (runs++ < 40) printf("    state +%u .. +%u differ\n", i, j - 1);
        }
        ++n;
      }
    }
    printf("    %ld state bytes differ (non-zero bytes: %ld idle, %ld not)\n", n, nz0, nz1);
  }
  // the wake: a soft note (40 LSB peak, 1 s), then a loud one
  int first = 0;
  long woke_idle = 0;
  g_ctl_worst = 0;
  const int soft = PlayNote(690, 690 * 64, 40, 25769803u, p, &first, &woke_idle);
  const int ctl_soft = g_ctl_worst;
  g_ctl_worst = 0;
  const int loud = PlayNote(400, 300 * 64, 22000, 17179869u, p, 0, &woke_idle);
  const int ctl_loud = g_ctl_worst;
  // silence again: idle again
  long again = -1;
  int again_diff = 0;
  for (long k = 0; k < kCfg.idle_max_blocks; ++k) {
    const Step s = Both(l, r, p);
    if (s.diff > again_diff) again_diff = s.diff;
    if (s.idle) { again = k; break; }
  }
  g_ctl = false;
  const int ctl_tail = g_ctl_worst > ctl_loud ? g_ctl_worst : ctl_loud;
  printf("  %-44s idle %6.2f s after the input (tail under %d LSB from %5.2f s); before it %d LSB; idle: %d LSB "
         "(block %ld; the old build's output %d LSB, its check level %d); wake: first block %d, soft note %d, loud %d "
         "LSB; idle again after %.2f s (%d LSB in that tail). Two that never idle, the earlier notes 1 %% apart: soft "
         "%d, loud %d, tail %d LSB\n",
         set.name, idle_at * kSec, kCfg.bound + 1, (tail_end + 1) * kSec, before, idle_diff, idle_diff_at, idle_ref,
         level_max_late, first, soft, loud, again * kSec, again_diff, ctl_soft, ctl_loud, ctl_tail);
  CHECK(before == 0, "%s: a difference before the idle state (%d LSB)", set.name, before);
  CHECK(not_idle == 0, "%s: %ld blocks left the idle state in silence", set.name, not_idle);
  CHECK(woke_idle == 0, "%s: %ld blocks idle while a note played", set.name, woke_idle);
  // idle: under the bound. Awake again: under the bound, or within what two runs of the old effect differ by.
  CHECK(idle_diff <= kCfg.bound && first <= kCfg.bound, "%s: a difference over %d LSB while idle or on the wake's "
        "first block", set.name, kCfg.bound);
  CHECK(soft <= kCfg.bound + 2 * ctl_soft && loud <= kCfg.bound + 2 * ctl_loud && again_diff <= kCfg.bound + 2 * ctl_tail,
        "%s: awake again, a difference over the bound and over twice the old effect's own", set.name);
  CHECK(again >= 0, "%s: no idle state after the second note", set.name);
}

// ---- 4: the states that hold a sound ----
void Holds(const Set& set) {
  int16_t p[16], pe[16];
  Set off = set;
  off.latch = 0;
  Params(off, p);
  Params(set, pe);
  int16_t l[kBlock], r[kBlock];
  // (a) a note, the state engaged in its tail (a knob state from the start), a minute of silence
  InitBoth();
  Silence(l, r);
  for (int b = 0; b < 60; ++b) Both(l, r, set.latch ? p : pe);
  const uint32_t idle0 = g_coop_idle_blocks;
  int worst = PlayNote(400, 520 * 64, 20000, 21474836u, set.latch ? p : pe, 0, 0);   // the latch goes on mid-note
  int d2 = PlayNote(120, 120 * 64, 20000, 21474836u * 2, pe, 0, 0);
  if (d2 > worst) worst = d2;
  long loud = 0;
  for (long b = 0; b < 41344; ++b) {                       // 60 s
    const Step s = Both(l, r, pe);
    if (s.diff > worst) worst = s.diff;
    if (s.ref_peak > 2) ++loud;
  }
  const long idled = static_cast<long>(g_coop_idle_blocks - idle0);
  // (b) engaged while idle (reached with the state off; a knob state: with the first setting)
  InitBoth();
  int16_t pb[16];
  Params(set.latch ? off : kCfg.sets[0], pb);
  for (int b = 0; b < 60; ++b) Both(l, r, pb);
  PlayNote(300, 300 * 64, 20000, 21474836u, pb, 0, 0);
  long idle_at = -1;
  for (long b = 0; b < kCfg.idle_max_blocks; ++b) {
    if (Both(l, r, pb).idle) { idle_at = b; break; }
  }
  for (int b = 0; b < 500; ++b) Both(l, r, pb);
  long after = 0;
  int worst_b = 0;
  for (long b = 0; b < 6000; ++b) {                        // 8.7 s engaged, in silence
    const Step s = Both(l, r, pe);
    if (s.idle) ++after;
    if (s.diff > worst_b) worst_b = s.diff;
  }
  printf("  %-44s a minute of silence: %ld idle blocks, %d LSB difference (sounding in %ld of 41344 blocks); "
         "engaged while idle (reached after %.2f s): %ld idle blocks after, %d LSB\n",
         set.name, idled, worst, loud, idle_at * kSec, after, worst_b);
  CHECK(idled == 0 && worst == 0, "%s: idled or differed while engaged", set.name);
  CHECK(idle_at >= 0 && after == 0 && worst_b <= kCfg.bound, "%s: engaged while idle", set.name);
}

// ---- 4b: a tail that never falls under the idle level ----
void StaysAwake(const Set& set) {
  InitBoth();
  int16_t p[16];
  Params(set, p);
  int16_t l[kBlock], r[kBlock];
  Silence(l, r);
  for (int b = 0; b < 60; ++b) Both(l, r, p);
  const uint32_t idle0 = g_coop_idle_blocks;
  int worst = PlayNote(520, 520 * 64, 20000, 21474836u, p, 0, 0);
  int late = 0, level = 0;
  const long total = 124032;                               // 3 minutes
  for (long b = 0; b < total; ++b) {
    const Step s = Both(l, r, p);
    if (s.diff > worst) worst = s.diff;
    if (b >= total - 6891) {                               // the last 10 s
      if (s.ref_peak > late) late = s.ref_peak;
      if (s.level > level) level = s.level;
    }
  }
  const long idled = static_cast<long>(g_coop_idle_blocks - idle0);
  printf("  %-44s 3 minutes of silence: %ld idle blocks, %d LSB difference; the output in the last 10 s still %d LSB "
         "(the check's level %d)\n", set.name, idled, worst, late, level);
  CHECK(idled == 0 && worst == 0, "%s: idled or differed", set.name);
}

// ---- 5: the controls moving while idle ----
void MovesWhileIdle() {
  InitBoth();
  Set s = kCfg.sets[0];
  int16_t p[16];
  Params(s, p);
  int16_t l[kBlock], r[kBlock];
  Silence(l, r);
  for (int b = 0; b < 60; ++b) Both(l, r, p);
  PlayNote(300, 300 * 64, 20000, 21474836u, p, 0, 0);
  long idle_at = -1;
  for (long b = 0; b < kCfg.idle_max_blocks; ++b) {
    if (Both(l, r, p).idle) { idle_at = b; break; }
  }
  int worst = 0;
  long idle_blocks = 0;
  for (long b = 0; b < 1500; ++b) {                        // every turn walks every block, a jump now and then
    for (int k = 0; k < 4; ++k) {
      s.k[k] += RandIn(20, 200) * ((Rand() & 1) ? 1 : -1);
      s.k[k] = s.k[k] < 0 ? 200 : (s.k[k] > 26000 ? 25500 : s.k[k]);   // under every hold region
    }
    if (b % 97 == 0) s.k[Rand() & 3] = RandIn(0, 26000);
    Params(s, p);
    const Step st = Both(l, r, p);
    if (st.idle) ++idle_blocks;
    if (st.diff > worst) worst = st.diff;
  }
  int first = 0;
  const int note = PlayNote(600, 400 * 64, 20000, 19000000u, p, &first, 0);
  printf("  the turns walking and jumping for 1500 silent blocks from the idle state (reached after %.2f s): %ld of "
         "them idle, %d LSB; then a note: first block %d, largest %d LSB\n", idle_at * kSec, idle_blocks, worst,
         first, note);
  CHECK(idle_at >= 0 && worst <= kCfg.bound && first <= kCfg.bound, "moves while idle");
  (void)note;                                              // reported: see test 3 for what the old effect's own is
}

// ---- the simulator: cycles ----
#ifdef __bfin__
void Cycles() {
  uint32_t syscfg;
  __asm__ __volatile__("%0 = SYSCFG;" : "=d"(syscfg));
  syscfg |= 2;   // CCEN
  __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(syscfg));
  for (int si = 0; si < kCfg.nsets; ++si) {
    const Set& set = kCfg.sets[si];
    coop_fx_scratch_dirty(0);
    FXSYM.init(0);
    int16_t p[16];
    Params(set, p);
    uint64_t sum[4] = { 0, 0, 0, 0 };
    uint32_t n[4] = { 0, 0, 0, 0 }, mx[4] = { 0, 0, 0, 0 };
    long idle_at = -1;
    bool was_idle = false;
    long b = 0, idle_run = 0;
    int phase = 0;                                         // 0 start + note, 1 silence, 2 idle, 3 the second note
    long t0 = 0;
    for (; b < 12000; ++b) {
      int16_t il[kBlock], ir[kBlock];
      if (phase == 0) for (int i = 0; i < kBlock; ++i) Note((b - 60) * kBlock + i, 300 * 64, 20000, 21474836u, &il[i], &ir[i]);
      else if (phase == 3) for (int i = 0; i < kBlock; ++i) Note((b - t0) * kBlock + i, 200 * 64, 20000, 21474836u, &il[i], &ir[i]);
      else Silence(il, ir);
      int32_t l[kBlock], r[kBlock];
      for (int i = 0; i < kBlock; ++i) {
        l[i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(il[i])) << 16);
        r[i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(ir[i])) << 16);
      }
      g_tape_echo_host_cycles += 20000;
      g_petri_host_cycles += 20000;
      const uint32_t c0 = RealCycles();
      FXSYM.render(0, l, r, p, kBlock);
      const uint32_t dc = RealCycles() - c0;
      // (the plugin is built as it ships, without the probe: a silent block under half a playing one is an idle one)
      const bool idle = phase != 0 && phase != 3 && n[0] && static_cast<uint64_t>(dc) * 2 * n[0] < sum[0];
      int k = phase;
      if (phase == 0 && b < 100) k = -1;                   // the clear, the first notes' blocks
      if (phase == 1 && idle) { phase = 2; k = 2; idle_at = b - 360; }
      if (phase == 3 && b == t0) k = -1, printf("    the block that wakes: %lu cycles\n", static_cast<unsigned long>(dc));
      if (k >= 0) { sum[k] += dc; ++n[k]; if (dc > mx[k]) mx[k] = dc; }
      if (phase == 0 && b == 359) phase = 1;
      if (phase == 2 && ++idle_run == 400) { phase = 3; t0 = b + 1; }
      if (phase == 3 && b - t0 == 199) break;
      was_idle = idle;
    }
    (void)was_idle;
    printf("  %-44s playing avg %lu max %lu; silent, not yet idle avg %lu max %lu (%lu blocks); idle avg %lu max %lu; "
           "playing again avg %lu max %lu\n", set.name,
           static_cast<unsigned long>(n[0] ? sum[0] / n[0] : 0), static_cast<unsigned long>(mx[0]),
           static_cast<unsigned long>(n[1] ? sum[1] / n[1] : 0), static_cast<unsigned long>(mx[1]),
           static_cast<unsigned long>(n[1]),
           static_cast<unsigned long>(n[2] ? sum[2] / n[2] : 0), static_cast<unsigned long>(mx[2]),
           static_cast<unsigned long>(n[3] ? sum[3] / n[3] : 0), static_cast<unsigned long>(mx[3]));
    if (idle_at < 0) printf("    (no idle state within %ld blocks)\n", b);
  }
}
#endif

}  // namespace

int main(int argc, char** argv) {
  printf("%s: the idle path against the same effect with it off (bound %d LSB of the 16-bit output)\n", kCfg.name,
         kCfg.bound);
#ifdef __bfin__
  (void)argc; (void)argv;
  printf("cycles a block (GNU simulator, compute only):\n");
  Cycles();
  return 0;
#else
  const long blocks = argc > 1 ? atol(argv[1]) : 20000;
  if (getenv("IDLE_MAX_BLOCKS")) kCfg.idle_max_blocks = atol(getenv("IDLE_MAX_BLOCKS"));   // a longer wait, to explore
  if (getenv("IDLE_ONLY")) {                                                              // one setting of test 3
    NoteThenSilence(kCfg.sets[atoi(getenv("IDLE_ONLY"))]);
    return g_fail ? 1 : 0;
  }
  printf("1, 2. input that never goes silent long enough:\n");
  NeverSilent(blocks, false);
  NeverSilent(blocks, true);
  printf("3. a note, silence, a soft note:\n");
  for (int i = 0; i < kCfg.nsets; ++i) NoteThenSilence(kCfg.sets[i]);
  printf("4. the states that hold a sound:\n");
  for (int i = 0; i < kCfg.nblock; ++i) Holds(kCfg.block[i]);
  if (kCfg.nawake) printf("4b. a tail that never falls under the idle level stays awake:\n");
  for (int i = 0; i < kCfg.nawake; ++i) StaysAwake(kCfg.awake[i]);
  printf("5. controls moving while idle:\n");
  MovesWhileIdle();
  if (g_fail) { printf("%s idle: %d checks FAILED\n", kCfg.name, g_fail); return 1; }
  printf("%s idle: all checks passed\n", kCfg.name);
  return 0;
#endif
}
