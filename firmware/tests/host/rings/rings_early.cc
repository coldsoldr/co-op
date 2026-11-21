// rings 0.13: the early cut (plugins/rings/src/op1_voice.h kEarlyLevel: a voice under the level for 5 s is faded out
// over 1 s and sleeps), for every model. What it must show: above the level nothing changes; the fade has no click;
// a strike during the hold, the fade or the sleep is clean; a faded voice costs what a sleeping one costs.
//
//   rings_early run OUT|- MODEL POLY DAMPING HOLD VOICES SCRIPT [STRUCTURE [BRIGHTNESS [TRANSPOSE]]]
// VOICES keys (the chord C3 G3 C4 E4 G4 C5, voice v's three blocks after voice v - 1's) struck and held HOLD seconds;
// every voice with a key down renders every block. SCRIPT: `none`; `all:P` (every key struck again every P seconds);
// or `v@T,v@T,...` (voice v struck at T seconds; a voice past VOICES starts there: a new key), with `d@T:V` among
// them for the damping control moved to V at T seconds.
// OUT: every block's six renders, 64 samples each (zeros for a voice with no key down), and OUT.gain: per block and
// voice the early cut's gain after the render (Q30; 0 asleep or a silent render; -1 no key). Prints, per voice, when
// the fade starts and when the voice sleeps, with the summed 16-bit output's peak in the quarter second before; per
// 5 s the summed peak, the voices awake and (simulator) the cycles a block. Every render's `out` is filled with a
// pattern first: a silent render must have written all 64 zeros.
//   rings_early cmp A B            sample by sample
//   rings_early until A B          the same, up to the first block in which any voice of A fades (A.gain)
//   rings_early fade A B           A's fades against B (the twin without the rule, the same script): A = B x the gain
//                                  to within the rounding (the models without reverb), and the largest
//                                  sample-to-sample step in each fade against B's in the same samples
//   rings_early strike A B V T     voice V's strike at T seconds in A against B: the step into the strike, the
//                                  largest difference and the level over the first 0.1 s and 1 s
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "coop_plugin.h"
#include "op1/op1.h"

extern "C" const coop_engine_t ENGINE_rings;
extern "C" const coop_screen_t SCREEN_rings = { nullptr, nullptr };   // rings_screen.cc is not in this build
namespace op1 { uint32_t host_cycles; }
alignas(32) static uint8_t l1_lease[4096];
alignas(32) static uint8_t l1_scratch[1312];
static uint32_t leased;
extern "C" void* coop_l1_lease(uint32_t bytes) {
  bytes = (bytes + 31u) & ~31u;
  if (leased + bytes > sizeof(l1_lease)) return nullptr;
  void* p = l1_lease + leased;
  leased += bytes;
  return p;
}
extern "C" void* coop_l1_scratch(uint32_t* bytes) {
  *bytes = sizeof(l1_scratch);
  return l1_scratch;
}
extern "C" {
uint32_t g_coop_idle_blocks;
uint32_t g_coop_silent_renders[6];
int32_t g_coop_voice_peak;
int32_t g_coop_voice_gain;            // COOP_EARLY_PROBE: the early cut's gain after the render (Q30), 0 asleep
}

namespace {

const int kOut = 64;                     // samples a render (the 44.1 kHz voice)
const int kVoices = 6;
const int32_t kFull = 1 << 30;
const uint32_t kCyclesPerBlock = 580499;
const int kPerSecond = 689;              // blocks a second (689.06)

uint32_t Increment(int note) {           // f / 88200 * 2^32, as rings_compare.cc
  const uint64_t a4 = 21426229ull;
  static const uint32_t kSemi[12] = { 1073741824u, 1137589835u, 1205234447u, 1276901417u, 1352829926u, 1433273777u,
                                      1518502650u, 1608803398u, 1704480463u, 1805857285u, 1913275797u, 2027097902u };
  const int d = note - 69;
  const int oct = d >= 0 ? d / 12 : -((11 - d) / 12);
  const int semi = d - oct * 12;
  const uint64_t inc = (a4 * kSemi[semi]) >> 30;
  return static_cast<uint32_t>(oct >= 0 ? inc << oct : inc >> -oct);
}

#ifdef __bfin__
inline uint32_t RealCycles() { uint32_t c; __asm__ __volatile__("%0 = CYCLES;" : "=d"(c)); return c; }
#endif

struct Strike { int voice; long block; };
Strike g_strikes[64];
int g_num_strikes;
long g_damping_at = -1;                  // `d@T:V` in the script: the damping control goes to V at T seconds
int g_damping_to;
long g_all_period;                       // blocks, 0 = none

// Seconds (a decimal) to blocks, without floating point in the parse (the simulator's strtod is fine, but keep it plain).
long Blocks(const char* s) {
  long whole = 0, frac = 0, scale = 1;
  while (*s >= '0' && *s <= '9') whole = whole * 10 + (*s++ - '0');
  if (*s == '.') {
    ++s;
    while (*s >= '0' && *s <= '9' && scale < 1000) { frac = frac * 10 + (*s++ - '0'); scale *= 10; }
  }
  return whole * kPerSecond + frac * kPerSecond / scale;
}

void ParseScript(const char* s) {
  g_num_strikes = 0;
  g_all_period = 0;
  if (!std::strcmp(s, "none")) return;
  if (!std::strncmp(s, "all:", 4)) { g_all_period = Blocks(s + 4); return; }
  while (*s && g_num_strikes < 64) {
    if (*s == 'd') {
      g_damping_at = Blocks(s + 2);
      while (*s && *s != ':') ++s;
      g_damping_to = std::atoi(s + 1);
      while (*s && *s != ',') ++s;
      if (*s == ',') ++s;
      continue;
    }
    const int v = *s - '0';
    s += 2;                              // "v@"
    g_strikes[g_num_strikes].voice = v;
    g_strikes[g_num_strikes].block = Blocks(s);
    ++g_num_strikes;
    while (*s && *s != ',') ++s;
    if (*s == ',') ++s;
  }
}

double Db(int peak) { return peak > 0 ? 20.0 * std::log10(peak / 32768.0) : -999.0; }

int Run(const char* path, int model, int poly, int damping, long hold_s, int voices, int structure, int brightness,
        int transpose) {
  FILE* f = nullptr;
  FILE* fg = nullptr;
  if (path) {
    char gain_path[512];
    std::snprintf(gain_path, sizeof(gain_path), "%s.gain", path);
    f = std::fopen(path, "wb");
    fg = std::fopen(gain_path, "wb");
    if (!f || !fg) return 2;
  }
  if (ENGINE_rings.init && ENGINE_rings.init() != 0) return 3;
#ifdef __bfin__
  {
    uint32_t s;
    __asm__ __volatile__("%0 = SYSCFG;" : "=d"(s));
    s |= 2;   // CCEN
    __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(s));
  }
#endif
  int16_t k[8] = { static_cast<int16_t>(structure), static_cast<int16_t>(brightness), static_cast<int16_t>(damping),
                   9830, 0x5320, static_cast<int16_t>(model), static_cast<int16_t>(poly), 0 };
  static const int kChord[kVoices] = { 48, 55, 60, 64, 67, 72 };
  const long total = hold_s * kPerSecond;
  bool down[kVoices] = { false };
  long struck[kVoices], fade_at[kVoices], sleep_at[kVoices];
  int fade_sum[kVoices], fade_own[kVoices];
  for (int v = 0; v < kVoices; ++v) struck[v] = fade_at[v] = sleep_at[v] = -1;
  uint32_t seen[kVoices] = { 0 };
  int not_silent[kVoices] = { 0 };
  const long trace_from = std::getenv("EARLY_TRACE") ? Blocks(std::getenv("EARLY_TRACE")) : -1;
  const long trace_step = std::getenv("EARLY_TRACE_STEP") ? std::atol(std::getenv("EARLY_TRACE_STEP")) : 43;
  static int recent_sum[172], recent_own[kVoices][172];     // the last quarter second's peaks, per block
  long all_asleep = -1, all_silent = -1;
  long bad_zeros = 0, silent_checked = 0, asleep_not_silent = 0;
  int peak5 = 0;
  uint64_t cyc = 0;
  uint32_t cyc_max = 0;
  uint32_t pitch[128];
  int16_t out[128];
  static int16_t record[kVoices][kOut];
  int32_t gains[kVoices];
  std::printf("rings_early: model %d, polyphony setting %d, damping %d, %d keys held %ld s\n", model, poly, damping,
              voices, hold_s);
  for (long b = 0; b < total; ++b) {
    op1::host_cycles += kCyclesPerBlock;
    if (b == g_damping_at) k[2] = static_cast<int16_t>(g_damping_to);
    for (int v = 0; v < kVoices; ++v) {
      bool strike = v < voices && b == 3 * v;
      if (g_all_period && v < voices && b > 3 * v && (b - 3 * v) % g_all_period == 0) strike = true;
      for (int i = 0; i < g_num_strikes; ++i) strike |= g_strikes[i].voice == v && g_strikes[i].block == b;
      if (strike) {
        ENGINE_rings.note_on(v, nullptr, 127 << 8, k);
        down[v] = true;
        struck[v] = b;
        if (b > 3 * kVoices) {
          std::printf("  %.2f s: voice %d struck%s\n", b / double(kPerSecond), v,
                      sleep_at[v] >= 0 ? " (it was asleep)" : (fade_at[v] >= 0 ? " (it was fading)" : ""));
        }
        fade_at[v] = sleep_at[v] = -1;
      }
    }
    uint32_t block_cycles = 0;
    int awake = 0, silent = 0, keys = 0;
    int32_t sum[kOut];
    for (int i = 0; i < kOut; ++i) sum[i] = 0;
    for (int v = 0; v < kVoices; ++v) {
      gains[v] = -1;
      for (int i = 0; i < kOut; ++i) record[v][i] = 0;
      recent_own[v][b % 172] = 0;
      if (!down[v]) continue;
      ++keys;
      const uint32_t inc = Increment(kChord[v] + transpose);
      for (int i = 0; i < 128; ++i) pitch[i] = inc;
      for (int i = 0; i < 128; ++i) out[i] = 0x5a5a;
#ifdef __bfin__
      const uint32_t t0 = RealCycles();
      ENGINE_rings.render(v, nullptr, out, reinterpret_cast<const int32_t*>(pitch), k);
      block_cycles += RealCycles() - t0;
#else
      ENGINE_rings.render(v, nullptr, out, reinterpret_cast<const int32_t*>(pitch), k);
#endif
      const bool was_silent = g_coop_silent_renders[v] != seen[v];
      seen[v] = g_coop_silent_renders[v];
      const int32_t gain = was_silent ? 0 : g_coop_voice_gain;
      gains[v] = gain;
      int own = 0;
      for (int i = 0; i < kOut; ++i) {
        record[v][i] = out[i];
        sum[i] += out[i];
        const int a = out[i] < 0 ? -out[i] : out[i];
        if (a > own) own = a;
      }
      recent_own[v][b % 172] = own;
      if (trace_from >= 0 && b >= trace_from && b % trace_step == 0 && !was_silent) {   // EARLY_TRACE=<seconds> [EARLY_TRACE_STEP=<blocks>, 43: 1/16 s]
        std::printf("    %.2f s voice %d: own peak %ld (Q27), gain %ld, output peak %d LSB\n", b / double(kPerSecond), v,
                    static_cast<long>(g_coop_voice_peak), static_cast<long>(gain), own);
      }
      if (was_silent) {
        ++silent;
        ++silent_checked;
        if (own) ++bad_zeros;
      }
      if (gain > 0) ++awake;
      // Asleep after a render that was not silent: the next ones must be (but for a steal's fade, 2 renders).
      if (gain == 0 && !was_silent && model != 5) { if (++not_silent[v] > 3) ++asleep_not_silent; } else { not_silent[v] = 0; }
    }
    int block_peak = 0;
    for (int i = 0; i < kOut; ++i) { const int a = sum[i] < 0 ? -sum[i] : sum[i]; if (a > block_peak) block_peak = a; }
    recent_sum[b % 172] = block_peak;
    if (block_peak > peak5) peak5 = block_peak;
    for (int v = 0; v < kVoices; ++v) {
      if (gains[v] < 0) continue;
      int rs = 0, ro = 0;
      for (int i = 0; i < 172; ++i) { if (recent_sum[i] > rs) rs = recent_sum[i]; if (recent_own[v][i] > ro) ro = recent_own[v][i]; }
      if (gains[v] < kFull && gains[v] > 0 && fade_at[v] < 0) { fade_at[v] = b; fade_sum[v] = rs; fade_own[v] = ro; }
      if (gains[v] == kFull && fade_at[v] >= 0 && sleep_at[v] < 0) {                     // it came back up
        std::printf("  voice %d: its fade from %.2f s turned back, the gain full again at %.2f s\n", v,
                    fade_at[v] / double(kPerSecond), b / double(kPerSecond));
        fade_at[v] = -1;
      }
      if (gains[v] == 0 && sleep_at[v] < 0) {
        sleep_at[v] = b;
        std::printf("  voice %d: ", v);
        if (fade_at[v] >= 0) {
          std::printf("fade from %.2f s after its strike (the sum's peak %d LSB, %.1f dBFS; its own output %d LSB), ",
                      (fade_at[v] - struck[v]) / double(kPerSecond), fade_sum[v], Db(fade_sum[v]), fade_own[v]);
        }
        std::printf("asleep %.2f s after its strike (the sum's peak %d LSB, %.1f dBFS)\n",
                    (b - struck[v]) / double(kPerSecond), rs, Db(rs));
      }
    }
    if (keys && awake == 0) { if (all_asleep < 0) all_asleep = b; } else { all_asleep = -1; }
    if (keys && silent == keys) { if (all_silent < 0) all_silent = b; } else { all_silent = -1; }
    cyc += block_cycles;
    if (block_cycles > cyc_max) cyc_max = block_cycles;
    if (f) std::fwrite(record, sizeof(record), 1, f);
    if (fg) std::fwrite(gains, sizeof(gains), 1, fg);
    if (b % (5 * kPerSecond) == 5 * kPerSecond - 1) {
      std::printf("  %3ld s: summed peak %5d LSB (%6.1f dBFS), %d awake, %d silent renders", (b + 1) / kPerSecond, peak5,
                  Db(peak5), awake, silent);
#ifdef __bfin__
      std::printf(", cycles a block avg %lu max %lu", static_cast<unsigned long>(cyc / (5 * kPerSecond)),
                  static_cast<unsigned long>(cyc_max));
#endif
      std::printf("\n");
      peak5 = 0;
      cyc = 0;
      cyc_max = 0;
    }
  }
  if (f) std::fclose(f);
  if (fg) std::fclose(fg);
  if (all_asleep >= 0) std::printf("  every voice asleep from %.2f s", all_asleep / double(kPerSecond)); else std::printf("  a voice still awake at the end");
  if (all_silent >= 0) std::printf("; every render silent from %.2f s\n", all_silent / double(kPerSecond)); else std::printf("; not every render silent at the end\n");
  std::printf("  silent renders: %ld, with a non-zero sample among the 64: %ld; asleep and still rendered (models without reverb): %ld\n",
              silent_checked, bad_zeros, asleep_not_silent);
  return bad_zeros || asleep_not_silent ? 1 : 0;
}

struct Pair {
  FILE* a = nullptr;
  FILE* b = nullptr;
  FILE* g = nullptr;
  bool Open(const char* pa, const char* pb, bool gain) {
    a = std::fopen(pa, "rb");
    b = std::fopen(pb, "rb");
    if (gain) {
      char gp[512];
      std::snprintf(gp, sizeof(gp), "%s.gain", pa);
      g = std::fopen(gp, "rb");
    }
    return a && b && (!gain || g);
  }
};

// until = true: only up to A's first fading block.
int Cmp(const char* pa, const char* pb, bool until) {
  Pair p;
  if (!p.Open(pa, pb, until)) return 2;
  static int16_t xa[kVoices][kOut], xb[kVoices][kOut];
  int32_t gains[kVoices];
  long n = 0, differ = 0, first = -1, fade = -1;
  int worst = 0;
  while (std::fread(xa, sizeof(xa), 1, p.a) == 1 && std::fread(xb, sizeof(xb), 1, p.b) == 1) {
    if (until) {
      if (std::fread(gains, sizeof(gains), 1, p.g) != 1) break;
      for (int v = 0; v < kVoices; ++v) if (gains[v] > 0 && gains[v] < kFull) fade = n;
      if (fade >= 0) break;
    }
    for (int v = 0; v < kVoices; ++v) {
      for (int i = 0; i < kOut; ++i) {
        const int d = std::abs(xa[v][i] - xb[v][i]);
        if (d) { ++differ; if (first < 0) first = n; }
        if (d > worst) worst = d;
      }
    }
    ++n;
  }
  std::printf("  %ld blocks (%.1f s) compared", n, n / double(kPerSecond));
  if (until) { if (fade >= 0) std::printf(", up to the first fade"); else std::printf(" (no voice fades)"); }
  std::printf(": %ld samples differ", differ);
  if (differ) std::printf(", largest %d LSB, the first %.2f s in", worst, first / double(kPerSecond));
  std::printf("\n");
  return differ ? 1 : 0;
}

// A's fades against B. In a fade (a voice's gain under full, until it is 0 or full again, and the block after):
//   the largest |A - B x gain| (the gain taken as a line across each render), where B is the twin's same sample;
//   the largest step between neighbouring samples, in A and in B.
int Fade(const char* pa, const char* pb, int model) {
  Pair p;
  if (!p.Open(pa, pb, true)) return 2;
  static int16_t xa[kVoices][kOut], xb[kVoices][kOut];
  int32_t gains[kVoices], last_gain[kVoices];
  int last_a[kVoices] = { 0 }, last_b[kVoices] = { 0 };
  int tail[kVoices] = { 0 };
  bool valid[kVoices];
  for (int v = 0; v < kVoices; ++v) { last_gain[v] = kFull; valid[v] = true; }
  long fades = 0, blocks = 0;
  double worst_fit = 0;
  int step_a = 0, step_b = 0, worst_excess = 0;
  int last_a_sample_before_sleep = 0;
  for (long n = 0; std::fread(xa, sizeof(xa), 1, p.a) == 1 && std::fread(xb, sizeof(xb), 1, p.b) == 1 &&
                   std::fread(gains, sizeof(gains), 1, p.g) == 1; ++n) {
    for (int v = 0; v < kVoices; ++v) {
      if (gains[v] < 0) continue;
      const int32_t g0 = last_gain[v], g1 = gains[v];
      const bool fading = (g0 < kFull && g0 > 0) || (g1 < kFull && g1 > 0);
      if (fading && g0 == kFull) ++fades;
      if (g1 == 0 && g0 > 0 && g0 < kFull) tail[v] = 2;    // a fade's end: this render and the first asleep one too
      if (fading || tail[v] > 0) {
        ++blocks;
        int sa = 0, sb = 0;
        for (int i = 0; i < kOut; ++i) {
          const int a = xa[v][i], bb = xb[v][i];
          const int da = std::abs(a - (i ? xa[v][i - 1] : last_a[v]));
          const int db = std::abs(bb - (i ? xb[v][i - 1] : last_b[v]));
          if (da > sa) sa = da;
          if (db > sb) sb = db;
          if (model != 5 && valid[v] && fading) {
            const double g = (g0 + (double(g1) - g0) * (i + 1) / kOut) / kFull;
            const double fit = std::fabs(a - bb * g);
            if (fit > worst_fit) worst_fit = fit;
          }
        }
        if (sa > step_a) step_a = sa;
        if (sb > step_b) step_b = sb;
        if (sa - sb > worst_excess) worst_excess = sa - sb;
        if (g1 == 0 && g0 > 0 && g0 < kFull) {
          for (int i = 0; i < kOut; ++i) if (std::abs(xa[v][i]) > last_a_sample_before_sleep) last_a_sample_before_sleep = std::abs(xa[v][i]);
        }
        if (tail[v] > 0 && !fading) --tail[v];
      }
      if (g1 == 0) valid[v] = false;                        // after a sleep the twin is no longer this voice's reference
      last_a[v] = xa[v][kOut - 1];
      last_b[v] = xb[v][kOut - 1];
      last_gain[v] = g1;
    }
  }
  std::printf("  %ld fades, %ld renders in them: the largest step between two samples %d LSB (the twin's in the same renders %d; "
              "the most a render's exceeds the twin's: %d)", fades, blocks, step_a, step_b, worst_excess);
  if (model != 5) std::printf("; |output - twin x gain| at most %.2f LSB", worst_fit);
  std::printf("; the fade's last render peaks at %d LSB\n", last_a_sample_before_sleep);
  return 0;
}

// Voice V's strike at T seconds: A against B.
int StrikeCmp(const char* pa, const char* pb, int v, const char* t) {
  Pair p;
  if (!p.Open(pa, pb, false)) return 2;
  const long at = Blocks(t);
  static int16_t xa[kVoices][kOut], xb[kVoices][kOut];
  int prev_a = 0, prev_b = 0, sprev_a = 0, sprev_b = 0;
  int before_a = 0, before_b = 0;                           // the voice's peak in the 20 blocks before the strike
  int step_a = 0, step_b = 0, sum_step_a = 0, sum_step_b = 0;
  int worst01 = 0, worst1 = 0;
  double ea01 = 0, eb01 = 0, ea1 = 0, eb1 = 0, sea1 = 0, seb1 = 0;
  int first_a = 0, first_b = 0;
  for (long n = 0; std::fread(xa, sizeof(xa), 1, p.a) == 1 && std::fread(xb, sizeof(xb), 1, p.b) == 1; ++n) {
    if (n >= at - 20 && n < at) {
      for (int i = 0; i < kOut; ++i) {
        if (std::abs(xa[v][i]) > before_a) before_a = std::abs(xa[v][i]);
        if (std::abs(xb[v][i]) > before_b) before_b = std::abs(xb[v][i]);
      }
    }
    if (n >= at && n < at + kPerSecond) {
      for (int i = 0; i < kOut; ++i) {
        int sa = 0, sb = 0;
        for (int u = 0; u < kVoices; ++u) { sa += xa[u][i]; sb += xb[u][i]; }
        if (n == at) {                                      // the strike's own render: the steps into and within it
          const int da = std::abs(xa[v][i] - prev_a), db = std::abs(xb[v][i] - prev_b);
          if (da > step_a) step_a = da;
          if (db > step_b) step_b = db;
          if (std::abs(sa - sprev_a) > sum_step_a) sum_step_a = std::abs(sa - sprev_a);
          if (std::abs(sb - sprev_b) > sum_step_b) sum_step_b = std::abs(sb - sprev_b);
          if (i == 0) { first_a = xa[v][0] - prev_a; first_b = xb[v][0] - prev_b; }
        }
        const int d = std::abs(xa[v][i] - xb[v][i]);
        const double a = xa[v][i], bb = xb[v][i];
        if (n < at + 69) { if (d > worst01) worst01 = d; ea01 += a * a; eb01 += bb * bb; }
        if (d > worst1) worst1 = d;
        ea1 += a * a;
        eb1 += bb * bb;
        sea1 += double(sa) * sa;
        seb1 += double(sb) * sb;
        prev_a = xa[v][i]; prev_b = xb[v][i]; sprev_a = sa; sprev_b = sb;
      }
    } else {
      prev_a = xa[v][kOut - 1];
      prev_b = xb[v][kOut - 1];
      sprev_a = sprev_b = 0;
      for (int u = 0; u < kVoices; ++u) { sprev_a += xa[u][kOut - 1]; sprev_b += xb[u][kOut - 1]; }
    }
  }
  auto db = [](double a, double b) { return 10.0 * std::log10((a + 1e-9) / (b + 1e-9)); };
  std::printf("  voice %d struck at %s s: before it the voice peaks at %d LSB (the other file: %d); the first sample's step %d (%d); "
              "the largest step in the strike's render %d (%d), in the sum %d (%d)\n", v, t, before_a, before_b,
              first_a, first_b, step_a, step_b, sum_step_a, sum_step_b);
  std::printf("    the voice against the other file's: largest difference %d LSB in 0.1 s, %d LSB in 1 s; level %+.2f dB over 0.1 s, "
              "%+.2f dB over 1 s (RMS %.0f LSB, the other %.0f); the sum's level %+.2f dB over 1 s\n", worst01, worst1,
              db(ea01, eb01), db(ea1, eb1), std::sqrt(ea1 / (kPerSecond * kOut)), std::sqrt(eb1 / (kPerSecond * kOut)),
              db(sea1, seb1));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 9 && !std::strcmp(argv[1], "run")) {
    ParseScript(argv[8]);
    return Run(std::strcmp(argv[2], "-") ? argv[2] : nullptr, std::atoi(argv[3]), std::atoi(argv[4]), std::atoi(argv[5]),
               std::atol(argv[6]), std::atoi(argv[7]), argc > 9 ? std::atoi(argv[9]) : 8192,
               argc > 10 ? std::atoi(argv[10]) : 19660, argc > 11 ? std::atoi(argv[11]) : 0);
  }
  if (argc >= 4 && !std::strcmp(argv[1], "cmp")) return Cmp(argv[2], argv[3], false);
  if (argc >= 4 && !std::strcmp(argv[1], "until")) return Cmp(argv[2], argv[3], true);
  if (argc >= 5 && !std::strcmp(argv[1], "fade")) return Fade(argv[2], argv[3], std::atoi(argv[4]));
  if (argc >= 6 && !std::strcmp(argv[1], "strike")) return StrikeCmp(argv[2], argv[3], std::atoi(argv[4]), argv[5]);
  std::fprintf(stderr, "usage: %s run OUT|- MODEL POLY DAMPING HOLD VOICES SCRIPT [STRUCTURE [BRIGHTNESS [TRANSPOSE]]] | "
               "cmp A B | until A B | fade A B MODEL | strike A B VOICE T\n", argv[0]);
  return 2;
}
