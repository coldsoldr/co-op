/*
 * coop-fw engine screens (P3 / E2, docs/engines.md "Screens"): a plugin engine shows its own SVG on the engine page.
 *
 * Kennel 3.1's model (dispatcher.c engine_screens_*, kn_screens.c), ported:
 * - The `.coop` file starts with the SVG, so the stock SVG loader reads it straight from the plugin file through the
 *   path "../../plugins/<file>" (relative to content/display: kennel kn_boot.c svg_path).
 * - For every engine with a screen, coop builds a second object of the iter screen class (SCREEN_CTOR with id 0x14,
 *   the table entry put back after), a copy of iter's screen vtable (0x019103FC) with update (+0x1C) and init (+0x24)
 *   pointed at the wrappers below, loads the SVG and calls the plugin's init.
 * - ScreenId::for_synth's case 14 (the jump-table word 0x018F8074, stock 0x01A3BFD0) goes to the stub in
 *   engine_screen.S, which calls coop_synth_screen_select: table slot 0x14 gets the engine's object for the slot on
 *   screen, or iter's own.
 * - R246 resets the drawing caches only of documents reachable from the table it is handed (kn_screens.c). A swapped
 *   out document would keep its caches; coop_screens_frame draws a frame-stable copy of the table and resets every
 *   document that left it, at the frame boundary, before the stock allocates for the next frame.
 *
 * Timing: the stock builds and loads its screens at boot; coop's plugin scan runs at the tape mount. The screens are
 * built here on the Gfx thread, on the first frame after both (the stock iter screen loaded, the scan done), so an
 * SVG is parsed on the thread that draws. Until then an engine shows iter's screen.
 *
 * Threads: build, frame and the wrappers on the Gfx thread; the select on the thread that asks for a screen id.
 *
 * e9 (docs/plugins.md "e9"): effect screens, the same way. An effect with COOP_FX_F_SCREEN gets an object of the iter
 * screen class with the filter page's id (0x0B, ScreenId::for_fx(5): every plugin runs as the filter type), its SVG
 * loaded from its file. ScreenId::for_fx's case 5 (the jump-table word 0x018F7FF8, stock 0x01A3BF26: kennel 3.1
 * make_firmware.py DATA_PATCHES) and every frame put it into table slot 0x0B while that effect is the one shown at the
 * effect page's position (FX 1, or FX 2 while in focus: coop_ui.cc), else the stock filter screen. Its update gets a
 * view (sdk/coop_plugin.h coop_fx_view_t) the core fills here for the shown position.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. The model and the frame fix are
 * hundekraft's (kennel 3.1, GPL-3.0-or-later).
 */
#include <stdint.h>
#include "coop_plugin.h"

#define SCREEN_TABLE   ((void *volatile *)0x01DF4E54u)                     /* 0x44 screen objects, by id */
#define SCREEN_SLOTS   0x44
#define SCREEN_CTOR    ((void *(*)(void *, int, const char *))0x01A3BAB0u) /* (obj, id, svg name): registers itself */
#define SVG_LOAD       ((void *(*)(void *))0x01A503D8u)                  /* (name) -> document, 0 on failure */
#define SCREENS_UPDATE ((void (*)(void *))0x01A3BB20u)
#define CURRENT_SCREEN (*(volatile int *)0x018B65C8u)                    /* the screen id shown (kennel CUR_SCREEN) */
#define ITER_SCREEN_VT ((const uint32_t *)0x019103FCu)
#define ITER_ID        0x14
#define SYNTH_SLOT     (*(volatile int *)0xFF8016D4u)
#define SYNTH_KNOBS(s) ((const int16_t *)(0xFF8013C4u + (uint32_t)((s) & 7) * 16u))
#define MALLOC         ((void *(*)(uint32_t))0x01A9F348u)
#define MAGIC          0x5300
#define SLOTS          64
#define OBJ_WORDS      32                                                  /* the iter screen class uses up to +0x74 */

extern "C" const coop_engine_t *coop_engine_at(int n);                    /* engine_host.cc */
extern volatile uint32_t g_plugins_booted, g_plugins_scan_done;           /* plugin_host.cc */
/* e9: plugin_host.cc / coop_ui.cc / engine_ui.cc */
extern "C" const coop_fx_screen_t *coop_fx_screen_of(int n);              /* the effect's screen (flag checked at load), 0 none */
extern "C" int coop_fx_is_plugin(int n);
extern "C" void coop_fx_view_core(int slot, int n, coop_fx_view_t *v);   /* words, latches, state, live */
extern "C" int coop_fx2_focused(int pos2);
extern "C" int coop_slot2_plugin(int pos2);
extern "C" int coop_slot2_knob_q15(int pos2, int k);
extern "C" int coop_scr_mods(void);

/* One screen object per plugin with a screen, by number (engines and effects share the numbers). fx: an effect's
 * (object id 0x0B, update gets a view), else an engine's (id 0x14). */
struct escr { uint32_t obj[OBJ_WORDS]; uint32_t vt[10]; char path[56]; const coop_screen_t *sc; const coop_fx_screen_t *fsc;
              int16_t n; uint8_t ready, inited, fx; };
static struct escr *g_es[SLOTS];
static char g_file[SLOTS][44];                                            /* the plugin file of each plugin with a screen */
static void *volatile g_iter_screen;                                      /* the stock iter screen object */
static void *volatile g_filter_screen;                                    /* e9: the stock filter screen object (slot 0x0B) */
static volatile int g_built, g_fx_built;
volatile uint32_t g_escr_built, g_escr_failed, g_escr_swaps, g_escr_resets, g_escr_updates;
volatile uint32_t g_fscr_built, g_fscr_failed, g_fscr_swaps, g_fscr_updates, g_fscr_skipped, g_fscr_te;   /* e9: effects;
                                                                             te: TE heap bytes their SVGs took (0 unknown) */

/* plugin_host.cc, boot (main thread), before any frame builds: this plugin's file carries its screen. */
extern "C" void coop_engine_screen_file(int n, const char *file)
{
    if (n <= 0 || n >= SLOTS) return;
    int i = 0;
    for (; file[i] && i < (int)sizeof g_file[0] - 1; i++) g_file[n][i] = file[i];
    g_file[n][i] = 0;
}
extern "C" void coop_fx_screen_file(int n, const char *file) { coop_engine_screen_file(n, file); }   /* e9 */
extern "C" uint32_t coop_screen_obj_bytes(void) { return sizeof(struct escr); }   /* e9: the budget counts it (plugin_host.cc) */

static int engine_shown(void)
{
    int n = SYNTH_KNOBS(SYNTH_SLOT)[4] - MAGIC;
    return (n > 0 && n < SLOTS && g_es[n] && !g_es[n]->fx && g_es[n]->ready && coop_engine_at(n)) ? n : 0;
}

/* ---- e9: which effect the effect page shows, at which position ----
 * The page's position byte (0x01DF4E3C: 0 synth, 1 drum, 3 master; coop_ui.cc FX_POS) and FX 2's focus there (lift /
 * drop, coop_ui.cc) give the slot: FX 2 in focus -> 3 (instrument) / 4 (master) and the plugin FX 2 holds; else FX 1 of
 * the position, a plugin when its type is the filter (5) and its selector word names one (plugin_host.cc). */
#define FX_POS_B     (*(volatile uint8_t *)0x01DF4E3Cu)
#define SHOWN_INDEX  (*(volatile int *)0x01DF4E38u)                       /* the slot / kit on screen (kennel OP1_SCREEN_SLOT) */
#define DRUM_KIT     (*(volatile int *)0xFF8016D8u)
#define FX_TYPE_S(i) (*(volatile int *)(0xFF801734u + (uint32_t)(i) * 4))  /* coop_ui.cc */
#define FX_TYPE_D(i) (*(volatile int *)(0xFF801754u + (uint32_t)(i) * 4))
#define FX_TYPE_M    (*(volatile int *)0xFF801090u)
#define FX_W_S(i)    ((const volatile int16_t *)(0xFF801444u + (uint32_t)(i) * 16))
#define FX_W_D(i)    ((const volatile int16_t *)(0xFF8015C4u + (uint32_t)(i) * 16))
#define FX_W_M       ((const volatile int16_t *)0xFF8016C4u)
#define FILTER_ID    0x0B                                                  /* ScreenId::for_fx(5) */
#define FILTER_TYPE  5
#define READ_FX_PARAM ((void *(*)(int, int))0x01A28DA0u)                  /* read_fx_param(slot, pos) -> std::vector<float> * */
static const int16_t fx_rng[4][2] = { { 64, 14528 }, { 0, 24576 }, { 0, 22528 }, { 8192, 32767 } };   /* plugin_host.cc */

static int fx_on_page(int *slot)                                          /* the plugin on the effect page (any), and its slot */
{
    int fp = FX_POS_B, pos2 = fp == 3 ? 1 : 0, n;
    if (coop_fx2_focused(pos2)) { *slot = 3 + pos2; n = coop_slot2_plugin(pos2); }
    else {
        int type; const volatile int16_t *w;
        if (fp == 3) { *slot = 2; type = FX_TYPE_M; w = FX_W_M; }
        else if (fp == 1) { int k = DRUM_KIT & 7; *slot = 1; type = FX_TYPE_D(k); w = FX_W_D(k); }
        else { int k = SYNTH_SLOT & 7; *slot = 0; type = FX_TYPE_S(k); w = FX_W_S(k); }
        n = type == FILTER_TYPE ? w[4] - MAGIC : 0;
    }
    return (n > 0 && n < SLOTS && coop_fx_is_plugin(n)) ? n : 0;
}
static int fx_shown(int *slot)                                            /* ... when that plugin's screen is ready */
{
    int n = fx_on_page(slot);
    return (n && g_es[n] && g_es[n]->fx && g_es[n]->ready) ? n : 0;
}
/* coop_ui.cc / coop_fx2view.cc: FX 2 of pos2 holds a plugin whose screen is ready (then its page is the filter id) */
extern "C" int coop_fx2_has_screen(int pos2)
{
    int n = coop_slot2_plugin(pos2 & 1);
    return (n > 0 && n < SLOTS && g_es[n] && g_es[n]->fx && g_es[n]->ready && coop_fx_is_plugin(n)) ? 1 : 0;
}
/* coop_ui.cc: an effect screen is what the page shows now */
extern "C" int coop_fx_screen_up(void)
{
    int slot, n = fx_shown(&slot);
    return n && CURRENT_SCREEN == FILTER_ID && SCREEN_TABLE[FILTER_ID] == (void *)g_es[n]->obj;
}

/* kennel op1_plugin.h op1_f01_to_q15: an IEEE float in [0, 1] -> 0..32767 without float code */
static int32_t f01_to_q15(uint32_t f)
{
    if ((int32_t)f <= 0) return 0;
    int e = (int)((f >> 23) & 0xff) - 127;
    if (e >= 0) return 32767;
    uint32_t m = (f & 0x7fffff) | 0x800000;
    int sh = 23 - 15 - e;
    if (sh >= 32) return 0;
    uint32_t v = m >> sh;
    return v > 32767 ? 32767 : (int32_t)v;
}
/* the view for slot (GUI thread): the turns as the stock pages see them, then plugin_host.cc's part */
static void view_fill(coop_fx_view_t *v, int slot, int n)
{
    volatile uint32_t *w = (volatile uint32_t *)v;
    for (unsigned i = 0; i < sizeof *v / 4; i++) w[i] = 0;
    v->slot = slot;
    if (slot <= 2) {
        int pos = slot == 2 ? 3 : slot;                                   /* the stock's position: 0 synth, 1 drum, 3 master */
        const uint32_t *const *vec = (const uint32_t *const *)READ_FX_PARAM(SHOWN_INDEX, pos);
        const uint32_t *b = vec ? vec[1] : 0, *e = vec ? vec[2] : 0;
        const volatile int16_t *raw = slot == 2 ? FX_W_M : slot == 1 ? FX_W_D(DRUM_KIT & 7) : FX_W_S(SYNTH_SLOT & 7);
        for (int k = 0; k < 4; k++) {
            if (b && b + k < e) v->knobs[k] = (int16_t)f01_to_q15(b[k]);
            else {                                                        /* no vector: the raw word through the ranges */
                int32_t q = ((int32_t)raw[k] - fx_rng[k][0]) * 32767 / (fx_rng[k][1] - fx_rng[k][0]);
                v->knobs[k] = (int16_t)(q < 0 ? 0 : q > 32767 ? 32767 : q);
            }
        }
    } else for (int k = 0; k < 4; k++) v->knobs[k] = (int16_t)coop_slot2_knob_q15(slot - 3, k);
    coop_fx_view_core(slot, n, v);
    v->mods = coop_scr_mods();
}

/* ---- the wrappers in each plugin screen's vtable ---- */
static struct escr *escr_of(void *self)
{
    for (int n = 1; n < SLOTS; n++) if (g_es[n] && (void *)g_es[n]->obj == self) return g_es[n];
    return 0;
}
static void escr_update(void *self, int dt)                               /* vt +0x1C, every frame while shown */
{
    struct escr *e = escr_of(self);
    if (!e || !e->ready) return;
    if (e->fx) {
        if (!e->fsc->update) return;
        int slot, n = fx_on_page(&slot);
        coop_fx_view_t v;
        view_fill(&v, slot, n == e->n ? n : e->n);
        g_fscr_updates++;
        e->fsc->update(self, dt, &v);
    } else if (e->sc->update) { g_escr_updates++; e->sc->update(self, dt); }
}
static void escr_init(void *self)                                         /* vt +0x24: the plugin's init, once */
{
    struct escr *e = escr_of(self);
    if (!e || e->inited) return;
    void (*init)(void *) = e->fx ? e->fsc->init : e->sc->init;
    if (init) { e->inited = 1; init(self); }
}

/* e9: the stock TE heap (TLSF, 0x840000 bytes at 0x0106B7CC, te-op1-cfw docs/memory-map.md §3) where a parsed SVG
 * lives. TLSF 2.4's control block starts with its signature 0x2A59FA59, then (statistics on: "te_new.cpp:31 out of
 * memory. tried to alloc %i; used %i; max %i") the used and max sizes. -1 when the block does not look like that.
 * [I: the layout is TLSF 2.4.6's; the stats line shows whether the signature matched.] */
#define TE_POOL       0x0106B7CCu
#define TE_POOL_BYTES 0x840000u
#define TE_RESERVE    (1024u * 1024u)                                      /* left in the TE heap after an effect screen's SVG */
extern "C" uint32_t coop_te_used(void)
{
    const volatile uint32_t *t = (const volatile uint32_t *)TE_POOL;
    if (t[0] != 0x2A59FA59u) return 0xFFFFFFFFu;
    uint32_t u = t[1];
    return u <= TE_POOL_BYTES ? u : 0xFFFFFFFFu;
}

static struct escr *build_one(int n, int id, const char *file)
{
    struct escr *e = (struct escr *)MALLOC(sizeof(struct escr));
    if (!e) return 0;
    volatile uint32_t *w = (volatile uint32_t *)e;
    for (unsigned i = 0; i < sizeof(struct escr) / 4; i++) w[i] = 0;
    char *p = e->path;
    for (const char *q = "../../plugins/"; *q; ) *p++ = *q++;
    for (const char *q = file; *q; ) *p++ = *q++;
    *p = 0;
    e->n = (int16_t)n;
    void *saved = SCREEN_TABLE[id];
    SCREEN_CTOR(e->obj, id, e->path);                                     /* registers itself in the table: put back */
    SCREEN_TABLE[id] = saved;
    for (int i = 0; i < 10; i++) e->vt[i] = ITER_SCREEN_VT[i];           /* is_visible etc. stay iter's (id == the object's) */
    e->vt[7] = (uint32_t)(uintptr_t)&escr_update;
    e->vt[9] = (uint32_t)(uintptr_t)&escr_init;
    e->obj[3] = (uint32_t)(uintptr_t)e->vt;
    return e;
}

static void build_all(void)
{
    g_iter_screen = SCREEN_TABLE[ITER_ID];
    for (int n = 1; n < SLOTS; n++) {
        const coop_engine_t *eng = coop_engine_at(n);
        if (!eng || !eng->screen || !g_file[n][0]) continue;
        struct escr *e = build_one(n, ITER_ID, g_file[n]);
        if (!e) { g_escr_failed++; continue; }
        e->sc = eng->screen;
        e->obj[0] = (uint32_t)(uintptr_t)SVG_LOAD((void *)(uintptr_t)e->obj[2]);
        if (!e->obj[0]) { g_escr_failed++; continue; }
        g_es[n] = e;
        escr_init(e->obj);
        __asm__ __volatile__("ssync;" ::: "memory");
        e->ready = 1;
        g_escr_built++;
    }
}

/* e9: the effect screens, once the stock filter screen has its document. An SVG is parsed into the TE heap; one that
 * would leave it under TE_RESERVE is skipped (that effect keeps the stock filter page), counted in g_fscr_skipped. */
static void build_fx_all(void)
{
    g_filter_screen = SCREEN_TABLE[FILTER_ID];
    for (int n = 1; n < SLOTS; n++) {
        const coop_fx_screen_t *sc = coop_fx_screen_of(n);
        if (!sc || !g_file[n][0] || g_es[n]) continue;
        uint32_t u0 = coop_te_used();
        if (u0 != 0xFFFFFFFFu && TE_POOL_BYTES - u0 < TE_RESERVE + 65536u) { g_fscr_skipped++; continue; }
        struct escr *e = build_one(n, FILTER_ID, g_file[n]);
        if (!e) { g_fscr_failed++; continue; }
        e->fsc = sc; e->fx = 1;
        e->obj[0] = (uint32_t)(uintptr_t)SVG_LOAD((void *)(uintptr_t)e->obj[2]);
        uint32_t u1 = coop_te_used();
        if (u0 != 0xFFFFFFFFu && u1 != 0xFFFFFFFFu && u1 > u0) g_fscr_te += u1 - u0;
        if (!e->obj[0]) { g_fscr_failed++; continue; }
        g_es[n] = e;
        escr_init(e->obj);
        __asm__ __volatile__("ssync;" ::: "memory");
        e->ready = 1;
        g_fscr_built++;
    }
}

/* ScreenId::for_synth case 14, through engine_screen.S: which object table slot 0x14 holds; returns the id. */
extern "C" int coop_synth_screen_select(void)
{
    void *iter = g_iter_screen;
    if (iter) {
        int n = engine_shown();
        void *want = n ? (void *)g_es[n]->obj : iter;
        if (SCREEN_TABLE[ITER_ID] != want) { SCREEN_TABLE[ITER_ID] = want; g_escr_swaps++; }
    }
    return ITER_ID;
}

/* e9: ScreenId::for_fx case 5 (the filter type, which every plugin effect runs as), through engine_screen.S, and every
 * frame (coop_screens_frame): table slot 0x0B holds the shown effect's screen object, or the stock filter screen. */
extern "C" int coop_fx_screen_select(void)
{
    void *filter = g_filter_screen;
    if (filter) {
        int slot, n = fx_shown(&slot);
        void *want = n ? (void *)g_es[n]->obj : filter;
        if (SCREEN_TABLE[FILTER_ID] != want) { SCREEN_TABLE[FILTER_ID] = want; g_fscr_swaps++; }
    }
    return FILTER_ID;
}

/* ---- the frame (kennel kn_screens.c): draw a frame-stable table, reset documents that left it ---- */
static void *g_frame[SCREEN_SLOTS];
static void *g_prev_docs[SCREEN_SLOTS];
static void *document(void *screen) { return screen ? *(void **)screen : 0; }
static void reset_document(void *doc)
{
    uint8_t *vt = *(uint8_t **)((uint8_t *)doc + 0x54);
    void (*reset)(void *) = *(void (**)(void *))(vt + 0x7C);
    reset((uint8_t *)doc + *(int16_t *)(vt + 0x78));
}

/* Image-time test switch (--set-word g_coop_scr_stock=1): the stock frame, no engine screens. */
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_scr_stock = 0; }

/* coop_ui.cc coop_ui_frame, in place of the stock screens_update(table). */
extern "C" void coop_screens_frame(void *table)
{
    if (g_coop_scr_stock) { SCREENS_UPDATE(table); return; }
    if (!g_built && g_plugins_booted) {                                   /* the screens, once: after the scan, once the stock
                                                                             iter screen has its document */
        void *it = SCREEN_TABLE[ITER_ID];
        if (it && document(it)) { g_built = 1; build_all(); }
    }
    if (!g_fx_built && g_plugins_scan_done) {                             /* e9: the effect screens, after the whole scan,
                                                                             once the stock filter screen has its document */
        void *f = SCREEN_TABLE[FILTER_ID];
        if (f && document(f)) { g_fx_built = 1; build_fx_all(); }
    }
    if (g_filter_screen) coop_fx_screen_select();                         /* e9: the page's position or focus may have
                                                                             changed without a new screen id (lift / drop) */
    void *volatile *live = (void *volatile *)table;
    for (int i = 0; i < SCREEN_SLOTS; i++) g_frame[i] = live[i];
    for (int i = 0; i < SCREEN_SLOTS; i++) {
        void *old = g_prev_docs[i];
        if (!old || old == document(g_frame[i])) continue;
        int present = 0;
        for (int j = 0; j < SCREEN_SLOTS && !present; j++) if (document(g_frame[j]) == old) present = 1;
        if (present) continue;                                            /* moved, not gone: keeps its caches */
        int dup = 0;
        for (int j = 0; j < i && !dup; j++) if (g_prev_docs[j] == old) dup = 1;
        if (!dup) { reset_document(old); g_escr_resets++; }
    }
    for (int i = 0; i < SCREEN_SLOTS; i++) g_prev_docs[i] = document(g_frame[i]);
    SCREENS_UPDATE(g_frame);
}

/* ---- the screen calls a plugin imports (sdk/coop_screen.h, sdk/coop_exports.cc) ---- */
#define NODE_FIND   ((void *(*)(void *, const char *, int32_t))0x019EB8ACu)   /* (document, id, assert_missing) */
#define NODE_XFORM  ((void (*)(void *, const int32_t *))0x019EAD58u)
#define LINE_RGBA   ((uint8_t *(*)(void *, const char *))0x01A3B854u)
#define FILL_RGBA   ((uint8_t *(*)(void *, const char *))0x01A3B96Cu)
#define SCREEN_DRAW ((void (*)(void *, int))0x01A3B4C8u)
#define RASTER      ((void *(*)(void))0x01A3B490u)
#define RASTER_LINE ((void (*)(void *, const int32_t *, const int32_t *, int32_t, const uint8_t *))0x01A24A4Cu)

/* te-op1-cfw ports/fx/op1/fx_screen.h: find in the screen's document, no assert; the style getters assert on a missing
 * id, so they are only called for an id the document has (a plugin's typo returns 0 instead of locking the OP-1). */
extern "C" void *coop_scr_find(void *scr, const char *id) { void *d = scr ? document(scr) : 0; return d ? NODE_FIND(d, id, 0) : 0; }
extern "C" void coop_scr_transform(void *node, const int32_t *m9) { if (node) NODE_XFORM(node, m9); }
extern "C" void coop_scr_visible(void *node, int on) { if (node) ((volatile uint8_t *)node)[0x50] = (uint8_t)(on != 0); }
extern "C" uint8_t *coop_scr_line_rgba(void *scr, const char *id) { return coop_scr_find(scr, id) ? LINE_RGBA(scr, id) : 0; }
extern "C" uint8_t *coop_scr_fill_rgba(void *scr, const char *id) { return coop_scr_find(scr, id) ? FILL_RGBA(scr, id) : 0; }
extern "C" void coop_scr_draw(void *scr, int dt) { SCREEN_DRAW(scr, dt); }
extern "C" void *coop_scr_raster(void) { return RASTER(); }
extern "C" void coop_scr_line(void *raster, const int32_t *p0, const int32_t *p1, int32_t width_q10, const uint8_t *rgba)
{
    if (raster) RASTER_LINE(raster, p0, p1, width_q10, rgba);
}
/* the four knob words of the synth slot on screen (0..32767, what render gets as params[0..3]) */
extern "C" void coop_scr_knobs(int16_t *out4)
{
    const int16_t *k = SYNTH_KNOBS(SYNTH_SLOT);
    for (int i = 0; i < 4; i++) out4[i] = k[i];
}
