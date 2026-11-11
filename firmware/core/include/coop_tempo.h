/* The tempo as coop hands it to plugins (the export coop_tempo_bpm_q16, sdk/coop_exports.cc, core/sdram/plugin_host.cc).
 *
 * The source is the stock's own BPM word, an IEEE single at FW_TEMPO_BPM_F32 (fw_addrs.h, symbols/sdram.sym
 * d_tempo_bpm_f32): SetTempo stores it before deriving the sequencer's tick period and the LFOs' tempo increment from
 * it, and the tape reads it through GetTempo. The conversion below is integer only (no float op): the word's exponent
 * and mantissa shifted into Q16, truncated. Every single from 20.0 to 400.0 has its exponent in 131..135, so the shift
 * is -3..+1 and the result fits 32 bits (400 << 16 = 26,214,400). Anything else (0, negative, NaN, infinity, out of
 * 20..400 BPM) gives 120 << 16, kennel's fallback (guide/plugin-guidelines.md section 2.3).
 *
 * Header-only so the host tests (tests/host/tape_echo) run the very same conversion.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef COOP_TEMPO_H
#define COOP_TEMPO_H
#include <stdint.h>

#define COOP_TEMPO_DEFAULT_Q16  (120u << 16)
#define COOP_TEMPO_F32_MIN      0x41A00000u            /* 20.0f */
#define COOP_TEMPO_F32_MAX      0x43C80000u            /* 400.0f */

/* bits: the IEEE single's word. Positive singles order as their words, so the range check is two compares (a negative
 * or a NaN word is over 0x7F800000 or has the sign bit, and fails it). */
static inline uint32_t coop_tempo_q16_from_f32(uint32_t bits)
{
    if (bits < COOP_TEMPO_F32_MIN || bits > COOP_TEMPO_F32_MAX) return COOP_TEMPO_DEFAULT_Q16;
    const uint32_t mant = (bits & 0x007FFFFFu) | 0x00800000u;    /* value = mant x 2^(exp - 150) */
    const int32_t sh = (int32_t)((bits >> 23) & 0xFFu) - 134;     /* Q16: mant x 2^(exp - 134) */
    return sh >= 0 ? mant << sh : mant >> -sh;
}

#endif
