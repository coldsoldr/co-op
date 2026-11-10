// The rings port's host test (guide/plugin-guidelines.md §7.4 step 7; docs/plugin-performance.md "How a rewrite is
// accepted"): plugins/rings (the coop engine, its constants made integer literals by defloat.py) against
// te-op1-cfw's ports/rings/op1/engine.cc (the original, with its double constants), both at the kennel build's
// configuration (22.05 kHz, 32-sample control blocks, upsampled 4x), on the host, where the fixed-point primitives
// are bit-exact emulations of the Blackfin builtins (te-op1-cfw tests/test_fixed.cc). A straight port must be
// bit-identical.
//
// This one source builds twice (Makefile): -DRINGS_REF calls te-op1-cfw's RingsNoteOn / RingsRender as the OP-1 did
// (voice objects as numbers, the pitch words in op1::sim_pitch_buffer, model / polyphony / engine in knobs[4..6] x
// 4096), without it the coop engine's note_on / render (model and polyphony as step indexes in params[5] / [6]). The
// call sequence is the same seeded scenario in both: notes on any voice in every model and polyphony (so steals,
// fades and the shared reverb), knob jumps and walks, per-voice pitch with a bend inside a block, gaps that let the
// shared reverb go idle. Each build writes every rendered block to a file; the Makefile compares the two.
//
//   rings_ref OUT.raw    /    rings_coop OUT.raw
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "op1/op1.h"

#ifdef RINGS_REF
extern "C" void RingsRender(void* self, int16_t* out, const int16_t* params);
extern "C" void RingsNoteOn(void* self, const int16_t* knobs);
namespace op1 {
uint32_t sim_pitch_buffer[kBlockSize];
uint32_t host_cycles;
}
#else
#include "coop_plugin.h"
extern "C" const coop_engine_t ENGINE_rings;
extern "C" const coop_screen_t SCREEN_rings = { nullptr, nullptr };   // rings_screen.cc is not in the host build
namespace op1 { uint32_t host_cycles; }
// The core's L1 exports, as host buffers.
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
#endif

namespace {

const int kVoices = 6;
const int kBlock = 128;
const int kBlocks = 689 * 90;            // ~90 s at 88.2 kHz, 128 samples a block
const uint32_t kCyclesPerBlock = 580499; // the OP-1's block budget: the shared reverb's idle gap counts these

uint32_t rng = 0x2545F491u;
uint32_t Next() {                        // xorshift32
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}
int Below(int n) { return static_cast<int>(Next() % static_cast<uint32_t>(n)); }

// f / 88200 * 2^32 for a MIDI note, from 440 Hz by semitone ratios in Q30 (integer only, so both builds agree).
uint32_t Increment(int note) {
  const uint64_t a4 = 21426229ull;      // 440 / 88200 * 2^32
  static const uint32_t kSemi[12] = {   // 2^(i/12), Q30
    1073741824u, 1137589835u, 1205234447u, 1276901417u, 1352829926u, 1433273777u,
    1518502650u, 1608803398u, 1704480463u, 1805857285u, 1913275797u, 2027097902u };
  int d = note - 69;
  int oct = d >= 0 ? d / 12 : -((11 - d) / 12);
  int semi = d - oct * 12;
  uint64_t inc = (a4 * kSemi[semi]) >> 30;
  return static_cast<uint32_t>(oct >= 0 ? inc << oct : inc >> -oct);
}

void NoteOn(int v, const int16_t* knobs4, int model, int poly) {
#ifdef RINGS_REF
  int16_t k[8] = { knobs4[0], knobs4[1], knobs4[2], knobs4[3], static_cast<int16_t>(model * 4096),
                   static_cast<int16_t>(poly * 4096), 4096 /* ENGINE_RINGS */, 0 };
  RingsNoteOn(reinterpret_cast<void*>(static_cast<uintptr_t>(0xffb00000u + v * 28)), k);
#else
  int16_t k[8] = { knobs4[0], knobs4[1], knobs4[2], knobs4[3], 0x5320, static_cast<int16_t>(model),
                   static_cast<int16_t>(poly), 0 };
  ENGINE_rings.note_on(v, nullptr, 127 << 8, k);
#endif
}

void Render(int v, int16_t* out, const uint32_t* pitch, const int16_t* knobs4, int model, int poly) {
#ifdef RINGS_REF
  (void)model;
  (void)poly;
  for (int i = 0; i < kBlock; ++i) op1::sim_pitch_buffer[i] = pitch[i];
  RingsRender(reinterpret_cast<void*>(static_cast<uintptr_t>(0xffb00000u + v * 28)), out, knobs4);
#else
  int16_t k[8] = { knobs4[0], knobs4[1], knobs4[2], knobs4[3], 0x5320, static_cast<int16_t>(model),
                   static_cast<int16_t>(poly), 0 };
  ENGINE_rings.render(v, nullptr, out, reinterpret_cast<const int32_t*>(pitch), k);
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s OUT.raw\n", argv[0]);
    return 2;
  }
  FILE* f = std::fopen(argv[1], "wb");
  if (!f) return 2;
#ifndef RINGS_REF
  if (ENGINE_rings.init && ENGINE_rings.init() != 0) return 3;
#endif
  int16_t knobs[4] = { 8192, 19660, 22937, 9830 };   // the kennel defaults
  int model = 0, poly = 0;
  int note[kVoices] = { 0 }, left[kVoices] = { 0 };    // left: blocks the stock envelope still renders the voice
  int bend_blocks = 0;
  long sounding_blocks = 0, nonzero = 0, notes = 0;
  int16_t out[kBlock];
  uint32_t pitch[kBlock];
  for (int b = 0; b < kBlocks; ++b) {
    op1::host_cycles += kCyclesPerBlock;
    // A new section every ~3 s: the next model / polyphony pair, so every pair is played (6 x 4 = 24 sections in
    // 90 s at ~3.75 s), then random ones.
    if (b % 2584 == 0) {
      const int s = b / 2584;
      model = s < 24 ? s % 6 : Below(6);
      poly = s < 24 ? (s / 6) % 4 : Below(4);
    }
    // Knobs: now and then a jump, otherwise a slow walk on one of them.
    if (Below(400) == 0) {
      knobs[Below(4)] = static_cast<int16_t>(Below(32768));
    } else if (Below(8) == 0) {
      const int k = Below(4);
      int v = knobs[k] + Below(1025) - 512;
      knobs[k] = static_cast<int16_t>(v < 0 ? 0 : (v > 32767 ? 32767 : v));
    }
    // Notes: on a random voice, more often in bursts (steals and fades with polyphony 1, 2, 4); a quiet gap of a few
    // seconds now and then (the shared reverb restarts empty after its idle gap).
    const bool gap = (b / 689) % 15 == 14;
    if (!gap && Below((b / 345) % 3 == 0 ? 6 : 40) == 0) {
      const int v = Below(kVoices);
      note[v] = 36 + Below(48);
      left[v] = 40 + Below(600);
      NoteOn(v, knobs, model, poly);
      ++notes;
    }
    if (bend_blocks > 0) --bend_blocks;
    else if (Below(300) == 0) bend_blocks = 20 + Below(100);
    for (int v = 0; v < kVoices; ++v) {
      if (left[v] <= 0) continue;
      --left[v];
      const uint32_t base = Increment(note[v]);
      for (int i = 0; i < kBlock; ++i) {   // a bend moves the words inside the block (the engine latches pitch[0])
        pitch[i] = bend_blocks ? base + ((base >> 6) * static_cast<uint32_t>((b * kBlock + i) % 4096)) / 4096 : base;
      }
      Render(v, out, pitch, knobs, model, poly);
      std::fwrite(out, sizeof(out), 1, f);
      ++sounding_blocks;
      for (int i = 0; i < kBlock; ++i) nonzero += out[i] != 0;
    }
  }
  std::fclose(f);
  std::printf("%ld notes, %ld voice blocks rendered, %ld non-zero samples\n", notes, sounding_blocks, nonzero);
  return 0;
}
