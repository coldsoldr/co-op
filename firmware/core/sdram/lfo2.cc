/*
 * coop-fw second LFO slot (Phase 3 "LFO x2", docs/lfo2.md) -- firmware payload.
 *
 * A second LFO per voice, built from the stock LFO classes: coop owns the objects (region-A .bss) and
 * initialises them exactly as the stock per-type getters do (lfo-interface.md section 2, disassembly
 * 0x019cc664 / 0x019d0290 / 0x019cf340), then calls the stock methods through the stock vtables:
 *   process(this + W[vt+0x10], &params[8], stage)   vt+0x14      trigger(this + off, &params)   vt+0x1c
 * The stock process modifies the destination data in place (engine knob scratch 0xff801724, pitch
 * array 0xff801950, gain array 0xff801d50 at stage 0 / drum stage 3), so running LFO2 right before the
 * render -- from coop_synth_render_thunk / coop_drum_render_thunk, after the stock LFO1 stage-0 pass
 * and the Core's knob copy -- composes with LFO1 and works whether or not LFO1 is active. The envelope
 * target (stage 1) is not offered on slot 2 (it would have to run before the ADSR fill).
 *
 * Types on slot 2: tremolo (0), value (1), random (2): the classes whose state is in the object. The
 * vptr-only types (element, midi, bend, crank) keep state in globals shared with LFO1 and are refused.
 * Parameters are clamped to the stock LFO Ranges table (0x01de5740, 7 x 8 x {min,max,scale}) so the
 * stock code never sees a value its own validation would reject.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>

#define LFO_TREMOLO 0
#define LFO_VALUE 1
#define LFO_RANDOM 2
#define LFO_OFF 7
#define SYNTH_VOICES 6
#define DRUM_VOICES 12
#define OBJ_BYTES 44                          /* the largest (random) */

/* stock vtables (lfo-interface.md section 3) [C] */
#define VT_TREMOLO      0x019107fcu
#define VT_VALUE_SYNTH  0x0191097cu
#define VT_VALUE_DRUM   0x0191099cu
#define VT_RANDOM_SYNTH 0x0191090cu
#define VT_RANDOM_DRUM  0x0191092cu
#define LFO_RANGES ((const volatile int16_t *)0x01de5740u)   /* {min,max,scale} x 8 per type, run-time filled */

typedef int (*lfo_process_fn)(void *self, const int16_t *params, int stage);
typedef void (*lfo_trigger_fn)(void *self, const int16_t *params);

/* --- controls (.data: inject_app --set-word; later co-op / the plugin UI) --- */
__attribute__((section(".data"))) volatile uint32_t g_coop_lfo2_type = LFO_OFF;     /* 0 tremolo, 1 value, 2 random, 7 off */
/* int16 pairs, one 32-bit word each so inject_app --set-word can set them: p0 | p1 << 16, ... */
__attribute__((section(".data"))) volatile uint32_t g_coop_lfo2_p01 = 0, g_coop_lfo2_p23 = 0, g_coop_lfo2_p45 = 0, g_coop_lfo2_p67 = 0;

static void read_param_words(uint32_t w[4])
{
    w[0] = g_coop_lfo2_p01; w[1] = g_coop_lfo2_p23; w[2] = g_coop_lfo2_p45; w[3] = g_coop_lfo2_p67;
}
__attribute__((section(".data"))) volatile uint32_t g_coop_lfo2_active = 1;

/* --- state --- */
static uint32_t g_synth_obj[SYNTH_VOICES][OBJ_BYTES / 4];
static uint32_t g_drum_obj[DRUM_VOICES][OBJ_BYTES / 4];
/* u8: LFO 2 per sound. Set 0 (the synth voices) follows the current synth slot's sound, set 1 (the drum voices) the
 * current kit's (coop_sound.cc "lfo2"); a sound without one falls back to the global words (the image's set-word
 * debug path and the old slot file). Each set keeps its own built type and parameters. */
#define SYNTH_INST (*(volatile int *)0xFF8016D4u)
#define DRUM_KIT   (*(volatile int *)0xFF8016D8u)
#define CUR_MODE   (*(volatile int *)0xFF907D58u)
extern "C" int coop_sound_lfo2(int kind, int slot, int *type, int *on, int16_t *p8);   /* coop_sound.cc */
extern "C" void coop_sound_set_lfo2(int kind, int slot, int type, int on, const int16_t *p);
static int16_t g_params[2][8];
static uint32_t g_built_type[2] = { LFO_OFF, LFO_OFF };
static int16_t g_built_p[2][8];
volatile uint32_t g_lfo2_processes, g_lfo2_triggers, g_lfo2_cycles_max;

static inline uint32_t coop_cycles(void)
{
    uint32_t v;
    __asm__ __volatile__("%0 = CYCLES;" : "=d"(v));
    return v;
}

/* the stock inner phase object init (0x019f15f8): [x] = 0x80000000; W[x+4] = 0x8000 */
static void phase_init(uint32_t *x)
{
    x[0] = 0x80000000u;
    ((volatile uint16_t *)x)[2] = 0x8000u;
}

/* Build one object as the stock getter would, with the vtable for its instrument. 0 = refused type. */
static int build(uint32_t *o, uint32_t type, int is_drum)
{
    for (int i = 0; i < OBJ_BYTES / 4; i++) o[i] = 0;
    switch (type) {
    case LFO_TREMOLO:                            /* ctor 0x019cbd88 */
        o[0] = VT_TREMOLO;
        phase_init(o + 1);                       /* +4 */
        phase_init(o + 3);                       /* +0xc */
        o[6] = 0;                                /* +0x18 */
        ((volatile uint16_t *)o)[10] = 0;        /* +0x14 */
        return 1;
    case LFO_VALUE:                              /* getter 0x019d0290 */
        o[0] = is_drum ? VT_VALUE_DRUM : VT_VALUE_SYNTH;
        phase_init(o + 1);
        return 1;
    case LFO_RANDOM:                             /* getter 0x019cf340: vptr, level 0, counter 0, targets/smoothed 0 */
        o[0] = is_drum ? VT_RANDOM_DRUM : VT_RANDOM_SYNTH;
        return 1;
    default:
        return 0;
    }
}

static void clamp_params(int set, uint32_t type, const int16_t *p)
{
    for (int k = 0; k < 8; k++) {
        int32_t v = p[k];
        int16_t lo = LFO_RANGES[(type * 8 + k) * 3], hi = LFO_RANGES[(type * 8 + k) * 3 + 1];
        if (lo < hi) {                           /* entry filled (the table is run-time initialised; zero = unknown) */
            if (v < lo) v = lo;
            if (v > hi) v = hi;
        }
        g_params[set][k] = (int16_t)v;
    }
}

/* the source for a set: the current sound's LFO 2, else the global words; type LFO_OFF = nothing to run */
static uint32_t source(int set, int16_t *p)
{
    int type, on;
    int slot = (set ? DRUM_KIT : SYNTH_INST) & 7;
    if (coop_sound_lfo2(set, slot, &type, &on, p)) return on ? (uint32_t)type : LFO_OFF;
    if (g_coop_lfo2_type == LFO_OFF || !g_coop_lfo2_active) return LFO_OFF;
    uint32_t w[4];
    read_param_words(w);
    for (int k = 0; k < 8; k++) p[k] = (int16_t)(w[k >> 1] >> ((k & 1) * 16));
    return g_coop_lfo2_type;
}

/* (Re)build a set's objects when its type or parameters change. Returns 1 if that set is live. */
static int sync(int set)
{
    int16_t p[8];
    uint32_t type = source(set, p);
    if (type == LFO_OFF) { g_built_type[set] = LFO_OFF; return 0; }
    int changed = type != g_built_type[set];
    for (int k = 0; k < 8; k++) if (p[k] != g_built_p[set][k]) changed = 1;
    if (changed) {
        if (type > LFO_RANDOM) { g_built_type[set] = LFO_OFF; return 0; }   /* refused types */
        if (set == 0) for (int v = 0; v < SYNTH_VOICES; v++) build(g_synth_obj[v], type, 0);
        else for (int v = 0; v < DRUM_VOICES; v++) build(g_drum_obj[v], type, 1);
        clamp_params(set, type, p);
        g_built_type[set] = type;
        for (int k = 0; k < 8; k++) g_built_p[set][k] = p[k];
    }
    return g_built_type[set] != LFO_OFF;
}

static inline void *self_of(uint32_t *o)
{
    const uint32_t *vt = (const uint32_t *)(uintptr_t)o[0];
    int16_t off = ((const int16_t *)vt)[8];      /* W[vt + 0x10]: this adjustment */
    return (uint8_t *)o + off;
}

static void process(uint32_t *o, int stage, int set)
{
    uint32_t t0 = coop_cycles();
    const uint32_t *vt = (const uint32_t *)(uintptr_t)o[0];
    ((lfo_process_fn)(uintptr_t)vt[5])(self_of(o), g_params[set], stage);   /* vt + 0x14 */
    g_lfo2_processes++;
    uint32_t dt = coop_cycles() - t0;
    if (dt > g_lfo2_cycles_max) g_lfo2_cycles_max = dt;
}

static void trigger(uint32_t *o, int set)
{
    const uint32_t *vt = (const uint32_t *)(uintptr_t)o[0];
    ((lfo_trigger_fn)(uintptr_t)vt[7])(self_of(o), g_params[set]);          /* vt + 0x1c */
    g_lfo2_triggers++;
}

/* v2 (the m1b result): sync's answer once a block per set. It read the sound and the parameter words for every voice
 * (code outside the per-voice section, fetched again after every render); they cannot change inside a block (the
 * voice loop runs in the audio interrupt), so the first voice's answer is every voice's. -1 = not asked this block
 * (coop_lfo2_block_start); a note-on's own sync (trigger, below) refreshes it. */
static int g_live[2] = { -1, -1 };
static __attribute__((noinline)) int sync_block(int set) { int l = sync(set); g_live[set] = l; return l; }
extern "C" void coop_lfo2_block_start(void) { g_live[0] = -1; g_live[1] = -1; }

/* From coop_synth_render_thunk, before the stock render: stage 0 (engine knobs / pitch / volume). */
#include "coop_cost.h"
extern "C" COOP_HOT_VOICE void coop_lfo2_pre_render_synth(uint32_t voice)
{
    if (voice >= SYNTH_VOICES) return;
    int l = g_live[0];
    if (l < 0) l = sync_block(0);
    if (l) process(g_synth_obj[voice], 0, 0);
}

/* From coop_drum_render_thunk, before the stock render: stage 3 (drum engine). */
extern "C" void coop_lfo2_pre_render_drum(uint32_t voice)
{
    if (voice >= DRUM_VOICES) return;
    int l = g_live[1];
    if (l < 0) l = sync_block(1);
    if (l) process(g_drum_obj[voice], 3, 1);
}

/* Note-on: from the Core::NoteOn hook (synth) and the drum first-render heuristic (stereo_bus.cc). */
extern "C" void coop_lfo2_trigger_synth(uint32_t voice)
{
    if (voice >= SYNTH_VOICES || !sync_block(0)) return;
    trigger(g_synth_obj[voice], 0);
}

extern "C" void coop_lfo2_trigger_drum(uint32_t voice)
{
    if (voice >= DRUM_VOICES || !sync_block(1)) return;
    trigger(g_drum_obj[voice], 1);
}

/* ---- u3: the LFO list's second column (coop_ui.cc) and the slot file (plugin_host.cc) ----
 * One LFO 2 for both instruments (the words are global; per sound is P3b). A type picked from the list gets its
 * parameters from the middle of the stock Ranges table (run-time filled: unknown entries give 0); sync() above rebuilds
 * the objects in the audio thread when the words change. */
/* u8: the instrument shown (synth or drum mode) and its current sound: where the UI's LFO 2 lives */
static int ui_kind(void) { return CUR_MODE == 1 ? 1 : 0; }
static int ui_slot(void) { return (ui_kind() ? DRUM_KIT : SYNTH_INST) & 7; }
extern "C" int coop_lfo2_get_type(void)                           /* the shown sound's LFO 2: 0..2, or 7 when off / none */
{
    int type, on; int16_t p[8];
    if (coop_sound_lfo2(ui_kind(), ui_slot(), &type, &on, p)) return on ? type : (int)LFO_OFF;
    return g_coop_lfo2_active ? (int)g_coop_lfo2_type : (int)LFO_OFF;
}
extern "C" int coop_lfo2_kind_type(int *on)                      /* the type the sound carries (off or on), 7 none */
{
    int type, o; int16_t p[8];
    if (coop_sound_lfo2(ui_kind(), ui_slot(), &type, &o, p)) { *on = o; return type; }
    *on = 0; return (int)LFO_OFF;
}
extern "C" void coop_lfo2_mid_params(int type, int16_t *p)        /* the middle of the type's ranges */
{
    for (int k = 0; k < 8; k++) {
        int16_t lo = LFO_RANGES[(type * 8 + k) * 3], hi = LFO_RANGES[(type * 8 + k) * 3 + 1];
        p[k] = (int16_t)(lo < hi ? (lo + hi) / 2 : 0);
    }
}
extern "C" void coop_lfo2_set_type(int type)                     /* the list's column: 0 tremolo, 1 value, 2 random, 7 off: onto the shown sound */
{
    if (type < 0 || (type > (int)LFO_RANDOM && type != (int)LFO_OFF)) return;
    int cur, on; int16_t p[8];
    int had = coop_sound_lfo2(ui_kind(), ui_slot(), &cur, &on, p);
    if (type == (int)LFO_OFF) { if (had) coop_sound_set_lfo2(ui_kind(), ui_slot(), cur, 0, p); return; }
    if (!had || cur != type) coop_lfo2_mid_params(type, p);
    coop_sound_set_lfo2(ui_kind(), ui_slot(), type, 1, p);
}
extern "C" void coop_lfo2_set_sound(int type, int on, const int16_t *p)   /* the LFO 2 page (coop_ui.cc): the shown sound's words */
{
    coop_sound_set_lfo2(ui_kind(), ui_slot(), type, on, p);
}
extern "C" const int16_t *coop_lfo2_ranges(int type) { return (const int16_t *)(LFO_RANGES + type * 24); }   /* {min, max, scale} x 8 */
extern "C" void coop_lfo2_words_get(uint8_t *out17)             /* type, then the four words little-endian */
{
    out17[0] = (uint8_t)g_coop_lfo2_type;
    uint32_t w[4] = { g_coop_lfo2_p01, g_coop_lfo2_p23, g_coop_lfo2_p45, g_coop_lfo2_p67 };
    for (int i = 0; i < 4; i++) for (int b = 0; b < 4; b++) out17[1 + 4 * i + b] = (uint8_t)(w[i] >> (8 * b));
}
extern "C" void coop_lfo2_words_set(const uint8_t *in17)
{
    uint32_t w[4];
    for (int i = 0; i < 4; i++) { w[i] = 0; for (int b = 0; b < 4; b++) w[i] |= (uint32_t)in17[1 + 4 * i + b] << (8 * b); }
    int type = in17[0];
    if (type > (int)LFO_RANDOM && type != (int)LFO_OFF) type = (int)LFO_OFF;
    g_coop_lfo2_p01 = w[0]; g_coop_lfo2_p23 = w[1]; g_coop_lfo2_p45 = w[2]; g_coop_lfo2_p67 = w[3];
    g_coop_lfo2_type = (uint32_t)type;
    g_coop_lfo2_active = 1;
}
