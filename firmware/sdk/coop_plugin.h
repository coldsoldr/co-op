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

struct coop_fx_screen_t;                               /* e9, below */

/* coop_fx_t.flags (e9). A core before e9 never reads this word, so a file with a flag it does not know plays there as
 * before (the flag's feature ignored: an effect screen falls back to the stock filter page). An e9 core refuses a flag
 * it does not know ("effect flags not supported yet"). */
#define COOP_FX_F_SCREEN    0x1u                       /* e9: `screen` below is present (plugin.json "screen") */
#define COOP_FX_FLAGS       COOP_FX_F_SCREEN           /* every flag this core knows */

typedef struct coop_fx_t {
    uint32_t magic;                                    /* COOP_FX_MAGIC */
    uint32_t api;                                      /* COOP_PLUGIN_API */
    uint32_t flags;                                    /* COOP_FX_F_*, 0 = none (e9) */
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
    /* e9: read only when flags has COOP_FX_F_SCREEN (a file before e9 ends at render); the effect's screen, below */
    const struct coop_fx_screen_t *screen;
} coop_fx_t;

/* ---- e6: effect controls (docs/plugins.md "Effect controls", guide/plugin-guidelines.md §4) ----
 * An effect declares up to eight controls in plugin.json "controls", as an engine does (the e4 table below), plus two
 * things only an effect has (or uses yet):
 *   "fine": true    on a continuous fn_turn control: shift + fn + turn on the same encoder moves the SAME control by
 *                   1/16 of the coarse step (coarse 328 Q15 a detent = 1 %; fine 20.5, as 20 and 21). Nothing else may
 *                   then be declared on that encoder's shift_fn_turn.
 *   "kind": "latch" on a click action, with "bit": 0..15 instead of "word": momentary state (freeze, hold), off
 *                   whenever the plugin is (re)initialised at the position (selected, a sound change), never saved,
 *                   never randomised. A click flips it. The stock restarts the instrument effect on a synth / drum
 *                   switch, so a latch at positions 0-1 is off after one (accepted: docs/ui-features.md §14).
 *   "step"          on a continuous control (engines and effects): Q15 a detent on its own action (default 328 = 1 %).
 *   "fine_step"     with "fine": true: Q15 a detent on shift + fn + turn (default step / 16, the remainder carried
 *                   between detents); 1..step. Kennel's gain (Q15 over +-6 dB): "step": 1365 (0.5 dB),
 *                   "fine_step": 273 (0.1 dB).
 *   "random"        as for engines (default true): randomise on the effect page draws the control (steps within the
 *                   count, continuous 0..32767); "random": false keeps it (input gain).
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

/* ---- e8: the position buffers (docs/plugins.md "e8", docs/fx-abi.md "Memory") ----
 * The core owns one working buffer per FX position (slot 0 synth, 1 drum, 2 master, 3 instrument FX 2, 4 master
 * FX 2); the effect active at a position works in that position's buffer. An effect declares the bytes it needs in
 * plugin.json "scratch": N (pack.py sets the file's scratch flag, so a core before e8 refuses the file, and compiles
 * the sources with -DCOOP_FX_SCRATCH_BYTES=N, so a port can static_assert its state fits). Such a file is relocated
 * ONCE and serves all five positions from the one image; its .bss holds only per-slot pointers or tiny per-slot
 * values (indexed by `slot`), never the effect's state.
 *
 * coop_fx_scratch(slot, &bytes): the slot's buffer (32-byte aligned, at least the declared bytes; *bytes = its size),
 * or 0 (slot outside 0..4, or the file declares no "scratch"). The address is fixed for the session, so a port may
 * keep it (per slot) from init. Call it from init or render only.
 *
 * The contents are NOT preserved across selections: whichever scratch effect is active at the position uses the same
 * memory, so init(slot), which the core calls every time the effect is selected (or restarted) there, must reset all
 * of the effect's state as if it found garbage (it does). init stays cheap (it may run in the audio interrupt): a
 * large line or tank is cleared over the next renders while the dry signal passes (guide/plugin-guidelines.md §2.3).
 * The same effect at two positions works in two buffers: two independent states.
 *
 * COOP_FX_SCRATCH_MAX: the most a file may declare (176 KiB: every kennel effect core fits it, kennel's position
 * buffer is 176,460 bytes); a larger declaration is refused by pack.py and by the core. The core sizes the five
 * buffers at boot to the largest declaration among the scratch files it loaded. */
#define COOP_FX_SLOTS        5
#define COOP_FX_SCRATCH_MAX  180224u
#ifdef __cplusplus
extern "C" {
#endif
void *coop_fx_scratch(int slot, uint32_t *bytes);
#ifdef __cplusplus
}
#endif

/* ---- e10: the tempo (docs/fx-abi.md "Tempo", guide/plugin-guidelines.md §2.3) ----
 * coop_tempo_bpm_q16(): the project tempo (the tempo screen's BPM, which the tape and the sequencer follow) in BPM as
 * Q16 (120 BPM = 120 << 16 = 7,864,320). It changes live as the tempo is turned and is valid from boot. 120 << 16 when
 * the tempo is unknown or outside 20..400 BPM. A plain word read, no lock: call it from render (once a block, never a
 * sample), init or the screen. Divide by it once a block, not per sample. A file that imports it is refused by a core
 * before e10 ("an import is not exported by the core"): it never plays there at a wrong tempo. */
#ifdef __cplusplus
extern "C" {
#endif
uint32_t coop_tempo_bpm_q16(void);
#ifdef __cplusplus
}
#endif

/* ---- screens (E2, docs/engines.md "Screens"): a plugin that ships an SVG in its file draws it through these. Both
 * run on the GUI thread; `scr` is the screen object, handed to the sdk/coop_screen.h calls. ---- */
typedef struct coop_screen_t {
    void (*init)(void *scr);                           /* once, after the SVG is loaded: look the nodes up */
    void (*update)(void *scr, int dt);                 /* every frame while the screen is shown; dt in ms */
} coop_screen_t;

/* ---- e9: effect screens (docs/plugins.md "e9") ----
 * An effect with plugin.json "screen": {"svg": ..., "symbol": <its coop_fx_screen_t>} ships its SVG at the front of the
 * file as an engine does, sets COOP_FX_F_SCREEN in coop_fx_t.flags and points coop_fx_t.screen at the object (pack.py
 * checks the three agree). Wherever that effect is the one shown on the effect page (FX 1 of the synth, the drum or
 * the master; FX 2 of the instrument or the master while it is in focus), the core shows this screen instead of the
 * stock filter page (FX 1) or coop's FX 2 page. A core before e9 ignores the flag and the field: the effect plays and
 * the old page shows.
 *
 * update gets the view below, filled by the core for the position on screen. Everything runs on the GUI thread, never
 * the audio thread: no float, no libc, no static constructors, no blocking. One screen object serves every position,
 * so per-position animation state is the plugin's to index by view->slot. */
typedef struct coop_fx_view_t {
    int32_t slot;                                      /* the position shown: 0 synth, 1 drum, 2 master FX 1, 3 instrument
                                                        * FX 2, 4 master FX 2 (render's `slot`) */
    int16_t knobs[4];                                  /* the four turns, 0..32767, as the stock pages see them: FX 1 with
                                                        * the LFO's movement included (read_fx_param normalised with the
                                                        * filter type's ranges: what render's params[0..3] normalise to);
                                                        * FX 2 the slot's Q15 knobs */
    int16_t words[3];                                  /* params[5..7] as render gets them (clamped to the controls; 0
                                                        * without controls) */
    uint16_t latches;                                  /* params[8]: the latch bits (e6) */
    int32_t mods;                                      /* held modifiers: bit 0 shift, bit 1 fn (coop_scr_mods) */
    /* What the DSP publishes for display. A scratch effect (e8) gets its position buffer here, read only, so the screen
     * reads the words its render published (see "Display data" below); an old-model effect (no "scratch") gets 0. */
    const volatile void *state;
    uint32_t state_bytes;                              /* the buffer's size (0 with state 0) */
    int32_t live;                                      /* 1 once the effect has been initialised at `slot` since it was
                                                        * selected there (state is then this effect's); 0 before: the
                                                        * buffer may still hold the previous effect's bytes */
    int32_t reserved[4];                               /* 0 */
} coop_fx_view_t;

/* Display data (the e9 path). render publishes what the screen shows (a level, a locked division, which heads play) as
 * whole aligned 32-bit words in a small struct of its own state, written once a block after the DSP ran (one store
 * each, never a value half-made); the screen reads them through view->state as volatile, only while view->live, and
 * clamps what it reads (a word may be from the block before). Never read DSP working state the render changes mid-block
 * (guide/plugin-guidelines.md §5.3). */
typedef struct coop_fx_screen_t {
    void (*init)(void *scr);                           /* once, after the SVG is loaded: look the nodes up */
    void (*update)(void *scr, int dt, const coop_fx_view_t *view);   /* every frame while shown; dt in ms; draw with
                                                                         coop_scr_draw (sdk/coop_screen.h) */
} coop_fx_screen_t;

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
