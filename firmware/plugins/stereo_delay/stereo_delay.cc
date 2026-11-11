/*
 * stereo_delay: the coop stereo feedback delay (core/include/coop_fx.h) as the first coop plugin (milestone P1).
 * Same DSP, same constants as the built-in master insert of master_suite.cc, so the gate is bit-identity: the master
 * bus through this file equals the master bus through the built-in delay. The delay lines live in the plugin's own
 * .bss (the loader zeroes it); the knobs are reserved for P2 (the core's parameter model) and ignored here.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_fx.h"

#define LINE_LEN 16384                                 /* samples per channel (master_suite.cc FXB_LEN) */
#define DELAY_SAMPLES 6615                             /* 0.15 s at 44.1 kHz */
#define FB_Q31  858993459                              /* 0.40 */
#define MIX_Q31 751619277                              /* 0.35 */

static int32_t g_lineL[LINE_LEN], g_lineR[LINE_LEN];
static coop_delay g_delay;

static int init(int slot)
{
    (void)slot;
    coop_delay_init(&g_delay, g_lineL, g_lineR, LINE_LEN);
    coop_delay_set(&g_delay, DELAY_SAMPLES, FB_Q31, MIX_Q31);
    return 0;
}

static void render(int slot, int32_t *l, int32_t *r, const int16_t *params, int n)
{
    (void)slot; (void)params;
    coop_delay_process(&g_delay, l, r, n);
}

extern "C" const coop_fx_t FX_stereo_delay = { COOP_FX_MAGIC, COOP_PLUGIN_API, 0, init, render };
