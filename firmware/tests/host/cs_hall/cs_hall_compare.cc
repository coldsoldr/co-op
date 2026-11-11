// The cs_hall port's host test: plugins/cs_hall (the coop effect) against te-op1-cfw's hall as its kennel build
// played it (ports/fx/src/hall + ports/fx/op1/half_rate.h, the glue of ports/knl/cs_hall/hall_knl.cc's
// RenderStereo transcribed below), block for block, over the same input and control script. One source, built
// three ways (Makefile):
//   -DHALL_REF   te-op1-cfw's code (host, C kernels)
//   (none)       plugins/cs_hall on the host (its C kernels)
//   bfin -msim   plugins/cs_hall in the GNU simulator with its assembly (hall_bfin.S, halfband_bfin.S), the code the
//                OP-1 runs; it also prints the render's cycles (the simulator: no cache or SDRAM stalls)
// Each writes the outputs as raw int16 (L, R interleaved); the Makefile compares them with cmp.
//
// What the kennel glue did and coop does differently, and how the script lines them up:
//   - kennel cleared the 84 KB tank in Init and on a restart (a > 5 ms pause between calls), and passed one block
//     dry; coop clears it over kClearBlocks blocks in render, dry, on every (re)start (e8: the tank is in the shared
//     position buffer). The reference plays those extra blocks dry without calling the hall.
//   - kennel's knobs came through op1_fx_param_q15 from the stock range table; coop maps the raw words with the
//     SDK's constant ranges. The script sends raw words in the ranges and the reference applies kennel's formula.
//   - the freeze latch: kennel's op1_fx_page[pos][3]; coop's params[8] bit 0.
//
// Usage: cs_hall_compare OUT.raw [--nogap]   (--nogap: no pause scenario; the simulator's CYCLES can't be stepped)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef HALL_REF
#include "fx/op1/half_rate.h"
#include "src/hall/hall.h"
namespace op1 { uint32_t host_cycles; }
namespace op1fx { uint32_t measured_cycles; }
static uint32_t& Clock() { return op1::host_cycles; }
#else
#include "coop_plugin.h"
extern "C" const coop_fx_t FX_cs_hall;
extern "C" long coop_fx_scratch_check(const coop_fx_t* fx, const int16_t* params, long blocks);   /* ../fx_scratch_host.cc */
#if defined(__bfin__)
static uint32_t sim_clock;
static uint32_t& Clock() { return sim_clock; }   // unused: the plugin reads CYCLES
static inline uint32_t ReadCycles() { uint32_t c; __asm__ __volatile__("%0 = CYCLES;" : "=d"(c)); return c; }
#else
extern "C" { uint32_t cs_hall_host_cycles; }
static uint32_t& Clock() { return cs_hall_host_cycles; }
#endif
#endif

namespace {

const int kBlock = 64;
const int kClearBlocks = 21;                      // ceil((4416 + 37971) / 2048): cs_hall.cc's clear
const int16_t kLo[4] = { 64, 0, 0, 8192 };        // the filter type's ranges (sdk/coop_plugin.h)
const int16_t kHi[4] = { 14528, 24576, 22528, 32767 };

uint32_t rng_state = 0x12345678u;
uint32_t Rand() {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}
int32_t RandRange(int32_t lo, int32_t hi) { return lo + static_cast<int32_t>(Rand() % static_cast<uint32_t>(hi - lo + 1)); }

// ---- the effect under test, behind one interface ----

#ifdef HALL_REF
// hall_knl.cc's instance and RenderStereo, with knl:: replaced by what it read.
struct Instance {
  op1fx::HallReverb hall;
  op1fx::HalfRate half;
};
Instance* inst;
int skip;                                          // blocks the coop build spends clearing: dry here too
bool tank_used;                                    // before e8 the clear was skipped while unused (unused now)

int32_t KennelQ15(const int16_t* p, int i) {       // op1_fx_param_q15 with the table filled
  int32_t v = (p[i] - kLo[i]) * 32767 / (kHi[i] - kLo[i]);
  return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}
int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }

void FxInit() {
  std::memset(static_cast<void*>(inst), 0, sizeof(*inst));   // knl::Zero
  inst->hall.Init();
  inst->half.Stop();
}

void FxRender(int16_t* l, int16_t* r, const int16_t* p) {
  if (skip > 0) { --skip; return; }
  if (!inst->half.Begin(op1::Cycles())) {
    inst->hall.Init();
    inst->half.End();
    skip = kClearBlocks;                           // e8: coop clears on every (re)start (its buffer is shared)
    return;
  }
  int32_t v[8];
  for (int k = 0; k < 4; ++k) v[k] = KennelQ15(p, k);
  v[4] = Clamp15(p[5]);   // P_PREDELAY
  v[5] = Clamp15(p[6]);   // P_EARLY
  v[7] = Clamp15(p[7]);   // P_WIDTH
  v[6] = 9830;            // P_MOTION
  op1fx::HallParams q;
  q.size = v[0]; q.decay = v[1]; q.damp = v[2]; q.predelay = v[4]; q.early = v[5]; q.motion = v[6];
  q.width = v[7]; q.freeze = (p[8] & 1) != 0;
  tank_used = true;
  inst->half.Decimate(l, r);
  inst->hall.Process(inst->half.in(0), inst->half.in(1), inst->half.wet(0), inst->half.wet(1), op1fx::kHalfN, q);
  inst->half.Mix(l, r, v[3]);
  inst->half.End();
}
uint32_t last_cost;
#else
uint32_t last_cost, max_cost;
uint64_t sum_cost;
uint32_t n_cost;
void FxInit() { FX_cs_hall.init(2); }               // e8: the slot rendered (each slot has its own state)
void FxRender(int16_t* l, int16_t* r, const int16_t* p) {
  int32_t L[kBlock], R[kBlock];
  for (int i = 0; i < kBlock; ++i) { L[i] = l[i] * 65536; R[i] = r[i] * 65536; }
#if defined(__bfin__)
  const uint32_t c0 = ReadCycles();
  FX_cs_hall.render(2, L, R, p, kBlock);
  last_cost = ReadCycles() - c0;
#else
  FX_cs_hall.render(2, L, R, p, kBlock);
#endif
  for (int i = 0; i < kBlock; ++i) { l[i] = static_cast<int16_t>(L[i] >> 16); r[i] = static_cast<int16_t>(R[i] >> 16); }
}
#endif

// ---- the script ----

FILE* out;
int16_t params[16];
uint32_t phase_l, phase_r;

enum Source { SILENCE, NOISE, SAW, FULL_NOISE, MONO_NOISE, IMPULSES };

void MakeInput(Source s, int16_t* l, int16_t* r, int block) {
  for (int i = 0; i < kBlock; ++i) {
    int32_t a = 0, b = 0;
    switch (s) {
      case SILENCE: break;
      case NOISE: a = static_cast<int16_t>(Rand()) >> 1; b = static_cast<int16_t>(Rand()) >> 1; break;
      case SAW:
        phase_l += 0x00a00000; phase_r += 0x00b40000;
        a = static_cast<int16_t>(phase_l >> 16) / 3; b = static_cast<int16_t>(phase_r >> 16) / 3; break;
      case FULL_NOISE: a = static_cast<int16_t>(Rand()); b = static_cast<int16_t>(Rand()); break;
      case MONO_NOISE: a = b = static_cast<int16_t>(Rand()) >> 2; break;
      case IMPULSES: a = b = ((block & 31) == 0 && i == 0) ? 24000 : 0; break;
    }
    l[i] = static_cast<int16_t>(a);
    r[i] = static_cast<int16_t>(b);
  }
}

void RandomKnobs(bool extremes) {
  for (int k = 0; k < 4; ++k) {
    int32_t v = RandRange(kLo[k], kHi[k]);
    if (extremes && (Rand() & 3) == 0) v = (Rand() & 1) ? kLo[k] : kHi[k];
    params[k] = static_cast<int16_t>(v);
  }
  for (int k = 5; k < 8; ++k) params[k] = static_cast<int16_t>(extremes && (Rand() & 3) == 0 ? ((Rand() & 1) ? 0 : 32767)
                                                                                         : RandRange(0, 32767));
}

void Play(Source s, int blocks, bool knobs_move) {
  int16_t l[kBlock], r[kBlock];
  for (int b = 0; b < blocks; ++b) {
    if (knobs_move && (Rand() % 97) == 0) {          // one knob or control a time, now and then
      const int k = RandRange(0, 7);
      if (k == 4) params[8] ^= 1;                     // freeze on / off
      else if (k < 4) params[k] = static_cast<int16_t>(RandRange(kLo[k], kHi[k]));
      else params[k] = static_cast<int16_t>(RandRange(0, 32767));
    }
    MakeInput(s, l, r, b);
    Clock() += 30000;
    FxRender(l, r, params);
#if !defined(HALL_REF) && defined(__bfin__)
    sum_cost += last_cost; ++n_cost; if (last_cost > max_cost) max_cost = last_cost;
#endif
    std::fwrite(l, sizeof l, 1, out);
    std::fwrite(r, sizeof r, 1, out);
  }
}

void Scenario(const char* name) {
#if !defined(HALL_REF) && defined(__bfin__)
  if (n_cost) std::printf("  cycles a block avg %u max %u over %u blocks\n", static_cast<unsigned>(sum_cost / n_cost),
                          static_cast<unsigned>(max_cost), static_cast<unsigned>(n_cost));
  sum_cost = 0; max_cost = 0; n_cost = 0;
#endif
  std::printf("%s\n", name);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: cs_hall_compare OUT.raw [--nogap]\n"); return 2; }
  const bool gap = !(argc > 2 && std::strcmp(argv[2], "--nogap") == 0);
  out = std::fopen(argv[1], "wb");
  if (!out) return 2;
#if !defined(HALL_REF) && defined(__bfin__)
  {   // the simulator starts with the cycle counter off (SYSCFG CCEN); the OP-1 runs it
    uint32_t syscfg;
    __asm__ __volatile__("%0 = SYSCFG;" : "=d"(syscfg));
    syscfg |= 2;
    __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(syscfg));
  }
#endif
#ifdef HALL_REF
  inst = static_cast<Instance*>(std::calloc(1, sizeof(Instance)));
#endif
  // the defaults (plugin.json): raw words for the four turns as a sound would hold them, the three words, freeze off
  params[0] = 9000; params[1] = 12288; params[2] = 6000; params[3] = 16000;
  params[4] = 0x5300; params[5] = 3277; params[6] = 16384; params[7] = 32767; params[8] = 0;

  Scenario("fresh load: noise bursts, the defaults");
  FxInit();
  for (int i = 0; i < 6; ++i) { Play(NOISE, 20, false); Play(SILENCE, 120, false); }
  Scenario("saw, every control moving now and then, size crossfades, freeze toggles");
  Play(SAW, 1500, true);
  Scenario("impulses, extremes (min / max of every knob, infinite decay)");
  for (int i = 0; i < 12; ++i) { RandomKnobs(true); Play(IMPULSES, 120, false); }
  Scenario("full-scale noise, knobs jumping");
  for (int i = 0; i < 20; ++i) { RandomKnobs(false); Play(FULL_NOISE, 25, true); }
  Scenario("freeze held over silence (infinite tail), then released");
  params[8] = 1; Play(SILENCE, 600, false); params[8] = 0; Play(SILENCE, 300, false);
  Scenario("sound change (init, the tank used: cleared over 21 blocks)");
  RandomKnobs(false); params[8] = 1;
  FxInit();
  params[8] = 0;
  Play(MONO_NOISE, 200, true);
  Play(SAW, 400, true);
  if (gap) {
    Scenario("a pause (bypassed > 5 ms): the restart clears the tank");
    Clock() += 3000000;
    Play(NOISE, 300, true);
  }
  Scenario("mono noise (L == R), all knobs moving");
  Play(MONO_NOISE, 800, true);
  Scenario("end");
  std::fclose(out);
#ifndef HALL_REF
  {   /* e8: two positions at once and a restart on a dirty position buffer (tests/host/fx_scratch_host.cc; stderr) */
    int16_t sp[16] = { 9000, 18000, 6000, 26000, 0x5300 + 32, 6000, 12000, 30000 };
    if (coop_fx_scratch_check(&FX_cs_hall, sp, 1500)) return 1;
  }
#endif
  return 0;
}
