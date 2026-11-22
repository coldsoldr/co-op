// rings: when a held voice goes to sleep (plugins/rings/src/op1_voice.h asleep(), rings.cc), for every model.
// The core renders a voice for as long as its key is down; a voice that still computes while its 16-bit output is all
// zeros is the waste this looks for, and a voice asleep must cost next to nothing and change no other voice.
//
//   rings_sleep run OUT.raw MODEL POLY DAMPING HOLD [VOICES [RESTRIKE [STRUCTURE [BRIGHTNESS]]]]
// VOICES keys (default 6) struck three blocks apart and held HOLD seconds; with RESTRIKE (seconds, 0 = none) voice 0's
// key is struck again every RESTRIKE seconds and voice 1's once at two thirds of HOLD (into strings asleep, half
// decayed or still loud, as it falls). Every voice renders every block. Writes each render's 128 samples and prints,
// per voice, the last render with a non-zero sample and the first render through a silent path; per 5 s, the summed
// output's peak, the voices awake and (simulator) the cycles a block.
//   rings_sleep cmp A.raw B.raw VOICES
// the two files sample by sample (the build as it ships against its twin, -DOP1RINGS_NO_SLEEP, or an older version).
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
// The engine's probe (COOP_IDLE_PROBE): renders through a silent path, per voice; and the reverb's idle renders.
extern "C" {
uint32_t g_coop_idle_blocks;
uint32_t g_coop_silent_renders[6];
int32_t g_coop_voice_peak;            // RenderMono's own peak of the last render (Q27: 4096 is one 16-bit LSB)
}

namespace {

const int kBlock = 128;
const uint32_t kCyclesPerBlock = 580499;

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

int g_structure = -1, g_brightness = -1;   // the command line's (the simulator passes no environment)

int Env(const char* name, int fallback) {
  const char* s = std::getenv(name);
  return s ? std::atoi(s) : fallback;
}

int Run(const char* path, int model, int poly, int damping, long hold_s, int voices, long restrike_s) {
  FILE* f = path ? std::fopen(path, "wb") : nullptr;
  if (ENGINE_rings.init && ENGINE_rings.init() != 0) return 3;
#ifdef __bfin__
  {
    uint32_t s;
    __asm__ __volatile__("%0 = SYSCFG;" : "=d"(s));
    s |= 2;   // CCEN
    __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(s));
  }
#endif
  // The plugin's defaults (plugin.json) but for the damping; SLEEP_STRUCTURE, SLEEP_BRIGHTNESS, SLEEP_POSITION and
  // SLEEP_TRANSPOSE (semitones) in the environment change the rest.
  int16_t k[8] = { static_cast<int16_t>(g_structure >= 0 ? g_structure : Env("SLEEP_STRUCTURE", 8192)),
                   static_cast<int16_t>(g_brightness >= 0 ? g_brightness : Env("SLEEP_BRIGHTNESS", 19660)),
                   static_cast<int16_t>(damping), static_cast<int16_t>(Env("SLEEP_POSITION", 9830)), 0x5320,
                   static_cast<int16_t>(model), static_cast<int16_t>(poly), 0 };
  const int transpose = Env("SLEEP_TRANSPOSE", 0);
  static const int kChord[6] = { 48, 55, 60, 64, 67, 72 };
  long last_sound[6], first_silent[6];
  for (int v = 0; v < 6; ++v) last_sound[v] = first_silent[v] = -1;
  uint32_t pitch[kBlock];
  int16_t out[kBlock];
  const long total = hold_s * 689;
  int peak = 0;
  uint64_t cyc = 0;
  uint32_t cyc_max = 0;
  uint32_t seen[6] = { 0 };
  long last_ge[3] = { -1, -1, -1 };                          // the summed output's last block at >= 1, 2, 4 LSB
  const bool quiet = std::getenv("SLEEP_QUIET") != nullptr;
  const bool trace = std::getenv("SLEEP_TRACE") != nullptr;  // per voice and 5 s: its own Q27 peak, its output's range
  int32_t own_peak[6] = { 0 };
  int lo[6] = { 0 }, hi[6] = { 0 };
  std::printf("rings_sleep: model %d, polyphony setting %d, damping %d, %d keys held %ld s%s\n", model, poly, damping,
              voices, hold_s, restrike_s ? ", with restrikes" : "");
  for (long b = 0; b < total; ++b) {
    op1::host_cycles += kCyclesPerBlock;
    for (int v = 0; v < voices; ++v) {
      bool strike = b == 3 * v;
      if (restrike_s && v == 0 && b > 0 && b % (restrike_s * 689) == 0) strike = true;
      if (restrike_s && v == 1 && b == total * 2 / 3) strike = true;
      if (strike) ENGINE_rings.note_on(v, nullptr, 127 << 8, k);
    }
    uint32_t block_cycles = 0;
    int awake = 0;
    int32_t sum[kBlock];
    for (int i = 0; i < kBlock; ++i) sum[i] = 0;
    for (int v = 0; v < voices; ++v) {
      if (b < 3 * v) continue;                               // its key is not down yet
      const uint32_t inc = Increment(kChord[v] + transpose);
      for (int i = 0; i < kBlock; ++i) pitch[i] = inc;
#ifdef __bfin__
      const uint32_t t0 = RealCycles();
      ENGINE_rings.render(v, nullptr, out, reinterpret_cast<const int32_t*>(pitch), k);
      block_cycles += RealCycles() - t0;
#else
      ENGINE_rings.render(v, nullptr, out, reinterpret_cast<const int32_t*>(pitch), k);
#endif
      if (ENGINE_rings.flags & COOP_ENGINE_F_44K) {
        for (int i = COOP_VOICE_BLOCK_44K - 1; i >= 0; --i) out[2 * i + 1] = out[2 * i] = out[i];
      }
      const bool was_silent = g_coop_silent_renders[v] != seen[v];
      if (!was_silent && g_coop_voice_peak > own_peak[v]) own_peak[v] = g_coop_voice_peak;
      for (int i = 0; i < kBlock; ++i) { if (out[i] < lo[v]) lo[v] = out[i]; if (out[i] > hi[v]) hi[v] = out[i]; }
      bool sound = false;
      for (int i = 0; i < kBlock; ++i) { if (out[i]) sound = true; sum[i] += out[i]; }
      if (sound) last_sound[v] = b;
      const bool silent = g_coop_silent_renders[v] != seen[v];
      seen[v] = g_coop_silent_renders[v];
      if (silent && first_silent[v] < 0) first_silent[v] = b;
      if (!silent) { ++awake; if (first_silent[v] >= 0 && !restrike_s) first_silent[v] = -2; }   // it woke unasked
      if (f) std::fwrite(out, sizeof(out), 1, f);
    }
    int block_peak = 0;
    for (int i = 0; i < kBlock; ++i) { const int a = sum[i] < 0 ? -sum[i] : sum[i]; if (a > block_peak) block_peak = a; }
    if (block_peak > peak) peak = block_peak;
    if (block_peak >= 1) last_ge[0] = b;
    if (block_peak >= 2) last_ge[1] = b;
    if (block_peak >= 4) last_ge[2] = b;
    cyc += block_cycles;
    if (block_cycles > cyc_max) cyc_max = block_cycles;
    if (b % 3445 == 3444 && quiet) { peak = 0; cyc = 0; cyc_max = 0; }
    if (b % 3445 == 0) { for (int v = 0; v < 6; ++v) { own_peak[v] = 0; lo[v] = hi[v] = 0; } }
    if (b % 3445 == 3444 && !quiet) {                        // every 5 s
      std::printf("  %3ld s: summed peak %5d LSB, %d voices awake at the end", (b + 1) / 689, peak, awake);
#ifdef __bfin__
      std::printf(", cycles a block avg %lu max %lu", static_cast<unsigned long>(cyc / 3445),
                  static_cast<unsigned long>(cyc_max));
#endif
      std::printf("\n");
      if (trace) {
        for (int v = 0; v < voices; ++v) {
          std::printf("        voice %d: own peak %ld (Q27), output %d..%d LSB\n", v, static_cast<long>(own_peak[v]), lo[v], hi[v]);
        }
      }
      peak = 0;
      cyc = 0;
      cyc_max = 0;
    }
  }
  if (f) std::fclose(f);
  // -80 dBFS is 3.3 LSB of the 16-bit output, -90 dBFS 1.04 LSB.
  std::printf("  summed output: last above -80 dBFS at %.1f s, last above -90 dBFS at %.1f s, last non-zero at %.1f s\n",
              (last_ge[2] + 1) / 689.0, (last_ge[1] + 1) / 689.0, (last_ge[0] + 1) / 689.0);
  for (int v = 0; v < voices; ++v) {
    std::printf("  voice %d: last non-zero sample %.2f s after its strike, first silent render %s", v,
                last_sound[v] < 0 ? -1.0 : (last_sound[v] + 1 - 3 * v) / 689.0,
                first_silent[v] == -1 ? "never" : (first_silent[v] == -2 ? "(woke again)" : ""));
    if (first_silent[v] >= 0) std::printf("%.2f s", (first_silent[v] - 3 * v) / 689.0);
    std::printf("\n");
  }
  return 0;
}

int Cmp(const char* pa, const char* pb, int voices) {
  FILE* a = std::fopen(pa, "rb");
  FILE* b = std::fopen(pb, "rb");
  if (!a || !b) return 2;
  static int16_t xa[kBlock], xb[kBlock];
  long n = 0, differ = 0, first = -1, last = -1;
  int worst = 0;
  while (std::fread(xa, sizeof(xa), 1, a) == 1 && std::fread(xb, sizeof(xb), 1, b) == 1) {
    for (int i = 0; i < kBlock; ++i) {
      const int d = xa[i] > xb[i] ? xa[i] - xb[i] : xb[i] - xa[i];
      if (d) { ++differ; if (first < 0) first = n; last = n; }
      if (d > worst) worst = d;
    }
    ++n;
  }
  const double per_s = 689.0 * voices;
  std::printf("  %ld renders compared: %ld samples differ, largest %d LSB", n, differ, worst);
  if (differ) std::printf(" (the first about %.2f s in, the last about %.2f s)", first / per_s, last / per_s);
  std::printf("\n");
  return differ ? 1 : 0;
}

// After a strike into a voice that slept, the samples cannot be the twin's: the strings' noise comes from one generator
// that every awake string draws from (upstream's, src/string_fixed.h), and the FM voice's phases stood still. What must
// agree is the level: the RMS of the voices' sum (what is heard; in the reverb model the reverb is shared out among the
// voices that are awake, so one voice's part of it moves when another sleeps) over 0.1 s windows, where either side is
// above 64 LSB (-54 dBFS).
int Levels(const char* pa, const char* pb, int voices) {
  FILE* a = std::fopen(pa, "rb");
  FILE* b = std::fopen(pb, "rb");
  if (!a || !b) return 2;
  static int16_t xa[kBlock], xb[kBlock];
  static int32_t ma[kBlock], mb[kBlock];
  double sa = 0, sb = 0, worst = 0, worst_at = 0, sum = 0;
  long windows = 0;
  bool more = true;
  for (long blk = 0; more; ++blk) {
    for (int v = 0; v < voices && more; ++v) {
      if (blk < 3 * v) continue;
      more = std::fread(xa, sizeof(xa), 1, a) == 1 && std::fread(xb, sizeof(xb), 1, b) == 1;
      if (!more) break;
      for (int i = 0; i < kBlock; ++i) { ma[i] += xa[i]; mb[i] += xb[i]; }
    }
    for (int i = 0; i < kBlock; ++i) { sa += double(ma[i]) * ma[i]; sb += double(mb[i]) * mb[i]; ma[i] = mb[i] = 0; }
    if (blk % 69 != 68) continue;
    {
      const double ra = std::sqrt(sa / (69.0 * kBlock)), rb = std::sqrt(sb / (69.0 * kBlock));
      sa = sb = 0;
      if (ra < 64 && rb < 64) continue;
      const double db = std::fabs(20.0 * std::log10((ra + 1e-9) / (rb + 1e-9)));
      sum += db;
      ++windows;
      if (db > worst) { worst = db; worst_at = (blk + 1) / 689.0; }
    }
  }
  std::printf("  levels, %ld windows of 0.1 s above -54 dBFS: mean difference %.3f dB, largest %.2f dB (at %.1f s)\n", windows,
              windows ? sum / windows : 0.0, worst, worst_at);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 7 && !std::strcmp(argv[1], "run")) {
    if (argc > 9) g_structure = std::atoi(argv[9]);
    if (argc > 10) g_brightness = std::atoi(argv[10]);
    return Run(std::strcmp(argv[2], "-") ? argv[2] : nullptr, std::atoi(argv[3]), std::atoi(argv[4]), std::atoi(argv[5]),
               std::atol(argv[6]), argc > 7 ? std::atoi(argv[7]) : 6, argc > 8 ? std::atol(argv[8]) : 0);
  }
  if (argc >= 5 && !std::strcmp(argv[1], "cmp")) return Cmp(argv[2], argv[3], std::atoi(argv[4]));
  if (argc >= 5 && !std::strcmp(argv[1], "lvl")) return Levels(argv[2], argv[3], std::atoi(argv[4]));
  std::fprintf(stderr, "usage: %s run OUT.raw|- MODEL POLY DAMPING HOLD [VOICES [RESTRIKE [STRUCTURE [BRIGHTNESS]]]] | cmp|lvl A.raw B.raw VOICES\n", argv[0]);
  return 2;
}
