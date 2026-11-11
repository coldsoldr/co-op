// The drive port's host / simulator test: plugins/drive's coop_fx_t (its render on the int32 bus, the knobs in the
// filter type's range units, word 5 the gain) against te-op1-cfw's DRIVE behind kennel's stereo glue (drive_ref.cc),
// block for block, bit for bit:
//   - 20,000 blocks of 64: sines, noise, full-scale random and silence, L and R different (L == R one block in
//     eight); the knobs jump at random every 37 blocks (the drive every 5), sweep like an LFO in some stretches, and
//     sometimes stand out of their ranges; the gain word changes every 151 blocks (sometimes out of 0..32767); both
//     sides re-initialised together every 2,750 blocks (a sound change). Output equal, and the low 16 bits 0.
//   - Identity: mix at its range's bottom (Q15 0) for 400 blocks, the other knobs jumping: after 40 blocks to settle,
//     the output is the input bit for bit.
//   - On the Blackfin (the device codegen: the plugin's assembly passes, -DOP1FX_DRIVE_ASM_ONLY), the plugin's
//     render's cycles per 64-sample block, knobs still and moving (bfin-elf-run: no cache, no SDRAM).
// `make -C tests/host/drive` (host) and `make -C tests/host/drive sim`.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <cstdint>
#include <cstdio>

#include "coop_plugin.h"

extern "C" const coop_fx_t FX_drive;
extern "C" void drive_ref_init(void);
extern "C" long coop_fx_scratch_check(const coop_fx_t* fx, const int16_t* params, long blocks);   /* ../fx_scratch_host.cc */
extern "C" void drive_ref_render(int16_t* l, int16_t* r, const int16_t* p, int n);

namespace {

const int kN = 64;
uint32_t rng = 0x2545F491u;
uint32_t Next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

const int16_t kLo[4] = { 64, 0, 0, 8192 }, kHi[4] = { 14528, 24576, 22528, 32767 };

int16_t KnobWord(int k) {
  const uint32_t r = Next();
  if ((r & 31) == 0) return static_cast<int16_t>(r >> 8);              // anything at all (a sound may hold it)
  return static_cast<int16_t>(kLo[k] + static_cast<int32_t>((r >> 9) % static_cast<uint32_t>(kHi[k] - kLo[k] + 1)));
}

// Sine by a phase accumulator and a parabola (no float on either side), noise, full scale, silence.
struct Source {
  int kind;
  uint32_t phase, inc;
  int32_t amp;
  void Pick() {
    kind = static_cast<int>(Next() % 5);
    inc = 0x00200000u + (Next() % 0x08000000u);
    amp = static_cast<int32_t>(Next() % 32768);
  }
  int16_t Sample() {
    switch (kind) {
      case 0: case 1: {
        phase += inc;
        const int32_t x = static_cast<int32_t>(phase >> 16) - 32768;   // -32768..32767, a saw
        const int32_t t = (x < 0 ? -x : x);                            // a triangle 0..32768
        const int32_t s = ((t - 16384) * 2 * amp) >> 15;
        return static_cast<int16_t>(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
      }
      case 2: return static_cast<int16_t>(((static_cast<int32_t>(static_cast<int16_t>(Next())) * amp) >> 15));
      case 3: return static_cast<int16_t>(Next());                    // full scale
      default: return 0;
    }
  }
};

#ifdef __bfin__
inline uint32_t Cycles() {
  uint32_t c;
  __asm__ __volatile__("%0 = CYCLES;" : "=d"(c));
  return c;
}
void EnableCycles() {
  uint32_t s;
  __asm__ __volatile__("%0 = SYSCFG;" : "=d"(s));
  s |= 2;
  __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(s));
}
#endif

int16_t params[COOP_FX_PARAMS_MIN];
int16_t rl[kN], rr[kN], il[kN], ir[kN];
int32_t cl[kN], cr[kN];
long g_changed, g_lr;

// One block through both; returns the samples that differ.
long Block(Source& a, Source& b, bool mono) {
  for (int i = 0; i < kN; ++i) {
    il[i] = a.Sample();
    ir[i] = mono ? il[i] : b.Sample();
    rl[i] = il[i];
    rr[i] = ir[i];
    cl[i] = static_cast<int32_t>(il[i]) * 65536;
    cr[i] = static_cast<int32_t>(ir[i]) * 65536;
  }
  FX_drive.render(0, cl, cr, params, kN);
  drive_ref_render(rl, rr, params, kN);
  long bad = 0;
  for (int i = 0; i < kN; ++i) {
    if ((cl[i] & 0xffff) || (cr[i] & 0xffff) || (cl[i] >> 16) != rl[i] || (cr[i] >> 16) != rr[i]) ++bad;
    g_changed += rl[i] != il[i];                   // the test is not vacuous: the effect did something
    g_lr += rl[i] != rr[i];
  }
  return bad;
}

}  // namespace

int main() {
  if (FX_drive.magic != COOP_FX_MAGIC || FX_drive.api != COOP_PLUGIN_API) { printf("bad object\n"); return 1; }
  params[4] = 0x5300 + 32;
  for (int k = 0; k < 4; ++k) params[k] = KnobWord(k);
  params[5] = 16384;
  FX_drive.init(0);
  drive_ref_init();
  Source a = {}, b = {};
  a.Pick();
  b.Pick();
  long bad = 0, bad_blocks = 0;
  int sweep = 0;
  const int kBlocks = 20000;
  for (int blk = 0; blk < kBlocks; ++blk) {
    if (blk % 2750 == 2749) { FX_drive.init(0); drive_ref_init(); }
    if (blk % 37 == 0) {
      for (int k = 0; k < 4; ++k) params[k] = KnobWord(k);
      sweep = static_cast<int>(Next() % 4) == 0 ? 1 + static_cast<int>(Next() % 3) : 0;
    }
    if (blk % 5 == 0) params[0] = KnobWord(0);
    if (sweep) {                                    // an LFO-like sweep on one knob
      const int k = sweep;
      int32_t v = params[k] + 97;
      if (v > kHi[k]) v = kLo[k];
      params[k] = static_cast<int16_t>(v);
    }
    if (blk % 151 == 0) {
      const uint32_t r = Next();
      params[5] = (r & 15) == 0 ? static_cast<int16_t>(r >> 8) : static_cast<int16_t>((r >> 9) % 32768);
    }
    if (blk % 200 == 0) { a.Pick(); b.Pick(); }
    const long d = Block(a, b, (Next() & 7) == 0);
    bad += d;
    if (d) ++bad_blocks;
  }
  printf("drive: %d blocks, coop plugin vs te-op1-cfw + kennel glue: %ld samples differ in %ld blocks%s\n", kBlocks, bad,
         bad_blocks, bad ? "" : " (bit-identical)");
  printf("drive: %ld of %d output samples differ from the input, %ld L from R\n", g_changed, kBlocks * kN, g_lr);

  // Identity at mix 0.
  FX_drive.init(0);
  drive_ref_init();
  params[3] = 8192;
  long ident = 0;
  for (int blk = 0; blk < 400; ++blk) {
    if (blk % 37 == 0) for (int k = 0; k < 3; ++k) params[k] = KnobWord(k);
    if (blk % 100 == 0) { a.Pick(); b.Pick(); }
    bad += Block(a, b, false);
    if (blk >= 40)
      for (int i = 0; i < kN; ++i) ident += (cl[i] >> 16) != il[i] || (cr[i] >> 16) != ir[i];
  }
  printf("drive: mix 0, 360 settled blocks: %ld samples differ from the input%s\n", ident, ident ? "" : " (identity)");

#ifdef __bfin__
  EnableCycles();
  for (int pass = 0; pass < 2; ++pass) {
    FX_drive.init(0);
    params[0] = 9000; params[1] = 12000; params[2] = 6000; params[3] = 30000; params[5] = 16384;
    a.kind = b.kind = 0;
    uint32_t total = 0, mx = 0;
    const int kRuns = 2000;
    for (int blk = 0; blk < kRuns; ++blk) {
      if (pass) {                                   // moving: drive as an LFO, tone and bias stepping
        params[0] = static_cast<int16_t>(64 + (blk * 131) % 14464);
        if (blk % 3 == 0) params[1] = static_cast<int16_t>((blk * 53) % 24576);
        if (blk % 7 == 0) params[2] = static_cast<int16_t>((blk * 71) % 22528);
      }
      for (int i = 0; i < kN; ++i) { cl[i] = a.Sample() * 65536; cr[i] = b.Sample() * 65536; }
      const uint32_t t0 = Cycles();
      FX_drive.render(0, cl, cr, params, kN);
      const uint32_t t = Cycles() - t0;
      if (blk >= 100) { total += t; if (t > mx) mx = t; }
    }
    printf("cycles %s: avg %lu max %lu per 64-sample block (bfin-elf-run)\n", pass ? "knobs moving" : "knobs still",
           static_cast<unsigned long>(total / (kRuns - 100)), static_cast<unsigned long>(mx));
  }
#endif
  {   /* e8: two positions at once and a restart on a dirty position buffer (tests/host/fx_scratch_host.cc) */
    int16_t p[COOP_FX_PARAMS_MIN] = { 9000, 15000, 6000, 28000, 0x5300 + 32, 20000 };
    if (coop_fx_scratch_check(&FX_drive, p, 1500)) bad++;
  }
  return bad || ident ? 1 : 0;
}
