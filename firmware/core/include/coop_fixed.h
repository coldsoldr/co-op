/* coop-fw fixed-point helpers (Q15). No FPU, no libc. Shared by every DSP stage; checked on the host
 * against a reference (tests/host/fixed_test.cc). Copyright (C) 2026 coldsoldr. GPL-3.0-or-later. */
#ifndef COOP_FIXED_H
#define COOP_FIXED_H
#include <stdint.h>

/* saturate a 32-bit value to int16 */
static inline int16_t coop_sat16(int32_t v)
{
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v);
}

/* Q15 multiply of two int16 samples/gains -> int16 (rounded, saturated). 0x7fff ~= 1.0. */
static inline int16_t coop_mul_q15(int16_t a, int16_t b)
{
    int32_t p = ((int32_t)a * (int32_t)b + (1 << 14)) >> 15;
    return coop_sat16(p);
}

/* saturating int16 add */
static inline int16_t coop_add16(int16_t a, int16_t b)
{
    return coop_sat16((int32_t)a + (int32_t)b);
}

/* apply a Q15 gain to a block in place */
static inline void coop_gain_block(int16_t *buf, int16_t gain, int n)
{
    for (int i = 0; i < n; i++) buf[i] = coop_mul_q15(buf[i], gain);
}

/* ---- Q31 (the master pair is int32/Q31) ---- */

static inline int32_t coop_sat32(int64_t v)
{
    return v > 2147483647LL ? 2147483647 : (v < -2147483648LL ? (int32_t)-2147483648LL : (int32_t)v);
}

/* ---- 1.31 fractional multiply: the hot-path primitive ----
 * coop_mul_fr(a,b) = (a*b) >> 31 for two Q31 operands, truncated toward -inf, saturated (only
 * (-1)*(-1) overflows, to +0x7fffffff). On the Blackfin it is the hardware fractional MAC
 * (__builtin_bfin_mult_fr1x32x32: ~6 instructions, no call), which is why the DSP uses it instead of
 * a 64-bit multiply. The host build uses a bit-identical int64 reference so the host tests validate
 * exactly what runs on the device. Verified against the reference on the bfin simulator
 * (tests/sim/fr_check). */
static inline int32_t coop_mul_fr(int32_t a, int32_t b)
{
#ifdef __bfin__
    return __builtin_bfin_mult_fr1x32x32(a, b);
#else
    int64_t r = ((int64_t)a * (int64_t)b) >> 31;        /* arithmetic (floor), like the MAC >>> */
    return r > 2147483647LL ? 2147483647 : (int32_t)r;  /* only INT_MIN*INT_MIN reaches +2^31 */
#endif
}

/* Q31 multiply of two int32 (truncated, saturated). 0x7fffffff ~= 1.0. */
static inline int32_t coop_mul_q31(int32_t a, int32_t b)
{
    return coop_mul_fr(a, b);
}

/* multiply a Q31 sample by a Q(frac) gain given as int32 with `frac` fractional bits (so gain can be
 * > 1.0 without overflowing the operand). Uses the fractional MAC and rescales by the 31-frac extra
 * fractional bits; `frac` must be in [1,31]. The low 31-frac bits of the result are zero (negligible
 * at these levels); gain magnitude must stay < 2^(31-frac) (e.g. < 2048x for frac=20). */
static inline int32_t coop_mul_qn(int32_t sample, int32_t gain, int frac)
{
    return coop_sat32((int64_t)coop_mul_fr(sample, gain) << (31 - frac));
}

static inline int32_t coop_add32(int32_t a, int32_t b)
{
    return coop_sat32((int64_t)a + (int64_t)b);
}

static inline int32_t coop_abs32(int32_t a)
{
    return a < 0 ? (a == (int32_t)-2147483648LL ? 2147483647 : -a) : a;
}

/* ---- stock-exact 16-bit fractional ops (Phase 2 milestone C, docs/stereo-bus.md) ----
 * The stock L1 voice/mix loops use the Blackfin 16-bit fractional ALU in its default mode:
 * `Rx.L = Ra.L * Rb.L` (signed 1.15 x 1.15, round to nearest, saturate), `Rx.L = Ra.L + Rb.L (S)` and
 * `Rx.H = Ra.L << 1 (S)`. The coop stages that stand in for stock code must reproduce them bit for
 * bit, so on the device they ARE those instructions (inline asm; the gcc builtins emit the truncating
 * (T) multiply, which differs). The host model is the biased-rounding default (RND_MOD = 0):
 * (a*b + 0x4000) >> 15; the only saturating case is (-1)*(-1). */
static inline int16_t coop_fr16_mul(int16_t a, int16_t b)
{
#ifdef __bfin__
    int32_t r;
    __asm__("%0.L = %1.L * %2.L;\n\t%0 = %0.L (X);" : "=&d"(r) : "d"(a), "d"(b));
    return (int16_t)r;
#else
    int32_t p = (int32_t)a * (int32_t)b;
    if (p == 0x40000000) return 32767;             /* -1 * -1 saturates */
    return (int16_t)((p + 0x4000) >> 15);          /* biased round to nearest (floor shift) */
#endif
}

static inline int16_t coop_fr16_add(int16_t a, int16_t b)   /* Rx.L = Ra.L + Rb.L (S) */
{
#ifdef __bfin__
    int32_t r;
    __asm__("%0.L = %1.L + %2.L (S);\n\t%0 = %0.L (X);" : "=&d"(r) : "d"(a), "d"(b));
    return (int16_t)r;
#else
    return coop_sat16((int32_t)a + (int32_t)b);
#endif
}

static inline int16_t coop_fr16_shl1(int16_t a)             /* Rx.H = Ra.L << 1 (S) */
{
#ifdef __bfin__
    int32_t r;
    __asm__("%0.L = %1.L << 1 (S);\n\t%0 = %0.L (X);" : "=&d"(r) : "d"(a));
    return (int16_t)r;
#else
    return coop_sat16((int32_t)a << 1);
#endif
}

/* 32-bit saturating add/sub: `R = Ra + Rb (S)`. */
static inline int32_t coop_sadd32(int32_t a, int32_t b)
{
#ifdef __bfin__
    return __builtin_bfin_add_fr1x32(a, b);
#else
    return coop_sat32((int64_t)a + (int64_t)b);
#endif
}
static inline int32_t coop_ssub32(int32_t a, int32_t b)
{
#ifdef __bfin__
    return __builtin_bfin_sub_fr1x32(a, b);
#else
    return coop_sat32((int64_t)a - (int64_t)b);
#endif
}

/* The stock 32x32 -> 1.31 product as the SDRAM DC blocker (0x019674f8) computes it: three MACs on the
 * 16-bit halves (unsigned lo*lo >> 16, mixed-mode hi*lo and lo*hi, >>> 15) plus the signed hi*hi
 * fraction, 32-bit saturated. Equals (c*x) >> 31 up to that sequence's own rounding; on the device it
 * is the same instruction sequence so the coop replica tracks the stock stage exactly. */
static inline int32_t coop_fr32_mul_stock(int32_t c, int32_t x)
{
#ifdef __bfin__
    int32_t r;
    __asm__("A1 = %1.L * %2.L (FU);\n\t"
            "A1 = A1 >> 16;\n\t"
            "A1 += %1.H * %2.L (M), A0 = %1.H * %2.H;\n\t"
            "A1 += %2.H * %1.L (M);\n\t"
            "A1 = A1 >>> 15;\n\t"
            "%0 = (A0 += A1);"
            : "=d"(r) : "d"(c), "d"(x) : "A0", "A1");
    return r;
#else
    int32_t cH = c >> 16, xH = x >> 16;
    uint32_t cL = (uint32_t)c & 0xffffu, xL = (uint32_t)x & 0xffffu;
    int64_t a1 = (int64_t)((cL * xL) >> 16);
    a1 += (int64_t)cH * (int64_t)xL;
    a1 += (int64_t)xH * (int64_t)cL;
    a1 >>= 15;                                        /* arithmetic */
    int64_t a0 = ((int64_t)cH * (int64_t)xH) << 1;    /* signed fraction */
    return coop_sat32(a0 + a1);
#endif
}

/* One `A0 += a.L * b.L (W32)` step (host model): signed fraction (product << 1), 32-bit saturated. */
static inline int32_t coop_mac_w32(int32_t acc, int16_t a, int16_t b)
{
    return coop_sat32((int64_t)acc + (((int64_t)a * (int64_t)b) << 1));
}

#endif
