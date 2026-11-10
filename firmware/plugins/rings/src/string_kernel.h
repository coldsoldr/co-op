// The string's per-sample loop without dispersion (StringFixed's common
// case: every sympathetic string), in C and in Blackfin assembly
// (string_kernel_bfin.S) with exactly the same arithmetic. Checked by
// tests/bench_string.cc (checksums of both on Blackfin, and of the C on the
// host).
//
// Per sample: Hermite read at the loop delay, plus the input; FIR damping
// (h0 x[n-1] + h1 (x[n] + x[n-2])); two TPT one-pole low-passes (G); clamp
// to +-7 (Q25); write; out += sample - linear read at the pickup delay.
//
// The delay line has kStringKernelGuard words after its kStringDelaySize
// samples that mirror its first ones, so a 4-sample read never wraps.

#ifndef OP1RINGS_STRING_KERNEL_H_
#define OP1RINGS_STRING_KERNEL_H_

#include <cstdint>

#include "src/fixed.h"

namespace op1rings {

const int32_t kStringKernelSize = 2048;   // kStringDelaySize
const int32_t kStringKernelGuard = 4;

// Layout shared with the assembly (offsets in bytes in the comments).
struct StringKernel {
  int32_t* line;              // 0: kStringKernelSize + kStringKernelGuard words
  int32_t write_ptr;          // 4: index
  int32_t loop_delay;         // 8: Q19
  int32_t loop_delay_step;    // 12
  int32_t comb_delay;         // 16: Q19
  int32_t comb_delay_step;    // 20
  int32_t h0;                 // 24: Q31
  int32_t h0_step;            // 28
  int32_t h1;                 // 32: Q31
  int32_t h1_step;            // 36
  int32_t g;                  // 40: Q31
  int32_t x1;                 // 44: FIR state, Q25
  int32_t x2;                 // 48
  int32_t s1;                 // 52: one-pole states, Q25
  int32_t s2;                 // 56
  // Dispersion's stretch allpass (StringKernelStretch only).
  int32_t* stretch_line;      // 60: kStretchKernelSize words
  int32_t stretch_ptr;        // 64: index
  int32_t stretch;            // 68: Q31, allpass share of the delay
  int32_t stretch_step;       // 72
  int32_t ap_gain;            // 76: Q31
  int32_t ap_gain_step;       // 80
  int32_t scratch;            // 84: the assembly's
};

const int32_t kStretchKernelSize = 1024;   // kStretchDelaySize

extern "C" void StringKernelAsm(StringKernel* k, const int32_t* in, int32_t* out, int32_t size);
extern "C" void StringKernelStretchAsm(StringKernel* k, const int32_t* in, int32_t* out,
                                       int32_t size);

// Writes s at index i, and in the guard if i is one of the first samples.
inline void StringKernelWrite(int32_t* line, int32_t i, int32_t s) {
  line[i] = s;
  if (i < kStringKernelGuard) line[i + kStringKernelSize] = s;
}

inline void StringKernelC(StringKernel* k, const int32_t* in, int32_t* out, int32_t size) {
  const int32_t kMask = kStringKernelSize - 1;
  const int32_t kWriteLimit = 7 << 25;
  int32_t* line = k->line;
  int32_t write_ptr = k->write_ptr;
  int32_t loop_delay = k->loop_delay;
  int32_t comb_delay = k->comb_delay;
  int32_t h0 = k->h0, h1 = k->h1;
  int32_t x1 = k->x1, x2 = k->x2, s1 = k->s1, s2 = k->s2;
  const int32_t g = k->g;
  for (int32_t i = 0; i < size; ++i) {
    loop_delay += k->loop_delay_step;
    comb_delay += k->comb_delay_step;

    // Hermite read (upstream DelayLine::ReadHermite).
    const int32_t* p = line + ((write_ptr + (loop_delay >> 19) - 1) & kMask);
    const int32_t f = static_cast<int32_t>(static_cast<uint32_t>(loop_delay << 13) >> 1);   // Q31
    const int32_t xm1 = p[0], x0 = p[1], x1r = p[2], x2r = p[3];
    const int32_t c = (x1r - xm1) >> 1;
    const int32_t v = x0 - x1r;
    const int32_t w = c + v;
    const int32_t a = w + v + ((x2r - x0) >> 1);
    const int32_t b_neg = w + a;
    int32_t r = MulQ31Fast(a, f) - b_neg;
    r = MulQ31Fast(r, f) + c;
    const int32_t s = MulQ31Fast(r, f) + x0 + in[i];

    // FIR, then the two one-poles, then the clamp.
    const int32_t y = MulQ31Fast(h0, x1) + MulQ31Fast(h1, s + x2);
    x2 = x1;
    x1 = s;
    h0 += k->h0_step;
    h1 += k->h1_step;
    const int32_t v1 = MulQ31Fast(y - s1, g);
    const int32_t lp1 = v1 + s1;
    s1 = lp1 + v1;
    const int32_t v2 = MulQ31Fast(lp1 - s2, g);
    int32_t lp = v2 + s2;
    if (lp > kWriteLimit) lp = kWriteLimit;
    if (lp < -kWriteLimit) lp = -kWriteLimit;
    s2 = lp + v2;

    StringKernelWrite(line, write_ptr, lp);
    write_ptr = (write_ptr - 1) & kMask;

    // Linear read at the pickup (upstream DelayLine::Read(float)).
    const int32_t* q = line + ((write_ptr + (comb_delay >> 19)) & kMask);
    const int32_t frac = static_cast<int32_t>(static_cast<uint32_t>(comb_delay << 13) >> 1);
    const int32_t aux = q[0] + MulQ31Fast(q[1] - q[0], frac);
    out[i] += lp - aux;
  }
  k->write_ptr = write_ptr;
  k->loop_delay = loop_delay;
  k->comb_delay = comb_delay;
  k->h0 = h0;
  k->h1 = h1;
  k->x1 = x1;
  k->x2 = x2;
  k->s1 = s1;
  k->s2 = s2;
}

// The string with dispersion, when only its stretch allpass is active
// (dispersion in [0, 0.75]: no delay noise, no curved bridge, no DC-blocked
// mix): StringFixed's dispersion loop with those terms at 0, bit-exact with
// it. The loop delay goes through upstream's delay_fm = 1, which in Q30
// clears its lowest bit.
inline void StringKernelStretchC(StringKernel* k, const int32_t* in, int32_t* out, int32_t size) {
  const int32_t kMask = kStringKernelSize - 1;
  const int32_t kStretchMask = kStretchKernelSize - 1;
  const int32_t kWriteLimit = 7 << 25;
  const int32_t kMinApDelay = 4 << 19;
  int32_t* line = k->line;
  int32_t* stretch_line = k->stretch_line;
  int32_t write_ptr = k->write_ptr;
  int32_t stretch_ptr = k->stretch_ptr;
  int32_t loop_delay = k->loop_delay;
  int32_t comb_delay = k->comb_delay;
  int32_t stretch = k->stretch, ap_gain = k->ap_gain;
  int32_t h0 = k->h0, h1 = k->h1;
  int32_t x1 = k->x1, x2 = k->x2, s1 = k->s1, s2 = k->s2;
  const int32_t g = k->g;
  for (int32_t i = 0; i < size; ++i) {
    loop_delay += k->loop_delay_step;
    comb_delay += k->comb_delay_step;
    stretch += k->stretch_step;
    ap_gain += k->ap_gain_step;
    const int32_t d = loop_delay & ~1;
    const int32_t ap_delay = MulQ31Fast(d, stretch);
    const int32_t main_delay = d - ap_delay;
    const int32_t read_delay = ap_delay >= kMinApDelay && main_delay >= kMinApDelay ? main_delay : d;

    // Hermite read.
    const int32_t* p = line + ((write_ptr + (read_delay >> 19) - 1) & kMask);
    const int32_t f = static_cast<int32_t>(static_cast<uint32_t>(read_delay << 13) >> 1);
    const int32_t xm1 = p[0], x0 = p[1], x1r = p[2], x2r = p[3];
    const int32_t c = (x1r - xm1) >> 1;
    const int32_t v = x0 - x1r;
    const int32_t w = c + v;
    const int32_t a = w + v + ((x2r - x0) >> 1);
    const int32_t b_neg = w + a;
    int32_t r = MulQ31Fast(a, f) - b_neg;
    r = MulQ31Fast(r, f) + c;
    int32_t s = MulQ31Fast(r, f) + x0;
    if (read_delay != d) {
      // The stretch allpass (stretch_.Allpass(s, (size_t) ap_delay, ap_gain)).
      const int32_t read = stretch_line[(stretch_ptr + (ap_delay >> 19)) & kStretchMask];
      const int32_t write = s + MulQ31Fast(read, ap_gain);
      stretch_line[stretch_ptr] = write;
      stretch_ptr = (stretch_ptr - 1) & kStretchMask;
      s = read - MulQ31Fast(write, ap_gain);
    }
    s += in[i];

    const int32_t y = MulQ31Fast(h0, x1) + MulQ31Fast(h1, s + x2);
    x2 = x1;
    x1 = s;
    h0 += k->h0_step;
    h1 += k->h1_step;
    const int32_t v1 = MulQ31Fast(y - s1, g);
    const int32_t lp1 = v1 + s1;
    s1 = lp1 + v1;
    const int32_t v2 = MulQ31Fast(lp1 - s2, g);
    int32_t lp = v2 + s2;
    if (lp > kWriteLimit) lp = kWriteLimit;
    if (lp < -kWriteLimit) lp = -kWriteLimit;
    s2 = lp + v2;

    StringKernelWrite(line, write_ptr, lp);
    write_ptr = (write_ptr - 1) & kMask;

    const int32_t* q = line + ((write_ptr + (comb_delay >> 19)) & kMask);
    const int32_t frac = static_cast<int32_t>(static_cast<uint32_t>(comb_delay << 13) >> 1);
    out[i] += lp - (q[0] + MulQ31Fast(q[1] - q[0], frac));
  }
  k->write_ptr = write_ptr;
  k->stretch_ptr = stretch_ptr;
  k->loop_delay = loop_delay;
  k->comb_delay = comb_delay;
  k->stretch = stretch;
  k->ap_gain = ap_gain;
  k->h0 = h0;
  k->h1 = h1;
  k->x1 = x1;
  k->x2 = x2;
  k->s1 = s1;
  k->s2 = s2;
}

inline void StringKernelStretchProcess(StringKernel* k, const int32_t* in, int32_t* out,
                                       int32_t size) {
#if defined(__bfin__) && !defined(OP1RINGS_NO_ASM)
  StringKernelStretchAsm(k, in, out, size);
#else
  StringKernelStretchC(k, in, out, size);
#endif
}

inline void StringKernelProcess(StringKernel* k, const int32_t* in, int32_t* out, int32_t size) {
#if defined(__bfin__) && !defined(OP1RINGS_NO_ASM)
  StringKernelAsm(k, in, out, size);
#else
  StringKernelC(k, in, out, size);
#endif
}

}  // namespace op1rings

#endif  // OP1RINGS_STRING_KERNEL_H_
