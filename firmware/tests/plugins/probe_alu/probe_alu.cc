/* m1 probe: pure compute. The render is the saw and the wait on CYCLES (registers only, a few instructions of code,
 * no memory beyond the voice's 128 outputs): the baseline the other two probes are read against.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include "probe.h"

static void render(int voice, void *state, int16_t *out, const int32_t *pitch, const int16_t *params)
{
    (void)voice;
    uint32_t t0 = probe_cycles();
    probe_saw(state, out, pitch);
    probe_wait(t0, probe_target(params));
}

extern "C" const coop_engine_t ENGINE_probe_alu = {
    COOP_ENGINE_MAGIC, COOP_PLUGIN_API, 0, 4, 0, probe_note_on, render, 0, 0,
};
