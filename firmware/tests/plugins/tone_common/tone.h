/*
 * v1 test engines (docs/engines.md "The 44.1 kHz voice"; test plugins, never shipped): the same tone rendered at
 * 88.2 kHz (tone88, flags 0) and at 44.1 kHz (tone44, COOP_ENGINE_F_44K), so op1emu can show that the 44.1 kHz path
 * plays the same pitch at the same level. The tone is a parabolic sine (y = 4x(1 - |x|) on the phase, ~3 % third
 * harmonic) at 1/4 of full scale, phase from the stock's pitch words; one 32-bit phase word of state per voice.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef TONE_H
#define TONE_H
#include <stdint.h>
#include "coop_plugin.h"

static inline int16_t tone_at(uint32_t ph)
{
    int32_t x = (int32_t)ph >> 16;                     /* -32768 .. 32767: -1 .. 1, a half cycle each side */
    int32_t a = x < 0 ? -x : x;
    return (int16_t)((x * (32768 - a)) >> 15);         /* x (1 - |x|): peak 8192 at |x| = 0.5 */
}
static void tone_note_on(int voice, void *state, int32_t velocity, const int16_t *params)
{
    (void)voice; (void)velocity; (void)params; *(uint32_t *)state = 0;
}
static void tone_render88(int voice, void *state, int16_t *out, const int32_t *pitch, const int16_t *params)
{
    (void)voice; (void)params;
    uint32_t ph = *(uint32_t *)state;
    for (int i = 0; i < COOP_VOICE_BLOCK; i++) { out[i] = tone_at(ph); ph += (uint32_t)pitch[i]; }
    *(uint32_t *)state = ph;
}
static void tone_render44(int voice, void *state, int16_t *out, const int32_t *pitch, const int16_t *params)
{
    (void)voice; (void)params;
    uint32_t ph = *(uint32_t *)state;
    for (int i = 0; i < COOP_VOICE_BLOCK_44K; i++) { out[i] = tone_at(ph); ph += 2u * (uint32_t)pitch[2 * i]; }
    *(uint32_t *)state = ph;
}
#endif
