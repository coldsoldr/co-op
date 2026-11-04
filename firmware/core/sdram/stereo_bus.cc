/*
 * coop-fw stereo instrument bus -- firmware payload (docs/stereo-bus.md, docs/stereo-bus-v2.md step S3).
 *
 * The stock R246 sums every synth and drum voice into ONE mono bus (0xff800bb4, 128 x int16 at 88.2 kHz), then
 * decimates it (L1 0xffa005d4), DC-blocks it (SDRAM 0x019674f8) back into the first 64 samples of the same buffer,
 * runs the active instrument's effect on it in place and mixes it into the master pair (loop B). S3 makes that
 * chain stereo: each voice is placed into one stereo pair right after its render (L = v * gL, R = v * gR), and
 * coop's instrument stage replaces the stock decimator + DC block at their three call sites: it decimates and
 * DC-blocks the pair and writes it out as mid (into the stock bus buffer, which the stock meters, the effect call
 * and the record pass read) plus side (coop's buffers). The effect dispatch (plugin_host.cc) and the mix loops
 * (coop kernels) take mid + side, which is the pair exactly; nothing outside the pair carries signal any more.
 * With every voice centred L = R = the stock bus, so spread 0 is the stock output bit for bit (no effect running).
 *
 * L1 sites rewritten by tools/l1_stereo_insert.py (stock bytes checked first):
 *   - synth / drum render calls: trampolines to coop_synth/drum_render_thunk -> the per-voice hooks below;
 *   - mix loops A and B: coop_mix_thunk -> coop_mix_entry;
 *   - S3: the decimator + DC call pairs at 0xffa01eb4, 0xffa02456, 0xffa02888 (14 bytes each) become an inline
 *     far call to coop_instr_thunk (stereo_thunks.S) -> coop_instr_stage.
 * Addresses are [C] from the decrypted R246 disassembly and the op1emu write tracer.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_stereo.h"
#include "coop_l1.h"

/* --- stock buffers and words (all [C], write-traced 2026-11-02/03) --- */
#define VOICE_OUT   ((const int16_t *)0xff80113cu)   /* render out (R1 of render; 0xffa009de, 0xffa0068c) */
#define VOICE_ENV   ((const int16_t *)0xff801d50u)   /* per-voice envelope, 128 x int16 (0xffa009d6) */
#define VOICE_GAIN  ((const volatile int16_t *)0x01b4ef6eu) /* W[P4]/W[P5] sum gain (0xffa00a5e, 0xffa00742) */
#define MONO_BUS64  ((int16_t *)0xff800bb4u)         /* the stock bus; after the DC block its first 64 samples are what
                                                        the effect, loop A / B and the record pass read (P3, 0xffa00f00) */
#define HB_COEFS    ((const int16_t *)0x0106a6f0u)   /* half-band even taps (P4, 0xffa00648) */
#define HB_RAMP_W   ((volatile uint32_t *)0xff80107cu) /* lo = ramp, hi = step (P5, 0xffa005ee); the decimator writes
                                                          the ramp back (W[P5] = R0.L, 0xffa0067e) */
#define DC_COEF_W   ((const volatile int32_t *)0x01bab53cu)  /* DC blocker coefficient (P1, 0x01967502) */
#define MBUS_L      ((volatile int16_t *)0xff800d34u)  /* master pair (I2/I0 of loop B) */
#define MBUS_R      ((volatile int16_t *)0xff800db4u)

/* --- controls (in .data so inject_app --set-word can set them per image) --- */
__attribute__((section(".data"))) volatile uint32_t g_coop_spread = 0;      /* Q15: 0 = all voices centred, 32767 = full field */
__attribute__((section(".data"))) volatile uint32_t g_coop_stereo_enable = 1; /* 0 = the stock decimator + DC run (coop_instr_thunk) */
/* Emulator-only test aid: the stock SPORT conversion sums e34/f34 to mono (speaker path, 0xffa01698)
 * unless the output-mode byte at 0x018b621d is nonzero (headphones/line out: the stereo paths at
 * 0xffa0142a / 0xffa015f0; the test at 0xffa0140c). op1emu never sets it, so a stereo check in the
 * emulator forces it. Leave 0 for the device: the jack detection decides there. */
__attribute__((section(".data"))) volatile uint32_t g_coop_force_stereo_out = 0;
#define OUTPUT_MODE_B ((volatile uint8_t *)0x018b621du)

/* --- state: the per-block hot data in the L1 pool (coop_l1.h, T4), with region-A .bss as the fallback --- */
static int32_t s_vL[COOP_VBLK], s_vR[COOP_VBLK];      /* the instrument pair at 88.2 kHz (all voices) */
static int16_t s_pL[COOP_MBLK], s_pR[COOP_MBLK];      /* decimated + DC-blocked */
static int16_t s_sL[COOP_MBLK], s_sR[COOP_MBLK];      /* its side: L - mid, R - mid */
static coop_hb2_state s_hb2;
static coop_dc_state s_dcL, s_dcR;
static coop_spread s_pan_synth, s_pan_drum;
static int16_t s_sin[65];
static int32_t *g_vL = s_vL, *g_vR = s_vR;
static int16_t *g_pL = s_pL, *g_pR = s_pR;
static int16_t *g_decL = s_sL, *g_decR = s_sR;          /* the side the effect, the mix and the record tap take */
static coop_hb2_state *g_hb2 = &s_hb2;
static coop_dc_state *g_dcL = &s_dcL, *g_dcR = &s_dcR;
static coop_spread *g_pan_synth = &s_pan_synth, *g_pan_drum = &s_pan_drum;
static const int16_t *g_sin = coop_sin_q15;            /* the pan law's quarter sine, copied into L1 */
volatile uint32_t g_stereo_l1;                         /* 1 = the hot state is in L1 */
static uint32_t g_rendered_prev, g_rendered_cur;       /* bits 0-5 synth voices, 16-27 drum voices */
static int g_pair_dirty;                               /* voices accumulated since the stage last consumed the pair */
static int g_stage_done;                               /* the instrument stage ran this block: mid + side are valid */
static int g_inited;
static int g_side_this_block;                          /* the mix took the side this block (the record tap follows it) */
volatile uint32_t g_dbg_twice;                         /* debug: a voice reached the hook twice in one block */
volatile uint32_t g_dbg_sum_coop, g_dbg_sum_stock;     /* debug: sum |bus| >> 4, coop vs stock, at 88.2 kHz */
volatile uint32_t g_dbg_synth_hooks, g_dbg_drum_hooks;

/* liveness / perf counters (read from the emulator or a probe) */
volatile uint32_t g_stereo_blocks, g_stereo_hooks, g_stereo_mixes, g_stereo_side_blocks, g_stereo_note_ons;
volatile uint32_t g_stereo_hook_cycles_max, g_stereo_mix_cycles_max, g_stereo_mix_cycles_win;
volatile uint32_t g_stereo_hook_cycles_win, g_stereo_hook_cycles_sum_win, g_stereo_mix_cycles_sum_win;   /* stats v3 */
volatile uint32_t g_stereo_stage_cycles_max, g_stereo_stage_calls;                                     /* S3 */

extern "C" int16_t *g_mix_ramp_ptr;                    /* P1 of loop A / B, stored by coop_mix_thunk */
int16_t *g_mix_ramp_ptr;
extern "C" uint32_t g_mix_site;                         /* the return address of the CALL at the site (loop A 0xffa00f30, loop B 0xffa00fb2) */
uint32_t g_mix_site;
extern "C" int16_t *g_mix_a_rec;                        /* loop A: [SP+0x3c] at the site = its ramp/meter record (P1 there is the loop count) */
int16_t *g_mix_a_rec;
extern "C" uint32_t g_mix_p0_out;                       /* P0 the thunk restores: the stock loops leave P0 untouched and the code after them uses it */
uint32_t g_mix_p0_out;
#define MIX_A_SITE  0xffa00f30u
#define VOICE_GAIN_W ((const volatile int16_t *)0x01b4ef6eu)

#include "coop_cost.h"
static inline uint32_t coop_cycles(void)
{
    uint32_t v;
    __asm__ __volatile__("%0 = CYCLES;" : "=d"(v));
    return v;
}

static void stereo_init(void)
{
    if (coop_l1_state() == 1) {
        g_vL = (int32_t *)coop_l1_or(sizeof s_vL, s_vL);
        g_vR = (int32_t *)coop_l1_or(sizeof s_vR, s_vR);
        g_pL = (int16_t *)coop_l1_or(sizeof s_pL, s_pL);
        g_pR = (int16_t *)coop_l1_or(sizeof s_pR, s_pR);
        g_decL = (int16_t *)coop_l1_or(sizeof s_sL, s_sL);
        g_decR = (int16_t *)coop_l1_or(sizeof s_sR, s_sR);
        g_hb2 = (coop_hb2_state *)coop_l1_or(sizeof s_hb2, &s_hb2);
        g_dcL = (coop_dc_state *)coop_l1_or(sizeof s_dcL, &s_dcL);
        g_dcR = (coop_dc_state *)coop_l1_or(sizeof s_dcR, &s_dcR);
        g_pan_synth = (coop_spread *)coop_l1_or(sizeof s_pan_synth, &s_pan_synth);
        g_pan_drum = (coop_spread *)coop_l1_or(sizeof s_pan_drum, &s_pan_drum);
        int16_t *sin_l1 = (int16_t *)coop_l1_alloc(sizeof s_sin);
        if (sin_l1) { for (int i = 0; i < 65; i++) sin_l1[i] = coop_sin_q15[i]; g_sin = sin_l1; }
        g_stereo_l1 = g_vL != s_vL;
    }
    for (int i = 0; i < COOP_VBLK; i++) { g_vL[i] = 0; g_vR[i] = 0; }
    for (int i = 0; i < COOP_MBLK; i++) { g_decL[i] = 0; g_decR[i] = 0; }
    coop_hb2_init(g_hb2); coop_dc_init(g_dcL); coop_dc_init(g_dcR);
    coop_spread_init(g_pan_synth, 0);
    coop_spread_init(g_pan_drum, 0);
    g_pan_drum->rng = 0x7F4A7C15u;                     /* a different stream from the synth's */
    g_inited = 1;
}

static void pair_clear(void)
{
    int32_t *l = g_vL, *r = g_vR;
    for (int i = 0; i < COOP_VBLK; i++) { l[i] = 0; r[i] = 0; }
    g_pair_dirty = 0;
}

/* Called by CoopAudioCb before the stock callback: a block starts. */
extern "C" void coop_tape_clamp_track(void);           /* tape_stereo.cc: a saved track word >= 4 trips a stock assert */

extern "C" void coop_eng_block_end(void);          /* coop_stats.cc: the previous block's per-engine render sums */
extern "C" void coop_stereo_block_start(void)
{
    coop_eng_block_end();
    coop_l1_sine_copy();                               /* before the stock callback's first block (T4c) */
    if (!g_inited) stereo_init();
    coop_tape_clamp_track();
    if (g_coop_force_stereo_out) *OUTPUT_MODE_B = 1;
    g_rendered_prev = g_rendered_cur;
    g_rendered_cur = 0;
    g_side_this_block = 0;
    g_stage_done = 0;
    if (g_pair_dirty) pair_clear();                    /* a block whose stage did not run must not carry voices over */
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

/* Debug: what one drum voice really adds to the stock bus. At each drum hook the bus as it is now minus the bus at
 * the previous drum hook = the previous voice's stock contribution; compare it with that voice's coop value
 * 2 * (out * gain) and with out * gain. Sums of |x| >> 4 over the run. */
extern __attribute__((section(".data"))) volatile uint32_t g_coop_dbg_cmp;
volatile uint32_t g_dbg_d_stock, g_dbg_d_v1, g_dbg_d_v2, g_dbg_d_n;
static int16_t s_dbg_prev_bus[COOP_VBLK], s_dbg_prev_v1[COOP_VBLK];
static int s_dbg_have;
static uint32_t s_dbg_blk;
static void dbg_drum_probe(int16_t gain)
{
    const int16_t *bus = MONO_BUS64;
    if (s_dbg_have && s_dbg_blk == g_stereo_blocks) {
        for (int i = 0; i < COOP_VBLK; i++) {
            int32_t ds = (int32_t)bus[i] - s_dbg_prev_bus[i], v1 = s_dbg_prev_v1[i], v2 = coop_fr16_shl1((int16_t)v1);
            g_dbg_d_stock += (uint32_t)(ds < 0 ? -ds : ds) >> 4;
            g_dbg_d_v1 += (uint32_t)(v1 < 0 ? -v1 : v1) >> 4;
            g_dbg_d_v2 += (uint32_t)(v2 < 0 ? -v2 : v2) >> 4;
        }
        g_dbg_d_n++;
    }
    for (int i = 0; i < COOP_VBLK; i++) { s_dbg_prev_bus[i] = bus[i]; s_dbg_prev_v1[i] = coop_fr16_mul(VOICE_OUT[i], gain); }
    s_dbg_have = 1; s_dbg_blk = g_stereo_blocks;
}

/* Common per-voice path: bookkeeping, drum note-on heuristic (a drum voice that was not rendered last
 * block gets a fresh random pan scaled by the spread; synth voices get theirs from the note-on hook),
 * then the voice's whole contribution placed into the pair. */
/* The stock per-voice sum into the mono bus is patched out (clean-up after S2: tools/l1_stereo_insert.py, VOICE_SUM_SKIPS;
 * the bus was only ever overwritten by the mid). Where the stock bus is still needed, with the stereo bus off
 * (g_coop_stereo_enable = 0: the stock decimator and DC block run on it) and for the debug compare, the hook does that
 * sum itself, as the stock loops did it (0xffa00d2c..0xffa00d70 synth: out * env * gain; 0xffa00922..0xffa0093e drum:
 * out * gain << 1 (S); each added with saturation). */
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_voice_sum_own = 1; }
/* performance: the rewritten stock DSynth voice loop (stereo_thunks.S); 0 runs the stock loop (A/B, kill switch) */
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_fast_dsynth = 1; }   /* marker for the package tool */
/* performance: the rewritten stock cwo stereo loop and its skipped array constructors (cwo_fast.S); 0 runs the stock code */
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_fast_cwo = 1; }
/* performance: the rewritten stock phone output loop (phone_fast.S); 0 runs the stock loop (A/B, kill switch) */
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_fast_phone = 1; }
/* performance: the stock punch stereo render, both channels in one pass (punch_fast.S); 0 runs the stock render */
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_fast_punch = 1; }
/* performance: the rewritten stock nitro mono render loops (nitro_fast.S); 0 runs the stock loops (A/B, kill switch) */
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_fast_nitro = 1; }
static void voice_sum_stock(int is_drum, int16_t gain)
{
    int16_t *bus = MONO_BUS64;
    if (is_drum) {                                                       /* the stock instructions themselves (0xffa0092e..
                                                                             0xffa0093a): op1emu differs by 1 LSB from the
                                                                             equivalent helper sequence, the bfin simulator not */
        for (int i = 0; i < COOP_VBLK; i++) {
            int32_t r0 = (uint16_t)VOICE_OUT[i], r1 = (uint16_t)gain, b = bus[i];
            __asm__("%1.L = %0.L * %1.L;\n\t%0 = %2;\n\t%0.H = %1.L << 1 (S);\n\t%0.L = %0.H + %0.L (S);"
                    : "+&d"(r0), "+&d"(r1) : "d"(b));
            bus[i] = (int16_t)r0;
        }
        return;
    }
    for (int i = 0; i < COOP_VBLK; i++)
        bus[i] = coop_fr16_add(bus[i], coop_fr16_mul(coop_fr16_mul(VOICE_OUT[i], VOICE_ENV[i]), gain));
}

static void voice_hook(coop_spread *pans, int voice, uint32_t bit, int is_drum)
{
    if (!g_coop_stereo_enable) { voice_sum_stock(is_drum, *VOICE_GAIN); return; }
    uint32_t t0 = coop_cycles();
    g_stereo_hooks++;
    if (g_rendered_cur & bit) g_dbg_twice++;           /* the same voice hooked twice in one block */
    if (is_drum) g_dbg_drum_hooks++; else g_dbg_synth_hooks++;
    g_rendered_cur |= bit;
    if (is_drum && !(g_rendered_prev & bit)) {   /* a drum voice starting: its note-on, as far as we can see */
        coop_spread_note_on(pans, voice);
        coop_lfo2_trigger_drum(voice);
    }
    coop_pan_delta d = coop_pan_deltas_tab(pans->pan[voice], g_sin);
    int16_t gain = *VOICE_GAIN;
    if (g_coop_dbg_cmp && is_drum) dbg_drum_probe(gain);
    if (g_coop_dbg_cmp) voice_sum_stock(is_drum, gain);                  /* the stock bus the compare reads */
    if (is_drum) coop_pair_accum_drum(g_vL, g_vR, VOICE_OUT, gain, d);
    else         coop_pair_accum_synth(g_vL, g_vR, VOICE_OUT, VOICE_ENV, gain, d);
    g_pair_dirty = 1;
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

/* R7 at the drum render call counts 12 down to 1 (0xffa0072a, 0xffa00946). The render's R0 (active) only sets the
 * voice's done flag (0xffa00906..0xffa0090c): the stock sums the voice either way, so the hook does too. */
extern "C" void coop_drum_voice_hook(uint32_t r7, uint32_t active)
{
    (void)active;
    if (r7 == 0 || r7 > COOP_DRUM_VOICES) return;
    int voice = COOP_DRUM_VOICES - (int)r7;
    voice_hook(g_pan_drum, voice, 1u << (16 + voice), 1);
}

/* S3: stands in for the stock decimator (0xffa005d4) + DC block (0x019674f8) at their three call sites
 * (coop_instr_thunk). Decimates the pair with the stock ramp word and writes the ramp back as the stock decimator
 * does, DC-blocks both sides with the stock coefficient, and splits the pair into mid (the stock bus buffer) and
 * side. Then clears the pair for the next block. */
/* Debug (op1emu, --set-word g_coop_dbg_cmp=1, spread 0): coop_instr_thunk also runs the stock decimator + DC block
 * on the stock bus (its per-voice sum still runs) and the stage compares stage by stage. Counts blocks with any
 * mismatch and keeps the first one's (index, coop, stock). */
__attribute__((section(".data"))) volatile uint32_t g_coop_dbg_cmp = 0;
volatile uint32_t g_dbg_blocks, g_dbg_acc_bad, g_dbg_dec_bad, g_dbg_dc_bad;
volatile uint32_t g_dbg_acc_first, g_dbg_dec_first, g_dbg_dc_first;   /* index << 24 | (coop & 0xfff) << 12 | (stock & 0xfff) */
static int16_t s_dbg_bus[COOP_VBLK];
static uint32_t s_dbg_ramp;
#define STOCK_DEC_OUT ((const int16_t *)0xff801e50u)  /* the stock decimator's output, the DC block's input */

extern "C" void coop_dbg_pre(void)
{
    const int16_t *b = MONO_BUS64;
    for (int i = 0; i < COOP_VBLK; i++) s_dbg_bus[i] = b[i];
    s_dbg_ramp = *HB_RAMP_W;
}

static inline uint32_t dbg_word(int i, int32_t a, int32_t b) { return ((uint32_t)i << 24) | (((uint32_t)a & 0xfff) << 12) | ((uint32_t)b & 0xfff); }

struct coop_stage_io {                                                 /* coop_stage.S IO_* offsets */
    int32_t *vL, *vR; coop_hb2_state *hb; const int16_t *coefs; uint32_t ramp_step; coop_dc_state *dcL, *dcR; int32_t c;
    int16_t *mid, *sl, *sr, *pL, *pR;
};
extern "C" int coop_stage_asm(struct coop_stage_io *io);
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_fast_stage = 1; }   /* 0: the C path (A/B) */

extern "C" void coop_instr_stage(void)
{
    uint32_t t0 = coop_cycles();
    if (!g_inited) stereo_init();
    g_stereo_stage_calls++;
    int dbg = g_coop_dbg_cmp != 0;
    if (dbg) {
        g_dbg_blocks++;
        for (int i = 0; i < COOP_VBLK; i++) {
            int32_t a = coop_sat16(g_vL[i]), b = s_dbg_bus[i];
            g_dbg_sum_coop += (uint32_t)(a < 0 ? -a : a) >> 4;
            g_dbg_sum_stock += (uint32_t)(b < 0 ? -b : b) >> 4;
        }
        for (int i = 0; i < COOP_VBLK; i++)
            if (coop_sat16(g_vL[i]) != s_dbg_bus[i]) { if (!g_dbg_acc_bad) g_dbg_acc_first = dbg_word(i, g_vL[i], s_dbg_bus[i]); g_dbg_acc_bad++; break; }
    }
    if (!dbg && g_coop_fast_stage) {                                   /* performance step 2: the same stage in assembly
                                                                          (coop_stage.S; tests/sim/kern_check.cc) */
        struct coop_stage_io io = { g_vL, g_vR, g_hb2, HB_COEFS, *HB_RAMP_W, g_dcL, g_dcR, *DC_COEF_W,
                                    MONO_BUS64, g_decL, g_decR, g_pL, g_pR };
        *(volatile int16_t *)HB_RAMP_W = (int16_t)coop_stage_asm(&io);
        g_pair_dirty = 0;
        g_stage_done = 1;
        g_stereo_side_blocks++;
        uint32_t dta = coop_cycles() - t0;
        if (dta > g_stereo_stage_cycles_max) g_stereo_stage_cycles_max = dta;
        coop_cost_add(COOP_COST_STAGE, dta);
        return;
    }
    uint32_t rw = dbg ? s_dbg_ramp : *HB_RAMP_W;
    int16_t ramp = coop_hb_decimate2(g_hb2, g_vL, g_vR, g_pL, g_pR, HB_COEFS, (int16_t)rw, (int16_t)(rw >> 16));
    *(volatile int16_t *)HB_RAMP_W = ramp;
    if (dbg)
        for (int i = 0; i < COOP_MBLK; i++)
            if (g_pL[i] != STOCK_DEC_OUT[i]) { if (!g_dbg_dec_bad) g_dbg_dec_first = dbg_word(i, g_pL[i], STOCK_DEC_OUT[i]); g_dbg_dec_bad++; break; }
    int32_t c = *DC_COEF_W;
    coop_dc_block(g_dcL, g_pL, g_pL, c, COOP_MBLK);
    coop_dc_block(g_dcR, g_pR, g_pR, c, COOP_MBLK);
    if (dbg)
        for (int i = 0; i < COOP_MBLK; i++)
            if (g_pL[i] != MONO_BUS64[i]) { if (!g_dbg_dc_bad) g_dbg_dc_first = dbg_word(i, g_pL[i], MONO_BUS64[i]); g_dbg_dc_bad++; break; }
    const int16_t *pl = g_pL, *pr = g_pR;
    int16_t *m = MONO_BUS64, *sl = g_decL, *sr = g_decR;
    for (int i = 0; i < COOP_MBLK; i++) {
        int32_t l = pl[i], r = pr[i], mid = (l + r) >> 1;
        m[i] = (int16_t)mid;
        sl[i] = coop_sat16(l - mid);
        sr[i] = coop_sat16(r - mid);
    }
    pair_clear();
    g_stage_done = 1;
    g_stereo_side_blocks++;
    uint32_t dt = coop_cycles() - t0;
    if (dt > g_stereo_stage_cycles_max) g_stereo_stage_cycles_max = dt;
    coop_cost_add(COOP_COST_STAGE, dt);
}

/* S3: the drum path's bus stage. In drum mode the stock runs 0x01a66922 (state, bus, params) on the summed 128-sample
 * bus after the voices and before the decimator (L1 0xffa02364 `CALL (P1)`, P1 retargeted at coop_drumbus_thunk):
 * a 128-sample in-place dynamics / level stage with two state words in the object it is handed and one global word
 * (0x01dfc80c). coop runs it on each side of the pair, the right side with its own copy of the three state words, so
 * each side is processed as the stock processes its mono bus (at spread 0: L = R = the stock bus, bit for bit). The
 * two sides are not linked: panned material sees per-side dynamics. [C in op1emu; the stage's purpose is I] */
#define DRUMBUS_FN   0x01a66922u
#define DRUMBUS_GLOB ((volatile uint32_t *)0x01dfc80cu)
typedef void (*drumbus_fn)(volatile uint32_t *state, int16_t *buf, const int16_t *params);
static uint32_t s_dbR[3];                              /* the right side's state: object words 0, 1 and the global */
static uint32_t s_dbD[3];                              /* debug: the stock bus's own copy */
static int16_t s_db_buf[COOP_VBLK];
volatile uint32_t g_drumbus_calls;

static void drumbus_side(int32_t *v, volatile uint32_t *state, const int16_t *params, uint32_t *own)
{
    drumbus_fn fn = (drumbus_fn)DRUMBUS_FN;
    uint32_t keep0 = 0, keep1 = 0, keepg = 0;
    if (own) { keep0 = state[0]; keep1 = state[1]; keepg = *DRUMBUS_GLOB; state[0] = own[0]; state[1] = own[1]; *DRUMBUS_GLOB = own[2]; }
    int16_t *t = s_db_buf;
    for (int i = 0; i < COOP_VBLK; i++) t[i] = coop_sat16(v[i]);
    fn(state, t, params);
    for (int i = 0; i < COOP_VBLK; i++) v[i] = t[i];
    if (own) { own[0] = state[0]; own[1] = state[1]; own[2] = *DRUMBUS_GLOB; state[0] = keep0; state[1] = keep1; *DRUMBUS_GLOB = keepg; }
}

extern "C" void coop_drum_bus(volatile uint32_t *state, int16_t *buf, const int16_t *params)
{
    g_drumbus_calls++;
    if (!g_coop_stereo_enable || !g_inited) { ((drumbus_fn)DRUMBUS_FN)(state, buf, params); return; }
    static int seeded;
    if (!seeded) {                                     /* the copies start where the stock state is */
        s_dbR[0] = s_dbD[0] = state[0]; s_dbR[1] = s_dbD[1] = state[1]; s_dbR[2] = s_dbD[2] = *DRUMBUS_GLOB;
        seeded = 1;
    }
    if (g_coop_dbg_cmp) {                              /* the stock bus too, on its own state copy (compared later) */
        uint32_t keep0 = state[0], keep1 = state[1], keepg = *DRUMBUS_GLOB;
        state[0] = s_dbD[0]; state[1] = s_dbD[1]; *DRUMBUS_GLOB = s_dbD[2];
        ((drumbus_fn)DRUMBUS_FN)(state, buf, params);
        s_dbD[0] = state[0]; s_dbD[1] = state[1]; s_dbD[2] = *DRUMBUS_GLOB;
        state[0] = keep0; state[1] = keep1; *DRUMBUS_GLOB = keepg;
    }
    drumbus_side(g_vL, state, params, 0);              /* the left side keeps the stock state */
    drumbus_side(g_vR, state, params, s_dbR);
    g_pair_dirty = 1;
}

/* The side for the mix loops and the record tap. 1 = present (the stage ran this block). */
extern "C" void coop_slot2_instr(int16_t *mid, int16_t *sl, int16_t *sr);   /* plugin_host.cc, S2 */

static int side_prepare(void)
{
    if (!g_stage_done) return 0;
    if (!g_side_this_block) coop_slot2_instr(MONO_BUS64, g_decL, g_decR);   /* S2: the instrument's second slot, once per
                                                                              block, after the stock effect, before the mix */
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

/* For the instrument effect (plugin_host.cc), which runs on the stock bus buffer after the stage: the side of the
 * pair (the buffer holds its mid). The effect runs on L = mid + sideL, R = mid + sideR and writes its output back
 * as the new mid and side. One pair for both positions: the stock has one instrument bus and one effect object. */
extern "C" int coop_stereo_side_rw(int instr, int16_t **l, int16_t **r)
{
    (void)instr;
    if (!g_stage_done) {                               /* no stage this block (stereo off): an empty side */
        for (int i = 0; i < COOP_MBLK; i++) { g_decL[i] = 0; g_decR[i] = 0; }
        g_stage_done = 1;
    }
    *l = g_decL; *r = g_decR;
    return 1;
}

/* For the stereo tape record tap (tape_stereo.cc), which runs after loop B in the same block: the side (M units)
 * if the mix added it this block, else 0 (returns 0). */
extern "C" int coop_stereo_side_bufs(const int16_t **l, const int16_t **r)
{
    if (!g_side_this_block) return 0;
    *l = g_decL; *r = g_decR;
    return 1;
}
