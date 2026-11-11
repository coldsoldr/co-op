// tape_drive: tape saturation, wobble (wow and flutter) and age as a coop effect. A port of te-op1-cfw's kennel
// effect tape_drive 0.3 (ports/knl/tape_drive/tape_knl.cc, coldsoldr; device-tested on kennel, te-op1-cfw
// docs/device-tests.md Test 57 / 61). The DSP is te-op1-cfw's ports/fx/src/tape (src/), unchanged but for its include
// paths and one initialiser; this file is the coop glue that replaces tape_knl.cc's kennel glue.
//
// Controls (README.md has the action table):
//   turns     blue saturate, green wobble, white age (clockwise = a newer, brighter tape: the core's age is the word
//             mirrored, as upstream), orange mix. params[0..3] arrive in the filter type's range units
//             (sdk/coop_plugin.h); Q15() maps them to 0..32767 with kennel's op1_fx_param_q15 formula, so the DSP
//             sees exactly the numbers it saw on kennel.
//   fn+blue   gain (word 5, Q15; 16384 = 0 dB, about -6..+6 dB), shift + fn + blue its fine (plugin.json "fine").
//             gain_oct = (word5 - 16384) * 4: Q16 octaves added to the saturation drive's exponent only, so the
//             level make-up and the dry side of the mix don't see it (at mix 0 nothing changes).
//
// Stereo: upstream's stereo path (TapeSat::Process(l, r)), at every position: one shared wobble, a lane (halfband
// up, quintic saturation, halfband down, head bump, HF loss, mix) per channel. coop's buses are int32 (the int16 bus
// widened by 16 bits); the DSP is int16. Each block's high halves go through the DSP, and the result keeps the
// input's low halves: out = wet16 << 16 | (in & 0xffff). The high half is bit-identical to upstream, and at mix 0 the
// whole int32 sample is the input, bit for bit (upstream's promise at 16 bits, kept at 32).
//
// State: one TapeSat (about 3.1 KB) in .bss; each of coop's five relocations has its own. init resets it (about
// 1.5 k cycles: the 2 KB wobble ring is cleared).
//
// Replaced from the kennel build: knl::Instance (the position's scratch buffer) -> .bss; op1_fx_param_q15 (the
// stock range table at 0x01DE5C50) -> Q15() over the ranges coop documents; knl::Fine (the shift byte at
// 0x01dd7f78, rewriting the stored word 5 from the render) -> the core's "fine" pairing; the screen (op1_fx_knobs,
// op1_fx_page, op1_fx_screen_pos, op1_fx_knobs_live, op1_screen_draw) -> not ported: effect screens aren't built in
// coop yet (README "Screen").
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#include <stdint.h>

#include "coop_plugin.h"
#include "src/tape.h"

namespace {

op1fx::TapeSat g_tape;                   // trivially constructible (no .ctors): Init() sets every field
alignas(4) int16_t g_l[op1fx::kTapeMaxBlock];
alignas(4) int16_t g_r[op1fx::kTapeMaxBlock];

inline int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }

// A turn's word in the filter type's range {lo..hi} -> 0..32767, as kennel's op1_fx_param_q15 (four divisions a
// block, outside the per-sample loop).
inline int32_t Q15(int32_t v, int32_t lo, int32_t hi) { return Clamp15((v - lo) * 32767 / (hi - lo)); }

// tape_knl.cc Params(), with coop's words.
op1fx::TapeParams Params(const int16_t* p) {
  op1fx::TapeParams q;
  q.saturate = Q15(p[0], 64, 14528);
  q.wobble = Q15(p[1], 0, 24576);
  q.age = 32767 - Q15(p[2], 0, 22528);      // clockwise = a newer tape
  q.mix = Q15(p[3], 8192, 32767);
  q.gain_oct = (Clamp15(p[5]) - 16384) * 4;  // Q16 octaves, -1..1 (about -6..+6 dB)
  return q;
}

int init(int slot) {
  (void)slot;
  g_tape.Init();
  return 0;
}

void render(int slot, int32_t* l, int32_t* r, const int16_t* params, int n) {
  (void)slot;
  if (n <= 0 || n > op1fx::kTapeMaxBlock || (n & 1)) return;   // the DSP takes n even, 2..64 (coop: always 64)
  const op1fx::TapeParams q = Params(params);                   // read once, for the whole block
  for (int i = 0; i < n; ++i) {
    g_l[i] = static_cast<int16_t>(l[i] >> 16);
    g_r[i] = static_cast<int16_t>(r[i] >> 16);
  }
  g_tape.Process(g_l, g_r, n, q);
  for (int i = 0; i < n; ++i) {
    l[i] = static_cast<int32_t>((static_cast<uint32_t>(static_cast<uint16_t>(g_l[i])) << 16) |
                                (static_cast<uint32_t>(l[i]) & 0xffffu));
    r[i] = static_cast<int32_t>((static_cast<uint32_t>(static_cast<uint16_t>(g_r[i])) << 16) |
                                (static_cast<uint32_t>(r[i]) & 0xffffu));
  }
}

}  // namespace

extern "C" const coop_fx_t FX_tape_drive = { COOP_FX_MAGIC, COOP_PLUGIN_API, 0, init, render };
