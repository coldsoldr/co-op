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
     * knob_map). params[4] = 0x5300 + the plugin's number. params[5..7] = 0, unless the file declares controls (e6,
     * below): then params[5..7] are the controls' words and params[8] the latch bits. A plugin that wants 0..1
     * divides by the range itself; the ABI v2 parameter store (guide/plugin-guidelines.md section 3) will hand over
     * declared ranges instead. */
    void (*render)(int slot, int32_t *l, int32_t *r, const int16_t *params, int n);
} coop_fx_t;

/* ---- e6: effect controls (docs/plugins.md "Effect controls", guide/plugin-guidelines.md §4) ----
 * An effect declares up to eight controls in plugin.json "controls", as an engine does (the e4 table below), plus two
 * things only an effect has (or uses yet):
 *   "fine": true    on a continuous fn_turn control: shift + fn + turn on the same encoder moves the SAME control by
 *                   1/16 of the coarse step (coarse 328 Q15 a detent = 1 %; fine 20.5, as 20 and 21). Nothing else may
 *                   then be declared on that encoder's shift_fn_turn.
 *   "kind": "latch" on a click action, with "bit": 0..15 instead of "word": momentary state (freeze, hold), off
 *                   whenever the plugin is (re)initialised at the position (selected, a sound change), never saved,
 *                   never randomised. A click flips it.
 * Saved controls ("steps", "continuous") name "word": 5, 6 or 7 and a "default"; they are saved with the sound (slots
 * 0-2: the sound's fx_params[5..7]; slots 3-4: coop's FX 2 file) and the file's FX list row starts them at the default.
 * The core dispatches them on the effect page at every position (FX 1 and FX 2, instrument and master), shows the
 * name and value top left in the encoder's colour, and passes them to render, which then receives AT LEAST 16 words:
 *   params[5..7]  the words, clamped by the core: the step index (0..n-1) for steps, Q15 (0..32767) for continuous,
 *                 0 for a word with no control. Clamp again: the clamp is the core's promise, not the stored value's.
 *   params[8]     COOP_FX_LATCH: bit b = the latch declared with "bit": b (1 = on). Bits without a latch are 0.
 *   params[9..15] 0 (reserved).
 * Read them once at the block start and keep them for the block. pack.py sets the file's page-2 flag for an effect
 * with controls, so a core before e6 refuses the file ("plugin flags not supported yet") instead of running it with
 * params[5..7] = 0 and no params[8]. A file without "controls" gets exactly what it got before e6. */
#define COOP_FX_LATCH       8                          /* params[COOP_FX_LATCH]: the latch bits (e6) */
#define COOP_FX_PARAMS_MIN  16                         /* words a render with declared controls may read */

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

/* coop_engine_t.flags (v1, docs/engines.md "The 44.1 kHz voice"). A core that predates a flag refuses the file
 * ("engine flags not supported yet") instead of playing it wrong. */
#define COOP_ENGINE_F_44K   0x1u                       /* render fills out[0..63] at 44.1 kHz; the core makes the 128
                                                        * the voice loop wants (each sample twice, after the render).
                                                        * plugin.json "voice_rate": 44100 (pack.py checks the two agree) */
#define COOP_ENGINE_FLAGS   COOP_ENGINE_F_44K          /* every flag this core knows */
#define COOP_VOICE_BLOCK_44K 64                        /* samples a render with COOP_ENGINE_F_44K, at 44,100 Hz */

typedef struct coop_engine_t {
    uint32_t magic;                                    /* COOP_ENGINE_MAGIC */
    uint32_t api;                                      /* COOP_PLUGIN_API */
    uint32_t flags;                                    /* COOP_ENGINE_F_*, 0 = the 88.2 kHz render */
    uint32_t voice_bytes;                              /* state the host keeps per voice (0..COOP_VOICE_BYTES_MAX),
                                                        * zeroed at load, 32-byte aligned; 0 = none */
    int (*init)(void);                                 /* main thread, once at load (tables); 0 = ready, else the
                                                        * file is refused. May be 0 */
    /* a note starts on `voice` (0..5): velocity Q15 (1..32767), params as for render. Pitch arrives with render. */
    void (*note_on)(int voice, void *state, int32_t velocity, const int16_t *params);
    /* one block of the voice into out[128]. pitch[128]: the per-sample phase increment the stock computed for this
     * voice (2^32 = one cycle a sample at 88.2 kHz; bend, LFO and glide included). params[0..3]: the slot's four
     * knob words (0..32767); params[4]: the core's selector (0x5300 + n), never written; params[5..7]: the free
     * preset words.
     * With COOP_ENGINE_F_44K: out[0..63] at 44.1 kHz (leave out[64..127] alone: the core overwrites them); pitch[]
     * is unchanged (128 words at 88.2 kHz), so output sample i's increment at 44.1 kHz is 2 * pitch[2 * i]. */
    void (*render)(int voice, void *state, int16_t *out, const int32_t *pitch, const int16_t *params);
    void (*note_off)(int voice, void *state);          /* may be 0: the stock envelope releases the voice */
    const coop_screen_t *screen;                       /* 0 = the stock iter screen (E2) */
} coop_engine_t;

/* ---- e4: controls beyond the four turns (docs/engines.md "Controls", guide/plugin-guidelines.md §3, §4) ----
 * An engine declares them in plugin.json "controls" (tools/plugin/pack.py packs them into the file); the core
 * dispatches click, shift + click, fn + turn, fn + click, shift + fn + turn and shift + fn + click on the engine's page,
 * stores the value in a free knob word 5, 6 or 7 (saved with the sound, passed as params[5..7]: the step index for a
 * steps control, Q15 for a continuous one; clamp what you read, a sound may hold anything) and shows the name and value.
 * The engine reads params[5..7]; nothing else to call. e6: "fine" (above) works on an engine's continuous fn_turn too;
 * latches are for effects only (an engine's params block has 8 words).
 *
 * ---- e4: L1 for engines ----
 * coop_l1_lease: from the engine's init only (main thread, at load): a zeroed, 32-byte aligned block in L1 data bank B,
 * kept for the session; up to 4096 bytes for all engines together; 0 when refused (no pool, over the limit, called
 * from anywhere else): keep an SDRAM fallback. SDRAM lines cost a bus wait whenever they aren't cached, L1 never
 * does: the per-sample hot data belongs here.
 * coop_l1_scratch: per-call scratch in the L1 scratchpad (1312 bytes at 0xFFB000E0), valid only inside the engine's
 * own note_on and render: nothing survives between calls. Both are core exports (sdk/coop_exports.cc). */
#ifdef __cplusplus
extern "C" {
#endif
void *coop_l1_lease(uint32_t bytes);
void *coop_l1_scratch(uint32_t *bytes);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
}
#endif
#endif
