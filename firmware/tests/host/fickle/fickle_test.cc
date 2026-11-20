// fickle's host test (plugins/fickle): the plugin as it ships, on the host and in the Blackfin GNU simulator.
//
//   fickle_test stress BLOCKS SEED   thousands of random blocks (noise, saws, impulses, silence, L != R; every wet mode,
//                                    routing and loop mode; knobs walking and jumping, clock sweeps; the latches in every
//                                    order; re-inits on dirty buffers) with the checks below; exit 1 on a failure
//   fickle_test wav OUTDIR           a few seconds of audio per wet mode and per loop mode (a capture, a playback, a dub,
//                                    a clock sweep) as 16-bit stereo WAV files, for the owner's ears
//   fickle_test sim BLOCKS           cycles a block (compute only) per wet mode and per loop mode, at the top clock
//                                    (the most clock samples a block) and at the default clock: in bfin-elf-run
//
// Checks (stress):
//   - silence after an init on a dirty position buffer (with every latch toggled) gives exactly silence: nothing the
//     effect reads was left there by another effect (the loop buffer is never cleared, only what was recorded is read);
//   - the display words stay in their ranges;
//   - silence in with freeze and loop off decays to silence (peak under 2 LSB) and then stays there with no DC;
//   - a captured loop keeps playing over silence, a freeze keeps the wet channel sounding, both end when released;
//   - e8: coop_fx_scratch_check (../fx_scratch_host.cc): the effect at slots 0 and 3 interleaved, and a restart on a
//     dirty buffer, give slot 0 the same output.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_plugin.h"
#include "state.h"

extern "C" const coop_fx_t FX_fickle;
extern "C" void coop_fx_scratch_dirty(int slot);
extern "C" long coop_fx_scratch_check(const coop_fx_t* fx, const int16_t* params, long blocks);

namespace {

const int kBlock = 64;
const int16_t kLo[4] = { 64, 0, 0, 8192 };
const int16_t kHi[4] = { 14528, 24576, 22528, 32767 };

uint32_t g_rng = 0x2545F491u;
uint32_t Rand() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
int32_t RandIn(int32_t lo, int32_t hi) { return lo + static_cast<int32_t>(Rand() % static_cast<uint32_t>(hi - lo + 1)); }

// The controls a test drives, as the core would pass them.
struct Ctl {
  int32_t knob[4];     // Q15
  int32_t wet, route, lmode;
  bool freeze, loop, dub;
};

void Params(const Ctl& c, int16_t* p) {
  for (int k = 0; k < 16; ++k) p[k] = 0;
  for (int k = 0; k < 4; ++k) p[k] = static_cast<int16_t>(kLo[k] + (c.knob[k] * (kHi[k] - kLo[k])) / 32767);
  p[4] = 0x5300;
  p[5] = static_cast<int16_t>(c.wet);
  p[6] = static_cast<int16_t>(c.route);
  p[7] = static_cast<int16_t>(c.lmode);
  p[8] = static_cast<int16_t>((c.freeze ? 1 : 0) | (c.loop ? 2 : 0) | (c.dub ? 4 : 0));
}

Ctl Defaults() {
  Ctl c;
  c.knob[0] = 14000; c.knob[1] = 12000; c.knob[2] = 29372; c.knob[3] = 16384;   // plugin.json
  c.wet = 0; c.route = 1; c.lmode = 0;
  c.freeze = c.loop = c.dub = false;
  return c;
}

// ---- sources ----
struct Source {
  int type;            // 0 silence, 1 noise, 2 saws (L != R), 3 impulses, 4 notes, 5 noise bursts, 6 mono saw (L == R)
  uint32_t pl, pr, il, ir, seed;
  int32_t amp, t, env, note_len;
};

void SourceNew(Source* s, int type) {
  s->type = type;
  s->pl = s->pr = 0;
  s->il = 20000000u + Rand() % 200000000u;
  s->ir = s->il + Rand() % 30000000u;
  s->seed = Rand() | 1;
  s->amp = RandIn(2000, 30000);
  s->t = 0;
  s->env = 0;
  s->note_len = RandIn(3000, 20000);
}

const uint32_t kPenta[10] = { 0, 2, 4, 7, 9, 12, 14, 16, 19, 21 };

int32_t Tri(uint32_t ph) {   // a triangle, +-32767
  const int32_t x = static_cast<int32_t>(ph >> 16) - 32768;   // -32768..32767 saw
  return (x < 0 ? -x : x) * 2 - 32768;
}

void SourceBlock(Source* s, int32_t* l, int32_t* r) {
  for (int i = 0; i < kBlock; ++i, ++s->t) {
    int32_t a = 0, b = 0;
    switch (s->type) {
    case 1:
      s->seed = s->seed * 1664525u + 1013904223u; a = static_cast<int32_t>(s->seed >> 16) - 32768;
      s->seed = s->seed * 1664525u + 1013904223u; b = static_cast<int32_t>(s->seed >> 16) - 32768;
      a = (a * s->amp) >> 15; b = (b * s->amp) >> 15;
      break;
    case 2: case 6:
      s->pl += s->il; s->pr += (s->type == 6 ? s->il : s->ir);
      a = ((static_cast<int32_t>(s->pl >> 16) - 32768) * s->amp) >> 15;
      b = s->type == 6 ? a : ((static_cast<int32_t>(s->pr >> 16) - 32768) * s->amp) >> 15;
      break;
    case 3:
      if (s->t % 4410 == 0) { a = s->amp; b = -s->amp / 2; }
      break;
    case 4:
    case 5:
      if (s->t % s->note_len == 0) {
        s->env = 32767;
        // 2^(n/12) x 110 Hz as a phase increment, by a small table walk (no float)
        const uint32_t n = kPenta[Rand() % 10];
        uint32_t inc = 10712000u;   // ~110 Hz a 44.1 kHz sample (2^32 x 110 / 44100)
        for (uint32_t k = 0; k < n; ++k) inc = static_cast<uint32_t>((static_cast<uint64_t>(inc) * 69433u) >> 16);   // x 2^(1/12)
        s->il = inc * 2; s->ir = inc * 2 + inc / 300;
      }
      s->env -= s->env >> 12;
      if (s->type == 4) {
        s->pl += s->il; s->pr += s->ir;
        a = (((Tri(s->pl) * s->env) >> 15) * s->amp) >> 15;
        b = (((Tri(s->pr) * s->env) >> 15) * s->amp) >> 15;
      } else {
        s->seed = s->seed * 1664525u + 1013904223u; a = static_cast<int32_t>(s->seed >> 16) - 32768;
        a = (((a * s->env) >> 15) * s->amp) >> 15; b = -a;
      }
      break;
    default:
      break;
    }
    l[i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(a))) << 16);
    r[i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(b))) << 16);
  }
}

int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; if (g_fail < 20) { printf("FAIL: " __VA_ARGS__); printf("\n"); } } } while (0)

const fickle::Display* Disp(int slot) {
  uint32_t bytes = 0;
  const fickle::State* s = static_cast<const fickle::State*>(coop_fx_scratch(slot, &bytes));
  return &s->disp;
}

void CheckDisplay(const fickle::Display* d, long b) {
  CHECK(d->clock_hz >= 1990 && d->clock_hz <= 32100, "block %ld: clock_hz %d", b, (int)d->clock_hz);
  CHECK(d->loop_pos >= 0 && d->loop_pos <= 32767, "block %ld: loop_pos %d", b, (int)d->loop_pos);
  CHECK(d->loop_len >= 0 && d->loop_len <= fickle::kLoopPairs, "block %ld: loop_len %d", b, (int)d->loop_len);
  CHECK(d->loop_state >= 0 && d->loop_state <= 3, "block %ld: loop_state %d", b, (int)d->loop_state);
  CHECK(d->wet_level >= 0 && d->wet_level <= 32767 && d->in_level >= 0 && d->in_level <= 32767 &&
        d->loop_level >= 0 && d->loop_level <= 32767 && d->head >= 0 && d->head <= 32767, "block %ld: levels", b);
  CHECK(d->wet_mode >= 0 && d->wet_mode <= 2, "block %ld: wet_mode", b);
}

int32_t Peak(const int32_t* l, const int32_t* r) {
  int32_t p = 0;
  for (int i = 0; i < kBlock; ++i) {
    const int32_t a = (l[i] >> 16) < 0 ? -(l[i] >> 16) : (l[i] >> 16), c = (r[i] >> 16) < 0 ? -(r[i] >> 16) : (r[i] >> 16);
    if (a > p) p = a;
    if (c > p) p = c;
  }
  return p;
}

// Every rendered block's output and display words folded into one number (FNV-1a), printed at the end: two builds of
// the plugin that print the same digest gave the same samples in every check of the run (the bit-exactness proof of a
// change that must not alter the sound: build the old sources with COOP=<their folder> and compare the lines).
uint64_t g_digest = 1469598103934665603ull;
void Digest(const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) g_digest = (g_digest ^ b[i]) * 1099511628211ull;
}
void DigestBlock(int slot, const int32_t* l, const int32_t* r) {
  Digest(l, kBlock * sizeof(int32_t));
  Digest(r, kBlock * sizeof(int32_t));
  Digest(Disp(slot), sizeof(fickle::Display));
}

void Render(int slot, const Ctl& c, int32_t* l, int32_t* r) {
  int16_t p[16];
  Params(c, p);
  FX_fickle.render(slot, l, r, p, kBlock);
  DigestBlock(slot, l, r);
}

void Silence(int32_t* l, int32_t* r) { for (int i = 0; i < kBlock; ++i) l[i] = r[i] = 0; }

// Silence after an init on a dirty buffer: exactly silence, whatever the latches do.
void DirtySilence() {
  for (int rep = 0; rep < 8; ++rep) {
    coop_fx_scratch_dirty(0);
    FX_fickle.init(0);
    Ctl c = Defaults();
    c.wet = rep % 3; c.lmode = RandIn(0, fickle::L_MODES - 1); c.route = RandIn(0, 2);
    c.knob[3] = 32767;
    long nonzero = 0;
    for (long b = 0; b < 3000; ++b) {
      if (b % 97 == 0) c.loop = !c.loop;
      if (b % 131 == 0) c.freeze = !c.freeze;
      if (b % 59 == 0) c.dub = !c.dub;
      if (b % 211 == 0) c.lmode = RandIn(0, fickle::L_MODES - 1);
      if (b % 300 == 0) c.knob[2] = RandIn(0, 32767);
      int32_t l[kBlock], r[kBlock];
      Silence(l, r);
      Render(0, c, l, r);
      for (int i = 0; i < kBlock; ++i) if (l[i] || r[i]) ++nonzero;
    }
    CHECK(nonzero == 0, "dirty-buffer silence: %ld nonzero samples (rep %d)", nonzero, rep);
  }
  printf("dirty buffers, silence in: silence out (8 runs x 3000 blocks, latches toggling)\n");
}

// Silence in, freeze and loop off: decays to silence, then no DC. Returns the blocks it took.
long Decay(Ctl c, long limit) {
  c.freeze = c.loop = c.dub = false;
  if (c.knob[1] > 28000) c.knob[1] = 28000;     // delay feedback under the hold region
  if (c.knob[0] > 26000) c.knob[0] = 26000;     // the reverb's longest RT60s (10 s at 24 kHz, 2 min at 2 kHz) are drones
  long quiet = -1;
  int32_t l[kBlock], r[kBlock];
  for (long b = 0; b < limit; ++b) {
    Silence(l, r);
    Render(0, c, l, r);
    if (Peak(l, r) < 2) { if (quiet < 0) quiet = b; }
    else quiet = -1;
    if (quiet >= 0 && b - quiet > 6000) break;   // longer than the longest delay (16320 clock samples at 2 kHz)
  }
  if (quiet < 0) return -1;
  long long sum = 0;
  long nz = 0;
  for (long b = 0; b < 1000; ++b) {
    Silence(l, r);
    Render(0, c, l, r);
    for (int i = 0; i < kBlock; ++i) { sum += (l[i] >> 16) + (r[i] >> 16); if (l[i] || r[i]) ++nz; }
  }
  CHECK(sum > -128000 / 4 && sum < 128000 / 4 && nz < 64000 / 2, "after the decay: dc sum %lld, nonzero %ld", sum, nz);
  return quiet;
}

void Stress(long blocks) {
  DirtySilence();
  coop_fx_scratch_dirty(0);
  FX_fickle.init(0);
  Ctl c = Defaults();
  Ctl target = c;
  Source src;
  SourceNew(&src, 2);
  long seg_end = 0, decays = 0, decay_max = 0, decay_fail = 0, inits = 0, wet_blocks = 0;
  long modes_seen[fickle::L_MODES] = { 0 }, wet_seen[3] = { 0 }, route_seen[3] = { 0 };
  bool walk = true;
  int32_t l[kBlock], r[kBlock];
  for (long b = 0; b < blocks; ++b) {
    if (b >= seg_end) {
      seg_end = b + RandIn(40, 500);
      SourceNew(&src, RandIn(0, 6));
      walk = (Rand() & 1) != 0;
      for (int k = 0; k < 4; ++k) target.knob[k] = (Rand() & 3) == 0 ? (Rand() & 1) * 32767 : RandIn(0, 32767);
      if (Rand() % 3 == 0) c.wet = RandIn(0, 2);
      if (Rand() % 3 == 0) c.route = RandIn(0, 2);
      if (Rand() % 2 == 0) c.lmode = RandIn(0, fickle::L_MODES - 1);
      if (Rand() % 40 == 0) {   // a restart on a dirty buffer (another effect was selected)
        coop_fx_scratch_dirty(0);
        FX_fickle.init(0);
        c.freeze = c.loop = c.dub = false;   // the core clears the latches at an init
        ++inits;
      }
      if (Rand() % 25 == 0) {   // the decay check
        const long q = Decay(c, 150000);
        if (q < 0) { ++decay_fail; CHECK(false, "block %ld: no decay to silence (wet %d time %d modify %d clock %d)", b,
                                         (int)c.wet, (int)c.knob[0], (int)c.knob[1], (int)c.knob[2]); }
        else { ++decays; if (q > decay_max) decay_max = q; }
        c.freeze = c.loop = c.dub = false;
      }
    }
    // the latches, in every order
    const uint32_t ev = Rand() % 1000;
    if (ev < 6) c.loop = !c.loop;
    else if (ev < 10) c.freeze = !c.freeze;
    else if (ev < 16) c.dub = !c.dub;
    // the knobs: walk or jump (a clock sweep is the white knob walking)
    for (int k = 0; k < 4; ++k) {
      if (walk) { const int32_t d = target.knob[k] - c.knob[k]; c.knob[k] += d > 300 ? 300 : (d < -300 ? -300 : d); }
      else c.knob[k] = target.knob[k];
    }
    modes_seen[c.lmode]++; wet_seen[c.wet]++; route_seen[c.route]++;
    SourceBlock(&src, l, r);
    int32_t il[kBlock], ir[kBlock];
    memcpy(il, l, sizeof(il)); memcpy(ir, r, sizeof(ir));
    Render(0, c, l, r);
    if (memcmp(il, l, sizeof(il)) || memcmp(ir, r, sizeof(ir))) ++wet_blocks;
    for (int i = 0; i < kBlock; ++i) CHECK((l[i] & 0xffff) == 0 && (r[i] & 0xffff) == 0, "block %ld: low bits", b);
    CheckDisplay(Disp(0), b);
  }
  printf("stress: %ld blocks, %ld changed by the effect, %ld re-inits, %ld decays to silence (longest %ld blocks, %.1f s), "
         "%ld failed\n", blocks, wet_blocks, inits, decays, decay_max, decay_max * 64 / 44100.0, decay_fail);
  printf("  blocks per wet mode %ld %ld %ld, per routing %ld %ld %ld, per loop mode", wet_seen[0], wet_seen[1], wet_seen[2],
         route_seen[0], route_seen[1], route_seen[2]);
  for (int m = 0; m < fickle::L_MODES; ++m) printf(" %ld", modes_seen[m]);
  printf("\n");
}

// A captured loop keeps playing over silence; a freeze keeps the wet channel; both end when released.
void Holds() {
  for (int wet = 0; wet < 3; ++wet) {
    for (int m = 0; m < fickle::L_MODES; ++m) {
      coop_fx_scratch_dirty(0);
      FX_fickle.init(0);
      Ctl c = Defaults();
      c.wet = wet; c.lmode = m; c.knob[3] = 32767; c.knob[1] = 8000;
      Source src;
      SourceNew(&src, 4);
      src.amp = 20000; src.note_len = 6000;
      int32_t l[kBlock], r[kBlock];
      for (int b = 0; b < 1000; ++b) { SourceBlock(&src, l, r); Render(0, c, l, r); }
      c.loop = true;
      long loud = 0, hot = 0, jumps = 0;
      int32_t last = 0;
      for (int b = 0; b < 2500; ++b) {
        Silence(l, r);
        Render(0, c, l, r);
        if (Peak(l, r) > 200) ++loud;
        for (int i = 0; i < kBlock; ++i) {
          const int32_t v = l[i] >> 16, d = v - last;
          if (v > 30000 || v < -30000) ++hot;
          if (d > 30000 || d < -30000) ++jumps;   // near full-scale flips: what garbage would give
          last = v;
        }
      }
      // the loop plays what it heard (notes at 20000 through the wet channel): no runaway, no garbage, no clicks
      CHECK(hot < 2500 * kBlock / 20 && jumps == 0, "wet %d loop mode %d: %ld samples near full scale, %ld jumps over 30000", wet, m,
            hot, jumps);
      // the env modes play only when hit (soft: gated, hard: from a slice), so silence needs a hit or two
      if (m != fickle::L_ENV_SOFT) CHECK(loud > 1200, "wet %d loop mode %d: the loop over silence loud in %ld of 2500 blocks",
                                         wet, m, loud);
      CHECK(Disp(0)->loop_state == 1 && Disp(0)->loop_len > 1000, "wet %d mode %d: loop state %d len %d", wet, m,
            (int)Disp(0)->loop_state, (int)Disp(0)->loop_len);
      c.loop = false;
      CHECK(Decay(c, 150000) >= 0, "wet %d loop mode %d: no silence after the loop", wet, m);
    }
    // freeze
    coop_fx_scratch_dirty(0);
    FX_fickle.init(0);
    Ctl c = Defaults();
    c.wet = wet; c.knob[3] = 32767; c.knob[0] = 20000;
    Source src;
    SourceNew(&src, 4);
    src.amp = 20000;
    int32_t l[kBlock], r[kBlock];
    for (int b = 0; b < 800; ++b) { SourceBlock(&src, l, r); Render(0, c, l, r); }
    c.freeze = true;
    long loud = 0;
    for (int b = 0; b < 6000; ++b) { Silence(l, r); Render(0, c, l, r); if (b > 3000 && Peak(l, r) > 100) ++loud; }
    CHECK(loud > 2500, "wet %d: frozen and loud in %ld of the last 3000 blocks", wet, loud);
    c.freeze = false;
    CHECK(Decay(c, 150000) >= 0, "wet %d: no silence after the freeze", wet);
  }
  printf("holds: a loop over silence in every wet x loop mode, a freeze in every wet mode, both released to silence\n");
}

// ---- WAV ----
struct Wav {
  FILE* f;
  uint32_t frames;
};

void Put32(FILE* f, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; fwrite(b, 1, 4, f); }
void Put16(FILE* f, uint32_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; fwrite(b, 1, 2, f); }

bool WavOpen(Wav* w, const char* path) {
  w->f = fopen(path, "wb");
  w->frames = 0;
  if (!w->f) return false;
  fwrite("RIFF", 1, 4, w->f); Put32(w->f, 0); fwrite("WAVEfmt ", 1, 8, w->f);
  Put32(w->f, 16); Put16(w->f, 1); Put16(w->f, 2); Put32(w->f, 44100); Put32(w->f, 44100 * 4); Put16(w->f, 4); Put16(w->f, 16);
  fwrite("data", 1, 4, w->f); Put32(w->f, 0);
  return true;
}

void WavBlock(Wav* w, const int32_t* l, const int32_t* r) {
  for (int i = 0; i < kBlock; ++i) { Put16(w->f, static_cast<uint32_t>(l[i]) >> 16); Put16(w->f, static_cast<uint32_t>(r[i]) >> 16); }
  w->frames += kBlock;
}

void WavClose(Wav* w) {
  fseek(w->f, 4, SEEK_SET); Put32(w->f, 36 + w->frames * 4);
  fseek(w->f, 40, SEEK_SET); Put32(w->f, w->frames * 4);
  fclose(w->f);
}

long Blocks(double s) { return static_cast<long>(s * 44100 / 64); }

const char* const kWetNames[3] = { "reverb", "delay", "slip" };
const char* const kLoopNames[fickle::L_MODES] = { "tape_1", "tape_2", "tape_half", "tape_1.5", "rev_1", "rev_2", "rev_half",
                                                  "ping", "env_soft", "env_hard", "stretch", "str_slow", "str_rev" };

void WavOut(const char* dir) {
  char path[512];
  // the wet modes: notes for 4 s, a clock sweep down and up, then 2.5 s frozen over silence
  for (int wet = 0; wet < 3; ++wet) {
    coop_fx_scratch_dirty(0);
    FX_fickle.init(0);
    Ctl c = Defaults();
    c.wet = wet; c.knob[3] = 20000;
    if (wet == 1) { c.knob[0] = 22000; c.knob[1] = 20000; }
    if (wet == 2) { c.knob[0] = 16000; c.knob[1] = 26000; }
    snprintf(path, sizeof(path), "%s/wet_%s.wav", dir, kWetNames[wet]);
    Wav w;
    if (!WavOpen(&w, path)) { printf("cannot write %s\n", path); g_fail++; return; }
    g_rng = 12345;
    Source src;
    SourceNew(&src, 4);
    src.amp = 18000; src.note_len = 11025;
    int32_t l[kBlock], r[kBlock];
    const long total = Blocks(10.0);
    for (long b = 0; b < total; ++b) {
      const double t = b * 64 / 44100.0;
      if (t >= 3.0 && t < 6.0) {   // the clock sweep: 24 kHz -> 2 kHz -> 32 kHz
        const double u = (t - 3.0) / 3.0;
        c.knob[2] = static_cast<int32_t>(u < 0.5 ? 29372 * (1 - 2 * u) : 32767 * (2 * u - 1));
      } else if (t >= 6.0) c.knob[2] = 29372;
      if (wet == 2 && t >= 1.5 && t < 3.0) c.knob[1] = 6000;    // slip: reverse
      if (wet == 2 && t >= 3.0) c.knob[1] = 26000;
      if (t >= 7.5) c.freeze = true;
      if (t < 7.5) SourceBlock(&src, l, r); else Silence(l, r);
      Render(0, c, l, r);
      WavBlock(&w, l, r);
    }
    WavClose(&w);
    printf("%s\n", path);
  }
  // the loop modes: notes for 2 s (listening), loop on (silence in), a clock sweep while it plays, a dub with new
  // notes, loop off
  for (int m = 0; m < fickle::L_MODES; ++m) {
    coop_fx_scratch_dirty(0);
    FX_fickle.init(0);
    Ctl c = Defaults();
    c.lmode = m; c.knob[3] = 22000; c.knob[1] = 6000; c.knob[0] = 9000;
    snprintf(path, sizeof(path), "%s/loop_%s.wav", dir, kLoopNames[m]);
    Wav w;
    if (!WavOpen(&w, path)) { printf("cannot write %s\n", path); g_fail++; return; }
    g_rng = 777;
    Source src;
    SourceNew(&src, 4);
    src.amp = 18000; src.note_len = 5512;
    Source hits;
    SourceNew(&hits, 5);
    hits.amp = 16000; hits.note_len = 9000;
    int32_t l[kBlock], r[kBlock];
    const long total = Blocks(11.0);
    for (long b = 0; b < total; ++b) {
      const double t = b * 64 / 44100.0;
      c.loop = t >= 2.0 && t < 10.0;
      c.dub = t >= 7.0 && t < 8.5;
      if (t >= 4.0 && t < 6.5) {     // the clock sweep while it plays: down an octave and back (tape pitch)
        const double u = (t - 4.0) / 2.5;
        const double k = u < 0.5 ? 1 - 2 * u : 2 * u - 1;
        c.knob[2] = static_cast<int32_t>(29372 - 8192 * (1 - k));
      } else c.knob[2] = 29372;
      const bool env = m == fickle::L_ENV_SOFT || m == fickle::L_ENV_HARD;
      if (t < 2.0 || (t >= 7.0 && t < 8.5)) SourceBlock(&src, l, r);
      else if (env && t >= 2.5 && t < 7.0) SourceBlock(&hits, l, r);   // hits to retrigger the env modes
      else Silence(l, r);
      Render(0, c, l, r);
      WavBlock(&w, l, r);
    }
    WavClose(&w);
    printf("%s\n", path);
  }
}

// ---- cycles (the simulator) ----
}  // namespace
extern "C" { uint32_t g_fickle_prof[8]; }   // FICKLE_PROFILE: fickle.cc adds each pass's cycles here
namespace {
#ifdef __bfin__
inline uint32_t Cycles() { uint32_t c; __asm__ __volatile__("%0 = CYCLES;" : "=d"(c)); return c; }
#else
inline uint32_t Cycles() { return 0; }
#endif

void Measure(const char* label, Ctl c, long blocks, bool play, bool dub) {
  coop_fx_scratch_dirty(0);
  FX_fickle.init(0);
  Source src;
  SourceNew(&src, 2);
  src.amp = 16000;
  int32_t l[kBlock], r[kBlock];
  for (int b = 0; b < 400; ++b) { SourceBlock(&src, l, r); Render(0, c, l, r); }   // fill the loop buffer a little
  c.loop = play;
  c.dub = dub;
  uint64_t sum = 0;
  uint32_t mx = 0;
  long mx_at = -1;
  for (int i = 0; i < 8; ++i) g_fickle_prof[i] = 0;
  int16_t p[16];
  Params(c, p);
  for (long b = 0; b < blocks; ++b) {
    if (b % 200 == 100) { c.knob[0] = RandIn(0, 32767); Params(c, p); }   // the time moves (delay crossfades, slip)
    SourceBlock(&src, l, r);
    const uint32_t t0 = Cycles();
    FX_fickle.render(0, l, r, p, kBlock);
    const uint32_t dt = Cycles() - t0;
    DigestBlock(0, l, r);
    sum += dt;
    if (dt > mx) { mx = dt; mx_at = b; }
  }
  printf("  %-28s avg %6lu  max %6lu (block %4ld)", label, static_cast<unsigned long>(sum / blocks), static_cast<unsigned long>(mx), mx_at);
#ifdef FICKLE_PROFILE
  printf("   [A %lu B %lu route %lu wet %lu duck %lu D %lu E %lu pub %lu]", (unsigned long)(g_fickle_prof[0] / blocks),
         (unsigned long)(g_fickle_prof[1] / blocks), (unsigned long)(g_fickle_prof[2] / blocks),
         (unsigned long)(g_fickle_prof[3] / blocks), (unsigned long)(g_fickle_prof[4] / blocks),
         (unsigned long)(g_fickle_prof[5] / blocks), (unsigned long)(g_fickle_prof[6] / blocks),
         (unsigned long)(g_fickle_prof[7] / blocks));
#endif
  printf("\n");
}

void Sim(long blocks) {
#ifdef __bfin__
  uint32_t s;
  __asm__ __volatile__("%0 = SYSCFG;" : "=d"(s));
  s |= 2;   // CCEN
  __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(s));
#endif
  char label[64];
  for (int clockset = 0; clockset < 2; ++clockset) {
    const int32_t clk = clockset ? 29372 : 32767;
    printf("cycles a block (compute only), clock %s:\n", clockset ? "default (24 kHz)" : "top (32 kHz)");
    for (int wet = 0; wet < 3; ++wet) {
      Ctl c = Defaults();
      c.wet = wet; c.knob[2] = clk; c.knob[1] = 20000;
      snprintf(label, sizeof(label), "wet %s, listening", kWetNames[wet]);
      Measure(label, c, blocks, false, false);
      c.freeze = true;
      snprintf(label, sizeof(label), "wet %s, frozen", kWetNames[wet]);
      Measure(label, c, blocks, false, false);
    }
    for (int m = 0; m < fickle::L_MODES; ++m) {
      Ctl c = Defaults();
      c.wet = 0; c.knob[2] = clk; c.lmode = m; c.route = 1;
      snprintf(label, sizeof(label), "reverb + loop %s + dub", kLoopNames[m]);
      Measure(label, c, blocks, true, true);
    }
    {
      Ctl c = Defaults();
      c.wet = 2; c.knob[2] = clk; c.lmode = fickle::L_STR_REV; c.route = 1; c.knob[1] = 1000;
      Measure("slip rev x2 + str rev + dub", c, blocks, true, true);
    }
  }
}

#ifdef FICKLE_FRAMES
// ---- screen frames (screen_rec.cc records the screen calls; render_frames.py draws them) ----
}  // namespace
extern "C" const coop_fx_screen_t SCREEN_fickle;
extern "C" void screen_rec_dump(FILE* f, const char* name);
namespace {

struct Scene {
  const char* name;
  int32_t wet, route, lmode;
  bool freeze, loop, dub;
  int32_t time, modv, clk, mix;
};

void Frames(const char* out) {
  FILE* f = fopen(out, "w");
  if (!f) { g_fail++; return; }
  static const Scene kScenes[] = {
    { "listening", 0, 1, 0, false, false, false, 14000, 18000, 29372, 16384 },
    { "loop_freeze", 1, 1, fickle::L_REV1, true, true, false, 20000, 20000, 29372, 20000 },
    { "dub_lowclock", 2, 2, fickle::L_ENV_HARD, false, true, true, 16000, 26000, 6000, 24000 },
    { "stretch_in", 0, 0, fickle::L_STR_SLOW, false, true, false, 24000, 6000, 20000, 12000 },
  };
  int dummy_scr = 0;
  SCREEN_fickle.init(&dummy_scr);
  for (const Scene& sc : kScenes) {
    coop_fx_scratch_dirty(0);
    FX_fickle.init(0);
    Ctl c = Defaults();
    c.wet = sc.wet; c.route = sc.route; c.lmode = sc.lmode;
    c.knob[0] = sc.time; c.knob[1] = sc.modv; c.knob[2] = sc.clk; c.knob[3] = sc.mix;
    Source src;
    g_rng = 4242;
    SourceNew(&src, 4);
    src.amp = 20000; src.note_len = 5000;
    Source hits;
    SourceNew(&hits, 5);
    hits.amp = 16000; hits.note_len = 7000;
    uint32_t bytes = 0;
    void* state = coop_fx_scratch(0, &bytes);
    coop_fx_view_t v;
    memset(&v, 0, sizeof(v));
    int32_t l[kBlock], r[kBlock];
    int dumped = 0;
    for (int b = 0; b < 1400; ++b) {
      if (b == 700) { c.freeze = sc.freeze; c.loop = sc.loop; }
      if (b == 760) c.dub = sc.dub;
      if (sc.lmode == fickle::L_ENV_HARD && b > 700) SourceBlock(&hits, l, r); else SourceBlock(&src, l, r);
      Render(0, c, l, r);
      if (b % 11 == 0) {                                // a frame every ~16 ms
        v.slot = 0;
        for (int k = 0; k < 4; ++k) v.knobs[k] = static_cast<int16_t>(c.knob[k]);
        v.words[0] = static_cast<int16_t>(c.wet); v.words[1] = static_cast<int16_t>(c.route);
        v.words[2] = static_cast<int16_t>(c.lmode);
        v.latches = static_cast<uint16_t>((c.freeze ? 1 : 0) | (c.loop ? 2 : 0) | (c.dub ? 4 : 0));
        v.state = state; v.state_bytes = bytes; v.live = 1;
        SCREEN_fickle.update(&dummy_scr, 16, &v);
        if (b == 1100 || b == 1210 || b == 1320) {
          char name[64];
          snprintf(name, sizeof(name), "%s_%d", sc.name, dumped++);
          screen_rec_dump(f, name);
        }
      }
    }
  }
  fclose(f);
  printf("frames: %s\n", out);
}
#endif

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: %s stress BLOCKS | wav OUTDIR | sim BLOCKS\n", argv[0]); return 2; }
  printf("fickle: state %u bytes (plugin.json scratch %u), display at +%u\n", static_cast<unsigned>(sizeof(fickle::State)),
         static_cast<unsigned>(COOP_FX_SCRATCH_BYTES), static_cast<unsigned>(offsetof(fickle::State, disp)));
  if (!strcmp(argv[1], "stress")) {
    const long blocks = argc > 2 ? atol(argv[2]) : 40000;
    if (argc > 3) g_rng = static_cast<uint32_t>(atol(argv[3])) * 2654435761u + 1u;
    Stress(blocks);
    Holds();
    int16_t p[16];
    Ctl c = Defaults();
    c.route = 2;
    Params(c, p);
    const long bad = coop_fx_scratch_check(&FX_fickle, p, 3000);
    CHECK(bad == 0, "scratch check: %ld blocks differ", bad);
    c.wet = 2; c.lmode = fickle::L_STRETCH;
    Params(c, p);
    CHECK(coop_fx_scratch_check(&FX_fickle, p, 3000) == 0, "scratch check (slip, stretch)");
  } else if (!strcmp(argv[1], "wav") && argc > 2) {
    WavOut(argv[2]);
#ifdef FICKLE_FRAMES
  } else if (!strcmp(argv[1], "frames") && argc > 2) {
    Frames(argv[2]);
#endif
  } else if (!strcmp(argv[1], "sim")) {
    Sim(argc > 2 ? atol(argv[2]) : 600);
  } else {
    return 2;
  }
  printf("output digest %08lx%08lx\n", static_cast<unsigned long>(g_digest >> 32),
         static_cast<unsigned long>(g_digest & 0xffffffffu));
  if (g_fail) { printf("fickle: %d checks FAILED\n", g_fail); return 1; }
  printf("fickle: all checks passed\n");
  return 0;
}
