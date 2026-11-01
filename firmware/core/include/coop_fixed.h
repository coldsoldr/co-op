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

#endif
