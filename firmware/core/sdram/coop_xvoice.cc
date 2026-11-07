/*
 * coop-fw F1 (2026-11-07): one extra synth voice outside the stock core, for the tracker (docs/tracker.md "F1").
 *
 * The core renders ONE engine family (its play mode); a second engine sounding at the same time has to be rendered
 * by coop. This is the feasibility probe the handover asked for: a stock FM voice (Fm.cpp, engine id 0) built in
 * coop memory with the stock's own constructor (0x019A21D8, 60 bytes; vtable 0x0190FF84: render +0x14 = 0x019A23D4,
 * note on +0x1c = 0x019A2248, note off = RTS), rendered once a block from coop's instrument stage (coop_instr_stage,
 * before the pair is decimated) and placed into the stereo pair like any stock voice (coop_pair_accum_synth: out *
 * env * gain, centred). The FM render reads three things outside its object: the per-sample pitch words at L1
 * 0xFF801950 (128 x uint32: f / 88200 * 2^32), the engine's shared parameter block through the pointer at
 * 0xFF906F6C (filled by the engine's setup 0x019A3D84 from the FM patch words at 0x01D05880), and the sine table
 * (retargeted at its L1 copy by the package, in every block). Both the pitch words and the pointer are swapped in
 * around the call and put back, so the live engine never sees them changed. The ADSR the stock applies after render
 * is coop's own here: a linear attack and release in Q15.
 *
 * Threads: the voice object is built on the main thread at the tape mount (the constructor allocates: 0x01AA3862);
 * the tracker's main-loop dispatch posts a note word; the audio callback (the stage) applies it and renders. The cost
 * is measured per block (g_xvoice_cycles_max, the stats "stages" line as xv_max). Fixed point only, no libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_stereo.h"

#define FM_CTOR    ((void *(*)(void *))0x019A21D8u)
#define FM_SETUP   ((void (*)(void))0x019A3D84u)                            /* copies the FM patch words into *FM_PARAM_PTR */
#define FM_RENDER  ((void (*)(void *, int16_t *, const int16_t *))0x019A23D4u)
#define FM_NOTEON  ((void (*)(void *))0x019A2248u)
#define FM_PARAM_PTR (*(volatile uint32_t *)0xFF906F6Cu)
#define PITCH      ((volatile uint32_t *)0xFF801950u)
#define OBJ_BYTES  64
#define PARAM_BYTES 128
#define ATTACK_STEP 186                                                      /* Q15 a sample: ~2 ms at 88.2 kHz */
#define RELEASE_STEP 3                                                       /* ~120 ms */
#define INC_A4     21426749u                                                 /* 440 / 88200 * 2^32 */

extern "C" void *coop_l1_alloc(uint32_t bytes);                              /* l1_pool.cc; 0 when the pool is not taken */

volatile uint32_t g_xvoice_ready, g_xvoice_notes, g_xvoice_blocks, g_xvoice_cycles_max, g_xvoice_cycles_sum;
static uint32_t s_obj_sdram[OBJ_BYTES / 4] __attribute__((aligned(4)));
static uint32_t s_param_sdram[PARAM_BYTES / 4] __attribute__((aligned(4)));
static void *s_obj;
static uint32_t s_params;
static int16_t s_out[COOP_VBLK] __attribute__((aligned(4)));
static int16_t s_env[COOP_VBLK] __attribute__((aligned(4)));
static uint32_t s_pitch_save[COOP_VBLK];
static volatile uint32_t s_req;                                              /* seq << 24 | on << 16 | vel << 8 | note; the main loop writes */
static uint32_t s_req_seen;
static int32_t s_env_q15, s_gate, s_sounding;
static int16_t s_gain;
static uint32_t s_inc;
static const int16_t s_knobs[4] = { 0x4000, 0x4000, 0x4000, 0x4000 };        /* the render's four encoder values (fract16) */
static const uint32_t semi_q24[12] = {                                       /* 2^(k/12), Q24 */
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

/* main thread, once (the tape mount): the object and its parameter block */
extern "C" void coop_xvoice_boot(void)
{
    if (g_xvoice_ready) return;
    void *o = coop_l1_alloc(OBJ_BYTES);
    s_obj = o ? o : (void *)s_obj_sdram;
    void *p = coop_l1_alloc(PARAM_BYTES);
    s_params = (uint32_t)(uintptr_t)(p ? p : (void *)s_param_sdram);
    for (int i = 0; i < PARAM_BYTES / 4; i++) ((uint32_t *)(uintptr_t)s_params)[i] = 0;
    FM_CTOR(s_obj);
    uint32_t saved = FM_PARAM_PTR;                                           /* the engine's setup fills *ptr: point it at ours */
    FM_PARAM_PTR = s_params;
    FM_SETUP();
    FM_PARAM_PTR = saved;
    g_xvoice_ready = 1;
}

/* the tracker (main loop): a note on or off; vel 1..127 */
extern "C" void coop_xvoice_note(int note, int vel, int on)
{
    static uint32_t seq;
    seq = (seq + 1) & 0xFF;
    s_req = (seq << 24) | ((uint32_t)(on ? 1 : 0) << 16) | ((uint32_t)(vel & 127) << 8) | (uint32_t)(note & 127);
}
extern "C" int coop_xvoice_sounding(void) { return s_sounding; }

/* audio callback, from coop_instr_stage before the pair is decimated: the pending note, then the block */
extern "C" void coop_xvoice_render(int32_t *L, int32_t *R)
{
    if (!g_xvoice_ready) return;
    uint32_t req = s_req;
    if (req != s_req_seen) {
        s_req_seen = req;
        int on = (req >> 16) & 1, note = req & 127, vel = (req >> 8) & 127;
        if (on) {
            s_inc = inc_of(note);
            s_gain = (int16_t)(vel * 258 > 32767 ? 32767 : vel * 258);
            FM_NOTEON(s_obj);
            s_gate = 1; s_sounding = 1;
            g_xvoice_notes++;
        } else s_gate = 0;
    }
    if (!s_sounding) return;
    uint32_t t0 = cycles();
    for (int i = 0; i < COOP_VBLK; i++) s_pitch_save[i] = PITCH[i];
    for (int i = 0; i < COOP_VBLK; i++) PITCH[i] = s_inc;
    uint32_t saved = FM_PARAM_PTR;
    FM_PARAM_PTR = s_params;
    FM_RENDER(s_obj, s_out, s_knobs);
    FM_PARAM_PTR = saved;
    for (int i = 0; i < COOP_VBLK; i++) PITCH[i] = s_pitch_save[i];
    int32_t e = s_env_q15;
    for (int i = 0; i < COOP_VBLK; i++) {
        if (s_gate) { e += ATTACK_STEP; if (e > 32767) e = 32767; }
        else { e -= RELEASE_STEP; if (e < 0) e = 0; }
        s_env[i] = (int16_t)e;
    }
    s_env_q15 = e;
    if (!s_gate && e == 0) s_sounding = 0;
    coop_pan_delta d = { 0, 0 };
    coop_pair_accum_synth(L, R, s_out, s_env, s_gain, d);
    g_xvoice_blocks++;
    uint32_t dt = cycles() - t0;
    if (dt > g_xvoice_cycles_max) g_xvoice_cycles_max = dt;
    g_xvoice_cycles_sum += dt;
}
