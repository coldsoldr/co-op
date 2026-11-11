/* m1 probe: code footprint only. Once a render, 4, 8 or 12 KB of distinct straight-line code (word 5: three 4 KB
 * blocks of NOPs, no loads, no branches, as the p6f icache probe), then the wait to the target cost. With six voices
 * the same code runs six times a block, as rings' does; what it evicts from the instruction cache is what the stock
 * refetches after it.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include "probe.h"

extern "C" void probe_code_a(void);
extern "C" void probe_code_b(void);
extern "C" void probe_code_c(void);

static void render(int voice, void *state, int16_t *out, const int32_t *pitch, const int16_t *params)
{
    (void)voice;
    uint32_t t0 = probe_cycles();
    int k = probe_step(params, 3);                    /* 0: 4 KB, 1: 8 KB, 2: 12 KB */
    if (k >= 2) probe_code_a();
    if (k >= 1) probe_code_b();
    probe_code_c();
    probe_saw(state, out, pitch);
    probe_wait(t0, probe_target(params));
}

extern "C" const coop_engine_t ENGINE_probe_code = {
    COOP_ENGINE_MAGIC, COOP_PLUGIN_API, 0, 4, 0, probe_note_on, render, 0, 0,
};
