/*
 * m1 probe engines (docs/request-rings-collateral.md section 2; test plugins, never shipped): each loads one resource
 * only (compute, code footprint, data / SDRAM), then waits in registers on CYCLES until the render has cost the
 * target (word 6: 25,000 / 12,500 / 37,500 cycles). So every probe costs a render what rings does, and only what it
 * leaves behind for the rest of the block differs. The voice plays a quiet saw from the stock's pitch, so a
 * recording shows the notes.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef PROBE_H
#define PROBE_H
#include <stdint.h>
#include "coop_plugin.h"

static inline uint32_t probe_cycles(void) { uint32_t v; __asm__ __volatile__("%0 = CYCLES;" : "=d"(v)); return v; }
static inline uint32_t probe_target(const int16_t *params)
{
    int k = params[6];
    return k == 1 ? 12500u : k == 2 ? 37500u : 25000u;
}
static inline int probe_step(const int16_t *params, int n) { int k = params[5]; return (k < 0 || k >= n) ? 0 : k; }
/* the voice: a saw at the stock pitch, +-4096 */
static inline void probe_saw(void *state, int16_t *out, const int32_t *pitch)
{
    uint32_t ph = *(uint32_t *)state;
    for (int i = 0; i < COOP_VOICE_BLOCK; i++) { ph += (uint32_t)pitch[i]; out[i] = (int16_t)((int32_t)ph >> 19); }
    *(uint32_t *)state = ph;
}
static inline void probe_wait(uint32_t t0, uint32_t target)
{
    while (probe_cycles() - t0 < target) { }
}
static void probe_note_on(int voice, void *state, int32_t velocity, const int16_t *params)
{
    (void)voice; (void)velocity; (void)params; *(uint32_t *)state = 0;
}
#endif
