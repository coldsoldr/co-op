/*
 * coop-fw F1 / F2 (2026-11-07): extra synth voices outside the stock core, for the tracker (docs/tracker.md).
 *
 * The core renders ONE engine family (its play mode); other engines sounding at the same time are rendered by coop.
 * Each tracker channel owns a slot here with one voice object per extra engine kind, built on the main thread at
 * the tape mount with the stock's own constructors, and rendered once a block from coop's instrument stage
 * (coop_instr_stage, before the pair is decimated) with the stock's render, then placed into the stereo pair like a
 * stock voice (coop_pair_accum_synth: out * env * gain, centred). The ADSR the stock applies after render is coop's
 * own here: a linear attack and release in Q15.
 *
 * What each engine reads outside its object (OP1_vdk.dis, 2026-11-07), and what coop does about it:
 *   all     the per-sample pitch words at L1 0xFF801950 (128 x uint32, f / 88200 * 2^32): filled per voice, the
 *           stock's words saved around the block and put back; the sine table: the package retargets it at the L1
 *           copy in every block; L1 scratch buffers (0xFF801B50, 0xFF801E50, 0xFF801F50): free at the stage.
 *   fm      the shared parameter block through the pointer at 0xFF906F6C (the engine's setup 0x019A3D84 fills it
 *           from the FM patch words at 0x01D05880): one coop block, the pointer swapped in around the calls.
 *   string  its 16 KB delay line at [+0x14] (the stock gives voice i 0x010225EC + i * 0x4000): a coop buffer each;
 *           the LCG word at 0xFF802BC8 is shared noise state, harmless.
 *   digital nothing else.
 *   dsynth  per-voice tables at 0x01CEB8C8 + i * 128 and 0x01CEBBC8 by the index at [+0x14] (its setup
 *           0x0197E0B0(this, i)): the slot number is the index, which CLASHES with the live engine's voice i when
 *           the live synth is DSynth (an F2 probe limit); it also reads the voice envelope at 0xFF801D50, so coop
 *           writes its own envelope there before the render (saved around the block too).
 * Threads: the objects are built on the main thread (constructors allocate); the tracker's main-loop dispatch posts
 * a note word per slot; the audio callback applies it and renders. Cost per block in g_xvoice_cycles_max (the stats
 * "stages" line, xv_max). Fixed point only, no libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_stereo.h"

#define XSLOTS 6
enum { XE_FM, XE_STRING, XE_DIGITAL, XE_DSYNTH, XE_N };
typedef void *(*ctor_fn)(void *);
typedef void (*render_fn)(void *, int16_t *, const int16_t *);
typedef void (*note_fn)(void *, const int16_t *);
typedef void (*setup_fn)(void *, int);
struct xeng { uint32_t ctor, size, render, noteon, noteoff, setup; };
static const struct xeng engs[XE_N] = {
    { 0x019A21D8u, 64, 0x019A23D4u, 0x019A2248u, 0,           0 },            /* fm (Fm.cpp) */
    { 0x01A4EF38u, 32, 0x01A4F098u, 0x01A4EFC8u, 0x01A4EFAEu, 0x01A4EF64u },  /* string (St.cpp): setup(this, i) sets the buffer */
    { 0x01A51F28u, 32, 0x01A51F7Au, 0x01A521E2u, 0,           0 },            /* digital (T10.cpp) */
    { 0x0197E058u, 72, 0x0197E1D8u, 0x0197E0EEu, 0x0197E168u, 0x0197E0B0u },  /* dsynth: setup(this, i) sets the index */
};
#define FM_SETUP     ((void (*)(void))0x019A3D84u)
#define FM_PARAM_PTR (*(volatile uint32_t *)0xFF906F6Cu)
#define PITCH        ((volatile uint32_t *)0xFF801950u)
#define VENV         ((volatile int16_t *)0xFF801D50u)
#define OBJ_MAX      128
#define STRING_BUF   0x4000u
#define ATTACK_STEP  186                                                      /* Q15 a sample: ~2 ms at 88.2 kHz */
#define RELEASE_STEP 3                                                        /* ~120 ms */
#define INC_A4       21426749u                                                /* 440 / 88200 * 2^32 */

extern "C" void *coop_l1_alloc(uint32_t bytes);                               /* l1_pool.cc; 0 when the pool is not taken */

volatile uint32_t g_xvoice_ready, g_xvoice_notes, g_xvoice_blocks, g_xvoice_cycles_max, g_xvoice_cycles_sum, g_xvoice_max_sounding, g_xvoice_l1;
struct xslot {
    void *obj[XE_N];
    int eng, gate, sounding;
    int32_t env_q15;
    int16_t gain;
    uint32_t inc, req_seen;
};
static struct xslot s_slot[XSLOTS];
static uint32_t s_obj_sdram[XSLOTS][XE_N][OBJ_MAX / 4];
static uint32_t s_string_buf[XSLOTS][STRING_BUF / 4];
static uint32_t s_fm_params[128 / 4];
static int16_t s_out[COOP_VBLK] __attribute__((aligned(4)));
static int16_t s_env[COOP_VBLK] __attribute__((aligned(4)));
static uint32_t s_pitch_save[COOP_VBLK];
static int16_t s_venv_save[COOP_VBLK];
static volatile uint32_t s_req[XSLOTS];                                       /* seq << 24 | eng << 20 | on << 16 | vel << 8 | note */
/* the render's parameter pointer: the stock passes the slot's 8 knob words (dsynth reads +0x8..+0xc). The ranges
 * differ per engine: string's knob 0 >> 7 indexes a 128-entry table from 128 down (0..0x3FFF valid), FM takes
 * fract16 (0x4000 worked in F1). Mid values per engine until the channel has a patch. */
static const int16_t s_knobs[XE_N][8] = {
    { 0x4000, 0x4000, 0x4000, 0x4000, 0, 0, 0, 0 },
    { 0x2000, 0x2000, 0x2000, 0x2000, 0, 0, 0, 0 },
    { 0x4000, 0x4000, 0x4000, 0x4000, 0, 0, 0, 0 },
    { 0x2000, 0x2000, 0x2000, 0x2000, 0x2000, 0x2000, 0x2000, 0x2000 },
};
volatile uint32_t g_xvoice_peak[XE_N];                                        /* the loudest |sample| each engine made: is it audible at all? */
static const uint32_t semi_q24[12] = {                                        /* 2^(k/12), Q24 */
    16777216u, 17774841u, 18831788u, 19951586u, 21137968u, 22394886u, 23726566u, 25137472u, 26632357u, 28216230u, 29894287u, 31671998u };

static inline uint32_t cycles(void) { uint32_t v; __asm__ __volatile__("%0 = CYCLES;" : "=d"(v)); return v; }
static uint32_t inc_of(int note)
{
    int d = note - 69, oct = 0;
    while (d < 0) { d += 12; oct--; }
    while (d >= 12) { d -= 12; oct++; }
    uint64_t v = ((uint64_t)INC_A4 * semi_q24[d]) >> 24;
    if (oct > 0) v <<= oct; else if (oct < 0) v >>= -oct;
    return (uint32_t)v;
}

/* main thread, once (the tape mount): every slot's objects */
extern "C" void coop_xvoice_boot(void)
{
    if (g_xvoice_ready) return;
    for (int i = 0; i < 128 / 4; i++) s_fm_params[i] = 0;
    uint32_t saved = FM_PARAM_PTR;                                            /* the FM setup fills *ptr: point it at ours */
    FM_PARAM_PTR = (uint32_t)(uintptr_t)s_fm_params;
    FM_SETUP();
    FM_PARAM_PTR = saved;
    for (int s = 0; s < XSLOTS; s++) {
        for (int e = 0; e < XE_N; e++) {
            void *o = coop_l1_alloc(engs[e].size);
            if (o) g_xvoice_l1++; else o = s_obj_sdram[s][e];
            s_slot[s].obj[e] = o;
            ((ctor_fn)(uintptr_t)engs[e].ctor)(o);
            if (engs[e].setup) ((setup_fn)(uintptr_t)engs[e].setup)(o, s);
            if (e == XE_STRING) *(uint32_t *)((uint8_t *)o + 0x14) = (uint32_t)(uintptr_t)s_string_buf[s];   /* our delay line, not the stock's */
        }
        s_slot[s].eng = -1;
    }
    g_xvoice_ready = 1;
}

/* the tracker (main loop): slot = the channel, eng 0..XE_N-1, a note on or off, vel 1..127 */
extern "C" void coop_xvoice_note(int slot, int eng, int note, int vel, int on)
{
    static uint32_t seq;
    if (slot < 0 || slot >= XSLOTS || eng < 0 || eng >= XE_N) return;
    seq = (seq + 1) & 0xFF;
    s_req[slot] = (seq << 24) | ((uint32_t)eng << 20) | ((uint32_t)(on ? 1 : 0) << 16) | ((uint32_t)(vel & 127) << 8) | (uint32_t)(note & 127);
}
extern "C" int coop_xvoice_engines(void) { return XE_N; }

/* audio callback, from coop_instr_stage before the pair is decimated: the pending notes, then the sounding slots */
extern "C" void coop_xvoice_render(int32_t *L, int32_t *R)
{
    if (!g_xvoice_ready) return;
    int any = 0;
    for (int s = 0; s < XSLOTS; s++) {
        struct xslot *x = &s_slot[s];
        uint32_t req = s_req[s];
        if (req != x->req_seen) {
            x->req_seen = req;
            int on = (req >> 16) & 1, note = req & 127, vel = (req >> 8) & 127, eng = (req >> 20) & 15;
            if (on) {
                if (x->sounding && x->eng >= 0 && x->eng != eng && engs[x->eng].noteoff)   /* another engine takes the slot: the old one lets go */
                    ((void (*)(void *))(uintptr_t)engs[x->eng].noteoff)(x->obj[x->eng]);
                x->eng = eng;
                x->inc = inc_of(note);
                x->gain = (int16_t)(vel * 258 > 32767 ? 32767 : vel * 258);
                ((note_fn)(uintptr_t)engs[eng].noteon)(x->obj[eng], s_knobs[eng]);
                x->gate = 1; x->sounding = 1;
                g_xvoice_notes++;
            } else {
                x->gate = 0;
                if (x->eng >= 0 && engs[x->eng].noteoff) ((void (*)(void *))(uintptr_t)engs[x->eng].noteoff)(x->obj[x->eng]);
            }
        }
        if (x->sounding) any++;
    }
    if (!any) return;
    uint32_t t0 = cycles();
    for (int i = 0; i < COOP_VBLK; i++) { s_pitch_save[i] = PITCH[i]; s_venv_save[i] = VENV[i]; }
    uint32_t saved_fm = FM_PARAM_PTR;
    FM_PARAM_PTR = (uint32_t)(uintptr_t)s_fm_params;
    int sounding = 0;
    for (int s = 0; s < XSLOTS; s++) {
        struct xslot *x = &s_slot[s];
        if (!x->sounding || x->eng < 0) continue;
        sounding++;
        int32_t e = x->env_q15;
        for (int i = 0; i < COOP_VBLK; i++) {
            if (x->gate) { e += ATTACK_STEP; if (e > 32767) e = 32767; }
            else { e -= RELEASE_STEP; if (e < 0) e = 0; }
            s_env[i] = (int16_t)e;
        }
        x->env_q15 = e;
        for (int i = 0; i < COOP_VBLK; i++) PITCH[i] = x->inc;
        if (x->eng == XE_DSYNTH) for (int i = 0; i < COOP_VBLK; i++) VENV[i] = s_env[i];   /* dsynth reads the voice envelope itself */
        ((render_fn)(uintptr_t)engs[x->eng].render)(x->obj[x->eng], s_out, s_knobs[x->eng]);
        { uint32_t pk = g_xvoice_peak[x->eng]; for (int i = 0; i < COOP_VBLK; i++) { int32_t a = s_out[i] < 0 ? -s_out[i] : s_out[i]; if ((uint32_t)a > pk) pk = (uint32_t)a; } g_xvoice_peak[x->eng] = pk; }
        coop_pan_delta d = { 0, 0 };
        coop_pair_accum_synth(L, R, s_out, s_env, x->gain, d);
        if (!x->gate && e == 0) x->sounding = 0;
    }
    FM_PARAM_PTR = saved_fm;
    for (int i = 0; i < COOP_VBLK; i++) { PITCH[i] = s_pitch_save[i]; VENV[i] = s_venv_save[i]; }
    g_xvoice_blocks++;
    if ((uint32_t)sounding > g_xvoice_max_sounding) g_xvoice_max_sounding = (uint32_t)sounding;
    uint32_t dt = cycles() - t0;
    if (dt > g_xvoice_cycles_max) g_xvoice_cycles_max = dt;
    g_xvoice_cycles_sum += dt;
}
