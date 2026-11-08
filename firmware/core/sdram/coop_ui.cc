/*
 * coop-fw: the UI hooks (P3a, u1 .. u7). docs/screens.md, docs/ui-features.md.
 *
 * No stock screen is replaced: coop draws over whatever the stock drew this frame, from the frame hook
 * (gfx_render_frame's CALL screens_update at 0x019B2690 -> coop_ui_frame, region-C trampoline; kennel's overlay
 * model). Keys come from coop_key_event (lander_off.cc), the encoders from the KeyHandler's encoder step (CALL
 * 0x019B6C0C at 0x019C2E94 -> coop_ui_knob). Text is the stock font (coop_text.h); the stroke font (kennel) draws
 * fills and bars. This file routes the keys, turns and frames to the features and owns FX 2's focus and the lists'
 * second column.
 *
 * FX 2 (docs/ui-features.md §7). On an effect page (screens 8..15, the position byte 0x01DF4E3C: 0 synth, 1 drum,
 * 3 master) **drop** focuses FX 2 and **lift** FX 1; the focus is kept per position, so T3 brings back whichever slot
 * was last touched. In focus, FX 2 with a STOCK type looks exactly like the stock effect page of that type
 * (coop_fx2view.cc feeds the page slot 2's knobs and animation and keeps its screen id up); the four encoders turn
 * slot 2's knobs (1 % a detent), the page's own key (T3; T2 on the mixer) switches FX 2 off and on ("fx2 off" top
 * right), Shift + blue picks another type (its name in blue top left until a blue push applies it). FX 2 holding a
 * plugin, or nothing, keeps coop's own page (black, the name, four bars), since the stock has no page for it.
 *
 * The lists (§7, §8): shift + T3 / T4 open the stock browser (screen 0x24; the list object at 0x01DE511C says which:
 * FX 0x01BAB748, LFO 0x01BAB738; the owner controller at 0x01DE5124, first word = mode). Stock loads the blue column
 * on the turn and leaves the right half empty, so coop draws a second column there ("none" and the rows), moved by
 * green (four detents a row), applied by the page key (T3 / T4) to FX 2 / LFO 2. Other closing keys discard it.
 *
 * u7: every file write asked for here is done by the main loop (coop_ui_service from coop_fx_service), never on the
 * KeyHandler thread (a yaffs write there crashed the device).
 *
 * Threads: keys and encoders in the KeyHandler thread, drawing in the Gfx thread. Fixed point only, no libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 * The stroke font is hundekraft's (kennel 3.1), GPL-3.0-or-later.
 */
#include <stdint.h>

#define SCREENS_UPDATE ((void (*)(void *))0x01A3BB20u)                 /* stock screens_update(table), gfx thread */
#define KNOB_STEP      ((void (*)(int, int16_t *))0x019B6C0Cu)          /* stock encoder step(knob, &delta) */
#define CUR_SCREEN     (*(volatile int *)0x018B65C8u)                  /* the screen id shown (kennel CUR_SCREEN) */
#define CUR_MODE       (*(volatile int *)0xFF907D58u)
#define FX_POS         (*(volatile uint8_t *)0x01DF4E3Cu)               /* effect position: 0 synth, 1 drum, 3 master */
#define SYNTH_INST     (*(volatile int *)0xFF8016D4u)
#define DRUM_KIT       (*(volatile int *)0xFF8016D8u)
#define FX_TYPE_S(i)   (*(volatile int *)(0xFF801734u + (uint32_t)(i) * 4))   /* FX 1's type per synth slot / drum kit / the master */
#define FX_TYPE_D(i)   (*(volatile int *)(0xFF801754u + (uint32_t)(i) * 4))
#define FX_TYPE_M      (*(volatile int *)0xFF801090u)
#define SHIFT_HELD     (*(volatile uint8_t *)0x01DD7F78u)
#define KNOB_TOUCHED(k) (*(volatile uint8_t *)(0x01DD7F74u + (k)))     /* the stock highlights that dial this frame */
#define LIST_PTR       (*(volatile uint32_t *)0x01DE511Cu)             /* the browser's list object (stale after close) */
#define OWNER_PTR      (*(volatile uint32_t *)0x01DE5124u)             /* the browser's owner controller: [+0] = mode */
#define LIST_FX        0x01BAB748u
#define LIST_LFO       0x01BAB738u
#define SCREEN_BROWSER 0x24
#define SCREEN_PLAYMODE 40
#define TAPE_SCREEN    21

typedef struct { int32_t x, y; } op1_pt;                               /* Q10, 320 x 160 units */
#define op1_raster() (((void *(*)(void))0x01A3B490u)())
#define op1_raster_line(r, p0, p1, w_q10, rgba) \
    (((void (*)(void *, const op1_pt *, const op1_pt *, int32_t, const uint8_t *))0x01A24A4Cu)((r), (p0), (p1), (w_q10), (rgba)))
#include "../../third_party/kennel/synth_sdk/screens/stroke_font.h"
#include "coop_text.h"

extern "C" int coop_slot2_set(int pos, int kind, int id, const uint16_t *knob);   /* plugin_host.cc */
extern "C" void coop_slot2_get(int pos, uint8_t *out10);
extern "C" void coop_slot2_request_save(void);
extern "C" void coop_slot2_bypass(int pos, int off);
extern "C" int coop_slot2_bypassed(int pos);
extern "C" int coop_slot2_req_type(int pos2);                         /* the stock type asked for, -1 none / plugin */
extern "C" const char *coop_plugin_name(int n);
extern "C" int coop_lfo2_get_type(void);                              /* lfo2.cc */
extern "C" void coop_lfo2_set_type(int type);
extern "C" int coop_settings_is_open(void);                            /* coop_settings.cc */
extern "C" int coop_settings_key(int code, int pressed);
extern "C" int coop_settings_knob(int knob, int steps);
extern "C" void coop_settings_draw(void *r);
extern "C" void coop_settings_service(void);
extern "C" int coop_random_key(int code, int pressed);                 /* coop_random.cc */
extern "C" int coop_fn_key(int code, int pressed);                     /* coop_tape_ui.cc */
extern "C" int coop_tape_key(int code, int pressed);
extern "C" void coop_tape_draw(void *r);
extern "C" void coop_markers_service(void);
extern "C" int coop_bend_key(int code, int pressed);                   /* coop_bend.cc */
extern "C" int coop_bend_knob(int knob, int steps);
extern "C" void coop_bend_draw(void *r);
extern "C" void coop_save_draw(void *r);                               /* coop_save.cc */
extern "C" int coop_rename_key(int code, int pressed);                 /* coop_rename.cc */
extern "C" int coop_rename_knob(int knob, int steps);
extern "C" void coop_rename_draw(void *r);
extern "C" void coop_rename_service(void);
extern "C" int coop_drumpan_knob(int knob, int steps);                 /* coop_drumpan.cc */
extern "C" void coop_drumpan_draw(void *r);
extern "C" void coop_fx2_post_screen(int pos, int fx2);                /* coop_fx2view.cc */

/* u8: a slot-2 set that may allocate (a new type: the twin object, a delay line, a tank) runs on the MAIN LOOP; the
 * KeyHandler thread only posts the request (the u7 lesson again: heap work off the main loop hung op1emu at the
 * first drop, and a yaffs write from the key thread crashed the device). Knob-only sets of the same type stay direct. */
static volatile int s_set_req;                                         /* 1 = pending */
static int s_set_pos, s_set_kind, s_set_id, s_set_off, s_set_focus, s_set_post_pos; static uint16_t s_set_k[4]; static int s_set_apply;
static void slot2_request(int pos, int kind, int id, const uint16_t *k, int off, int focus, int post_pos, int apply)
{
    s_set_pos = pos; s_set_kind = kind; s_set_id = id; s_set_off = off; s_set_focus = focus; s_set_post_pos = post_pos; s_set_apply = apply;
    for (int i = 0; i < 4; i++) s_set_k[i] = k[i];
    __asm__ __volatile__("ssync;" ::: "memory");
    s_set_req = 1;
}
static void slot2_service(void);                                       /* below (needs the slot state) */
volatile uint32_t g_ui_service_calls;
extern "C" void coop_ui_service(void) { g_ui_service_calls++; coop_settings_service(); coop_markers_service(); coop_rename_service(); slot2_service(); }

#define KEY_T3     0x08
#define KEY_T4     0x09
#define KEY_LIFT   0x0A
#define KEY_DROP   0x0B
#define KEY_ENC0   0x3B                                                 /* encoder pushes 0x3B..0x3E */
#define FX_TYPES   8
#define PLUG_MAX   64
#define DETENT     1024                                                 /* the step's delta per detent (kennel) */
#define KNOB_STEP_Q15 328                                               /* 1 % of 32767 */

static const char *const fx_names[FX_TYPES] = { "delay", "grid", "punch", "phone", "spring", "filter", "nitro", "cwo" };   /* lower case: the stock font */

volatile uint32_t g_ui_frames, g_ui_opens, g_ui_applies, g_ui_toggles, g_ui_col2_sets, g_ui_knobs2;   /* stats / tests */
static volatile uint8_t s_focus2[2];                                   /* per position (0 instrument, 1 master): FX 2 in focus */
static int s_pos;                                                      /* the position the focus state below was loaded for */
static int s_loaded;                                                   /* s_kind / s_id / s_knob / s_pick hold slot s_pos */
static int s_kind, s_id;                                               /* the applied effect */
static int s_pick;                                                     /* the list index Shift + blue points at */
static uint16_t s_knob[4];
static int s_dirty;                                                    /* a change not yet saved */
static uint64_t s_swallow;                                             /* presses taken: their releases too */

static int on_fx_page(void) { int s = CUR_SCREEN; return s >= 8 && s <= 15; }
static int pos_now(void) { return FX_POS == 3 ? 1 : 0; }
static int toggle_key(void) { return KEY_T3; }                         /* the page's own key (mixer page 2 too): the stock on / off of FX 1 */
extern "C" int coop_fx2_focused(int pos2) { return s_focus2[pos2 & 1]; }

/* the effect list: the stock types 0..7, then the loaded plugins (u8: no "none"; FX 2 is off instead, T3) */
static int list_len(void) { int n = FX_TYPES; for (int p = 1; p < PLUG_MAX; p++) if (coop_plugin_name(p)) n++; return n; }
static void list_entry(int i, int *kind, int *id)
{
    if (i < 0) i = 0;
    if (i < FX_TYPES) { *kind = 1; *id = i; return; }
    int k = FX_TYPES - 1;
    for (int p = 1; p < PLUG_MAX; p++) if (coop_plugin_name(p) && ++k == i) { *kind = 2; *id = p; return; }
    *kind = 1; *id = 0;
}
static int list_index(int kind, int id)
{
    if (kind == 1) return id;
    if (kind == 2) { int k = FX_TYPES - 1; for (int p = 1; p < PLUG_MAX; p++) if (coop_plugin_name(p)) { k++; if (p == id) return k; } }
    return 0;
}
static void load_slot(int pos)
{
    uint8_t b[10];
    coop_slot2_get(pos, b);
    s_kind = b[0]; s_id = b[1];
    for (int i = 0; i < 4; i++) s_knob[i] = (uint16_t)(b[2 + 2 * i] | (b[3 + 2 * i] << 8));
    s_pick = list_index(s_kind, s_id);
    s_pos = pos; s_loaded = 1;
}
static void focus_sync(void)                                           /* the position shown: its slot loaded */
{
    int pos = pos_now();
    if (!s_loaded || pos != s_pos) load_slot(pos);
}
static int focused_here(void) { return on_fx_page() && s_focus2[pos_now()]; }

/* ---- u8: LFO 2 on the stock LFO page (screens 24..30), as FX 2 on the effect page. The sound's LFO 2 (type, on,
 * eight params) lives in its JSON (coop_sound.cc); drop focuses it, lift LFO 1, T4 switches it off / on, the four
 * encoders edit its params as the stock's LFO page edits LFO 1's (ApplyRange: v += delta * scale >> 15, clamped to
 * the type's range; k + 4 with Shift), Shift + T4 opens the stock list whose green column picks the type. ---- */
extern "C" int coop_lfo2_kind_type(int *on);                           /* lfo2.cc */
extern "C" void coop_lfo2_set_sound(int type, int on, const int16_t *p);
extern "C" void coop_lfo2_mid_params(int type, int16_t *p);
extern "C" const int16_t *coop_lfo2_ranges(int type);
extern "C" int coop_sound_lfo2(int kind, int slot, int *type, int *on, int16_t *p8);
extern "C" void coop_lfo2_post_screen(int pos, int lfo2);              /* coop_fx2view.cc */
#define LFO_TYPE_S(i)  (*(volatile int *)(0xFF801384u + (uint32_t)(i) * 4))
#define LFO_TYPE_D(i)  (*(volatile int *)(0xFF8013A4u + (uint32_t)(i) * 4))
static volatile uint8_t s_lfo2_focus[2];                               /* per instrument: 0 synth, 1 drum */
static int on_lfo_page(void) { int s = CUR_SCREEN; return s >= 24 && s <= 30 && (CUR_MODE == 0 || CUR_MODE == 1); }
static int lfo_pos(void) { return CUR_MODE == 1 ? 1 : 0; }
extern "C" int coop_lfo2_focused(int pos) { return s_lfo2_focus[pos & 1]; }
static void lfo2_params(int16_t *p)                                    /* the shown sound's LFO 2 params (its type's middles when none) */
{
    int t, on;
    int slot = (lfo_pos() ? DRUM_KIT : SYNTH_INST) & 7;
    if (!coop_sound_lfo2(lfo_pos(), slot, &t, &on, p)) coop_lfo2_mid_params(0, p);
}
static void lfo2_default(void)                                         /* a sound without an LFO 2 takes LFO 1's type (one slot 2 can run), off */
{
    int t, on; int16_t p[8];
    int slot = (lfo_pos() ? DRUM_KIT : SYNTH_INST) & 7;
    if (coop_sound_lfo2(lfo_pos(), slot, &t, &on, p) && t >= 0 && t <= 2) return;
    int t1 = lfo_pos() ? LFO_TYPE_D(slot) : LFO_TYPE_S(slot);
    if (t1 < 0 || t1 > 2) t1 = 0;                                      /* element / midi / bend / crank cannot run on slot 2: tremolo */
    coop_lfo2_mid_params(t1, p);
    coop_lfo2_set_sound(t1, 0, p);
}
static void lfo2_knob(int k, int steps)                                /* k 0..7 (4..7 shifted) */
{
    int t, on; int16_t p[8];
    int slot = (lfo_pos() ? DRUM_KIT : SYNTH_INST) & 7;
    if (!coop_sound_lfo2(lfo_pos(), slot, &t, &on, p) || t < 0 || t > 2) return;
    const int16_t *rg = coop_lfo2_ranges(t);
    int lo = rg[k * 3], hi = rg[k * 3 + 1], scale = rg[k * 3 + 2];
    if (hi <= lo || !scale) return;                                    /* the stock's "s0": the encoder changes nothing */
    int32_t v = p[k] + ((int32_t)steps * DETENT * scale >> 15);
    if (v < lo) v = lo; else if (v > hi) v = hi;
    p[k] = (int16_t)v;
    coop_lfo2_set_sound(t, on, p);
    KNOB_TOUCHED(k & 3) = 1;
}
static void *r_or(void *r) { return r; }
/* u8: an empty FX 2 takes FX 1's type, switched off (no "none" on the list): the first focus fills it */
extern "C" void coop_slot2_bypass(int pos, int off);
static void slot2_default(int pos)
{
    if (s_kind) return;
    int fp = FX_POS, slot = fp == 1 ? DRUM_KIT : SYNTH_INST;
    int type = fp == 3 ? FX_TYPE_M : (fp == 1 ? FX_TYPE_D(slot) : FX_TYPE_S(slot));
    if (type < 0 || type > 7) type = 0;
    const int16_t *k1 = (const int16_t *)(fp == 3 ? 0xFF8016C4u : ((fp == 1 ? 0xFF8015C4u : 0xFF801444u) + (uint32_t)(slot & 7) * 16));   /* FX 1's knob words (read_fx_param 0x01A28DA0: int16 in range units) */
    const int16_t *rg = (const int16_t *)(0x01DE5C50u + (uint32_t)type * 0x30u);   /* the type's Ranges row: {min, max, default} x 8 */
    uint16_t k[4];
    for (int i = 0; i < 4; i++) {                                      /* range units -> q15 (the inverse of plugin_host.cc knob_map) */
        int lo = rg[3 * i], hi = rg[3 * i + 1], v = k1[i];
        if (hi < lo) { int s = lo; lo = hi; hi = s; }
        int32_t q = hi > lo ? ((int32_t)(v - lo) * 32767) / (hi - lo) : 0;
        k[i] = (uint16_t)(q < 0 ? 0 : (q > 32767 ? 32767 : q));
    }
    slot2_request(pos, 1, type, k, 1, 0, fp, 0);                        /* the main loop sets it (it allocates), off */
}
extern volatile uint32_t g_s2_trace;                                   /* plugin_host.cc: debug progress marker */
static void slot2_service(void)                                        /* main loop: the pending set, then the page */
{
    if (!s_set_req) return;
    s_set_req = 0;
    g_s2_trace = 0x30;
    if (!coop_slot2_set(s_set_pos, s_set_kind, s_set_id, s_set_k)) { g_s2_trace = 0x3f; return; }
    g_s2_trace = 0x31;
    if (s_set_off >= 0) coop_slot2_bypass(s_set_pos, s_set_off);
    if (s_set_focus) s_focus2[s_set_pos] = s_set_kind ? 1 : 0;
    s_loaded = 0;                                                      /* the key thread reloads the slot at its next look */
    if (s_set_apply) g_ui_applies++;
    coop_slot2_request_save();
    g_s2_trace = 0x32;
    if (s_set_post_pos >= 0) coop_fx2_post_screen(s_set_post_pos, 1);
    g_s2_trace = 0x33;
}
static void save_if_dirty(void) { if (s_dirty) { coop_slot2_request_save(); s_dirty = 0; } }

/* ---- the second column of the stock FX / LFO browser ---- */
static void name_of(int kind, int id, char *out);
static const int fx_list_type[7] = { 7, 0, 1, 6, 3, 2, 4 };          /* the stock list's rows (cwo, delay, grid, nitro,
                                                                          phone, punch, spring) as coop's type ids */
static const char *const lfo_col_names[4] = { "none", "tremolo", "value", "random" };
static int s_br_seen;                                                  /* the browser kind the column was set up for */
static int s_col2, s_col2_acc, s_col2_disp, s_col2_touched;
static int col2_count(int bk) { return bk == 1 ? 7 + (list_len() - FX_TYPES) : 4; }
static int browser_kind(void)                                          /* 0 none, 1 the FX list, 2 the LFO list */
{
    if (CUR_SCREEN != SCREEN_BROWSER) return 0;
    uint32_t l = LIST_PTR;
    return l == LIST_FX ? 1 : l == LIST_LFO ? 2 : 0;
}
static int browser_pos(void)                                           /* slot 2 position: 0 instrument, 1 master */
{
    uint32_t o = OWNER_PTR;
    if (o < 0x01000000u || o >= 0x02000000u) return FX_POS == 3 ? 1 : 0;
    return *(volatile int *)o == 3 ? 1 : 0;
}
static void col2_entry(int i, int *kind, int *id)                      /* the FX column: the seven (the stock list's order), the plugins */
{
    if (i < 0) i = 0;
    if (i < 7) { *kind = 1; *id = fx_list_type[i]; return; }
    list_entry(i - 7 + FX_TYPES, kind, id);                            /* the plugin rows of list_entry */
}
static int col2_index(int kind, int id)
{
    if (kind == 1) { for (int i = 0; i < 7; i++) if (fx_list_type[i] == id) return i; return 0; }
    if (kind == 2) { int k = list_index(kind, id); return k >= FX_TYPES ? k - FX_TYPES + 7 : 0; }
    return 0;
}
static void col2_name(int bk, int i, char *out)
{
    if (bk == 2) { const char *q = lfo_col_names[i & 3]; int k = 0; for (; q[k]; k++) out[k] = q[k]; out[k] = 0; return; }
    int kind, id; col2_entry(i, &kind, &id); name_of(kind, id, out);
}
static int br_sync(void)                                               /* (re)set the column when a browser comes up */
{
    int bk = browser_kind();
    if (bk != s_br_seen) {
        s_br_seen = bk; s_col2_acc = 0; s_col2_disp = 0; s_col2_touched = 0;
        if (bk == 1) { uint8_t b[10]; coop_slot2_get(browser_pos(), b); s_col2 = col2_index(b[0], b[1]); }
        else if (bk == 2) { int t = coop_lfo2_get_type(); s_col2 = t >= 0 && t <= 2 ? 1 + t : 0; }
        if (s_col2 >= 6) s_col2_disp = s_col2 - 5;
    }
    return bk;
}
static void col2_apply(int bk)                                         /* the page key closes the browser: load column 2 */
{
    if (!s_col2_touched) return;
    if (bk == 1) {
        int pos = browser_pos(), kind, id;
        uint8_t b[10]; uint16_t k[4];
        coop_slot2_get(pos, b);
        for (int i = 0; i < 4; i++) k[i] = (uint16_t)(b[2 + 2 * i] | (b[3 + 2 * i] << 8));
        col2_entry(s_col2, &kind, &id);
        slot2_request(pos, kind, id, k, -1, 1, -1, 0);               /* the main loop sets it (it may allocate) and focuses FX 2 */
        g_ui_col2_sets++;
    } else if (bk == 2) {
        coop_lfo2_set_type(s_col2 ? s_col2 - 1 : 7);
        coop_slot2_request_save(); g_ui_col2_sets++;
    }
    s_col2_touched = 0;
}

/* ---- keys (KeyHandler thread): 1 = consumed ---- */
extern "C" int coop_ui_key(int code, int pressed)
{
    if (coop_settings_is_open()) return coop_settings_key(code, pressed);
    if (coop_random_key(code, pressed)) return 1;                       /* synth / drum held + arrows: randomise / back */
    if (coop_fn_key(code, pressed)) return 1;                           /* Help: a tap on release, a hold is fn */
    if (coop_tape_key(code, pressed)) return 1;                         /* tape / mixer: solo, markers */
    if (coop_bend_key(code, pressed)) return 1;                         /* shift + arrows on the synth: the bend */
    if (coop_rename_key(code, pressed)) return 1;                       /* the preset browser: naming */
    uint64_t bit = (uint64_t)1 << (code & 63);
    if (!pressed && (s_swallow & bit)) { s_swallow &= ~bit; return 1; }
    if (!pressed) return 0;
    {
        int bk = br_sync();                                            /* a browser is up: the page key applies column 2 */
        if (bk) { if ((bk == 1 && code == KEY_T3) || (bk == 2 && code == KEY_T4)) col2_apply(bk); return 0; }
    }
    if (on_lfo_page() && (code == KEY_LIFT || code == KEY_DROP)) {     /* u8: lift: LFO 1 in focus; drop: LFO 2 (the shown sound's) */
        int pos = lfo_pos();
        if (code == KEY_DROP) { if (!s_lfo2_focus[pos]) { s_lfo2_focus[pos] = 1; lfo2_default(); g_ui_opens++; coop_lfo2_post_screen(pos, 1); } }
        else if (s_lfo2_focus[pos]) { s_lfo2_focus[pos] = 0; coop_lfo2_post_screen(pos, 0); }
        s_swallow |= bit;
        return 1;
    }
    if (on_lfo_page() && s_lfo2_focus[lfo_pos()] && code == KEY_T4 && !SHIFT_HELD) {   /* T4: LFO 2 off / on (Shift + T4: the stock list) */
        int on, t = coop_lfo2_kind_type(&on); int16_t p[8];
        if (t >= 0 && t <= 2) { lfo2_params(p); coop_lfo2_set_sound(t, !on, p); g_ui_toggles++; }
        s_swallow |= bit;
        return 1;
    }
    if (on_fx_page() && (code == KEY_LIFT || code == KEY_DROP)) {      /* lift: FX 1 in focus; drop: FX 2 */
        int pos = pos_now();
        if (code == KEY_DROP) { if (!s_focus2[pos]) { s_focus2[pos] = 1; load_slot(pos); slot2_default(pos); g_ui_opens++; coop_fx2_post_screen(FX_POS, 1); } }
        else if (s_focus2[pos]) { s_focus2[pos] = 0; save_if_dirty(); coop_fx2_post_screen(FX_POS, 0); }
        s_swallow |= bit;
        return 1;
    }
    if (!focused_here()) return 0;
    focus_sync();
    if (code == toggle_key() && !SHIFT_HELD) {                         /* FX 2 off / on, as the stock key does for FX 1; Shift + T3 opens the stock list (both columns) */
        coop_slot2_bypass(s_pos, !coop_slot2_bypassed(s_pos));
        s_dirty = 1; g_ui_toggles++;
        s_swallow |= bit;
        return 1;
    }
    if (code == KEY_ENC0) {                                            /* blue push: apply the effect pointed at */
        int kind, id;
        list_entry(s_pick, &kind, &id);
        if (kind == s_kind && id == s_id) { s_swallow |= bit; return 1; }   /* the same effect: nothing to do */
        slot2_request(s_pos, kind, id, s_knob, -1, 0, FX_POS, 1);    /* the main loop builds it, then shows its page */
        s_kind = kind; s_id = id;
        s_swallow |= bit;
        return 1;
    }
    if (code > KEY_ENC0 && code <= KEY_ENC0 + 3) { s_swallow |= bit; return 1; }   /* the other pushes: nothing */
    if (code >= 0x02 && code <= 0x09) save_if_dirty();                 /* a mode key or another T key: leaves the page, then acts */
    return 0;                                                          /* the keyboard, Shift and the rest play through */
}

/* ---- encoders (KeyHandler thread) ---- */
extern "C" void coop_ui_knob(int knob, int16_t *delta)
{
    int steps = *delta / DETENT;
    if (!steps) steps = *delta > 0 ? 1 : (*delta < 0 ? -1 : 0);
    if (coop_settings_is_open()) { coop_settings_knob(knob, steps); return; }
    if (coop_bend_knob(knob, steps)) return;                         /* shifted white / orange on the synth's T2 */
    if (coop_drumpan_knob(knob, steps)) return;                        /* shifted blue on the drum's T2 */
    if (coop_rename_knob(knob, steps)) return;                         /* naming: every turn */
    {
        int bk = br_sync();
        if (bk) {
            if (knob == 1 || knob == 5) {                              /* green: column 2, four detents a row as blue */
                int n = col2_count(bk);
                s_col2_acc += steps;
                while (s_col2_acc >= 4) { s_col2_acc -= 4; if (s_col2 < n - 1) s_col2++; }
                while (s_col2_acc <= -4) { s_col2_acc += 4; if (s_col2 > 0) s_col2--; }
                if (s_col2 - s_col2_disp >= 6) s_col2_disp = s_col2 - 5;
                if (s_col2 < s_col2_disp) s_col2_disp = s_col2;
                s_col2_touched = 1;
                return;
            }
            KNOB_STEP(knob, delta);                                    /* blue: the stock column */
            return;
        }
    }
    if (on_lfo_page() && s_lfo2_focus[lfo_pos()]) {                    /* u8: the LFO page's encoders edit LFO 2 as the stock edits LFO 1 */
        if (knob >= 0 && knob <= 3) lfo2_knob(knob + (SHIFT_HELD ? 4 : 0), steps);
        return;
    }
    if (!focused_here()) { KNOB_STEP(knob, delta); return; }
    focus_sync();
    if (knob == 0 && SHIFT_HELD) {                                     /* Shift + blue: the effect list */
        int n = list_len();
        s_pick = ((s_pick + steps) % n + n) % n;
        return;
    }
    if (knob < 0 || knob > 3) return;
    int32_t v = (int32_t)s_knob[knob] + steps * KNOB_STEP_Q15;
    s_knob[knob] = (uint16_t)(v < 0 ? 0 : (v > 32767 ? 32767 : v));
    coop_slot2_set(s_pos, s_kind, s_id, s_knob);                       /* the same effect: the knobs only */
    KNOB_TOUCHED(knob) = 1;                                            /* the stock page highlights the dial */
    s_dirty = 1; g_ui_knobs2++;
}

/* ---- drawing (Gfx thread) ---- */
static const uint8_t c_bg[4] = { 0, 0, 0, 255 }, c_text[4] = { 0xDF, 0xD9, 0xFF, 255 }, c_title[4] = { 0xAE, 0xB1, 0xDC, 255 },
                     c_grey[4] = { 0x62, 0x5D, 0x77, 255 }, c_bar[4] = { 0x38, 0x35, 0x72, 255 };
static const uint8_t c_enc[4][4] = { { 0x69, 0x8E, 0xFF, 255 }, { 0x00, 0xED, 0x95, 255 }, { 0xFF, 0xFF, 0xFF, 255 }, { 0xFF, 0x3A, 0x5D, 255 } };
enum { TX_TITLE_I, TX_TITLE_M, TX_NAME, TX_NAME_BLUE, TX_NAME_GREY, TX_PUSH, TX_OFF, TX_HINT, TX_HINT2, TX_PICK, TX_PICK2, TX_OFF2,
       TX_K0, TX_KD0 = TX_K0 + 4, TX_ROW0 = TX_KD0 + 4, TX_N = TX_ROW0 + 6 };
static const uint8_t c_col2[4] = { 0x00, 0xED, 0x95, 255 }, c_col2_cursor[4] = { 0x00, 0x47, 0x5B, 255 };   /* the stock's own right-column colours */
static struct coop_text s_tx[TX_N];

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static void name_of(int kind, int id, char *out)                       /* lower case for the stock font, 15 chars */
{
    const char *s = kind == 1 && id >= 0 && id < FX_TYPES ? fx_names[id] : kind == 2 ? coop_plugin_name(id) : "none";
    if (!s) s = "?";
    int i = 0;
    for (; s[i] && i < 15; i++) out[i] = lower(s[i] == '_' ? ' ' : s[i]);
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
static void draw_col2(void *r, int bk)                                 /* the stock browser's right half: its geometry
                                                                          (presetbrowser.svg: cursor x 185..300, rows 23 apart) */
{
    int n = col2_count(bk);
    for (int i = 0; i < 6; i++) {
        int row = s_col2_disp + i;
        if (row >= n) break;
        int32_t y = 6 + 23 * i;
        if (row == s_col2) for (int32_t yy = y; yy < y + 18; yy += 2) fill(r, 185, yy, 300, yy + 4, c_col2_cursor);
        char name[16]; col2_name(bk, row, name);
        coop_text_draw(&s_tx[TX_ROW0 + i], r, 191, y + 2, 12, c_col2, name, 0);
    }
}
static void draw_fx2_page(void *r)                                     /* coop's page: FX 2 holds a plugin or nothing */
{
    g_ui_frames++;
    for (int32_t y = -4; y < 164; y += 4) fill(r, -4, y, 324, y + 8, c_bg);   /* thin overlapping strokes: wide ones
                                                                                left rows uncovered at their edges (op1emu) */
    int off = coop_slot2_bypassed(s_pos);
    if (s_pos) coop_text_draw(&s_tx[TX_TITLE_M], r, 12, 8, 12, c_title, "master fx 2", 0);
    else coop_text_draw(&s_tx[TX_TITLE_I], r, 12, 8, 12, c_title, "instrument fx 2", 0);
    char name[16];
    int pk, pi;
    list_entry(s_pick, &pk, &pi);
    int pending = pk != s_kind || pi != s_id;
    name_of(pk, pi, name);
    if (pending) coop_text_draw(&s_tx[TX_NAME_BLUE], r, 12, 26, 24, c_enc[0], name, 0);
    else if (off) coop_text_draw(&s_tx[TX_NAME_GREY], r, 12, 26, 24, c_grey, name, 0);
    else coop_text_draw(&s_tx[TX_NAME], r, 12, 26, 24, c_text, name, 0);
    if (pending) coop_text_draw(&s_tx[TX_PUSH], r, 12, 60, 12, c_enc[0], "push blue to set", 0);
    else if (off) coop_text_draw(&s_tx[TX_OFF], r, 12, 60, 12, c_text, "fx2 off", 0);
    for (int k = 0; k < 4; k++) {                                      /* four bars in the encoder colours */
        int32_t x = 12 + k * 76, pct = (int32_t)(((uint32_t)s_knob[k] * 100u + 16383u) / 32767u);
        fill(r, x, 120, x + 64, 126, c_bar);
        fill(r, x, 120, x + (int32_t)((pct * 64) / 100), 126, off ? c_grey : c_enc[k]);
        char t[4]; num(t, pct);
        if (off) coop_text_draw(&s_tx[TX_KD0 + k], r, x, 130, 12, c_grey, t, 0);
        else coop_text_draw(&s_tx[TX_K0 + k], r, x, 130, 12, c_enc[k], t, 0);
    }
    coop_text_draw(&s_tx[TX_HINT], r, 12, 84, 12, c_grey, "lift fx1   t3 on off", 0);
    coop_text_draw(&s_tx[TX_HINT2], r, 12, 100, 12, c_grey, "shift blue  type", 0);
}

extern "C" void coop_ui_frame(void *table)
{
    SCREENS_UPDATE(table);
    { void *rv = op1_raster(); if (rv) coop_save_draw(rv); }            /* "overwriting" over the save clock, any page */
    if (coop_settings_is_open()) { void *rs = op1_raster(); if (rs) coop_settings_draw(rs); return; }
    if (CUR_MODE == 2 && CUR_SCREEN == TAPE_SCREEN) { void *rt = op1_raster(); if (rt) coop_tape_draw(rt); return; }
    if (CUR_MODE == 0 && CUR_SCREEN == SCREEN_PLAYMODE) { void *rp = op1_raster(); if (rp) coop_bend_draw(rp); return; }
    if (CUR_MODE == 1) { void *rd = op1_raster(); if (rd) coop_drumpan_draw(rd); }   /* the PAN page on the drum's shifted T2 */
    if (CUR_SCREEN == SCREEN_BROWSER && LIST_PTR == 0x01DDD688u) { void *rn = op1_raster(); if (rn) coop_rename_draw(rn); return; }
    {
        int bk = br_sync();
        if (bk) { void *rb = op1_raster(); if (rb) draw_col2(rb, bk); return; }
    }
    if (on_lfo_page() && s_lfo2_focus[lfo_pos()]) return;              /* u8: the stock LFO page shows LFO 2 whole, its own "LFO OFF" included (coop_fx2view.cc) */
    if (!focused_here()) return;                                       /* FX 1 in focus: the stock page alone */
    focus_sync();
    void *r = op1_raster();
    if (!r) return;
    if (coop_slot2_req_type(s_pos) < 0) { draw_fx2_page(r); return; }   /* a plugin, or nothing: coop's page */
    /* a stock type: the stock page shows slot 2 whole, dials, animation and its own "FX OFF" (coop_fx2view.cc); only a
     * pending pick is added */
    int pk, pi;
    list_entry(s_pick, &pk, &pi);
    if (pk != s_kind || pi != s_id) {
        char name[16]; name_of(pk, pi, name);
        coop_text_draw(&s_tx[TX_PICK], r, 12, 2, 12, c_enc[0], name, 0);
        coop_text_draw(&s_tx[TX_PICK2], r, 12, 16, 12, c_enc[0], "push blue", 0);
    }
}
