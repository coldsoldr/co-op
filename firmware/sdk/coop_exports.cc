/*
 * Core symbols a coop plugin file may import, by ELF name (with the C underscore): the loader (plugin_load.cc)
 * resolves a file's import table against this list and refuses a file that imports anything else. Append-only:
 * never remove or change an entry a released plugin uses (modelled on kennel's kn_exports.c).
 *
 * Milestone P1: nothing yet. The first plugins carry their own DSP and their own buffers.
 * E2 (2026-11-09): the screen calls of sdk/coop_screen.h (core/sdram/engine_screen.cc).
 * e4 (2026-11-10): the L1 lease and scratch (sdk/coop_plugin.h, core/sdram/l1_pool.cc), the shown slot's eight words
 * and the held modifiers (sdk/coop_screen.h, core/sdram/engine_ui.cc).
 * e8 (2026-11-11): coop_fx_scratch, an effect's position buffer (sdk/coop_plugin.h, core/sdram/plugin_host.cc).
 * e10 (2026-11-11): coop_tempo_bpm_q16, the tempo in BPM as Q16 (sdk/coop_plugin.h, core/sdram/plugin_host.cc).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "../core/sdram/plugin_load.h"

extern "C" {
void *coop_scr_find(void *scr, const char *id);
void coop_scr_transform(void *node, const int32_t *m9);
void coop_scr_visible(void *node, int on);
uint8_t *coop_scr_line_rgba(void *scr, const char *id);
uint8_t *coop_scr_fill_rgba(void *scr, const char *id);
void coop_scr_draw(void *scr, int dt);
void *coop_scr_raster(void);
void coop_scr_line(void *raster, const int32_t *p0, const int32_t *p1, int32_t width_q10, const uint8_t *rgba);
void coop_scr_knobs(int16_t *out4);
void *coop_l1_lease(uint32_t bytes);
void *coop_l1_scratch(uint32_t *bytes);
void coop_scr_words(int16_t *out8);
int coop_scr_mods(void);
void *coop_fx_scratch(int slot, uint32_t *bytes);
uint32_t coop_tempo_bpm_q16(void);
}
#define EXPORT(f) { "_" #f, (uint32_t)(uintptr_t)&f }

extern "C" const kn_export coop_exports[] = {
    EXPORT(coop_scr_find),                           /* E2: the screen calls (sdk/coop_screen.h), in this order */
    EXPORT(coop_scr_transform),
    EXPORT(coop_scr_visible),
    EXPORT(coop_scr_line_rgba),
    EXPORT(coop_scr_fill_rgba),
    EXPORT(coop_scr_draw),
    EXPORT(coop_scr_raster),
    EXPORT(coop_scr_line),
    EXPORT(coop_scr_knobs),
    EXPORT(coop_l1_lease),                           /* e4 */
    EXPORT(coop_l1_scratch),
    EXPORT(coop_scr_words),
    EXPORT(coop_scr_mods),
    EXPORT(coop_fx_scratch),                         /* e8: the position buffers (sdk/coop_plugin.h, plugin_host.cc) */
    EXPORT(coop_tempo_bpm_q16),                      /* e10: the tempo, Q16 BPM (sdk/coop_plugin.h, plugin_host.cc) */
    { "_coop_exports_end", 0 },                      /* a placeholder so the table is never empty */
};
extern "C" const int coop_exports_count = 15;         /* entries a plugin may use (the placeholder is not one) */
