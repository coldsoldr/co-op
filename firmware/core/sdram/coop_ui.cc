/*
 * coop-fw P3a: the second effect slot's page on the OP-1 (docs/screens.md).
 *
 * No stock screen is replaced: the page is an overlay drawn over whatever the stock screen drew this frame, from the
 * frame hook (gfx_render_frame's CALL screens_update at 0x019B2690 -> coop_ui_frame, region-C trampoline; kennel
 * kn_core.c kn_screens_update does the same for its overlays). Keys come from coop_key_event (lander_off.cc), the
 * encoders from the KeyHandler's encoder step (CALL 0x019B6C0C at 0x019C2E94 -> coop_ui_knob; kennel
 * knob_event_hook). Text is kennel's stroke font (third_party/kennel/synth_sdk/screens/stroke_font.h) drawn with the
 * stock raster line.
 *
 * Reached from an effect page: the current screen id (0x018B65C8) is one of the eight stock effect screens 8..15
 * (for_fx 0x01A3BEC8: delay 8, grid 9, punch 10, filter 11, nitro 12, cwo 13, phone 14, spring 15) and the effect
 * position byte (0x01DF4E3C: 0 synth, 1 drum, 3 master) says whose. Shift + Help opens the page for that position
 * (instrument slot 2 for synth and drum, master slot 2 for the master); Shift + Help again, Help, or any key that
 * leaves the page (mode keys, T1..T4) closes it, and those keys then act as usual; the keyboard plays through.
 * On the page: the four encoders turn the four knobs (1 % a detent); Shift + blue chooses the effect (none, the eight
 * stock types, the loaded plugins) and a blue push applies it, so turning past grid does not construct every type on
 * the way (grid's init takes about three audio blocks). Knob changes apply at once; the slot is saved to its file
 * when the page closes.
 *
 * Threads: keys and encoders in the KeyHandler thread, drawing in the Gfx thread; coop_slot2_set may allocate (never
 * from the audio interrupt). Fixed point only, no libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 * The stroke font is hundekraft's (kennel 3.1), GPL-3.0-or-later.
 */
#include <stdint.h>

#define SCREENS_UPDATE ((void (*)(void *))0x01A3BB20u)                 /* stock screens_update(table), gfx thread */
#define KNOB_STEP      ((void (*)(int, int16_t *))0x019B6C0Cu)          /* stock encoder step(knob, &delta) */
#define CUR_SCREEN     (*(volatile int *)0x018B65C8u)                  /* the screen id shown (kennel CUR_SCREEN) */
#define FX_POS         (*(volatile uint8_t *)0x01DF4E3Cu)               /* effect position: 0 synth, 1 drum, 3 master */
#define SHIFT_HELD     (*(volatile uint8_t *)0x01DD7F78u)

typedef struct { int32_t x, y; } op1_pt;                               /* Q10, 320 x 160 units */
#define op1_raster() (((void *(*)(void))0x01A3B490u)())
#define op1_raster_line(r, p0, p1, w_q10, rgba) \
    (((void (*)(void *, const op1_pt *, const op1_pt *, int32_t, const uint8_t *))0x01A24A4Cu)((r), (p0), (p1), (w_q10), (rgba)))
#include "../../third_party/kennel/synth_sdk/screens/stroke_font.h"

extern "C" int coop_slot2_set(int pos, int kind, int id, const uint16_t *knob);   /* plugin_host.cc */
extern "C" void coop_slot2_get(int pos, uint8_t *out10);
extern "C" int coop_slot2_save(void);
extern "C" const char *coop_plugin_name(int n);

#define KEY_HELP   0x00
#define KEY_ENC0   0x3B                                                 /* encoder pushes 0x3B..0x3E */
#define FX_TYPES   8
#define PLUG_MAX   64
#define DETENT     1024                                                 /* the step's delta per detent (kennel) */
#define KNOB_STEP_Q15 328                                               /* 1 % of 32767 */

static const char *const fx_names[FX_TYPES] = { "DELAY", "GRID", "PUNCH", "PHONE", "SPRING", "FILTER", "NITRO", "CWO" };

volatile uint32_t g_ui_frames, g_ui_opens, g_ui_applies;              /* stats / tests */
static volatile int s_open;                                            /* the page is up */
static int s_pos;                                                      /* slot 2 position: 0 instrument, 1 master */
static int s_kind, s_id;                                               /* the applied effect */
static int s_pick;                                                     /* the list index Shift + blue points at */
static uint16_t s_knob[4];
static int s_dirty;                                                    /* a change not yet saved */
static uint64_t s_swallow;                                             /* presses taken: their releases too */

static int on_fx_page(void) { int s = CUR_SCREEN; return s >= 8 && s <= 15; }

/* the effect list: 0 none, 1..8 the stock types, then the loaded plugins */
static int list_len(void) { int n = 1 + FX_TYPES; for (int p = 1; p < PLUG_MAX; p++) if (coop_plugin_name(p)) n++; return n; }
static void list_entry(int i, int *kind, int *id)
{
    if (i <= 0) { *kind = 0; *id = 0; return; }
    if (i <= FX_TYPES) { *kind = 1; *id = i - 1; return; }
    int k = FX_TYPES;
    for (int p = 1; p < PLUG_MAX; p++) if (coop_plugin_name(p) && ++k == i) { *kind = 2; *id = p; return; }
    *kind = 0; *id = 0;
}
static int list_index(int kind, int id)
{
    if (kind == 1) return 1 + id;
    if (kind == 2) { int k = FX_TYPES; for (int p = 1; p < PLUG_MAX; p++) if (coop_plugin_name(p)) { k++; if (p == id) return k; } }
    return 0;
}

static void load_slot(int pos)
{
    uint8_t b[10];
    coop_slot2_get(pos, b);
    s_kind = b[0]; s_id = b[1];
    for (int i = 0; i < 4; i++) s_knob[i] = (uint16_t)(b[2 + 2 * i] | (b[3 + 2 * i] << 8));
    s_pick = list_index(s_kind, s_id);
}

static void close_page(void)
{
    if (!s_open) return;
    s_open = 0;
    if (s_dirty) { coop_slot2_save(); s_dirty = 0; }
}

/* from coop_key_event (KeyHandler thread): 1 = consumed */
extern "C" int coop_ui_key(int code, int pressed)
{
    uint64_t bit = (uint64_t)1 << (code & 63);
    if (!pressed && (s_swallow & bit)) { s_swallow &= ~bit; return 1; }
    if (s_open && !on_fx_page()) close_page();                         /* the stock screen changed under us */
    if (!pressed) return 0;
    if (code == KEY_HELP && SHIFT_HELD && (s_open || on_fx_page())) {   /* Shift + Help: toggle */
        if (s_open) close_page();
        else { s_pos = FX_POS == 3 ? 1 : 0; load_slot(s_pos); s_open = 1; g_ui_opens++; }
        s_swallow |= bit;
        return 1;
    }
    if (!s_open) return 0;
    if (code == KEY_HELP) { close_page(); s_swallow |= bit; return 1; }
    if (code == KEY_ENC0) {                                            /* blue push: apply the effect pointed at */
        int kind, id;
        list_entry(s_pick, &kind, &id);
        if (coop_slot2_set(s_pos, kind, id, s_knob)) { s_kind = kind; s_id = id; s_dirty = 1; g_ui_applies++; }
        s_swallow |= bit;
        return 1;
    }
    if (code > KEY_ENC0 && code <= KEY_ENC0 + 3) { s_swallow |= bit; return 1; }   /* the other pushes: nothing */
    if (code >= 0x02 && code <= 0x09) close_page();                    /* a mode key or T1..T4 leaves the page, then acts */
    return 0;                                                          /* the keyboard, Shift and the rest play through */
}

/* encoder step hook (KeyHandler thread): the page takes every turn while it is up */
extern "C" void coop_ui_knob(int knob, int16_t *delta)
{
    if (!s_open || !on_fx_page()) { if (s_open) close_page(); KNOB_STEP(knob, delta); return; }
    int steps = *delta / DETENT;
    if (!steps) steps = *delta > 0 ? 1 : (*delta < 0 ? -1 : 0);
    if (knob == 0 && SHIFT_HELD) {                                     /* Shift + blue: the effect list */
        int n = list_len();
        s_pick = ((s_pick + steps) % n + n) % n;
        return;
    }
    if (knob < 0 || knob > 3) return;
    int32_t v = (int32_t)s_knob[knob] + steps * KNOB_STEP_Q15;
    s_knob[knob] = (uint16_t)(v < 0 ? 0 : (v > 32767 ? 32767 : v));
    coop_slot2_set(s_pos, s_kind, s_id, s_knob);                       /* the same effect: the knobs only */
    s_dirty = 1;
}

/* ---- drawing (Gfx thread) ---- */
static const uint8_t c_bg[4] = { 0, 0, 0, 255 }, c_text[4] = { 255, 255, 255, 255 }, c_dim[4] = { 110, 110, 110, 255 };
static const uint8_t c_enc[4][4] = { { 40, 120, 255, 255 }, { 40, 200, 90, 255 }, { 235, 235, 235, 255 }, { 255, 130, 30, 255 } };

static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static void name_of(int kind, int id, char *out)                       /* upper case for the stroke font, 15 chars */
{
    const char *s = kind == 1 && id >= 0 && id < FX_TYPES ? fx_names[id] : kind == 2 ? coop_plugin_name(id) : "NONE";
    if (!s) s = "?";
    int i = 0;
    for (; s[i] && i < 15; i++) out[i] = upper(s[i] == '_' ? ' ' : s[i]);
    out[i] = 0;
}
static void num(char *out, int v)                                      /* 0..100 */
{
    int i = 0;
    if (v >= 100) out[i++] = '1', out[i++] = '0', out[i++] = '0';
    else { if (v >= 10) out[i++] = (char)('0' + v / 10); out[i++] = (char)('0' + v % 10); }
    out[i] = 0;
}
static void fill(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t *c)   /* units, a thick line */
{
    int32_t ym = (y0 + y1) << 9, w = (y1 - y0) << 10;
    sf_line(r, x0 << 10, ym, x1 << 10, ym, w, c);
}

extern "C" void coop_ui_frame(void *table)
{
    SCREENS_UPDATE(table);
    if (!s_open) return;
    if (!on_fx_page()) return;                                         /* closed by the next key or turn */
    g_ui_frames++;
    void *r = op1_raster();
    if (!r) return;
    const int32_t S = 2 << 10, W = 1536;                               /* 2 px per font unit, 1.5 px strokes */
    for (int32_t y = -4; y < 164; y += 4) fill(r, -4, y, 324, y + 8, c_bg);   /* thin overlapping strokes: wide ones
                                                                                left rows uncovered at their edges (op1emu) */
    sf_text(r, 12 << 10, 10 << 10, S, s_pos ? "MASTER FX 2" : "INSTRUMENT FX 2", W, c_dim);
    char name[16];
    int pk, pi;
    list_entry(s_pick, &pk, &pi);
    int pending = pk != s_kind || pi != s_id;
    name_of(pk, pi, name);
    sf_text(r, 12 << 10, 32 << 10, 3 << 10, name, 2048, pending ? c_enc[0] : c_text);
    if (pending) sf_text(r, 12 << 10, 60 << 10, S, "PUSH BLUE TO SET", W, c_enc[0]);
    for (int k = 0; k < 4; k++) {                                      /* four bars in the encoder colours */
        int32_t x = 12 + k * 76, pct = (int32_t)(((uint32_t)s_knob[k] * 100u + 16383u) / 32767u);
        fill(r, x, 120, x + 64, 126, c_dim);
        fill(r, x, 120, x + (int32_t)((pct * 64) / 100), 126, c_enc[k]);
        char t[4]; num(t, pct);
        sf_text(r, x << 10, 132 << 10, S, t, W, c_enc[k]);
    }
    sf_text(r, 12 << 10, 84 << 10, S, "SHIFT+BLUE: EFFECT", W, c_dim);
}
