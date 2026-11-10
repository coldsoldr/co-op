/*
 * e4 memory gate (docs/engines.md "Memory"): an engine with ~600 KB of .bss, the size of rings' voice states and
 * reverb (~572 KB), to show heap 0 takes it at boot beside the other plugins and the loader zeroes it. It plays
 * silence; it touches its buffer once a note so the memory is really there.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_plugin.h"

int32_t g_big[600 * 1024 / 4];                       /* not static: kept and written, so the .bss is real */
int bigbss_sum;

static void note_on(int voice, void *state, int32_t velocity, const int16_t *params)
{
    (void)state; (void)velocity; (void)params;
    int32_t s = 0;
    for (int i = voice; i < (int)(sizeof g_big / sizeof g_big[0]); i += 4096) s += g_big[i];
    bigbss_sum = s;                                    /* 0 on the first notes: the loader zeroed it */
    g_big[voice * 4096 + 1] = velocity;
}
static void render(int voice, void *state, int16_t *out, const int32_t *pitch, const int16_t *params)
{
    (void)voice; (void)state; (void)pitch; (void)params;
    for (int i = 0; i < COOP_VOICE_BLOCK; i++) out[i] = 0;
}

extern "C" const coop_engine_t ENGINE_bigbss = {
    COOP_ENGINE_MAGIC, COOP_PLUGIN_API, 0, 0, 0, note_on, render, 0, 0,
};
