/* Soft overload (ARCHITECTURE.md "Audio budget and overload"). When the whole audio callback stays near the block
 * length (90 % of the measured block, kn_audio_block_cycles(), for KN_SHED_BLOCKS blocks; hysteresis down to 75 %), one plugin
 * synth voice at a time is silenced instead of bypassing the whole engine:
 *   1. voices already in release (adsr state 4 / 5), quietest envelope level first;
 *   2. then the oldest held voice (state 1..3, largest age), never the newest held one.
 * Silencing = one more real render faded out linearly over the block, then the engine render is skipped and the
 * voice writes zeros (the cost goes away at once). A released voice also gets R246's own voice_kill (0x0191209C:
 * state 8, age 0, as VoiceCtl's resets do): adsr_block fades it and frees it (active flag cleared at level < 16).
 * A held voice is never killed and VoiceCtl's held-note list (0x01DFE734) is never written: the voice stays
 * allocated and silent, so a later note_off is R246's own path (voice_release -> state 4 / 5); we then kill it.
 * A new note on the voice (producer note_on, or the voice restarted by VoiceCtl: state 1..3 with its age going
 * back) makes it play again. Poly play mode only (adsr word 4 >> 12 == 0): unison / mono / legato keep today's
 * behaviour. Stock engines are not touched (their renders do not pass through the dispatcher).
 * Runs in the audio interrupt; integers only, a few loads per voice per block.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE). */
#include "kn_shed.h"
#include "../kn_mem.h"                 /* kn_hot */

#ifdef OP1_HOST
extern int32_t host_vs[6 * 6];          /* voice_state: 6 words per voice */
extern uint8_t host_active[6];
extern int host_play_mode;
#define VS(v)       (&host_vs[(v) * 6])
#define ACTIVE(v)   (host_active[v])
#define PLAY_MODE() (host_play_mode)
#else
/* voice_state 0x18 bytes at 0xFF8000D4 + v * 0x18: +0 state, +4 age (+1 per block, 0x10000000 after note-off),
 * +8 envelope level 1.31 (docs/firmware/audio-graph.md "voice_state") */
#define VS(v)       ((volatile int32_t *)(0xFF8000D4u + (uint32_t)(v) * 0x18u))
#define ACTIVE(v)   (((volatile uint8_t *)0xFF80105C)[v])
/* play mode of the requested synth slot, as voicectl_synth_note_on / _off read it: adsr word 4 >> 12 (0 = poly) */
#define PLAY_MODE() (((const volatile int16_t *)(0x01B60EF0 + (*(volatile uint32_t *)0xFF8016D4 & 7) * 16))[4] >> 12)
#endif
#define NV 6

volatile uint32_t kn_shed_voices;
static uint8_t mute[NV] kn_hot;        /* KN_SHED_RENDER / _RAMP / _SILENT */
static int32_t seen_age[NV] kn_hot;    /* a silenced voice's age at its last render */
static uint8_t rendered kn_hot;        /* plugin voices rendered (not silenced) this block */
static uint32_t plugin_cy kn_hot;      /* their render cycles this block */
static uint8_t hot kn_hot;
uint32_t kn_shed_hi kn_hot, kn_shed_lo kn_hot;
static uint32_t block_cy kn_hot;

void kn_shed_begin(void) { rendered = 0; plugin_cy = 0; }

void kn_shed_note_on(int v) { if ((unsigned)v < NV) mute[v] = KN_SHED_RENDER; }

int kn_shed_render(int v)
{
    if ((unsigned)v >= NV || !mute[v]) return KN_SHED_RENDER;
    volatile int32_t *s = VS(v);
    int32_t st = s[0], age = s[1];
    if (st >= 1 && st <= 3) {          /* adsr_block counts age only in states 1..5 (not in 6 / 8, nor 0) */
        if (age <= seen_age[v]) { mute[v] = KN_SHED_RENDER; return KN_SHED_RENDER; }   /* restarted */
        seen_age[v] = age;
    }
    if (mute[v] == KN_SHED_RAMP) { mute[v] = KN_SHED_SILENT; return KN_SHED_RAMP; }
    return KN_SHED_SILENT;
}

void kn_shed_ramp(int16_t *out)
{
    for (int i = 0; i < 128; i++) out[i] = (int16_t)((out[i] * (128 - i)) >> 7);
}

void kn_shed_rendered(int v, uint32_t cycles)
{
    if ((unsigned)v >= NV) return;
    rendered |= (uint8_t)(1u << v);
    plugin_cy = cycles > 0xffffffffu - plugin_cy ? 0xffffffffu : plugin_cy + cycles;
}

/* voice_kill; any later start (state 1..3, small age) then counts as a restart */
static void kill(int v) { volatile int32_t *s = VS(v); s[1] = 0; s[0] = 8; seen_age[v] = 0x7FFFFFFF; }

static uint16_t over_blocks kn_hot;
int kn_bypass_tick(uint32_t used) { return kn_overrun_tick(&over_blocks, used); }

int kn_shed_end(uint32_t used)
{
    for (int v = 0; v < NV; v++)          /* a silenced held voice got its note_off: free it */
        if (mute[v] && ACTIVE(v) && (VS(v)[0] == 4 || VS(v)[0] == 5)) kill(v);
    uint32_t b = kn_audio_block_cycles();
    if (b != block_cy) { block_cy = b; kn_shed_hi = b * KN_SHED_HI_PCT / 100; kn_shed_lo = b * KN_SHED_LO_PCT / 100; }
    if (used >= kn_shed_hi) { if (hot < 255) hot++; }
    else if (used < kn_shed_lo) hot = 0;
    if (hot < KN_SHED_BLOCKS || PLAY_MODE() != 0 || plugin_cy < (used >> 2)) return 0;
    int rel = -1, old = -1, young = -1, nheld = 0;
    int32_t rel_level = 0, old_age = 0, young_age = 0;
    for (int v = 0; v < NV; v++) {
        if (!(rendered & (1u << v)) || mute[v] || !ACTIVE(v)) continue;
        volatile int32_t *s = VS(v);
        int32_t st = s[0];
        if (st == 4 || st == 5) {
            if (rel < 0 || s[2] < rel_level) { rel = v; rel_level = s[2]; }
        } else if (st >= 1 && st <= 3) {
            int32_t age = s[1];
            nheld++;
            if (old < 0 || age > old_age) { old = v; old_age = age; }
            if (young < 0 || age < young_age) { young = v; young_age = age; }
        }
    }
    int v = rel >= 0 ? rel : (nheld >= 2 ? old : -1);
    if (v < 0) return 0;                  /* nothing to shed: the last-resort bypass (dispatcher.c) stays */
    mute[v] = KN_SHED_RAMP;
    if (v == rel) kill(v); else seen_age[v] = VS(v)[1];
    hot = 0;
    kn_shed_voices++;
    return 1;
}
