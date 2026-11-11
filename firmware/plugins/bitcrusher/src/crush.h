// CRUSH: sample-rate and bit-depth reduction, in fixed point at the effect rate (44.1 kHz, 64 samples a call). No
// anti-aliasing: the aliasing is the sound. From te-op1-cfw's ports/fx/src/crush/crush.h (coldsoldr, the core the
// kennel bitcrusher plugin wrapped, te-op1-cfw 5aa314b), with the input gain of ports/knl/common/knl_fx.h (GainQ14).
//
// - Rate: a 32-bit phase accumulator; when it wraps, the input is sampled and held. rate 0..32767 maps
//   44.1 kHz -> 500 Hz exponentially (44100 x (500 / 44100)^r: 6.46 octaves down).
// - Bits: the held sample is rounded to 16 -> 2 bits.
// - Jitter: each new hold restarts the phase at a random fraction of a period, so the holds come early by up to
//   jitter x one period.
// - Gain: into the crusher only (-6..+6 dB); the dry side of the mix is the signal as it came.
// - Mix: dry/wet, linear.
// Stereo shares one clock; each channel holds its own sample.
//
// What changed for coop: the render works on coop's buffers (int32 planar, the int16 bus widened by 16 bits, both
// channels always: there is no mono position in coop). The crusher reads the top 16 bits, as upstream read the int16
// sample; the dry side keeps the low bits it came with (dry + the int16 mix delta << 16), so with the host's widened
// int16 the output is upstream's sample << 16, bit for bit (tests/host/bitcrusher). Upstream's int16 / mono-only
// Process and kennel's page and shift handling are gone.
//
// Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.

#ifndef COOP_BITCRUSHER_CRUSH_H_
#define COOP_BITCRUSHER_CRUSH_H_

#include <stdint.h>

namespace crush {

struct Params {
  int32_t rate;       // 0..32767: the amount of rate reduction (0 = 44.1 kHz)
  int32_t bits;       // 0..32767: 16 bits -> 2
  int32_t jitter;     // 0..32767
  int32_t gain_q14;   // input gain into the crusher, Q14 (16384 = 0 dB)
  int32_t mix;        // 0..32767
};

// 2^(-i/16) in Q31, i = 0..16 (2^31 x 2^(-i/16), rounded).
const uint32_t kExp2NegQ31[17] = {
  2147483648u, 2056437387u, 1969251188u, 1885761398u, 1805811301u, 1729250827u, 1655936265u,
  1585730000u, 1518500250u, 1454120821u, 1392470869u, 1333434672u, 1276901417u, 1222764986u,
  1170923762u, 1121280436u, 1073741824u,
};

// 2^(-x) x 2^32, x in Q16 (0..32 octaves): the phase increment of a rate x octaves under the effect rate. Per block.
inline uint32_t Exp2NegQ16(int32_t x) {
  const int32_t octaves = x >> 16;
  if (octaves >= 31) return 1;
  const int32_t frac = x & 0xffff;   // Q16: 16 steps of 4096
  const int32_t i = frac >> 12;
  const uint32_t a = kExp2NegQ31[i], b = kExp2NegQ31[i + 1];
  const uint32_t m = a - static_cast<uint32_t>((static_cast<uint64_t>(a - b) * (frac & 0xfff)) >> 12);
  // m is 2^(-frac) in Q31 (2^30 < m <= 2^31): x 2 for Q32, then the octaves.
  const uint64_t q32 = static_cast<uint64_t>(m) << 1;
  const uint64_t inc = q32 >> octaves;
  return inc > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(inc);
}

// An input gain word as a Q14 factor: -6 dB at 0, 0 dB at the centre, +6 dB at the top (10^(dB / 20) x 16384, one
// table entry per dB, interpolated). te-op1-cfw knl_fx.h GainQ14. Per block (two divisions).
const int32_t kGainQ14[13] = { 8211, 9215, 10338, 11600, 13016, 14605, 16384, 18382, 20626, 23143, 25966, 29135, 32690 };
inline int32_t GainQ14(int32_t word) {
  word = word < 0 ? 0 : (word > 32767 ? 32767 : word);
  const int32_t pos = word * 12;   // 0 .. 12 x 32767
  const int32_t i = pos / 32767;
  const int32_t frac = pos - i * 32767;
  if (i >= 12) return kGainQ14[12];
  return kGainQ14[i] + (((kGainQ14[i + 1] - kGainQ14[i]) * frac) / 32767);
}

// The state: plain data (no constructor: it lives in .bss, guidelines §2.1), set up by Init.
struct Crush {
  uint32_t phase;
  int32_t held[2];
  uint32_t rng;

  void Init() {
    phase = 0;
    held[0] = held[1] = 0;
    rng = 0x12345678u;
  }

  static inline int32_t Gain(int32_t x, int32_t g_q14) {
    const int32_t y = (x * g_q14) >> 14;
    return y > 32767 ? 32767 : y < -32768 ? -32768 : y;
  }

  static inline int32_t Quantise(int32_t x, int32_t mask, int32_t half) {
    int32_t q = x + half;
    if (q > 32767) q = 32767;
    return q & mask;
  }

  // dry32 + the upstream int16 mix delta << 16. |wet - dry| < 2^16, mix <= 2^15: the product fits 32 bits; the
  // result lies between dry and wet, so it stays inside int32.
  static inline int32_t Mix(int32_t dry32, int32_t wet, int32_t mix) {
    const int32_t dry = dry32 >> 16;
    return dry32 + static_cast<int32_t>(static_cast<uint32_t>(((wet - dry) * mix) >> 15) << 16);
  }

  // In place, both channels, n samples.
  void Process(int32_t* l, int32_t* r, int32_t n, const Params& p) {
    // 6.462 octaves (log2(44100 / 500)) in Q16 over the knob: 423 501 / 32767.
    const int32_t octaves_q16 = static_cast<int32_t>((static_cast<int64_t>(p.rate) * 423501) >> 15);
    const uint32_t inc = Exp2NegQ16(octaves_q16);
    const int32_t bits = 16 - (p.bits * 14 + 16383) / 32767;   // 16..2
    const int32_t drop = 16 - bits;
    const int32_t mask = drop ? ~((1 << drop) - 1) : ~0;
    const int32_t half = drop ? 1 << (drop - 1) : 0;
    const uint32_t jitter = static_cast<uint32_t>(p.jitter) << 1;   // Q16
    const int32_t mix = p.mix >= 32767 ? 32768 : p.mix;             // Q15, the top exactly wet
    const int32_t g = p.gain_q14;
    const bool every = inc == 0xffffffffu;                           // 44.1 kHz: a hold every sample
    uint32_t ph = phase, rn = rng;
    int32_t hl = held[0], hr = held[1];
    for (int32_t i = 0; i < n; ++i) {
      const int32_t xl = l[i], xr = r[i];
      const uint32_t next = ph + inc;
      if (next < ph || every) {
        hl = Quantise(Gain(xl >> 16, g), mask, half);
        hr = Quantise(Gain(xr >> 16, g), mask, half);
        rn = rn * 1664525u + 1013904223u;
        // A random fraction of a period, x jitter: the next hold comes early.
        ph = next + ((((rn >> 16) * jitter) >> 16) << 16);
      } else {
        ph = next;
      }
      l[i] = Mix(xl, hl, mix);
      r[i] = Mix(xr, hr, mix);
    }
    phase = ph;
    rng = rn;
    held[0] = hl;
    held[1] = hr;
  }
};

}  // namespace crush

#endif  // COOP_BITCRUSHER_CRUSH_H_
