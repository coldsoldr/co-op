/*
 * coop-fw engine host (P3 / E1, docs/engines.md): plugin synth engines in the stock voice loop.
 *
 * The hidden iter engine (instrument type 14) is hijacked as kennel 3.1 does it (dispatcher.c): the package points
 * iter's voice vtable (0x019103CC: render +0x14, note on +0x1C, note off +0x2C) at the three dispatchers below, and
 * the instr_types name compare (0x019B975C) at coop_instr_name_eq, so a plugin's name in the engine list (a
 * synth_types row, plugin_host.cc) loads as iter. The slot's fifth knob word picks the plugin: params[4] = 0x5300 + n.
 * Anything else plays the stock iter voice, which is also what a sound naming a missing plugin plays.
 *
 * Voices: the six iter voice objects sit at 0xFFB00000 + i * 0x24 (kennel op1_voice_index). The stock computes the
 * voice's per-sample pitch words (0xFF801950) before the render and writes its velocity (0xFF801064, vel << 8) around
 * the note on; the dispatchers hand both to the plugin, so a plugin never names a firmware address. The plugin's
 * per-voice state (coop_engine_t.voice_bytes, six voices) comes from heap 0 at load.
 *
 * Threads: the dispatchers run in the audio interrupt (the voice loop); coop_engine_add runs on the main thread at
 * boot, before the engine is published (g_eng[n].e last, after an ssync).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_plugin.h"
#include "coop_cost.h"

#define ITER_RENDER   ((void (*)(void *, int16_t *, const int16_t *))0x019B99D4u)
#define ITER_NOTE_ON  ((void (*)(void *, const int16_t *))0x019B9B8Au)
#define ITER_NOTE_OFF ((void (*)(void *))0x019B996Cu)
#define MALLOC        ((void *(*)(uint32_t))0x01A9F348u)                  /* heap 0 */
#define FREE          ((void (*)(void *))0x01AA0040u)
#define VOICE_BASE    0xFFB00000u
#define VOICE_SIZE    0x24u
#define PITCH_INC     ((const int32_t *)0xFF801950u)                     /* the voice being rendered, 128 words */
#define VOICE_VEL     ((const volatile int16_t *)0xFF801064u)            /* per voice: velocity << 8 */
#define MAGIC         0x5300
#define SLOTS         64

struct eng { const coop_engine_t *e; uint8_t *state; uint32_t stride; };
static struct eng g_eng[SLOTS];
static uint8_t g_voice_eng[COOP_VOICES];                                  /* the engine each voice started with (0 = iter) */
volatile uint32_t g_eng_loaded, g_eng_renders, g_eng_noteons, g_eng_iter_renders, g_eng_switches, g_eng_foreign;
volatile uint32_t g_eng_cycles_max;
extern volatile int g_l1_lease_open;                                       /* l1_pool.cc (e4): coop_l1_lease only in init */

/* e4: cost per engine, per voice render and per block (all its voices in one block), for the stats `engines` line */
struct ecost { uint64_t sum, bsum; uint32_t n, max, bn, bmax, bacc; };
static struct ecost g_ecost[SLOTS];
static uint32_t g_peng_blk, g_peng_blk_pf[2];                             /* m1: this block's plugin renders and their counters */

extern "C" const coop_engine_t *coop_engine_at(int n) { return (n > 0 && n < SLOTS) ? g_eng[n].e : 0; }

/* Main thread, boot: per-voice state, the plugin's init, then publish. 0 = registered, else why not. */
extern "C" const char *coop_engine_add(int n, const coop_engine_t *e)
{
    if (n <= 0 || n >= SLOTS || g_eng[n].e) return "engine number taken";
    if (e->voice_bytes > COOP_VOICE_BYTES_MAX) return "engine voice state too large";
    uint32_t stride = (e->voice_bytes + 31u) & ~31u;
    uint8_t *raw = 0, *state = 0;
    if (stride) {
        raw = (uint8_t *)MALLOC(stride * COOP_VOICES + 32u);
        if (!raw) return "plugin memory full";
        state = (uint8_t *)(((uint32_t)(uintptr_t)raw + 31u) & ~31u);
        for (uint32_t i = 0; i < stride * COOP_VOICES; i += 4) *(volatile uint32_t *)(state + i) = 0;
    }
    g_l1_lease_open = 1;
    int bad = e->init && e->init() != 0;
    g_l1_lease_open = 0;
    if (bad) { if (raw) FREE(raw); return "engine init failed"; }
    g_eng[n].state = state;
    g_eng[n].stride = stride;
    __asm__ __volatile__("ssync;" ::: "memory");
    g_eng[n].e = e;                                                       /* playable from here */
    g_eng_loaded++;
    return 0;
}

static inline int engine_of(const int16_t *params)
{
    int n = params[4] - MAGIC;
    return (n > 0 && n < SLOTS && g_eng[n].e) ? n : 0;
}
static inline int voice_of(const void *self)                             /* -1: not one of the six iter voices */
{
    uint32_t d = (uint32_t)(uintptr_t)self - VOICE_BASE;
    if (d >= VOICE_SIZE * COOP_VOICES || d % VOICE_SIZE) return -1;
    return (int)(d / VOICE_SIZE);
}
static inline void *state_of(int n, int v) { return g_eng[n].state ? g_eng[n].state + g_eng[n].stride * (uint32_t)v : 0; }
static inline int32_t velocity_q15(int v)                                 /* kennel op1_voice_vel_gain: 0 / out of range = full */
{
    int32_t vel = VOICE_VEL[v] >> 8;
    if (vel <= 0 || vel > 127) return 32767;
    return vel * 32767 / 127;
}

static void voice_start(void *self, const int16_t *params, int v, int n)
{
    if (g_voice_eng[v] != n) g_eng_switches++;
    g_voice_eng[v] = (uint8_t)n;
    if (!n) { ITER_NOTE_ON(self, params); return; }
    g_eng[n].e->note_on(v, state_of(n, v), velocity_q15(v), params);
    g_eng_noteons++;
}

/* iter vtable +0x1C (through the voice loop's note-on thunk, coop_synth_noteon_thunk). */
extern "C" void coop_eng_note_on(void *self, const int16_t *params)
{
    int v = voice_of(self);
    if (v < 0) { g_eng_foreign++; ITER_NOTE_ON(self, params); return; }
    voice_start(self, params, v, engine_of(params));
}

/* iter vtable +0x14 (through coop_synth_render_thunk: the cost line names this function). */
extern "C" void coop_eng_render(void *self, int16_t *out, const int16_t *params)
{
    int v = voice_of(self);
    if (v < 0) { g_eng_foreign++; ITER_RENDER(self, out, params); return; }
    int n = engine_of(params);
    if (n != g_voice_eng[v]) voice_start(self, params, v, n);            /* the engine changed under a live voice */
    if (!n) { g_eng_iter_renders++; ITER_RENDER(self, out, params); return; }
    uint32_t pf0[2]; coop_pf_now(pf0);
    uint32_t c0 = coop_cost_now();
    coop_mark_at_pf(0x14, c0, pf0[0], pf0[1]);                           /* m1: the plugin's own render starts */
    g_eng[n].e->render(v, state_of(n, v), out, PITCH_INC, params);
    uint32_t dt = coop_cost_now() - c0;
    coop_cost_add_pf(COOP_COST_PENG, dt, pf0);                            /* m1: per render, per section (the runner) */
    coop_mark(0x1C);                                                      /* m1: ... and ends */
    g_peng_blk += dt;
    if (g_coop_pf_live) { uint32_t p[2]; coop_pf_now(p); g_peng_blk_pf[0] += p[0] - pf0[0]; g_peng_blk_pf[1] += p[1] - pf0[1]; }
    if (dt > g_eng_cycles_max) g_eng_cycles_max = dt;
    g_eng_renders++;
    struct ecost *c = &g_ecost[n];
    c->sum += dt; c->n++; if (dt > c->max) c->max = dt;
    c->bacc += dt;
}

/* e4: the block's total per engine (coop_stats.cc coop_eng_block_end, after the voice loop) */
extern "C" void coop_engine_block_end(void)
{
    if (g_peng_blk) {                                                     /* m1: every plugin render of the block */
        struct coop_cost *c = &g_coop_cost[COOP_COST_PENG_B];
        c->sum += g_peng_blk; c->n++; if (g_peng_blk > c->max) c->max = g_peng_blk;
        c->p0 += g_peng_blk_pf[0]; c->p1 += g_peng_blk_pf[1];
        g_peng_blk = 0; g_peng_blk_pf[0] = 0; g_peng_blk_pf[1] = 0;
    }
    for (int n = 1; n < SLOTS; n++) {
        struct ecost *c = &g_ecost[n];
        if (!c->bacc) continue;
        c->bsum += c->bacc; c->bn++; if (c->bacc > c->bmax) c->bmax = c->bacc;
        c->bacc = 0;
    }
}
/* for the stats: 1 when engine n has rendered; avg / max / count per voice render and per block */
extern "C" int coop_engine_cost(int n, uint32_t *out6)
{
    if (n <= 0 || n >= SLOTS || !g_ecost[n].n) return 0;
    const struct ecost *c = &g_ecost[n];
    out6[0] = (uint32_t)(c->sum / c->n); out6[1] = c->max; out6[2] = c->n;
    out6[3] = c->bn ? (uint32_t)(c->bsum / c->bn) : 0; out6[4] = c->bmax; out6[5] = c->bn;
    return 1;
}

/* iter vtable +0x2C. */
extern "C" void coop_eng_note_off(void *self)
{
    int v = voice_of(self);
    if (v < 0) { ITER_NOTE_OFF(self); return; }
    int n = g_voice_eng[v];
    if (!n || !g_eng[n].e) { ITER_NOTE_OFF(self); return; }
    if (g_eng[n].e->note_off) g_eng[n].e->note_off(v, state_of(n, v));
}

/* ---- the instr_types name compare (0x019B975C, through a region-C trampoline): bool eq(table_name, std::string *).
 * The table's "iter" entry also matches every name that is not a stock engine: a plugin's name, a plugin that is gone,
 * an unknown name (kennel: an unknown synth type asserted patch.cpp:390 and locked the OP-1). ---- */
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; } dstring;
static int streq_n(const char *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i] || !b[i]) return 0;
    return b[n] == 0;
}
static const char *const stock_synth_names[] = { "fm", "digital", "drwave", "string", "cluster", "pulse", "sampler",
                                                 "phase", "drum", "dbox", "ls", "dna", "dsynth", "voltage", 0 };
extern "C" int coop_instr_name_eq(const char *name, const dstring *s)
{
    const char *p = s->cap > 15 ? s->u.ptr : s->u.buf;
    if (streq_n(p, name, s->size)) return 1;
    if (name[0] == 'i' && name[1] == 't' && name[2] == 'e' && name[3] == 'r' && !name[4]) {
        if (!s->size) return 0;
        for (const char *const *q = stock_synth_names; *q; q++) if (streq_n(p, *q, s->size)) return 0;
        return 1;
    }
    return 0;
}
