// The host / GNU simulator stand-in for the core's position buffers (e8: core/sdram/plugin_host.cc coop_fx_scratch,
// sdk/coop_plugin.h), linked into every scratch effect port's host test (tests/host/<id>).
//
// Five buffers, 32-byte aligned, of the bytes the port declares (plugin.json "scratch", passed by the test's Makefile
// as -DCOOP_FX_SCRATCH_BYTES, rounded up to 32 as the core does), filled with random bytes before the first use: on the
// OP-1 a buffer holds whatever the effect active there before left, so an effect's init must not rely on zeroed
// memory, and a port that does fails its bit-exact comparison here.
//
// coop_fx_scratch_dirty(slot): fill a buffer with fresh garbage (another effect used the position).
// coop_fx_scratch_check(fx, params, blocks): the e8 contract on one effect, independent of its DSP:
//   1. reference: init(0) on a dirty buffer, `blocks` blocks of a fixed stereo signal at slot 0;
//   2. two positions: init(0) and init(3) on dirty buffers, the same signal at slot 0 interleaved with another signal
//      and other knobs at slot 3: slot 0's output must equal the reference word for word (independent states);
//   3. restart: slot 0's buffer dirtied (another effect was selected there), init(0), the signal again: equal again.
// Returns the number of mismatching blocks (0 = pass) and prints one line (stderr: harnesses compare stdout).
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#include <stdint.h>
#include <stdio.h>

#include "coop_plugin.h"

#ifndef COOP_FX_SCRATCH_BYTES
#error "build with -DCOOP_FX_SCRATCH_BYTES=<plugin.json scratch>"
#endif

namespace {

const uint32_t kBytes = (COOP_FX_SCRATCH_BYTES + 31u) & ~31u;
static_assert(kBytes <= COOP_FX_SCRATCH_MAX, "the declaration is over COOP_FX_SCRATCH_MAX");

alignas(32) uint8_t g_buf[COOP_FX_SLOTS][kBytes];
bool g_filled;
uint32_t g_seed = 0x6A09E667u;

uint32_t Next() { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5; return g_seed; }

void Fill(int slot) {
  uint32_t* w = reinterpret_cast<uint32_t*>(g_buf[slot]);
  for (uint32_t i = 0; i < kBytes / 4; ++i) w[i] = Next();
}

void FillAll() {
  for (int s = 0; s < COOP_FX_SLOTS; ++s) Fill(s);
  g_filled = true;
}

int16_t Signal(uint32_t* st, int which, int i) {   // a saw plus noise, different per signal
  *st = *st * 1664525u + 1013904223u;
  const int32_t noise = static_cast<int32_t>(*st >> 20) - 2048;
  const int32_t saw = ((i * (which ? 977 : 613)) & 0xffff) - 32768;
  return static_cast<int16_t>((saw >> 2) + noise);
}

void Block(int which, long b, int32_t* l, int32_t* r) {
  uint32_t st = static_cast<uint32_t>(b) * 2654435761u + (which ? 12345u : 0u);
  for (int i = 0; i < 64; ++i) {
    l[i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(Signal(&st, which, static_cast<int>(b * 64 + i)))) << 16);
    r[i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(Signal(&st, which, static_cast<int>(b * 64 + i + 17)))) << 16);
  }
}

uint32_t Hash(const int32_t* l, const int32_t* r) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < 64; ++i) { h = (h ^ static_cast<uint32_t>(l[i])) * 16777619u; h = (h ^ static_cast<uint32_t>(r[i])) * 16777619u; }
  return h;
}

}  // namespace

extern "C" void* coop_fx_scratch(int slot, uint32_t* bytes) {
  if (slot < 0 || slot >= COOP_FX_SLOTS) { if (bytes) *bytes = 0; return 0; }
  if (!g_filled) FillAll();
  if (bytes) *bytes = kBytes;
  return g_buf[slot];
}

extern "C" void coop_fx_scratch_dirty(int slot) {
  if (!g_filled) FillAll();
  if (slot >= 0 && slot < COOP_FX_SLOTS) Fill(slot);
}

extern "C" long coop_fx_scratch_check(const coop_fx_t* fx, const int16_t* params, long blocks) {
  static uint32_t ref[4096];
  if (blocks > 4096) blocks = 4096;
  int16_t other[COOP_FX_PARAMS_MIN];
  for (int k = 0; k < COOP_FX_PARAMS_MIN; ++k) other[k] = params[k];
  other[0] = 2000; other[1] = 20000; other[2] = 3000; other[3] = 30000;   // other knobs at slot 3
  int32_t l[64], r[64];
  coop_fx_scratch_dirty(0);
  if (fx->init(0) != 0) { fprintf(stderr, "scratch check: init(0) refused\n"); return blocks; }
  for (long b = 0; b < blocks; ++b) { Block(0, b, l, r); fx->render(0, l, r, params, 64); ref[b] = Hash(l, r); }
  long bad_two = 0, bad_restart = 0;
  coop_fx_scratch_dirty(0);
  coop_fx_scratch_dirty(3);
  if (fx->init(0) != 0 || fx->init(3) != 0) { fprintf(stderr, "scratch check: init refused\n"); return blocks; }
  for (long b = 0; b < blocks; ++b) {
    Block(1, b, l, r); fx->render(3, l, r, other, 64);
    Block(0, b, l, r); fx->render(0, l, r, params, 64);
    if (Hash(l, r) != ref[b]) bad_two++;
    Block(1, b + 7, l, r); fx->render(3, l, r, other, 64);
  }
  coop_fx_scratch_dirty(0);
  if (fx->init(0) != 0) { fprintf(stderr, "scratch check: init(0) refused on restart\n"); return blocks; }
  for (long b = 0; b < blocks; ++b) { Block(0, b, l, r); fx->render(0, l, r, params, 64); if (Hash(l, r) != ref[b]) bad_restart++; }
  fprintf(stderr, "scratch check (%u-byte position buffers, dirty): %ld blocks; slot 0 beside slot 3: %ld differ; restart on a"
         " dirty buffer: %ld differ\n", static_cast<unsigned>(kBytes), blocks, bad_two, bad_restart);
  return bad_two + bad_restart;
}
