/*
 * coop-fw stereo instrument bus -- firmware payload (Phase 2 milestone C1/C2, docs/stereo-bus.md).
 *
 * Three L1 sites are rewritten by tools/l1_stereo_insert.py (stock bytes checked first):
 *   - synth render call 0xffa00d22..0xffa00d2a: `CALL (P2)` becomes a CALL to an L1 trampoline that
 *     loads P2 = *(vtable+0x14) and jumps to coop_synth_render_thunk (stereo_thunks.S). The thunk runs
 *     the stock render, then coop_synth_voice_hook(R7 = voice index), then returns to the stock
 *     per-voice sum, which still runs unchanged into the mono bus.
 *   - drum render call 0xffa008fc..0xffa00906: same shape with *(vtable+0x24), R7 = 12 - voice.
 *   - mix loop B 0xffa00fae: the LSETUP becomes CALL trampoline -> coop_mix_thunk -> coop_mix_entry,
 *     followed by a JUMP.S over the stock loop body. The thunk hands over R0 (gain), R1 (ramp),
 *     R2 (step) and P1 (ramp state) exactly as loop B had them.
 *
 * Addresses below are [C] from the decrypted R246 disassembly and the op1emu write tracer
 * (docs/stereo-bus.md "The stock pipeline"); the comments cite the instruction that establishes each.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_stereo.h"

/* --- stock buffers and words (all [C], write-traced 2026-11-02) --- */
#define VOICE_OUT   ((const int16_t *)0xff80113cu)   /* render out (R1 of render; 0xffa009de, 0xffa0068c) */
#define VOICE_ENV   ((const int16_t *)0xff801d50u)   /* per-voice envelope, 128 x int16 (0xffa009d6) */
#define VOICE_GAIN  ((const volatile int16_t *)0x01b4ef6eu) /* W[P4]/W[P5] sum gain (0xffa00a5e, 0xffa00742) */
#define MONO_BUS64  ((const int16_t *)0xff800bb4u)   /* DC-blocked 64-sample mono bus loop B reads (P3, 0xffa00f00) */
#define HB_COEFS    ((const int16_t *)0x0106a6f0u)   /* half-band even taps (P4, 0xffa00648) */
#define HB_RAMP_W   ((const volatile uint32_t *)0xff80107cu) /* lo = ramp, hi = step (P5, 0xffa005ee) */
#define DC_COEF_W   ((const volatile int32_t *)0x01bab53cu)  /* DC blocker coefficient (P1, 0x01967502) */
#define MBUS_L      ((volatile int16_t *)0xff800d34u)  /* master pair (I2/I0 of loop B) */
#define MBUS_R      ((volatile int16_t *)0xff800db4u)

/* --- controls (in .data so inject_app --set-word can set them per image) --- */
__attribute__((section(".data"))) volatile uint32_t g_coop_spread = 0;      /* Q15: 0 = stock mono, 32767 = full field */
__attribute__((section(".data"))) volatile uint32_t g_coop_stereo_enable = 1; /* 0 = hooks do nothing (mix stays stock) */
/* Emulator-only test aid: the stock SPORT conversion sums e34/f34 to mono (speaker path, 0xffa01698)
 * unless the output-mode byte at 0x018b621d is nonzero (headphones/line out: the stereo paths at
 * 0xffa0142a / 0xffa015f0; the test at 0xffa0140c). op1emu never sets it, so a stereo check in the
 * emulator forces it. Leave 0 for the device: the jack detection decides there. */
__attribute__((section(".data"))) volatile uint32_t g_coop_force_stereo_out = 0;
#define OUTPUT_MODE_B ((volatile uint8_t *)0x018b621du)

/* --- state (region-A .bss) --- */
static int32_t g_sdL[COOP_VBLK], g_sdR[COOP_VBLK];     /* difference buses, 88.2 kHz */
static int16_t g_decL[COOP_MBLK], g_decR[COOP_MBLK];   /* after decimation + DC block */
static coop_hb_state g_hbL, g_hbR;
static coop_dc_state g_dcL, g_dcR;
static coop_spread g_pan_synth, g_pan_drum;
static uint32_t g_rendered_prev, g_rendered_cur;       /* bits 0-5 synth voices, 16-27 drum voices */
static uint32_t g_hb_ramp;                             /* stock ramp word as of this block, before the stock decimator */
static uint32_t g_side_tail;                           /* blocks left to keep decimating after the last panned voice */
static int g_side_dirty;                               /* dL/dR hold data that must be cleared next block */
static int g_inited;

/* liveness / perf counters (read from the emulator or a probe) */
volatile uint32_t g_stereo_blocks, g_stereo_hooks, g_stereo_mixes, g_stereo_side_blocks, g_stereo_note_ons;
volatile uint32_t g_stereo_hook_cycles_max, g_stereo_mix_cycles_max;

extern "C" int16_t *g_mix_ramp_ptr;                    /* P1 of loop B, stored by coop_mix_thunk */
int16_t *g_mix_ramp_ptr;

static inline uint32_t coop_cycles(void)
{
    uint32_t v;
    __asm__ __volatile__("%0 = CYCLES;" : "=d"(v));
    return v;
}

static void stereo_init(void)
{
    coop_hb_init(&g_hbL); coop_hb_init(&g_hbR);
    coop_dc_init(&g_dcL); coop_dc_init(&g_dcR);
    coop_spread_init(&g_pan_synth, 0);
    coop_spread_init(&g_pan_drum, 0);
    g_pan_drum.rng = 0x7F4A7C15u;                      /* a different stream from the synth's */
    g_inited = 1;
}

/* Called by CoopAudioCb before the stock callback: a block starts. */
extern "C" void coop_stereo_block_start(void)
{
    if (!g_inited) stereo_init();
    if (g_coop_force_stereo_out) *OUTPUT_MODE_B = 1;
    g_hb_ramp = *HB_RAMP_W;
    g_rendered_prev = g_rendered_cur;
    g_rendered_cur = 0;
    if (g_side_dirty) {
        for (int i = 0; i < COOP_VBLK; i++) { g_sdL[i] = 0; g_sdR[i] = 0; }
        g_side_dirty = 0;
    }
    int16_t spread = (int16_t)(g_coop_spread > 32767u ? 32767u : g_coop_spread);
    coop_spread_set(&g_pan_synth, spread);
    coop_spread_set(&g_pan_drum, spread);
    g_stereo_blocks++;
}

/* C2: the Core's note-on dispatch (voice loop, 0xffa00e34 `CALL (P0)`, P0 = *(vtable+0x1c)) runs
 * through coop_synth_noteon_thunk, which calls this after the engine's note_on with R7 = the voice
 * index: the voice draws a fresh random pan scaled by the spread. Retriggers of a sounding voice get a
 * new position too, which the rendered-last-block heuristic (kept for drums) cannot see. */
extern "C" void coop_lfo2_trigger_synth(uint32_t voice);   /* second LFO slot (lfo2.cc) */
extern "C" void coop_lfo2_trigger_drum(uint32_t voice);

extern "C" void coop_synth_note_on_hook(uint32_t voice, uint32_t dispatched)
{
    if (voice >= COOP_SYNTH_VOICES || !dispatched) return;
    if (!g_inited) stereo_init();
    g_stereo_note_ons++;
    coop_spread_note_on(&g_pan_synth, (int)voice);
    coop_lfo2_trigger_synth(voice);
}

/* Common per-voice path: bookkeeping, drum note-on heuristic (a drum voice that was not rendered last
 * block gets a fresh random pan scaled by the spread; synth voices get theirs from the note-on hook),
 * then the difference accumulate if the voice is off centre. */
static void voice_hook(coop_spread *pans, int voice, uint32_t bit, int is_drum)
{
    if (!g_coop_stereo_enable) return;
    uint32_t t0 = coop_cycles();
    g_stereo_hooks++;
    g_rendered_cur |= bit;
    if (is_drum && !(g_rendered_prev & bit)) {   /* a drum voice starting: its note-on, as far as we can see */
        coop_spread_note_on(pans, voice);
        coop_lfo2_trigger_drum(voice);
    }
    g_hb_ramp = *HB_RAMP_W;                            /* the stock step for this block is set by now */
    coop_pan_delta d = coop_pan_deltas(pans->pan[voice]);
    if (d.dL == 0 && d.dR == 0) return;                /* centred: nothing beyond the stock sum */
    int16_t gain = *VOICE_GAIN;
    if (is_drum) coop_side_accum_drum(g_sdL, g_sdR, VOICE_OUT, gain, d);
    else         coop_side_accum_synth(g_sdL, g_sdR, VOICE_OUT, VOICE_ENV, gain, d);
    g_side_dirty = 1;
    g_side_tail = 4;
    uint32_t dt = coop_cycles() - t0;
    if (dt > g_stereo_hook_cycles_max) g_stereo_hook_cycles_max = dt;
}

/* R7 at the synth render call = voice index 0..5 (0xffa009c2 zeroes it, 0xffa00da0 increments). */
extern "C" void coop_synth_voice_hook(uint32_t r7)
{
    if (r7 >= COOP_SYNTH_VOICES) return;
    voice_hook(&g_pan_synth, (int)r7, 1u << r7, 0);
}

/* R7 at the drum render call counts 12 down to 1 (0xffa0072a, 0xffa00946). */
extern "C" void coop_drum_voice_hook(uint32_t r7)
{
    if (r7 == 0 || r7 > COOP_DRUM_VOICES) return;
    int voice = COOP_DRUM_VOICES - (int)r7;
    voice_hook(&g_pan_drum, voice, 1u << (16 + voice), 1);
}

/* Stands in for mix loop B. R0.L = gain, R1.L = ramp (== *g_mix_ramp_ptr), R2.L = step. */
extern "C" void coop_mix_entry(uint32_t r0, uint32_t r1, uint32_t r2)
{
    (void)r1;
    uint32_t t0 = coop_cycles();
    int16_t gain = (int16_t)r0, step = (int16_t)r2;
    int16_t *ramp = g_mix_ramp_ptr;
    g_stereo_mixes++;
    if (g_side_tail) {
        int16_t hr = (int16_t)g_hb_ramp, hs = (int16_t)(g_hb_ramp >> 16);
        coop_hb_decimate(&g_hbL, g_sdL, g_decL, HB_COEFS, hr, hs);
        coop_hb_decimate(&g_hbR, g_sdR, g_decR, HB_COEFS, hr, hs);
        int32_t c = *DC_COEF_W;
        coop_dc_block(&g_dcL, g_decL, g_decL, c, COOP_MBLK);
        coop_dc_block(&g_dcR, g_decR, g_decR, c, COOP_MBLK);
        coop_mix_b(MBUS_L, MBUS_R, MONO_BUS64, g_decL, g_decR, gain, ramp, step);
        g_stereo_side_blocks++;
        if (--g_side_tail == 0) {                      /* the rings are flushed; drop the DC tail */
            coop_hb_init(&g_hbL); coop_hb_init(&g_hbR);
            coop_dc_init(&g_dcL); coop_dc_init(&g_dcR);
        }
    } else {
        coop_mix_b_mono(MBUS_L, MBUS_R, MONO_BUS64, gain, ramp, step);
    }
    uint32_t dt = coop_cycles() - t0;
    if (dt > g_stereo_mix_cycles_max) g_stereo_mix_cycles_max = dt;
}
