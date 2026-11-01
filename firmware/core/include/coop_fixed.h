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

/* Q31 multiply of two int32 (rounded, saturated). 0x7fffffff ~= 1.0. */
static inline int32_t coop_mul_q31(int32_t a, int32_t b)
{
    int64_t p = ((int64_t)a * (int64_t)b + (1LL << 30)) >> 31;
    return coop_sat32(p);
}

/* multiply a Q31 sample by a Q(frac) gain given as int32 with `frac` fractional bits (so gain can be
 * > 1.0 without overflowing the operand). */
static inline int32_t coop_mul_qn(int32_t sample, int32_t gain, int frac)
{
    int64_t p = ((int64_t)sample * (int64_t)gain + ((int64_t)1 << (frac - 1))) >> frac;
    return coop_sat32(p);
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
