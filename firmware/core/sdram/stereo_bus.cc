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
#include "coop_l1.h"

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

/* --- state: the per-block hot data in the L1 pool (coop_l1.h, T4), with region-A .bss as the fallback --- */
/* P2b: one side pair per instrument (0 synth, 1 drum), so an insert at a mono FX position sees its own instrument in
 * stereo; the mix and the record tap take the sum. */
#define INSTR 2
static int32_t s_sdL[INSTR][COOP_VBLK], s_sdR[INSTR][COOP_VBLK];
static int16_t s_decL[INSTR][COOP_MBLK], s_decR[INSTR][COOP_MBLK];
static int16_t s_sumL[COOP_MBLK], s_sumR[COOP_MBLK];
static coop_hb2_state s_hb2[INSTR];
static coop_dc_state s_dcL[INSTR], s_dcR[INSTR];
static coop_spread s_pan_synth, s_pan_drum;
static int16_t s_sin[65];
static int32_t *g_sdL[INSTR] = { s_sdL[0], s_sdL[1] }, *g_sdR[INSTR] = { s_sdR[0], s_sdR[1] };   /* difference buses, 88.2 kHz */
static int16_t *g_idecL[INSTR] = { s_decL[0], s_decL[1] }, *g_idecR[INSTR] = { s_decR[0], s_decR[1] };   /* per instrument, decimated */
static int16_t *g_decL = s_sumL, *g_decR = s_sumR;     /* the sum the mix and the record tap take */
static coop_hb2_state *g_hb2[INSTR] = { &s_hb2[0], &s_hb2[1] };
static coop_dc_state *g_dcL[INSTR] = { &s_dcL[0], &s_dcL[1] }, *g_dcR[INSTR] = { &s_dcR[0], &s_dcR[1] };
static coop_spread *g_pan_synth = &s_pan_synth, *g_pan_drum = &s_pan_drum;
static const int16_t *g_sin = coop_sin_q15;            /* the pan law's quarter sine, copied into L1 */
volatile uint32_t g_stereo_l1;                         /* 1 = the hot state is in L1 */
static uint32_t g_rendered_prev, g_rendered_cur;       /* bits 0-5 synth voices, 16-27 drum voices */
static uint32_t g_hb_ramp;                             /* stock ramp word as of this block, before the stock decimator */
static uint32_t g_side_tail[INSTR];                    /* blocks left to keep decimating after the last panned voice */
static int g_side_dirty[INSTR];                        /* dL/dR hold data that must be cleared next block */
static int g_side_dec_done_i[INSTR], g_side_live[INSTR];   /* per instrument: decimated this block; carries a side */
static int g_sum_done;                                 /* the summed pair is built for this block */
static int g_inited;
static int g_side_this_block;                          /* loop B added decimated difference buses this block */

/* liveness / perf counters (read from the emulator or a probe) */
volatile uint32_t g_stereo_blocks, g_stereo_hooks, g_stereo_mixes, g_stereo_side_blocks, g_stereo_note_ons;
volatile uint32_t g_stereo_hook_cycles_max, g_stereo_mix_cycles_max, g_stereo_mix_cycles_win;
volatile uint32_t g_stereo_hook_cycles_win, g_stereo_hook_cycles_sum_win, g_stereo_mix_cycles_sum_win;   /* stats v3 */

extern "C" int16_t *g_mix_ramp_ptr;                    /* P1 of loop A / B, stored by coop_mix_thunk */
int16_t *g_mix_ramp_ptr;
extern "C" uint32_t g_mix_site;                         /* the return address of the CALL at the site (loop A 0xffa00f30, loop B 0xffa00fb2) */
uint32_t g_mix_site;
extern "C" int16_t *g_mix_a_rec;                        /* loop A: [SP+0x3c] at the site = its ramp/meter record (P1 there is the loop count) */
int16_t *g_mix_a_rec;
extern "C" uint32_t g_mix_p0_out;                       /* P0 the thunk restores: the stock loops leave P0 untouched and the code after them uses it */
uint32_t g_mix_p0_out;
static int g_side_dec_done;                            /* the difference buses are decimated once per block (both loops use them) */
#define MIX_A_SITE  0xffa00f30u
#define VOICE_GAIN_W ((const volatile int16_t *)0x01b4ef6eu)

static inline uint32_t coop_cycles(void)
{
    uint32_t v;
    __asm__ __volatile__("%0 = CYCLES;" : "=d"(v));
    return v;
}

static void stereo_init(void)
{
    if (coop_l1_state() == 1) {
        for (int k = 0; k < INSTR; k++) {
            g_sdL[k] = (int32_t *)coop_l1_or(sizeof s_sdL[k], s_sdL[k]);
            g_sdR[k] = (int32_t *)coop_l1_or(sizeof s_sdR[k], s_sdR[k]);
            g_idecL[k] = (int16_t *)coop_l1_or(sizeof s_decL[k], s_decL[k]);
            g_idecR[k] = (int16_t *)coop_l1_or(sizeof s_decR[k], s_decR[k]);
            g_hb2[k] = (coop_hb2_state *)coop_l1_or(sizeof s_hb2[k], &s_hb2[k]);
            g_dcL[k] = (coop_dc_state *)coop_l1_or(sizeof s_dcL[k], &s_dcL[k]);
            g_dcR[k] = (coop_dc_state *)coop_l1_or(sizeof s_dcR[k], &s_dcR[k]);
        }
        g_decL = (int16_t *)coop_l1_or(sizeof s_sumL, s_sumL);
        g_decR = (int16_t *)coop_l1_or(sizeof s_sumR, s_sumR);
        g_pan_synth = (coop_spread *)coop_l1_or(sizeof s_pan_synth, &s_pan_synth);
        g_pan_drum = (coop_spread *)coop_l1_or(sizeof s_pan_drum, &s_pan_drum);
        int16_t *sin_l1 = (int16_t *)coop_l1_alloc(sizeof s_sin);
        if (sin_l1) { for (int i = 0; i < 65; i++) sin_l1[i] = coop_sin_q15[i]; g_sin = sin_l1; }
        g_stereo_l1 = g_sdL[0] != s_sdL[0];
    }
    for (int k = 0; k < INSTR; k++) { coop_hb2_init(g_hb2[k]); coop_dc_init(g_dcL[k]); coop_dc_init(g_dcR[k]); }
    coop_spread_init(g_pan_synth, 0);
    coop_spread_init(g_pan_drum, 0);
    g_pan_drum->rng = 0x7F4A7C15u;                     /* a different stream from the synth's */
    g_inited = 1;
}

/* Called by CoopAudioCb before the stock callback: a block starts. */
extern "C" void coop_tape_clamp_track(void);           /* tape_stereo.cc: a saved track word >= 4 trips a stock assert */

extern "C" void coop_stereo_block_start(void)
{
    coop_l1_sine_copy();                               /* before the stock callback's first block (T4c) */
    if (!g_inited) stereo_init();
    coop_tape_clamp_track();
    if (g_coop_force_stereo_out) *OUTPUT_MODE_B = 1;
    g_hb_ramp = *HB_RAMP_W;
    g_rendered_prev = g_rendered_cur;
    g_rendered_cur = 0;
    g_side_dec_done = 0;
    g_side_this_block = 0;
    g_sum_done = 0;
    for (int k = 0; k < INSTR; k++) {
        g_side_dec_done_i[k] = 0; g_side_live[k] = 0;
        if (g_side_dirty[k]) {
            int32_t *l = g_sdL[k], *r = g_sdR[k];
            for (int i = 0; i < COOP_VBLK; i++) { l[i] = 0; r[i] = 0; }
            g_side_dirty[k] = 0;
        }
    }
    int16_t spread = (int16_t)(g_coop_spread > 32767u ? 32767u : g_coop_spread);
    coop_spread_set(g_pan_synth, spread);
    coop_spread_set(g_pan_drum, spread);
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
    coop_spread_note_on(g_pan_synth, (int)voice);
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
    coop_pan_delta d = coop_pan_deltas_tab(pans->pan[voice], g_sin);
    if (d.dL == 0 && d.dR == 0) return;                /* centred: nothing beyond the stock sum */
    int16_t gain = *VOICE_GAIN;
    if (is_drum) coop_side_accum_drum(g_sdL[1], g_sdR[1], VOICE_OUT, gain, d);
    else         coop_side_accum_synth(g_sdL[0], g_sdR[0], VOICE_OUT, VOICE_ENV, gain, d);
    g_side_dirty[is_drum] = 1;
    g_side_tail[is_drum] = 4;
    uint32_t dt = coop_cycles() - t0;
    if (dt > g_stereo_hook_cycles_max) g_stereo_hook_cycles_max = dt;
    if (dt > g_stereo_hook_cycles_win) g_stereo_hook_cycles_win = dt;
    g_stereo_hook_cycles_sum_win += dt;
}

/* R7 at the synth render call = voice index 0..5 (0xffa009c2 zeroes it, 0xffa00da0 increments). */
extern "C" void coop_synth_voice_hook(uint32_t r7)
{
    if (r7 >= COOP_SYNTH_VOICES) return;
    voice_hook(g_pan_synth, (int)r7, 1u << r7, 0);
}

/* R7 at the drum render call counts 12 down to 1 (0xffa0072a, 0xffa00946). */
extern "C" void coop_drum_voice_hook(uint32_t r7)
{
    if (r7 == 0 || r7 > COOP_DRUM_VOICES) return;
    int voice = COOP_DRUM_VOICES - (int)r7;
    voice_hook(g_pan_drum, voice, 1u << (16 + voice), 1);
}

/* One instrument's side pair for this block: decimated + DC-blocked once if voices are (or were just) panned there,
 * else zeroed. 1 = it carries a side. */
static int dec_instr(int k)
{
    if (g_side_dec_done_i[k]) return g_side_live[k];
    g_side_dec_done_i[k] = 1;
    if (!g_side_tail[k]) {
        int16_t *l = g_idecL[k], *r = g_idecR[k];
        for (int i = 0; i < COOP_MBLK; i++) { l[i] = 0; r[i] = 0; }
        return 0;
    }
    int16_t hr = (int16_t)g_hb_ramp, hs = (int16_t)(g_hb_ramp >> 16);
    coop_hb_decimate2(g_hb2[k], g_sdL[k], g_sdR[k], g_idecL[k], g_idecR[k], HB_COEFS, hr, hs);
    int32_t c = *DC_COEF_W;
    coop_dc_block(g_dcL[k], g_idecL[k], g_idecL[k], c, COOP_MBLK);
    coop_dc_block(g_dcR[k], g_idecR[k], g_idecR[k], c, COOP_MBLK);
    if (--g_side_tail[k] == 0) {                       /* the rings are flushed; drop the DC tail */
        coop_hb2_init(g_hb2[k]);
        coop_dc_init(g_dcL[k]); coop_dc_init(g_dcR[k]);
    }
    g_side_live[k] = 1;
    return 1;
}

/* The summed difference buses for this block (both mix loops and the record tap take them). 1 = present. */
static int side_prepare(void)
{
    if (!g_side_tail[0] && !g_side_tail[1] && !g_side_live[0] && !g_side_live[1]) return 0;
    int live = dec_instr(0) | dec_instr(1);
    if (!live) return 0;
    if (!g_sum_done) {
        const int16_t *l0 = g_idecL[0], *r0 = g_idecR[0], *l1 = g_idecL[1], *r1 = g_idecR[1];
        for (int i = 0; i < COOP_MBLK; i++) {
            g_decL[i] = coop_sat16((int32_t)l0[i] + l1[i]);
            g_decR[i] = coop_sat16((int32_t)r0[i] + r1[i]);
        }
        g_sum_done = 1;
        g_side_dec_done = 1;
        g_stereo_side_blocks++;
    }
    g_side_this_block = 1;
    return 1;
}

/* Stands in for mix loop B (R0.L = gain, R1.L = ramp == *g_mix_ramp_ptr, R2.L = step; returns R1 as the stock loop
 * leaves it: the final ramp sign-extended) and, when g_mix_site says so, for mix loop A (R0.L = step, R0.H = gL,
 * R2.H = gR, g_mix_ramp_ptr = the ramp/meter record; returns R1 with .H = the final ramp and .L as on entry). */
extern "C" uint32_t coop_mix_entry(uint32_t r0, uint32_t r1, uint32_t r2)
{
    uint32_t t0 = coop_cycles();
    g_stereo_mixes++;
    uint32_t ret;
    if (g_mix_site == MIX_A_SITE) {
        g_mix_p0_out = 0x01b4ef6eu;                     /* loop A reloads P0 = [FP-0x4] = the sum gain word every iteration */
        int16_t step = (int16_t)r0, gL = (int16_t)(r0 >> 16), gR = (int16_t)(r2 >> 16);
        int16_t *rec = g_mix_a_rec;
        int16_t ramp = side_prepare()
            ? coop_mix_a(MBUS_L, MBUS_R, MONO_BUS64, g_decL, g_decR, gL, gR, step, rec, VOICE_GAIN_W)
            : coop_mix_a_mono(MBUS_L, MBUS_R, MONO_BUS64, gL, gR, step, rec, VOICE_GAIN_W);
        ret = (r1 & 0xffffu) | ((uint32_t)(uint16_t)ramp << 16);
    } else {
        g_mix_p0_out = 0x40;                            /* loop B: P0 = its LSETUP count, used by the code after it */
        int16_t gain = (int16_t)r0, step = (int16_t)r2;
        int16_t *ramp = g_mix_ramp_ptr;
        if (side_prepare()) coop_mix_b(MBUS_L, MBUS_R, MONO_BUS64, g_decL, g_decR, gain, ramp, step);
        else coop_mix_b_mono(MBUS_L, MBUS_R, MONO_BUS64, gain, ramp, step);
        ret = (uint32_t)(int32_t)*ramp;
    }
    uint32_t dt = coop_cycles() - t0;
    if (dt > g_stereo_mix_cycles_max) g_stereo_mix_cycles_max = dt;
    if (dt > g_stereo_mix_cycles_win) g_stereo_mix_cycles_win = dt;
    g_stereo_mix_cycles_sum_win += dt;
    return ret;
}

/* For an FX plugin at a mono position (plugin_host.cc, P2b), which runs before the mix: that instrument's decimated
 * side pair for this block (zeroed when no voice is panned there). The plugin reads it as its stereo input beside
 * the mono bus and writes its output's difference back into it; the pair counts as live from here on. */
extern "C" int coop_stereo_side_rw(int instr, int16_t **l, int16_t **r)
{
    int k = instr & 1;
    dec_instr(k);
    g_side_live[k] = 1;
    g_sum_done = 0;                                    /* the sum is built after the inserts */
    *l = g_idecL[k]; *r = g_idecR[k];
    return 1;
}

/* For the stereo tape record tap (tape_stereo.cc), which runs after loop B in the same block: the
 * decimated difference buses (M units) if the mix added them this block, else 0 (returns 0). */
extern "C" int coop_stereo_side_bufs(const int16_t **l, const int16_t **r)
{
    if (!g_side_this_block) return 0;
    *l = g_decL; *r = g_decR;
    return 1;
}
