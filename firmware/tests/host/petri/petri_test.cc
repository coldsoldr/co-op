// petri's host test (guide/plugin-guidelines.md §7.4, docs/plugin-performance.md): there is no upstream to compare
// against, so the test drives the plugin's coop_fx_t as the core does and checks what must hold for any input:
//
//   1. stress: BLOCKS random 64-sample stereo blocks: noise, saws, impulses, a riff, silence, DC, L != R; every effect;
//      the four turns walking and jumping (in the filter type's range units, as the core passes them), the three words
//      walking and jumping, hold and reverse toggling, restarts (a pause over 5 ms, a re-init). Every output sample has
//      its low 16 bits clear (the int16 bus widened); no run of 48 or more full-scale samples (a stuck overflow) while
//      the input is at most -6 dB; the display words in range.
//   2. decay: for each effect, 3 s of the riff with repeats and space at their top, hold and reverse off, then silence:
//      the output must fall to within 2 LSB of zero and stay there (no stuck DC, no limit cycle) inside the allowed
//      time; with hold on it keeps playing (the held slice; reported, not asserted).
//   2b. clicks: a plucked sine through each effect (wet only; defaults, reverse with a time jump, more activity then
//      hold): no second difference over 3000 LSB (a step) anywhere.
//   3. e8: tests/host/fx_scratch_host.cc's coop_fx_scratch_check (slot 0 beside slot 3 on dirty buffers, a restart).
//   4. listening: each effect renders 8 s of the riff to build/petri_wav/<n>_<effect>.wav (16-bit stereo 44.1 kHz,
//      the plugin's defaults, mix 60 %; wet/ the same at mix 100 %), plus the dry riff, and a few hold / reverse takes.
//   5. in the Blackfin simulator (`make sim`): the two assembly kernels against their C references, and the render's
//      cycles a block per effect, avg and max (compute only: no SDRAM waits, which the device stats give).
//
//   petri_test [WAVDIR [BLOCKS]]      (simulator: petri_test.elf - BLOCKS, cycle figures only)
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "coop_plugin.h"
#include "state.h"

extern "C" const coop_fx_t FX_petri;
extern "C" long coop_fx_scratch_check(const coop_fx_t* fx, const int16_t* params, long blocks);   // ../fx_scratch_host.cc
extern "C" void coop_fx_scratch_dirty(int slot);
extern "C" void* coop_fx_scratch(int slot, uint32_t* bytes);
uint32_t g_petri_host_cycles;

namespace {

const int kBlock = 64;
const char* const kNames[11] = { "mosaic", "seq", "glide", "haze", "tunnel", "strum", "blocks", "cut", "arp",
                                 "pattern", "warp" };
const int16_t kRangeLo[4] = { 64, 0, 0, 8192 };
const int16_t kRangeHi[4] = { 14528, 24576, 22528, 32767 };
const int32_t kDefaults[4] = { 16384, 13107, 13107, 16384 };   // plugin.json defaults (Q15)
const int32_t kFilterDefault = 30000, kSpaceDefault = 8000;

uint32_t rng = 12345;
uint32_t Rand() {
  rng = rng * 1664525u + 1013904223u;
  return rng >> 8;
}
int32_t RandRange(int32_t lo, int32_t hi) { return lo + static_cast<int32_t>(Rand() % static_cast<uint32_t>(hi - lo + 1)); }

int16_t ToRange(int32_t q15, int k) {   // a Q15 knob to the filter type's range units (what the core passes)
  return static_cast<int16_t>(kRangeLo[k] + ((kRangeHi[k] - kRangeLo[k]) * q15) / 32767);
}

void SetParams(int16_t* p, const int32_t* knob, int32_t effect, int32_t filter, int32_t space, bool hold, bool rev) {
  for (int k = 0; k < 16; ++k) p[k] = 0;
  for (int k = 0; k < 4; ++k) p[k] = ToRange(knob[k], k);
  p[4] = 0x5300;
  p[5] = static_cast<int16_t>(effect);
  p[6] = static_cast<int16_t>(filter);
  p[7] = static_cast<int16_t>(space);
  p[8] = static_cast<int16_t>((hold ? 1 : 0) | (rev ? 2 : 0));
}

// ---- the sources (integer only: the simulator build too) ----

// A sine from a phase (parabolic approximation, Q15).
int32_t Sine(uint32_t ph) {
  const int32_t x = static_cast<int32_t>(ph >> 16) - 32768;        // -32768..32767: one cycle
  const int32_t ax = x < 0 ? -x : x;
  const int32_t y = (4 * x * 1LL * (32768 - ax)) >> 15;           // 4 x (1 - |x|), Q15
  return y > 32767 ? 32767 : (y < -32767 ? -32767 : y);
}

// The riff: a plucked saw-ish voice on a pentatonic line (a note every 250 ms, two octaves), a soft kick on the beat,
// a little stereo spread. Deterministic in the sample index.
struct Riff {
  uint32_t ph_l, ph_r, kick_ph;
  int32_t env, kick_env, note;
  void Reset() { ph_l = ph_r = kick_ph = 0; env = kick_env = 0; note = 0; }
  void Block(long b, int16_t* l, int16_t* r) {
    static const int32_t kInc[10] = { 21474836, 24104305, 27056516, 32177512, 36112840,   // A2 B2 C#3 E3 F#3 (x2^32/44100)
                                      42949673, 48208610, 54113032, 64355024, 72225680 };
    static const int8_t kLine[16] = { 0, 2, 4, 7, 5, 3, 1, 4, 9, 7, 5, 2, 6, 4, 8, 3 };
    for (int i = 0; i < kBlock; ++i) {
      const long n = b * kBlock + i;
      if (n % 11025 == 0) {                         // a note every 250 ms
        note = kLine[(n / 11025) % 16];
        env = 30000;
      }
      if (n % 22050 == 0) kick_env = 32000;
      const uint32_t inc = static_cast<uint32_t>(kInc[note]);
      ph_l += inc;
      ph_r += inc + (inc >> 9);                     // slightly detuned on the right
      const int32_t saw_l = static_cast<int32_t>(ph_l >> 17) - 16384;
      const int32_t saw_r = static_cast<int32_t>(ph_r >> 17) - 16384;
      env -= env >> 11;                             // ~ 46 ms decay constant
      kick_ph += static_cast<uint32_t>(4000000 + (kick_env << 7));
      kick_env -= kick_env >> 9;
      const int32_t kick = (Sine(kick_ph) * kick_env) >> 16;
      int32_t a = ((saw_l * env) >> 15) + kick, c = ((saw_r * env) >> 15) + kick;
      l[i] = static_cast<int16_t>(a > 32767 ? 32767 : (a < -32768 ? -32768 : a));
      r[i] = static_cast<int16_t>(c > 32767 ? 32767 : (c < -32768 ? -32768 : c));
    }
  }
};

// Stress sources: 0 silence, 1 noise, 2 saws (L != R), 3 impulses, 4 the riff, 5 DC + noise, 6 full-scale square.
struct Source {
  int kind;
  uint32_t ph_l, ph_r, inc_l, inc_r, noise;
  int32_t level;
  Riff riff;
  void Block(long b, int16_t* l, int16_t* r) {
    if (kind == 4) {
      riff.Block(b, l, r);
      for (int i = 0; i < kBlock; ++i) { l[i] = static_cast<int16_t>((l[i] * level) >> 15); r[i] = static_cast<int16_t>((r[i] * level) >> 15); }
      return;
    }
    for (int i = 0; i < kBlock; ++i) {
      int32_t a = 0, c = 0;
      noise = noise * 1103515245u + 12345u;
      const int32_t nz = static_cast<int32_t>(noise >> 16) - 32768;
      ph_l += inc_l;
      ph_r += inc_r;
      switch (kind) {
        case 1: a = nz; c = -nz / 2 + static_cast<int32_t>((noise >> 3) & 0x3fff) - 8192; break;
        case 2: a = static_cast<int32_t>(ph_l >> 16) - 32768; c = static_cast<int32_t>(ph_r >> 16) - 32768; break;
        case 3: a = ((b * kBlock + i) % 3001 == 0) ? 32767 : 0; c = ((b * kBlock + i) % 2203 == 0) ? -32768 : 0; break;
        case 5: a = 12000 + (nz >> 4); c = -9000 + (nz >> 4); break;
        case 6: a = (ph_l >> 31) ? 32767 : -32768; c = (ph_r >> 31) ? 32767 : -32768; break;
        default: break;
      }
      a = (a * level) >> 15;
      c = (c * level) >> 15;
      l[i] = static_cast<int16_t>(a);
      r[i] = static_cast<int16_t>(c);
    }
  }
};

// ---- WAV out ----

struct Wav {
  std::FILE* f;
  uint32_t frames;
  bool Open(const char* path) {
    f = std::fopen(path, "wb");
    frames = 0;
    if (!f) return false;
    uint8_t h[44] = { 0 };
    std::fwrite(h, 1, 44, f);
    return true;
  }
  void Put(const int16_t* l, const int16_t* r, int n) {
    if (!f) return;
    for (int i = 0; i < n; ++i) {
      const uint8_t b[4] = { static_cast<uint8_t>(l[i]), static_cast<uint8_t>(static_cast<uint16_t>(l[i]) >> 8),
                             static_cast<uint8_t>(r[i]), static_cast<uint8_t>(static_cast<uint16_t>(r[i]) >> 8) };
      std::fwrite(b, 1, 4, f);
    }
    frames += static_cast<uint32_t>(n);
  }
  static void Le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
  void Close() {
    if (!f) return;
    uint8_t h[44];
    std::memcpy(h, "RIFF", 4);
    Le32(h + 4, 36 + frames * 4);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    Le32(h + 16, 16);
    h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;    // PCM, stereo
    Le32(h + 24, 44100);
    Le32(h + 28, 44100 * 4);
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    std::memcpy(h + 36, "data", 4);
    Le32(h + 40, frames * 4);
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(h, 1, 44, f);
    std::fclose(f);
    f = 0;
  }
};

#ifdef __bfin__
inline uint32_t RealCycles() {
  uint32_t c;
  __asm__ __volatile__("%0 = CYCLES;" : "=d"(c));
  return c;
}
#endif

// One render of 64 int16 pairs through the plugin at `slot` (the Q31 bus), with the low-bit check.
// Every rendered block's output and display words folded into one number (FNV-1a), printed at the end: two builds of
// the plugin that print the same digest gave the same samples in every check of the run (the bit-exactness proof of a
// change that must not alter the sound: build the old sources with COOP=<their folder> and compare the lines).
uint64_t g_digest = 1469598103934665603ull;
void Digest(const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) g_digest = (g_digest ^ b[i]) * 1099511628211ull;
}

bool Render(int slot, const int16_t* il, const int16_t* ir, int16_t* ol, int16_t* orr, const int16_t* p, uint32_t* cyc) {
  int32_t l[kBlock], r[kBlock];
  for (int i = 0; i < kBlock; ++i) {
    l[i] = static_cast<int32_t>(il[i]) << 16;
    r[i] = static_cast<int32_t>(ir[i]) << 16;
  }
  g_petri_host_cycles += 20000;
#ifdef __bfin__
  const uint32_t t0 = RealCycles();
#endif
  FX_petri.render(slot, l, r, p, kBlock);
#ifdef __bfin__
  if (cyc) *cyc = RealCycles() - t0;
#else
  if (cyc) *cyc = 0;
#endif
  bool ok = true;
  for (int i = 0; i < kBlock; ++i) {
    if ((l[i] & 0xffff) || (r[i] & 0xffff)) ok = false;
    ol[i] = static_cast<int16_t>(l[i] >> 16);
    orr[i] = static_cast<int16_t>(r[i] >> 16);
  }
  Digest(l, sizeof(l));
  Digest(r, sizeof(r));
  {
    uint32_t bytes = 0;
    const coop_petri::Instance* st = static_cast<const coop_petri::Instance*>(coop_fx_scratch(slot, &bytes));
    Digest(&st->disp, sizeof(st->disp));
  }
  return ok;
}

const coop_petri::Instance* State(int slot) {
  uint32_t bytes;
  return static_cast<const coop_petri::Instance*>(coop_fx_scratch(slot, &bytes));
}

bool DisplayOk(int slot) {
  const coop_petri::Display& d = State(slot)->disp;
  if (d.effect < -1 || d.effect > 10 || d.head < 0 || d.head > 32767 || d.level < 0 || d.level > 32768) return false;
  for (int i = 0; i < 8; ++i) {
    if (d.v[i].pos < -1 || d.v[i].pos > 32767 || d.v[i].level < 0 || d.v[i].level > 32767) return false;
    if (d.v[i].rate < -2048 || d.v[i].rate > 2048) return false;
  }
  return true;
}

// ---- 1. stress ----
[[maybe_unused]] int Stress(long blocks) {
  int16_t p[16];
  int32_t knob[4] = { kDefaults[0], kDefaults[1], kDefaults[2], kDefaults[3] };
  int32_t effect = 0, filter = kFilterDefault, space = kSpaceDefault;
  bool hold = false, rev = false;
  Source src;
  std::memset(&src, 0, sizeof(src));
  src.kind = 4; src.level = 32767; src.inc_l = 40000000; src.inc_r = 61000000; src.noise = 1;
  src.riff.Reset();
  FX_petri.init(0);
  long bad_bits = 0, bad_runs = 0, bad_disp = 0, wet = 0, restarts = 0, inits = 0;
  int run_l = 0, run_r = 0;
  long fx_blocks[11] = { 0 };
  for (long b = 0; b < blocks; ++b) {
    if (Rand() % 300 == 0) {
      src.kind = RandRange(0, 6);
      src.level = (src.kind == 6 || Rand() % 4 == 0) ? 32767 : RandRange(2000, 16384);
      src.inc_l = static_cast<uint32_t>(RandRange(1000000, 200000000));
      src.inc_r = static_cast<uint32_t>(RandRange(1000000, 200000000));
    }
    for (int k = 0; k < 4; ++k) {
      const uint32_t ev = Rand() % 1000;
      if (ev < 3) knob[k] = RandRange(0, 32767);
      else if (ev < 60) knob[k] += RandRange(-500, 500);
      knob[k] = knob[k] < 0 ? 0 : (knob[k] > 32767 ? 32767 : knob[k]);
    }
    if (b % 4000 >= 3300 && b % 4000 < 3700) {   // stretches with every turn moving every block (0.3: the plugin keeps
      for (int k = 0; k < 4; ++k) {              // each turn's value until its word moves)
        knob[k] += RandRange(40, 200) * ((Rand() & 1) ? 1 : -1);
        knob[k] = knob[k] < 0 ? 300 : (knob[k] > 32767 ? 32400 : knob[k]);
      }
    }
    if (Rand() % 700 == 0) effect = RandRange(0, 10);
    if (Rand() % 4000 == 0) effect = RandRange(-3, 14);               // a stored word out of range: the plugin clamps
    if (Rand() % 300 == 0) filter = Rand() % 2 ? RandRange(0, 32767) : filter + RandRange(-2000, 2000);
    if (Rand() % 300 == 0) space = Rand() % 2 ? RandRange(0, 32767) : space + RandRange(-2000, 2000);
    filter = filter < 0 ? 0 : (filter > 32767 ? 32767 : filter);
    space = space < 0 ? 0 : (space > 32767 ? 32767 : space);
    if (Rand() % 500 == 0) hold = !hold;
    if (Rand() % 600 == 0) rev = !rev;
    if (Rand() % 5000 == 0) { FX_petri.init(0); hold = rev = false; ++inits; }   // a sound change: latches cleared
    if (Rand() % 6000 == 0) { g_petri_host_cycles += 3000000; ++restarts; }     // a pause (bypass) over 5 ms
    SetParams(p, knob, effect, filter, space, hold, rev);
    int16_t il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
    src.Block(b, il, ir);
    if (!Render(0, il, ir, ol, orr, p, 0)) ++bad_bits;
    const bool loud = src.level > 16384;
    for (int i = 0; i < kBlock; ++i) {
      run_l = (ol[i] >= 32767 || ol[i] <= -32768) ? run_l + 1 : 0;
      run_r = (orr[i] >= 32767 || orr[i] <= -32768) ? run_r + 1 : 0;
      if (!loud && (run_l == 48 || run_r == 48)) ++bad_runs;
      if (ol[i] != il[i] || orr[i] != ir[i]) { ++wet; }
    }
    if (!DisplayOk(0)) ++bad_disp;
    const int32_t fx = State(0)->disp.effect;
    if (fx >= 0 && fx < 11) ++fx_blocks[fx];
  }
  std::printf("stress: %ld blocks, %ld restarts, %ld re-inits; low bits set %ld, stuck full-scale runs %ld, display out "
              "of range %ld; samples changed by the effect %ld\n  blocks per effect:", blocks, restarts, inits, bad_bits,
              bad_runs, bad_disp, wet);
  for (int k = 0; k < 11; ++k) std::printf(" %s %ld", kNames[k], fx_blocks[k]);
  std::printf("\n");
  int fail = (bad_bits || bad_runs || bad_disp || wet == 0) ? 1 : 0;
  for (int k = 0; k < 11; ++k) if (fx_blocks[k] == 0) fail = 1;
  return fail;
}

// ---- 2. decay ----
// Feed the riff for `feed` blocks, then silence; the blocks until the output stays within 2 LSB (or -1).
long DecayBlocks(int fx, bool hold, long feed, long max_silence, int32_t repeats, int32_t space, int32_t* tail_peak) {
  int16_t p[16];
  int32_t knob[4] = { kDefaults[0], 26000, repeats, 32767 };       // mix full wet: the tail alone
  FX_petri.init(0);
  Riff riff;
  riff.Reset();
  int16_t il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
  for (long b = 0; b < feed; ++b) {
    SetParams(p, knob, fx, kFilterDefault, space, false, false);
    riff.Block(b, il, ir);
    Render(0, il, ir, ol, orr, p, 0);
  }
  for (int i = 0; i < kBlock; ++i) il[i] = ir[i] = 0;
  long last_loud = -1;
  int32_t peak_end = 0;
  for (long b = 0; b < max_silence; ++b) {
    SetParams(p, knob, fx, kFilterDefault, space, hold && b > 4, false);
    Render(0, il, ir, ol, orr, p, 0);
    int32_t pk = 0;
    for (int i = 0; i < kBlock; ++i) {
      const int32_t a = ol[i] < 0 ? -ol[i] : ol[i], c = orr[i] < 0 ? -orr[i] : orr[i];
      if (a > pk) pk = a;
      if (c > pk) pk = c;
    }
    if (pk > 2) last_loud = b;
    if (b >= max_silence - 690) peak_end = pk > peak_end ? pk : peak_end;
  }
  *tail_peak = peak_end;
  return last_loud + 1 < max_silence - 690 ? last_loud + 1 : -1;
}

[[maybe_unused]] int Decay() {
  int fail = 0;
  std::printf("decay (riff 3 s, activity 26000, then silence; blocks of 1.45 ms until within 2 LSB):\n");
  for (int fx = 0; fx < 11; ++fx) {
    int32_t pk_def, pk_max, pk_hold;
    const long d_def = DecayBlocks(fx, false, 2067, 20000, 13107, kSpaceDefault, &pk_def);           // defaults
    const long d_max = DecayBlocks(fx, false, 2067, 60000, 32767, 32767, &pk_max);                   // all at the top
    const long d_hold = DecayBlocks(fx, true, 2067, 4000, 13107, kSpaceDefault, &pk_hold);           // held
    std::printf("  %-8s defaults %6ld (%5.1f s) end %4d   top %6ld (%5.1f s) end %4d   held: %6ld end %5d\n",
                kNames[fx], d_def, d_def * 64 / 44100.0, static_cast<int>(pk_def), d_max, d_max * 64 / 44100.0,
                static_cast<int>(pk_max), d_hold, static_cast<int>(pk_hold));
    if (d_def < 0 || d_max < 0) fail = 1;
  }
  return fail;
}

// ---- 2b. clicks ----
// A smooth source (a 220 Hz sine, plucked every 300 ms with a 3 ms attack, L and R a little apart): an output whose
// second difference passes 3000 LSB has a step in it (the sine four octaves up at full scale bends by under 2000). Wet only;
// variants: 0 the defaults, 1 reverse with a time jump, 2 more activity, then hold.
[[maybe_unused]] int Clicks(bool report) {
  int16_t p[16];
  int fail = 0;
  const long blocks = 6 * 44100 / kBlock;
  if (report) std::printf("clicks (a plucked 220 Hz sine, wet only, 6 s; second differences over 3000 LSB):\n ");
  for (int fx = 0; fx < 11; ++fx) {
    for (int variant = 0; variant < 3; ++variant) {
      FX_petri.init(0);
      int32_t knob[4] = { kDefaults[0], variant == 2 ? 30000 : kDefaults[1], kDefaults[2], 32767 };
      uint32_t ph = 0;
      int32_t env = 0;
      int16_t il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock], last_l = 0, last_r = 0, prev_l = 0, prev_r = 0;
      long clicks = 0;
      for (long b = 0; b < blocks; ++b) {
        if (variant == 1 && b == blocks / 3) knob[0] = 4000;            // a time jump mid-take
        SetParams(p, knob, fx, 32767, 0, variant == 2 && b > blocks / 2, variant == 1);
        for (int i = 0; i < kBlock; ++i) {
          const long n = b * kBlock + i;
          const long m = n % 13230;                                      // 300 ms
          const int32_t target = m < 132 ? static_cast<int32_t>((m * 16000) / 132) : 16000 - static_cast<int32_t>(((m - 132) * 16000) / 13098);
          env = target;
          ph += 21424795u;                                               // 220 Hz
          il[i] = static_cast<int16_t>((Sine(ph) * env) >> 15);
          ir[i] = static_cast<int16_t>((Sine(ph + 0x20000000u) * env) >> 15);
        }
        Render(0, il, ir, ol, orr, p, 0);
        if (b < 24) { prev_l = ol[kBlock - 2]; prev_r = orr[kBlock - 2]; last_l = ol[kBlock - 1]; last_r = orr[kBlock - 1]; continue; }   // the clear
        for (int i = 0; i < kBlock; ++i) {
          const int32_t dl = ol[i] - 2 * last_l + prev_l, dr = orr[i] - 2 * last_r + prev_r;   // a step shows here
          if (dl > 3000 || dl < -3000 || dr > 3000 || dr < -3000) ++clicks;
          prev_l = last_l;
          prev_r = last_r;
          last_l = ol[i];
          last_r = orr[i];
        }
      }
      if (report) std::printf(" %s/%d %ld", kNames[fx], variant, clicks);
      if (clicks > 0) fail = 1;
    }
  }
  if (report) std::printf("\n");
  return fail;
}

// ---- 4. listening ----
[[maybe_unused]] void Listen(const char* dir) {
  char path[512];
  int16_t p[16];
  Riff riff;
  const long blocks = 8 * 44100 / kBlock;
  int16_t il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
  {
    Wav w;
    std::snprintf(path, sizeof(path), "%s/00_dry.wav", dir);
    if (!w.Open(path)) { std::fprintf(stderr, "cannot write %s\n", path); return; }
    riff.Reset();
    for (long b = 0; b < blocks; ++b) { riff.Block(b, il, ir); w.Put(il, ir, kBlock); }
    w.Close();
  }
  struct Take { int fx; const char* tag; int32_t act, rep; bool hold_mid, rev; };
  Take takes[11 + 4];
  int nt = 0;
  for (int fx = 0; fx < 11; ++fx) takes[nt++] = { fx, "", kDefaults[1], kDefaults[2], false, false };
  takes[nt++] = { 0, "_hold", 26000, 26000, true, false };
  takes[nt++] = { 3, "_hold", 26000, 20000, true, false };
  takes[nt++] = { 2, "_reverse", 20000, 20000, false, true };
  takes[nt++] = { 9, "_reverse", 26000, 16000, false, true };
  for (int t = 0; t < 2 * nt; ++t) {                                    // the takes at mix 60 %, then wet only
    const bool wet = t >= nt;
    const int tk = t % nt;
    Wav w;
    std::snprintf(path, sizeof(path), "%s/%s%02d_%s%s.wav", dir, wet ? "wet/" : "", takes[tk].fx + 1,
                  kNames[takes[tk].fx], takes[tk].tag);
    if (!w.Open(path)) continue;
    FX_petri.init(0);
    riff.Reset();
    int32_t knob[4] = { kDefaults[0], takes[tk].act, takes[tk].rep, wet ? 32767 : 19661 };   // mix 60 % or wet
    for (long b = 0; b < blocks; ++b) {
      const bool held = takes[tk].hold_mid && b >= blocks / 3;
      SetParams(p, knob, takes[tk].fx, kFilterDefault, kSpaceDefault, held, takes[tk].rev);
      riff.Block(b, il, ir);
      if (held) for (int i = 0; i < kBlock; ++i) { il[i] = static_cast<int16_t>(il[i] >> 2); ir[i] = static_cast<int16_t>(ir[i] >> 2); }
      Render(0, il, ir, ol, orr, p, 0);
      w.Put(ol, orr, kBlock);
    }
    w.Close();
  }
  std::printf("listening: %d takes and the dry riff in %s\n", nt, dir);
}

// ---- 5. cycles ----
[[maybe_unused]] void Cycles(long blocks) {
  int16_t p[16];
  std::printf("cycles a block (simulator compute; the riff, knobs walking, filter and space walking, %ld blocks each):\n",
              blocks);
  uint64_t all_sum = 0;
  uint32_t all_n = 0, all_max = 0;
  for (int fx = 0; fx < 11; ++fx) {
    FX_petri.init(0);
    Riff riff;
    riff.Reset();
    int32_t knob[4] = { kDefaults[0], kDefaults[1], kDefaults[2], kDefaults[3] };
    int32_t filter = kFilterDefault, space = kSpaceDefault;
    uint64_t sum = 0;
    uint32_t mx = 0, n = 0;
    int16_t il[kBlock], ir[kBlock], ol[kBlock], orr[kBlock];
    for (long b = 0; b < blocks; ++b) {
      for (int k = 0; k < 3; ++k) {
        if (Rand() % 100 == 0) knob[k] = RandRange(0, 32767);
      }
      if (b == blocks / 2) knob[1] = 32767;                             // the second half at full activity
      if (Rand() % 200 == 0) filter = RandRange(0, 32767);
      if (Rand() % 200 == 0) space = RandRange(0, 32767);
      SetParams(p, knob, fx, filter, space, (b / 700) % 3 == 2, (b / 500) % 2 == 1);
      riff.Block(b, il, ir);
      uint32_t c;
      Render(0, il, ir, ol, orr, p, &c);
      if (b < 40) continue;                                              // the clear
      sum += c;
      ++n;
      if (c > mx) mx = c;
    }
    std::printf("  %-8s avg %6lu  max %6lu\n", kNames[fx], static_cast<unsigned long>(sum / (n ? n : 1)),
                static_cast<unsigned long>(mx));
    all_sum += sum;
    all_n += n;
    if (mx > all_max) all_max = mx;
  }
  std::printf("  all      avg %6lu  max %6lu\n", static_cast<unsigned long>(all_sum / (all_n ? all_n : 1)),
              static_cast<unsigned long>(all_max));
  {   // the clearing blocks after a restart
    FX_petri.init(0);
    int16_t il[kBlock] = { 0 }, ir[kBlock] = { 0 }, ol[kBlock], orr[kBlock];
    int32_t knob[4] = { kDefaults[0], kDefaults[1], kDefaults[2], kDefaults[3] };
    SetParams(p, knob, 0, kFilterDefault, kSpaceDefault, false, false);
    uint32_t mx = 0, c;
    for (int b = 0; b < 20; ++b) { Render(0, il, ir, ol, orr, p, &c); if (c > mx) mx = c; }
    std::printf("  clearing blocks: max %lu\n", static_cast<unsigned long>(mx));
  }
}

#ifdef __bfin__
// The voice kernel (src/voice_bfin.S) against its C reference on random frames, positions, rates and gains.
uint32_t g_rec[petri::kRecFrames + 1];
int KernelCheck(int cases) {
  long bad = 0;
  for (uint32_t i = 0; i <= static_cast<uint32_t>(petri::kRecFrames); ++i) g_rec[i] = (Rand() << 8) ^ Rand();
  for (int c = 0; c < cases; ++c) {
    int32_t a[2][32], b[2][32];
    for (int i = 0; i < 32; ++i) a[0][i] = b[0][i] = RandRange(-200000, 200000), a[1][i] = b[1][i] = RandRange(-200000, 200000);
    const uint32_t pos = (Rand() << 8) ^ Rand();
    const int32_t rate = RandRange(-4 * petri::kRateOne, 4 * petri::kRateOne);
    const int32_t g0l = RandRange(0, 32767), g0r = RandRange(0, 32767), g1l = RandRange(0, 32767), g1r = RandRange(0, 32767);
    const uint32_t g = (static_cast<uint32_t>(g0r) << 16) | static_cast<uint32_t>(g0l);
    const uint32_t d = (static_cast<uint32_t>((g1r - g0r) >> 5) << 16) | (static_cast<uint32_t>((g1l - g0l) >> 5) & 0xffff);
    PetriVoiceAsm(g_rec, pos, rate, a[0], a[1], g, d);
    PetriVoiceC(g_rec, pos, rate, b[0], b[1], g, d);
    for (int i = 0; i < 32; ++i) if (a[0][i] != b[0][i] || a[1][i] != b[1][i]) { ++bad; break; }
  }
  // the space kernel: two copies of a random buffer, random inputs, decay and states; outputs, buffer and states equal
  static int16_t za[petri::kSpaceSize], zb[petri::kSpaceSize];
  long bad_space = 0;
  for (int i = 0; i < petri::kSpaceSize; ++i) za[i] = zb[i] = static_cast<int16_t>(Rand());
  uint32_t lpa = Rand(), lpb = lpa;
  for (int c = 0; c < cases; ++c) {
    int16_t x[16];
    uint32_t ya[16], yb[16];
    const int big = c & 1;
    for (int i = 0; i < 16; ++i) x[i] = static_cast<int16_t>(big ? Rand() : RandRange(-3000, 3000));
    const uint32_t dk = static_cast<uint32_t>(RandRange(0, 32767));
    const int q = RandRange(16, petri::kSpaceSlide + 15);
    PetriSpaceAsm(za + q, x, ya, &lpa, (dk << 16) | dk);
    PetriSpaceC(zb + q, x, yb, &lpb, (dk << 16) | dk);
    bool same = lpa == lpb;
    for (int i = 0; i < 16; ++i) if (ya[i] != yb[i]) same = false;
    if (!same) ++bad_space;
  }
  for (int i = 0; i < petri::kSpaceSize; ++i) if (za[i] != zb[i]) { ++bad_space; break; }
  std::printf("space kernel: asm against C, %d random calls: %ld differ\n", cases, bad_space);
  bad += bad_space;
  std::printf("voice kernel: asm against C, %d random calls: %ld differ\n", cases, bad);
  return bad ? 1 : 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifdef __bfin__
  {
    uint32_t s;
    __asm__ __volatile__("%0 = SYSCFG;" : "=d"(s));
    s |= 2;   // CCEN: the cycle counter runs
    __asm__ __volatile__("SYSCFG = %0; CSYNC;" : : "d"(s));
  }
  const long cblocks = argc > 2 ? std::atol(argv[2]) : 1500;
  const int bad = KernelCheck(3000);
  Cycles(cblocks);
  std::printf("output digest %08lx%08lx\n", static_cast<unsigned long>(g_digest >> 32),
              static_cast<unsigned long>(g_digest & 0xffffffffu));
  std::printf(bad ? "petri (simulator): FAIL\n" : "petri (simulator): pass\n");
  return bad;
#else
  const char* dir = argc > 1 ? argv[1] : 0;
  const long blocks = argc > 2 ? std::atol(argv[2]) : 60000;
  std::printf("petri: sizeof(Instance) %lu, scratch %d\n", static_cast<unsigned long>(sizeof(coop_petri::Instance)),
              COOP_FX_SCRATCH_BYTES);
  int fail = Stress(blocks);
  fail |= Decay();
  fail |= Clicks(true);
  {
    int16_t p[16];
    int32_t knob[4] = { kDefaults[0], 26000, 20000, 20000 };
    long bad = 0;
    for (int fx = 0; fx < 11; ++fx) {
      SetParams(p, knob, fx, kFilterDefault, 16000, false, fx & 1);
      bad += coop_fx_scratch_check(&FX_petri, p, 1500);
    }
    std::printf("e8 scratch check (every effect, 1500 blocks each): %ld blocks differ\n", bad);
    if (bad) fail = 1;
  }
  if (dir) Listen(dir);
  std::printf("output digest %08lx%08lx\n", static_cast<unsigned long>(g_digest >> 32),
              static_cast<unsigned long>(g_digest & 0xffffffffu));
  std::printf(fail ? "petri: FAIL\n" : "petri: pass\n");
  return fail;
#endif
}
