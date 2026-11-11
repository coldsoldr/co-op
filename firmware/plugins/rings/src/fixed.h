// Fixed-point primitives. On Blackfin these map to the GCC fract builtins
// (a 32x32 fractional multiply is ~5 cycles; software float is ~330 cycles
// per add or multiply, see tests/bench_ops.cc). On the host they're
// emulated bit-exactly, so host renders predict the device output.

#ifndef OP1RINGS_FIXED_H_
#define OP1RINGS_FIXED_H_

#include <cstdint>

namespace op1rings {

// Q formats used in the port:
//   Q27 (s4.27, range +-16): audio signals and filter states.
//   Q31 (s0.31, range +-1):  coefficients and gains.
static const int32_t kQ27One = 1 << 27;

// (The float conversions of the te-op1-cfw tests are not in the coop copy.)

// (a * b) >> 31, as computed by __builtin_bfin_mult_fr1x32x32: three partial
// products, the low x low one truncated to its top 16 bits, the result
// saturated (only -1 * -1 overflows). Portable version, used on the host and
// by the Blackfin unit test.
inline int32_t MulQ31Reference(int32_t a, int32_t b) {
  int32_t a_hi = a >> 16, b_hi = b >> 16;
  uint32_t a_lo = static_cast<uint32_t>(a) & 0xffff;
  uint32_t b_lo = static_cast<uint32_t>(b) & 0xffff;
  // A1 = (a_lo * b_lo) >> 16 (unsigned); A1 += a_hi * b_lo + b_hi * a_lo;
  // A1 >>>= 15; A0 = (a_hi * b_hi) << 1; result = sat32(A0 + A1).
  int64_t a1 = static_cast<int64_t>((a_lo * b_lo) >> 16);
  a1 += static_cast<int64_t>(a_hi) * static_cast<int64_t>(b_lo);
  a1 += static_cast<int64_t>(b_hi) * static_cast<int64_t>(a_lo);
  a1 >>= 15;
  int64_t a0 = (static_cast<int64_t>(a_hi) * static_cast<int64_t>(b_hi)) * 2;
  int64_t r = a0 + a1;
  if (r > INT32_MAX) r = INT32_MAX;
  if (r < INT32_MIN) r = INT32_MIN;
  return static_cast<int32_t>(r);
}

inline int32_t MulQ31(int32_t a, int32_t b) {
#ifdef __bfin__
  return __builtin_bfin_mult_fr1x32x32(a, b);
#else
  return MulQ31Reference(a, b);
#endif
}

// (a * b) >> 31 without the low x low partial product (error < 2 LSB). This
// is the 4-instruction sequence used in the assembly kernels:
//   A0 = a.H * b.H, A1 = a.H * b.L (M);  A1 += b.H * a.L (M);
//   A1 = A1 >>> 15;  r = (A0 += A1);
inline int32_t MulQ31FastReference(int32_t a, int32_t b) {
  int64_t a_hi = a >> 16, b_hi = b >> 16;
  int64_t a_lo = static_cast<uint32_t>(a) & 0xffff;
  int64_t b_lo = static_cast<uint32_t>(b) & 0xffff;
  int64_t a1 = (a_hi * b_lo + b_hi * a_lo) >> 15;
  int64_t r = a_hi * b_hi * 2 + a1;
  if (r > INT32_MAX) r = INT32_MAX;
  if (r < INT32_MIN) r = INT32_MIN;
  return static_cast<int32_t>(r);
}

inline int32_t MulQ31Fast(int32_t a, int32_t b) {
#ifdef __bfin__
  int32_t r;
  __asm__("A0 = %1.H * %2.H, A1 = %1.H * %2.L (M);\n\t"
          "A1 += %2.H * %1.L (M);\n\t"
          "A1 = A1 >>> 15;\n\t"
          "%0 = (A0 += A1);"
          : "=d"(r) : "d"(a), "d"(b) : "A0", "A1");
  return r;
#else
  return MulQ31FastReference(a, b);
#endif
}

// (a * w) >> 15 for a 16-bit w (the top half of a Q31 weight):
//   A0 = w * a.H, A1 = w * a.L (M);  A1 = A1 >>> 15;  r = (A0 += A1);
inline int32_t MulQ31ByQ15(int32_t a, int16_t w) {
  int64_t a_hi = a >> 16;
  int64_t a_lo = static_cast<uint32_t>(a) & 0xffff;
  int64_t r = a_hi * w * 2 + ((a_lo * w) >> 15);
  if (r > INT32_MAX) r = INT32_MAX;
  if (r < INT32_MIN) r = INT32_MIN;
  return static_cast<int32_t>(r);
}

inline int32_t AddSat(int32_t a, int32_t b) {
#ifdef __bfin__
  return __builtin_bfin_add_fr1x32(a, b);
#endif
  int64_t r = static_cast<int64_t>(a) + b;
  if (r > INT32_MAX) r = INT32_MAX;
  if (r < INT32_MIN) r = INT32_MIN;
  return static_cast<int32_t>(r);
}

// Saturating a << shift. Don't use __builtin_bfin_shl_fr1x32 here: with a
// constant shift, GCC 14 emits a 16-bit shift (0x0100c3d6 << 1 gives
// 0xffff87ac; found by tests/dump_modal.cc).
inline int32_t ShlSat(int32_t a, int shift) {
#ifdef __bfin__
  // A constant shift is one instruction, `<< n (S)`, written out by hand
  // (the generic code below compiles to a branch that's taken in the usual
  // case, ~10 cycles).
  if (__builtin_constant_p(shift) && shift > 0 && shift < 32) {
    int32_t r;
    __asm__("%0 = %1 << %2 (S);" : "=d"(r) : "d"(a), "i"(shift));
    return r;
  }
#endif
  // No overflow iff a has at least `shift` redundant sign bits (SIGNBITS).
  if (__builtin_clrsb(a) >= shift) return a << shift;
  return a < 0 ? INT32_MIN : INT32_MAX;
}

}  // namespace op1rings

#endif  // OP1RINGS_FIXED_H_
