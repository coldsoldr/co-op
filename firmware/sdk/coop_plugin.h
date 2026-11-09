/*
 * coop-fw plugin ABI, version 1 (Phase 4: effects since P1, synth engines since E1, docs/engines.md). Fixed point
 * only, no float, no libc, no static constructors; the hot loop must stay small (docs/performance.md). A plugin is
 * one `.coop` file (kennel's plugin file format with the magic `COOP`, docs/plugins.md) in /yaffs2/plugins, loaded at
 * boot and relocated into heap 0.
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

/* ---- screens (E2, docs/engines.md "Screens"): a plugin that ships an SVG in its file draws it through these. Both
 * run on the GUI thread; `scr` is the screen object, handed to the sdk/coop_screen.h calls. ---- */
typedef struct coop_screen_t {
    void (*init)(void *scr);                           /* once, after the SVG is loaded: look the nodes up */
    void (*update)(void *scr, int dt);                 /* every frame while the screen is shown; dt in ms */
} coop_screen_t;

/* ---- synth engines (P3 / E1, docs/engines.md). The file's kind is 1 (synth); it exports one coop_engine_t. The
 * engine plays in the stock voice loop as the iter engine: six voices, each rendered as mono int16 at the stock
 * voice rate (88.2 kHz, 128 samples a block); the stock envelope, gain and LFO follow, and coop places the voice in
 * stereo. Everything runs in the audio interrupt except init and the screen. ---- */
#define COOP_ENGINE_MAGIC 0x4E454F43u                  /* "COEN" */
#define COOP_VOICES       6
#define COOP_VOICE_BLOCK  128                          /* samples a render, at 88,200 Hz */
#define COOP_VOICE_BYTES_MAX 8192u

typedef struct coop_engine_t {
    uint32_t magic;                                    /* COOP_ENGINE_MAGIC */
    uint32_t api;                                      /* COOP_PLUGIN_API */
    uint32_t flags;                                    /* 0 (reserved) */
    uint32_t voice_bytes;                              /* state the host keeps per voice (0..COOP_VOICE_BYTES_MAX),
                                                        * zeroed at load, 32-byte aligned; 0 = none */
    int (*init)(void);                                 /* main thread, once at load (tables); 0 = ready, else the
                                                        * file is refused. May be 0 */
    /* a note starts on `voice` (0..5): velocity Q15 (1..32767), params as for render. Pitch arrives with render. */
    void (*note_on)(int voice, void *state, int32_t velocity, const int16_t *params);
    /* one block of the voice into out[128]. pitch[128]: the per-sample phase increment the stock computed for this
     * voice (2^32 = one cycle a sample at 88.2 kHz; bend, LFO and glide included). params[0..3]: the slot's four
     * knob words (0..32767); params[4]: the core's selector (0x5300 + n), never written; params[5..7]: the free
     * preset words. */
    void (*render)(int voice, void *state, int16_t *out, const int32_t *pitch, const int16_t *params);
    void (*note_off)(int voice, void *state);          /* may be 0: the stock envelope releases the voice */
    const coop_screen_t *screen;                       /* 0 = the stock iter screen (E2) */
} coop_engine_t;

#ifdef __cplusplus
}
#endif
#endif
