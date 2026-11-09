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
#define ITER_SCREEN_VT ((const uint32_t *)0x019103FCu)
#define ITER_ID        0x14
#define SYNTH_SLOT     (*(volatile int *)0xFF8016D4u)
#define SYNTH_KNOBS(s) ((const int16_t *)(0xFF8013C4u + (uint32_t)((s) & 7) * 16u))
#define MALLOC         ((void *(*)(uint32_t))0x01A9F348u)
#define MAGIC          0x5300
#define SLOTS          64
#define OBJ_WORDS      32                                                  /* the iter screen class uses up to +0x74 */

extern "C" const coop_engine_t *coop_engine_at(int n);                    /* engine_host.cc */
extern volatile uint32_t g_plugins_booted;                                /* plugin_host.cc */

struct escr { uint32_t obj[OBJ_WORDS]; uint32_t vt[10]; char path[56]; const coop_screen_t *sc; uint8_t ready, inited; };
static struct escr *g_es[SLOTS];
static char g_file[SLOTS][44];                                            /* the plugin file of each engine with a screen */
static void *volatile g_iter_screen;                                      /* the stock iter screen object */
static volatile int g_built;
volatile uint32_t g_escr_built, g_escr_failed, g_escr_swaps, g_escr_resets, g_escr_updates;

/* plugin_host.cc, boot (main thread), before any frame builds: this engine's file carries its screen. */
extern "C" void coop_engine_screen_file(int n, const char *file)
{
    if (n <= 0 || n >= SLOTS) return;
    int i = 0;
    for (; file[i] && i < (int)sizeof g_file[0] - 1; i++) g_file[n][i] = file[i];
    g_file[n][i] = 0;
}

static int engine_shown(void)
{
    int n = SYNTH_KNOBS(SYNTH_SLOT)[4] - MAGIC;
    return (n > 0 && n < SLOTS && g_es[n] && g_es[n]->ready && coop_engine_at(n)) ? n : 0;
}

/* ---- the wrappers in each engine screen's vtable ---- */
static struct escr *escr_of(void *self)
{
    for (int n = 1; n < SLOTS; n++) if (g_es[n] && (void *)g_es[n]->obj == self) return g_es[n];
    return 0;
}
static void escr_update(void *self, int dt)                               /* vt +0x1C, every frame while shown */
{
    struct escr *e = escr_of(self);
    if (e && e->ready && e->sc->update) { g_escr_updates++; e->sc->update(self, dt); }
}
static void escr_init(void *self)                                         /* vt +0x24: the plugin's init, once */
{
    struct escr *e = escr_of(self);
    if (e && !e->inited && e->sc->init) { e->inited = 1; e->sc->init(self); }
}

static void build_all(void)
{
    g_iter_screen = SCREEN_TABLE[ITER_ID];
    for (int n = 1; n < SLOTS; n++) {
        const coop_engine_t *eng = coop_engine_at(n);
        if (!eng || !eng->screen || !g_file[n][0]) continue;
        struct escr *e = (struct escr *)MALLOC(sizeof(struct escr));
        if (!e) { g_escr_failed++; continue; }
        volatile uint32_t *w = (volatile uint32_t *)e;
        for (unsigned i = 0; i < sizeof(struct escr) / 4; i++) w[i] = 0;
        char *p = e->path;
        for (const char *q = "../../plugins/"; *q; ) *p++ = *q++;
        for (const char *q = g_file[n]; *q; ) *p++ = *q++;
        *p = 0;
        e->sc = eng->screen;
        void *saved = SCREEN_TABLE[ITER_ID];
        SCREEN_CTOR(e->obj, ITER_ID, e->path);                            /* registers itself in the table: put back */
        SCREEN_TABLE[ITER_ID] = saved;
        for (int i = 0; i < 10; i++) e->vt[i] = ITER_SCREEN_VT[i];       /* is_visible etc. stay iter's */
        e->vt[7] = (uint32_t)(uintptr_t)&escr_update;
        e->vt[9] = (uint32_t)(uintptr_t)&escr_init;
        e->obj[3] = (uint32_t)(uintptr_t)e->vt;
        e->obj[0] = (uint32_t)(uintptr_t)SVG_LOAD((void *)(uintptr_t)e->obj[2]);
        if (!e->obj[0]) { g_escr_failed++; continue; }
        g_es[n] = e;
        escr_init(e->obj);
        __asm__ __volatile__("ssync;" ::: "memory");
        e->ready = 1;
        g_escr_built++;
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
