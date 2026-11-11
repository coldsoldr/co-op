// FFloat: a small software floating-point type for control-rate code.
//
// The BF524 has no FPU and libgcc's soft-float costs ~330 cycles per add or
// multiply (tests/bench_ops.cc). FFloat keeps a normalised 32-bit mantissa
// and a separate exponent and uses the 32x32 fract multiply (~5 cycles) and
// SIGNBITS (via __builtin_clrsb), so an operation costs tens of cycles. It
// has no IEEE rounding, infinities or NaNs, and results are truncated, but
// its mantissa (31 bits) is wider than float's (24 bits).
//
// Everything is integer arithmetic, so host and Blackfin results are
// bit-identical (checked by tests/test_ffloat.cc).
//
// value = m * 2^e, with m normalised: for m != 0, clrsb(m) == 0, i.e.
// |m| in [2^30, 2^31). Zero is m == 0 (e is then ignored).

#ifndef OP1RINGS_FFLOAT_H_
#define OP1RINGS_FFLOAT_H_

#include <cstdint>

#include "src/fixed.h"

namespace op1rings {

class FFloat {
 public:
  constexpr FFloat() : m_(0), e_(kZeroExponent) { }

  // A constant from its parts (value = m 2^e, m normalised or 0): the coop
  // build's literals (defloat.py made them from upstream's FFloat(double)
  // constructor, which a coop plugin file can't carry).
  static constexpr FFloat Raw(int32_t m, int32_t e) { return FFloat(m, e, 0); }

  static FFloat FromParts(int32_t m, int32_t e) {
    FFloat r;
    r.SetNormalized(m, e);
    return r;
  }

  // From an integer, or from a fixed-point value with `fraction_bits`.
  static FFloat FromInt(int32_t x) { return FromParts(x, 0); }
  static FFloat FromQ(int32_t x, int32_t fraction_bits) {
    return FromParts(x, -fraction_bits);
  }

  // To fixed point with `fraction_bits` (saturating, truncating).
  int32_t ToQ(int32_t fraction_bits) const {
    if (m_ == 0) return 0;
    int32_t shift = e_ + fraction_bits;
    if (shift >= 0) {
      if (shift >= 1) return m_ < 0 ? INT32_MIN : INT32_MAX;
      return m_;
    }
    if (shift <= -32) return m_ < 0 ? -1 : 0;
    return m_ >> -shift;
  }
  int32_t ToQ31() const { return ToQ(31); }
  int32_t ToQ27() const { return ToQ(27); }

  // Floor to integer (saturating).
  int32_t Floor() const { return ToQ(0); }

  int32_t mantissa() const { return m_; }
  int32_t exponent() const { return e_; }
  bool is_zero() const { return m_ == 0; }
  bool is_negative() const { return m_ < 0; }


  friend FFloat operator*(FFloat a, FFloat b) {
    if (a.m_ == 0 || b.m_ == 0) return FFloat();
    // |a.m * b.m| >= 2^60, so the Q31 product is >= 2^29: one or two bits
    // of renormalisation.
    return FromParts(MulQ31Fast(a.m_, b.m_), a.e_ + b.e_ + 31);
  }

  friend FFloat operator+(FFloat a, FFloat b) {
    if (a.m_ == 0) return b;
    if (b.m_ == 0) return a;
    if (a.e_ < b.e_) { FFloat t = a; a = b; b = t; }
    int32_t d = a.e_ - b.e_;
    if (d >= 31) return a;
    // Halve both first so the sum can't overflow.
    int32_t s = (a.m_ >> 1) + (b.m_ >> (d + 1));
    return FromParts(s, a.e_ + 1);
  }

  friend FFloat operator-(FFloat a) {
    FFloat r;
    if (a.m_ == 0) return r;
    if (a.m_ == INT32_MIN) {
      r.m_ = 1 << 30;
      r.e_ = a.e_ + 1;
    } else {
      r.m_ = -a.m_;
      r.e_ = a.e_;
      if (__builtin_clrsb(r.m_) != 0) r.SetNormalized(r.m_, r.e_);
    }
    return r;
  }

  friend FFloat operator-(FFloat a, FFloat b) { return a + (-b); }
  friend FFloat operator/(FFloat a, FFloat b) { return a * Reciprocal(b); }

  FFloat& operator+=(FFloat b) { return *this = *this + b; }
  FFloat& operator-=(FFloat b) { return *this = *this - b; }
  FFloat& operator*=(FFloat b) { return *this = *this * b; }
  FFloat& operator/=(FFloat b) { return *this = *this / b; }

  friend bool operator<(FFloat a, FFloat b) { return (a - b).m_ < 0; }
  friend bool operator>(FFloat a, FFloat b) { return (b - a).m_ < 0; }
  friend bool operator<=(FFloat a, FFloat b) { return !(b < a); }
  friend bool operator>=(FFloat a, FFloat b) { return !(a < b); }
  friend bool operator==(FFloat a, FFloat b) {
    return a.m_ == b.m_ && (a.m_ == 0 || a.e_ == b.e_);
  }
  friend bool operator!=(FFloat a, FFloat b) { return !(a == b); }

  // Multiply or divide by 2^n exactly.
  FFloat Scaled(int32_t n) const {
    FFloat r = *this;
    if (r.m_ != 0) r.e_ += n;
    return r;
  }

  // 1 / b (b != 0). Newton-Raphson on the mantissa in Q30.
  static FFloat Reciprocal(FFloat b) {
    if (b.m_ == 0) return FFloat::FromParts(INT32_MAX, 1000);  // "huge"
    bool negative = b.m_ < 0;
    int32_t d = negative ? (b.m_ == INT32_MIN ? INT32_MAX : -b.m_) : b.m_;
    // d / 2^31 = x in [0.5, 1); y ~ 1/x in (1, 2], Q30.
    // y0 = 32/17 (1.5 - x) (= 48/17 - 32/17 x, max relative error 1/17).
    const int32_t k32_17 = 2021161080;                      // 32/17, Q30
    int32_t t = (3 << 29) - (d >> 1);                       // 1.5 - x, Q30
    int32_t y = ShlSat(MulQ31Fast(k32_17, t), 1);               // Q30
    for (int i = 0; i < 3; ++i) {
      int32_t xy = MulQ31Fast(d, y);                            // Q30, ~1
      int32_t two_minus = static_cast<int32_t>(0x80000000u - static_cast<uint32_t>(xy));
      y = ShlSat(MulQ31Fast(y, two_minus), 1);                  // Q30
    }
    // b = x 2^(31 + e) -> 1/b = (y / 2^30) 2^(-31 - e).
    FFloat r = FromParts(y, -61 - b.e_);
    return negative ? -r : r;
  }

  // 1 / sqrt(b) for b > 0 (returns 0 for b <= 0).
  static FFloat ReciprocalSqrt(FFloat b) {
    if (b.m_ <= 0) return FFloat();
    // Make (31 + e) even so the exponent halves exactly; x in [0.25, 1).
    int32_t m = b.m_, e = b.e_;
    if (((31 + e) & 1) != 0) { m >>= 1; ++e; }
    // Initial guess from the top 4 bits of x (x in [0.25, 1): index 4..15).
    static const int32_t kGuess[16] = {
      0, 0, 0, 0,
      2024667000, 1831380208, 1684624773, 1568300315,   // 1/sqrt((i+0.5)/16), Q30
      1473161629, 1393471397, 1325455684, 1266516759,
      1214800200, 1168942037, 1127913670, 1090922784,
    };
    int32_t y = kGuess[(m >> 27) & 15];                 // Q30
    for (int i = 0; i < 3; ++i) {
      int32_t y2 = MulQ31Fast(y, y);                        // Q29
      int32_t xy2 = MulQ31Fast(m, y2);                      // Q29
      int32_t t = (3 << 29) - xy2;                      // Q29
      y = ShlSat(MulQ31Fast(y, t), 1);                      // Q30 (y t / 2)
    }
    // b = x 2^(31 + e) -> 1/sqrt(b) = (y / 2^30) 2^(-(31 + e) / 2).
    return FromParts(y, -30 - (31 + e) / 2);
  }

  static FFloat Sqrt(FFloat b) {
    if (b.m_ <= 0) return FFloat();
    return b * ReciprocalSqrt(b);
  }

  static FFloat Abs(FFloat a) { return a.m_ < 0 ? -a : a; }
  static FFloat Min(FFloat a, FFloat b) { return b < a ? b : a; }
  static FFloat Max(FFloat a, FFloat b) { return a < b ? b : a; }

 private:
  constexpr FFloat(int32_t m, int32_t e, int) : m_(m), e_(e) { }
  static const int32_t kZeroExponent = -100000;

  void SetNormalized(int32_t m, int32_t e) {
    if (m == 0) {
      m_ = 0;
      e_ = kZeroExponent;
      return;
    }
    int32_t n = __builtin_clrsb(m);
    m_ = m << n;
    e_ = e - n;
  }

  int32_t m_;
  int32_t e_;
};

}  // namespace op1rings

#endif  // OP1RINGS_FFLOAT_H_
