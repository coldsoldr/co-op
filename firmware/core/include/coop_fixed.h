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

#endif
