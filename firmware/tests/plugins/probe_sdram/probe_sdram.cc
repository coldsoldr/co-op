/* m1 probe: data cache and SDRAM traffic only. Each render touches 64 cache lines (2 KB) of its own 128 KB .bss,
 * moving on through a region of 16, 64 or 128 KB (word 5), then waits to the target cost. Six voices touch 12 KB a
 * block: a 16 KB region can stay in the data cache after its first pass, a 128 KB one misses every line, every block,
 * and evicts the stock's data as rings' delay lines and mode states do.
 *   steps 0..2  "r":  sequential reads, every word of each line (16 / 64 / 128 KB)
 *   steps 3..5  "w":  sequential writes, every word of each line
 *   step 6      "rs": one read per line, lines 4,128 bytes apart (a new SDRAM row nearly every access), 128 KB
 *   step 7      "ws": the same with one write per line
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include "probe.h"

#define LINES  64
#define REGION (128u * 1024u)
uint32_t g_probe_buf[REGION / 4] __attribute__((aligned(32)));   /* not static: kept */
uint32_t g_probe_sink;
static uint32_t s_pos;                                             /* byte offset of the next line, shared by the voices */

static void render(int voice, void *state, int16_t *out, const int32_t *pitch, const int16_t *params)
{
    (void)voice;
    uint32_t t0 = probe_cycles();
    int k = probe_step(params, 8);
    uint32_t size = (k == 0 || k == 3) ? 16384u : (k == 1 || k == 4) ? 65536u : REGION;
    uint32_t pos = s_pos & (size - 1u), acc = 0;
    volatile uint32_t *b = g_probe_buf;
    if (k < 3) {
        for (int l = 0; l < LINES; l++, pos = (pos + 32u) & (size - 1u))
            for (int w = 0; w < 8; w++) acc += b[pos / 4 + w];
    } else if (k < 6) {
        for (int l = 0; l < LINES; l++, pos = (pos + 32u) & (size - 1u))
            for (int w = 0; w < 8; w++) b[pos / 4 + w] = pos + (uint32_t)w;
    } else if (k == 6) {
        for (int l = 0; l < LINES; l++, pos = (pos + 4128u) & (size - 1u)) acc += b[pos / 4];
    } else {
        for (int l = 0; l < LINES; l++, pos = (pos + 4128u) & (size - 1u)) b[pos / 4] = pos;
    }
    s_pos = pos;
    g_probe_sink += acc;
    probe_saw(state, out, pitch);
    probe_wait(t0, probe_target(params));
}

extern "C" const coop_engine_t ENGINE_probe_sdram = {
    COOP_ENGINE_MAGIC, COOP_PLUGIN_API, 0, 4, 0, probe_note_on, render, 0, 0,
};
