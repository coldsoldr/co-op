/*
 * coop-fw plugin ABI, version 1 (Phase 4, milestone P1: effects). Fixed point only, no float, no libc, no static
 * constructors; the hot loop must stay small (docs/performance.md). A plugin is one `.coop` file (kennel's plugin
 * file format with the magic `COOP`, docs/plugins.md) in /yaffs2/plugins, loaded at boot and relocated into heap 0.
 *
 * An FX plugin exports one `coop_fx_t` object (plugin.json "symbol"); the core finds it through the file's entry
 * offset and checks `magic` and `api` after relocation. The master insert slot hands it the master bus as two
 * int32 planar buffers (the int16 bus widened by 16 bits: Q31 with the stock level) of n samples, in place.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef COOP_PLUGIN_H
#define COOP_PLUGIN_H
#include <stdint.h>

#define COOP_PLUGIN_API 1                              /* the file's api_version must equal the core's */
#define COOP_FX_MAGIC   0x58464F43u                    /* "COFX" */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct coop_fx_t {
    uint32_t magic;                                    /* COOP_FX_MAGIC */
    uint32_t api;                                      /* COOP_PLUGIN_API */
    uint32_t flags;                                    /* 0 (reserved) */
    /* when a position (slot: 0 synth, 1 drum, 2 master) selects the plugin, from the audio thread, before its first
     * render there; cheap (reset state, bind buffers); 0 = ready, else the plugin is dropped */
    int (*init)(int slot);
    /* the audio interrupt: l[n], r[n] in place; params[0..3] = the four knobs, Q15 0..32767 */
    void (*render)(int slot, int32_t *l, int32_t *r, const int16_t *params, int n);
} coop_fx_t;

#ifdef __cplusplus
}
#endif
#endif
