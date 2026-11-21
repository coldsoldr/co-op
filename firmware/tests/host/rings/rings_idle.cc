// rings 0.11: the shared reverb's rounding and its idle state (plugins/rings/src/reverb_fixed.cc Compress,
// op1/reverb_bus.h; docs/efficiency-audit.md finding 6), in the string + reverb model with keys held. The core renders
// a voice for as long as its key is down, so until 0.10 the reverb, and each held voice's limiter and upsampler, ran
// on until the key came up; and the reverb's floored rounding left a held key's output on a steady offset (-52 LSB).
//
// One script, the engine as it ships (four voices, every key held throughout, so every voice renders every block):
//   part 1  10 s, a note every 0.2 s (no string sleeps)
//   part 2  a chord struck and held HOLD seconds
//   part 3  a soft note into the held chord, 3 s; a loud one, 5 s
// `rings_idle run OUT.raw [DAMPING [HOLD]]` plays it and writes every rendered block (DAMPING: the damping knob,
// 0..32767, default 24000: it sets the string's and the reverb's decay; HOLD default 24).
// `rings_idle cmp A.raw B.raw IDLE_BLOCK [HOLD]` compares two such files part by part.
//
// The Makefile's `idle` builds three: the engine as it ships; its twin with -DOP1RINGS_NO_REVERB_IDLE (the same
// reverb, never idle); and 0.10's sources (OLD=<folder>, when given). Shipped against the twin must be the same
// sample for sample in every part, idle or not, before and after the wake. Shipped against 0.10 is the change in the
// sound: the report gives the difference and, per file, where the tail ends and what level a held key rests on.
// `idle-sim`: the cycles of a block of four held voices, strings asleep, before the idle state and in it.
// `rings_idle reverb`: the reverb alone (ReverbFixed), a quarter second of noise and then ten minutes of silence, at
// the patch's reverb time and at the longest: when its output is exact zero for good, or what it rests on.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "coop_plugin.h"
#include "op1/op1.h"
#include "src/reverb_fixed.h"

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
// The engine's probe (COOP_IDLE_PROBE): renders that took the idle state's silent path.
extern "C" {
uint32_t g_coop_idle_blocks;
uint32_t g_coop_silent_renders[6];   // every silent render, per voice (rings_sleep.cc reads it)
int32_t g_coop_voice_peak;           // the last render's own peak, Q27 (the same)
}

namespace {

const int kVoices = 4;                   // the model plays four at most
const int kBlock = 128;                  // samples a render the core hears (each of the engine's 64 twice)
const int kModel = 5, kPoly = 0;         // string + reverb, polyphony "all"
const uint32_t kCyclesPerBlock = 580499;

// The script's parts, in blocks (689 a second).
const long kPart1 = 6890;
long g_part2 = 16536;                    // HOLD seconds x 689
const long kPart3a = 2067, kPart3b = 3445;
long Total() { return kPart1 + g_part2 + kPart3a + kPart3b; }

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

int Run(const char* path, int damping) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return 2;
  if (ENGINE_rings.init && ENGINE_rings.init() != 0) return 3;
#ifdef __bfin__
  {
    uint32_t s;
    __asm__ __volatile__("%0 = SYSCFG;" : "=d"(s));
    s |= 2;   // CCEN
    __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(s));
  }
  uint64_t cyc[3] = { 0, 0, 0 };         // a block's four renders: playing (part 1) / the chord asleep, not idle / idle
  uint32_t cyc_n[3] = { 0, 0, 0 }, cyc_max[3] = { 0, 0, 0 };
  long wake_cycles = 0;
#endif
  // The kennel defaults but the damping: structure, brightness, damping, position.
  int16_t k[8] = { 8192, 19660, static_cast<int16_t>(damping), 9830, 0x5320, kModel, kPoly, 0 };
  static const int kChord[kVoices] = { 48, 55, 60, 64 };
  int note[kVoices] = { 48, 55, 60, 64 };
  long first_idle = -1, idle_renders = 0, all_idle_from = -1, last_sound = -1, idle_in_part1 = 0, idle_in_part3 = 0;
  uint32_t pitch[kBlock];
  int16_t out[kBlock];
  const long total = Total();
  for (long b = 0; b < total; ++b) {
    op1::host_cycles += kCyclesPerBlock;
    if (b < kPart1) {
      if (b % 138 == 0) {
        const int v = static_cast<int>((b / 138) % kVoices);
        note[v] = kChord[v] + static_cast<int>((b / 552) % 5);
        ENGINE_rings.note_on(v, nullptr, 127 << 8, k);
      }
    } else if (b == kPart1) {
      for (int v = 0; v < kVoices; ++v) {
        note[v] = kChord[v];
        ENGINE_rings.note_on(v, nullptr, 127 << 8, k);
      }
    } else if (b == kPart1 + g_part2) {
      note[1] = 67;
      ENGINE_rings.note_on(1, nullptr, 6 << 8, k);          // soft
    } else if (b == kPart1 + g_part2 + kPart3a) {
      note[2] = 52;
      ENGINE_rings.note_on(2, nullptr, 127 << 8, k);        // loud
    }
    const uint32_t before = g_coop_idle_blocks;
#ifdef __bfin__
    uint32_t block_cycles = 0;
#endif
    bool sound = false;
    for (int v = 0; v < kVoices; ++v) {                      // every key is held: every voice renders, every block
      const uint32_t inc = Increment(note[v]);
      for (int i = 0; i < kBlock; ++i) pitch[i] = inc;
#ifdef __bfin__
      const uint32_t t0 = RealCycles();
      ENGINE_rings.render(v, nullptr, out, reinterpret_cast<const int32_t*>(pitch), k);
      block_cycles += RealCycles() - t0;
#else
      ENGINE_rings.render(v, nullptr, out, reinterpret_cast<const int32_t*>(pitch), k);
#endif
      if (ENGINE_rings.flags & COOP_ENGINE_F_44K) {          // what the core does: every sample twice, in place
        for (int i = COOP_VOICE_BLOCK_44K - 1; i >= 0; --i) out[2 * i + 1] = out[2 * i] = out[i];
      }
      for (int i = 0; i < kBlock; ++i) if (out[i]) sound = true;
      std::fwrite(out, sizeof(out), 1, f);
    }
    const uint32_t idled = g_coop_idle_blocks - before;
    if (idled && first_idle < 0) first_idle = b;
    if (idled && !sound && all_idle_from < 0 && b >= kPart1) all_idle_from = b;
    if (b < kPart1) idle_in_part1 += idled;
    if (b >= kPart1 + g_part2 && b < kPart1 + g_part2 + 345) idle_in_part3 += idled;              // 0.5 s after a note
    if (sound && b >= kPart1 && b < kPart1 + g_part2) last_sound = b;
    idle_renders += idled;
#ifndef __bfin__
    if (std::getenv("RINGS_IDLE_TRACE") && b >= kPart1 && b % 689 == 0)
      std::printf("    %5.1f s after the chord: %u of 4 renders idle, sound %d\n", (b - kPart1) / 689.0, idled, sound);
#else
    {
      int s = -1;
      if (b >= 2000 && b < kPart1) s = 0;
      else if (b >= kPart1 && b < kPart1 + g_part2 && !sound) s = idled ? 2 : 1;
      if (s >= 0) { cyc[s] += block_cycles; ++cyc_n[s]; if (block_cycles > cyc_max[s]) cyc_max[s] = block_cycles; }
      if (b == kPart1 + g_part2) wake_cycles = block_cycles;
    }
#endif
  }
  std::fclose(f);
  std::printf("rings_idle run (damping %d, the chord held %.0f s): the chord's last non-zero output %.2f s after it; "
              "renders through the idle state's silent path: %ld (%ld while notes played in part 1, %ld in the "
              "half second after the soft note); ", damping, g_part2 / 689.0,
              last_sound < 0 ? -1.0 : (last_sound + 1 - kPart1) / 689.0, idle_renders, idle_in_part1, idle_in_part3);
  if (first_idle >= 0) {
    std::printf("the first in block %ld, the held chord idle from %.2f s after it was struck (IDLE_BLOCK %ld)\n",
                first_idle, all_idle_from < 0 ? -1.0 : (all_idle_from - kPart1) / 689.0, all_idle_from);
  } else {
    std::printf("never idle (IDLE_BLOCK 0)\n");
  }
#ifdef __bfin__
  static const char* const kName[3] = { "playing (a note every 0.2 s)", "the chord asleep, not idle", "idle" };
  std::printf("cycles a block, the four held voices' renders together (GNU simulator, compute only):\n");
  for (int s = 0; s < 3; ++s) {
    std::printf("  %-30s avg %6lu max %6lu (%lu blocks)\n", kName[s],
                static_cast<unsigned long>(cyc_n[s] ? cyc[s] / cyc_n[s] : 0), static_cast<unsigned long>(cyc_max[s]),
                static_cast<unsigned long>(cyc_n[s]));
  }
  std::printf("  the block of the note that wakes: %ld\n", wake_cycles);
#endif
  return 0;
}

// The reverb alone: wet only, the model's diffusion and input gain; time in Q31.
void ReverbTail(const char* label, int32_t time_q31) {
  static op1rings::ReverbFixed rv;
  rv.Init();
  rv.set_amount(INT32_MAX);
  rv.set_diffusion(1342177280);
  rv.set_time(time_q31);
  rv.set_input_gain(429496730);
  rv.set_lp(1932735283);   // 0.9
  uint32_t seed = 7;
  const long total = 413400;           // blocks of 32 at 22.05 kHz: 10 minutes
  long last = -1;
  int64_t rest_sum = 0;
  int32_t rest_peak = 0;
  int32_t x[32];
  for (long b = 0; b < total; ++b) {
    for (int i = 0; i < 32; ++i) {
      seed = seed * 1664525u + 1013904223u;
      x[i] = b < 172 ? static_cast<int32_t>(seed >> 6) - (1 << 25) : 0;   // +-0.25 of full scale, Q27
    }
    rv.Process(x, 32);
    for (int i = 0; i < 32; ++i) {
      if (x[i]) last = b;
      if (b >= total - 6890) {
        rest_sum += x[i];
        const int32_t a = x[i] < 0 ? -x[i] : x[i];
        if (a > rest_peak) rest_peak = a;
      }
    }
  }
  std::printf("  the reverb alone, %s: exact zero for good from %.2f s after the noise; in the last 10 s of 10 minutes "
              "its output's peak is %ld and its mean %ld (Q27: a 16-bit LSB is 4096)\n", label,
              last < 172 ? 0.0 : (last + 1 - 172) * 32 / 22050.0, static_cast<long>(rest_peak),
              static_cast<long>(rest_sum / (6890 * 32)));
}

struct Part {
  const char* name;
  long from, to;                         // blocks
  bool same;                             // the twin's check: must be identical
};

double Root(double v) {
  if (v <= 0) return 0;
  double x = v;
  for (int i = 0; i < 80; ++i) x = 0.5 * (x + v / x);
  return x;
}

// One file's held chord: where its swing (max - min over a quarter second, the four voices summed) last exceeds a
// level, and the mean it rests on in the part's last two seconds.
void Tail(const char* path, const char* label) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return;
  static int16_t x[kVoices * kBlock];
  std::fseek(f, kPart1 * static_cast<long>(sizeof(x)), SEEK_SET);
  static const int kLevels[4] = { 2048, 256, 32, 0 };
  long last[4] = { -1, -1, -1, -1 };
  int lo = 0, hi = 0;
  double mean = 0;
  long mean_n = 0;
  for (long b = 0; b < g_part2; ++b) {
    if (std::fread(x, sizeof(x), 1, f) != 1) break;
    if (b % 172 == 0) { lo = 1 << 30; hi = -(1 << 30); }
    for (int i = 0; i < kBlock; ++i) {
      int s = 0;
      for (int v = 0; v < kVoices; ++v) s += x[v * kBlock + i];
      if (s < lo) lo = s;
      if (s > hi) hi = s;
      if (b >= g_part2 - 1378) { mean += s; ++mean_n; }
    }
    if (b % 172 == 171) {
      for (int k = 0; k < 4; ++k) if (hi - lo > kLevels[k]) last[k] = b;
    }
  }
  std::fclose(f);
  std::printf("  %-10s the held chord's swing last over 2048 LSB at %.2f s, over 256 at %.2f s, over 32 at %.2f s, over 0 "
              "at %.2f s; it rests on %.2f LSB\n", label, (last[0] + 1) / 689.0, (last[1] + 1) / 689.0,
              (last[2] + 1) / 689.0, (last[3] + 1) / 689.0, mean_n ? mean / mean_n : 0.0);
}

int Cmp(const char* pa, const char* pb, long idle_from, bool twin) {
  FILE* a = std::fopen(pa, "rb");
  FILE* b = std::fopen(pb, "rb");
  if (!a || !b) return 2;
  const long idle_at = idle_from > kPart1 ? idle_from : kPart1 + g_part2;
  const Part parts[] = {
    { "1. a note every 0.2 s (10 s)", 0, kPart1, true },
    { "2. the chord held, first 3 s", kPart1, kPart1 + 2067, true },
    { "2. the chord held, to the idle state", kPart1 + 2067, idle_at, true },
    { "2. the chord held, idle", idle_at, kPart1 + g_part2, true },
    { "3. the soft note (3 s)", kPart1 + g_part2, kPart1 + g_part2 + kPart3a, true },
    { "3. the loud note (5 s)", kPart1 + g_part2 + kPart3a, Total(), true },
  };
  int fail = 0;
  static int16_t xa[kVoices * kBlock], xb[kVoices * kBlock];
  long blk = 0;
  for (const Part& p : parts) {
    long differ = 0, at = -1;
    int worst = 0, peak_a = 0, peak_b = 0;
    double sa = 0, sb = 0, sd = 0, md = 0;
    long n = 0;
    for (; blk < p.to; ++blk) {
      if (std::fread(xa, sizeof(xa), 1, a) != 1 || std::fread(xb, sizeof(xb), 1, b) != 1) { std::printf("short file\n"); return 2; }
      for (int i = 0; i < kVoices * kBlock; ++i) {
        const int d = xa[i] > xb[i] ? xa[i] - xb[i] : xb[i] - xa[i];
        if (d) ++differ;
        if (d > worst) { worst = d; at = blk; }
        const int ua = xa[i] < 0 ? -xa[i] : xa[i], ub = xb[i] < 0 ? -xb[i] : xb[i];
        if (ua > peak_a) peak_a = ua;
        if (ub > peak_b) peak_b = ub;
      }
      for (int i = 0; i < kBlock; ++i) {       // the levels: of the four voices summed (what is heard)
        int ma = 0, mb = 0;
        for (int v = 0; v < kVoices; ++v) { ma += xa[v * kBlock + i]; mb += xb[v * kBlock + i]; }
        sa += static_cast<double>(ma) * ma;
        sb += static_cast<double>(mb) * mb;
        sd += static_cast<double>(ma - mb) * (ma - mb);
        md += ma - mb;
        ++n;
      }
    }
    if (p.to <= p.from) continue;
    const double mean = md / (n ? n : 1);
    std::printf("  %-38s %8ld samples differ, largest %5d LSB (block %ld); peak %5d / %5d LSB; the voices summed: "
                "RMS %.2f / %.2f; their difference: mean %.2f, RMS %.2f, RMS about its mean %.2f LSB\n", p.name, differ, worst, at, peak_a,
                peak_b, Root(sa / (n ? n : 1)), Root(sb / (n ? n : 1)), mean, Root(sd / (n ? n : 1)),
                Root(sd / (n ? n : 1) - mean * mean));
    if (twin && differ) { ++fail; std::printf("FAIL: the idle build and its twin differ\n"); }
  }
  std::fclose(a);
  std::fclose(b);
  Tail(pa, "first:");
  Tail(pb, "second:");
  std::printf(fail ? "rings idle: FAILED\n" : "rings idle: compared\n");
  return fail;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef __bfin__
  (void)argc; (void)argv;
  return Run("/dev/null", 24000);
#else
  if (argc >= 2 && !std::strcmp(argv[1], "reverb")) {
    ReverbTail("time 0.81 (the test's patch)", 1739461754);
    ReverbTail("time 0.98 (damping at the top)", 2104533975);
    return 0;
  }
  if (argc >= 3 && !std::strcmp(argv[1], "run")) {
    if (argc > 4) g_part2 = std::atol(argv[4]) * 689;
    return Run(argv[2], argc > 3 ? std::atoi(argv[3]) : 24000);
  }
  if (argc >= 5 && (!std::strcmp(argv[1], "cmp") || !std::strcmp(argv[1], "twin"))) {
    if (argc > 5) g_part2 = std::atol(argv[5]) * 689;
    return Cmp(argv[2], argv[3], std::atol(argv[4]), !std::strcmp(argv[1], "twin"));
  }
  std::fprintf(stderr, "usage: %s run OUT.raw [DAMPING [HOLD]] | twin|cmp A.raw B.raw IDLE_BLOCK [HOLD]\n", argv[0]);
  return 2;
#endif
}
