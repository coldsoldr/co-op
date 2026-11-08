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
    /* when a position selects the plugin, before its first render there; cheap (reset state, bind buffers); 0 =
     * ready, else the plugin is dropped. Slots: 0 synth, 1 drum, 2 master (the stock positions: init runs on the
     * audio thread), 3 the instrument's second slot, 4 the master's second slot (init runs on the main loop). */
    int (*init)(int slot);
    /* the audio interrupt: l[n], r[n] in place. params[0..3] are the four knobs IN THE FILTER TYPE'S RANGE UNITS,
     * not Q15 (the host as of p6k): {64..14528}, {0..24576}, {0..22528}, {8192..32767}; at slots 0-2 the stock knob
     * words clamped into those ranges, at slots 3-4 coop's Q15 knobs mapped into them (plugin_host.cc fx_sanitize /
     * knob_map). params[4] = 0x5300 + the plugin's number, params[5..7] = 0. A plugin that wants 0..1 divides by
     * the range itself; the ABI v2 parameter store (guide/plugin-guidelines.md section 3) will hand over declared
     * ranges instead. */
    void (*render)(int slot, int32_t *l, int32_t *r, const int16_t *params, int n);
} coop_fx_t;

#ifdef __cplusplus
}
#endif
#endif
