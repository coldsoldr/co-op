/*
 * coop-fw P3a / u1: the second effect slot's page on the OP-1 (docs/screens.md, docs/ui-features.md §7).
 *
 * No stock screen is replaced: the page is an overlay drawn over whatever the stock screen drew this frame, from the
 * frame hook (gfx_render_frame's CALL screens_update at 0x019B2690 -> coop_ui_frame, region-C trampoline; kennel
 * kn_core.c kn_screens_update does the same for its overlays). Keys come from coop_key_event (lander_off.cc), the
 * encoders from the KeyHandler's encoder step (CALL 0x019B6C0C at 0x019C2E94 -> coop_ui_knob; kennel
 * knob_event_hook). Text is kennel's stroke font (third_party/kennel/synth_sdk/screens/stroke_font.h) drawn with the
 * stock raster line. The settings menu (coop_settings.cc) rides the same three hooks and takes them while it is open.
 *
 * Reached from an effect page: the current screen id (0x018B65C8) is one of the eight stock effect screens 8..15
 * (for_fx 0x01A3BEC8: delay 8, grid 9, punch 10, filter 11, nitro 12, cwo 13, phone 14, spring 15) and the effect
 * position byte (0x01DF4E3C: 0 synth, 1 drum, 3 master) says whose. u1 (the owner's decision, 2026-11-07): on an
 * effect page **drop** focuses FX 2 (the page opens) and **lift** focuses FX 1 (it closes); the focus is remembered
 * per position, so T3 brings back whichever slot was last touched. While FX 2 is focused the page's own key (T3 on
 * the synth and drum, T2 on the mixer) toggles FX 2 off and on ("FX2 OFF"), as the stock key does for FX 1. A mode
 * key or another T key leaves the page and then acts as usual; the keyboard plays through. (P3a's Shift + Help is
 * gone; lift and drop on an effect page no longer copy / paste the sound, they still do on the other pages.)
 * On the page: the four encoders turn the four knobs (1 % a detent); Shift + blue chooses the effect (none, the eight
 * stock types, the loaded plugins) and a blue push applies it, so turning past grid does not construct every type on
 * the way (grid's init takes about three audio blocks). Knob changes apply at once; the slot (type, knobs, off) is
 * saved to its file when the page closes.
 *
 * u3, the lists (docs/ui-features.md §7, §8; the stock browser mapped 2026-11-07, docs/screens.md "u3"): shift + T3
 * (and shift + T4) open the stock browser, screen 0x24, one class for every list; the list object at 0x01DE511C
 * says which (FX 0x01BAB748, LFO 0x01BAB738), the owner controller at 0x01DE5124 whose first word is its mode (0
 * synth, 1 drum, 3 mixer) says whose. Stock loads the blue column on the turn and leaves the right half of the
 * screen empty (its "preset" column, green by its own design), and sends green, white and orange to a child knob
 * whose range is 0. coop draws a second column there, "none" and the rows of the list, moved by the green encoder
 * (4 detents a row, as blue), and applies it when the page key (T3 / T4) closes the browser: FX 2 of that position
 * (coop_slot2_set, then FX 2 is the slot in focus) or LFO 2 (coop_lfo2_set_type). Any other closing key discards
 * the green choice. The mixer's master FX page is page 2, key T3 (its browser too), not T2 as u1 had it.
 *
 * Threads: keys and encoders in the KeyHandler thread, drawing in the Gfx thread; coop_slot2_set may allocate (never
 * from the audio interrupt). Fixed point only, no libc. u2: the text is the stock font (coop_text.h), the numbers in
 * their encoder colours, a pending choice in blue (a blue push applies it).
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
#define LIST_PTR       (*(volatile uint32_t *)0x01DE511Cu)             /* the browser's list object (stale after close) */
#define OWNER_PTR      (*(volatile uint32_t *)0x01DE5124u)             /* the browser's owner controller: [+0] = mode */
#define LIST_FX        0x01BAB748u
#define LIST_LFO       0x01BAB738u
#define SCREEN_BROWSER 0x24

typedef struct { int32_t x, y; } op1_pt;                               /* Q10, 320 x 160 units */
#define op1_raster() (((void *(*)(void))0x01A3B490u)())
#define op1_raster_line(r, p0, p1, w_q10, rgba) \
    (((void (*)(void *, const op1_pt *, const op1_pt *, int32_t, const uint8_t *))0x01A24A4Cu)((r), (p0), (p1), (w_q10), (rgba)))
#include "../../third_party/kennel/synth_sdk/screens/stroke_font.h"
#include "coop_text.h"

extern "C" int coop_slot2_set(int pos, int kind, int id, const uint16_t *knob);   /* plugin_host.cc */
extern "C" void coop_slot2_get(int pos, uint8_t *out10);
extern "C" int coop_slot2_save(void);
extern "C" void coop_slot2_bypass(int pos, int off);
extern "C" int coop_slot2_bypassed(int pos);
extern "C" const char *coop_plugin_name(int n);
extern "C" int coop_lfo2_get_type(void);                              /* lfo2.cc */
extern "C" void coop_lfo2_set_type(int type);
extern "C" int coop_settings_is_open(void);                            /* coop_settings.cc */
extern "C" int coop_settings_key(int code, int pressed);
extern "C" int coop_settings_knob(int knob, int steps);
extern "C" void coop_settings_draw(void *r);
extern "C" int coop_random_key(int code, int pressed);                 /* coop_random.cc (u4) */
extern "C" int coop_fn_key(int code, int pressed);                     /* coop_tape_ui.cc (u4): Help as fn, solo, markers */
extern "C" int coop_tape_key(int code, int pressed);
extern "C" void coop_tape_draw(void *r);
extern "C" int coop_bend_key(int code, int pressed);                   /* coop_bend.cc (u5) */
extern "C" int coop_bend_knob(int knob, int steps);
extern "C" void coop_bend_draw(void *r);
extern "C" void coop_save_draw(void *r);                               /* coop_save.cc (u5) */
extern "C" int coop_rename_key(int code, int pressed);                 /* coop_rename.cc (u5) */
extern "C" int coop_rename_knob(int knob, int steps);
extern "C" int coop_rename_active(void);
extern "C" void coop_rename_draw(void *r);
#define SCREEN_PLAYMODE 40
#define CUR_MODE       (*(volatile int *)0xFF907D58u)
#define TAPE_SCREEN    21

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

volatile uint32_t g_ui_frames, g_ui_opens, g_ui_applies, g_ui_toggles, g_ui_col2_sets;   /* stats / tests */
static volatile int s_open;                                            /* the page is up */
static int s_pos;                                                      /* slot 2 position: 0 instrument, 1 master */
static int s_kind, s_id;                                               /* the applied effect */
static int s_pick;                                                     /* the list index Shift + blue points at */
static uint16_t s_knob[4];
static int s_dirty;                                                    /* a change not yet saved */
static uint64_t s_swallow;                                             /* presses taken: their releases too */
static volatile uint8_t s_focus2[2];                                   /* per position: FX 2 was the slot last touched */

static int on_fx_page(void) { int s = CUR_SCREEN; return s >= 8 && s <= 15; }
static int pos_now(void) { return FX_POS == 3 ? 1 : 0; }
static int toggle_key(void) { return KEY_T3; }                         /* the page's own key (mixer page 2 too): the stock on / off of FX 1 */

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
static void open_page(void)
{
    s_pos = pos_now();
    load_slot(s_pos);
    s_open = 1;
    g_ui_opens++;
}
static void close_page(void)
{
    if (!s_open) return;
    s_open = 0;
    if (s_dirty) { coop_slot2_save(); s_dirty = 0; }
}

/* ---- u3: the second column of the stock FX / LFO browser ---- */
static void name_of(int kind, int id, char *out);                   /* below, the drawing section */
static const int fx_list_type[7] = { 7, 0, 1, 6, 3, 2, 4 };          /* the stock list's rows (cwo, delay, grid, nitro,
                                                                          phone, punch, spring) as coop's type ids */
static const char *const lfo_col_names[4] = { "none", "tremolo", "value", "random" };
static int s_br_seen;                                                  /* the browser kind the column was set up for */
static int s_col2, s_col2_acc, s_col2_disp, s_col2_touched;
static int col2_count(int bk) { return bk == 1 ? 8 + (list_len() - 1 - FX_TYPES) : 4; }
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
static void col2_entry(int i, int *kind, int *id)                      /* the FX column: none, the seven, the plugins */
{
    if (i <= 0) { *kind = 0; *id = 0; return; }
    if (i <= 7) { *kind = 1; *id = fx_list_type[i - 1]; return; }
    list_entry(i - 8 + 1 + FX_TYPES, kind, id);                        /* the plugin rows of list_entry */
}
static int col2_index(int kind, int id)
{
    if (kind == 1) { for (int i = 0; i < 7; i++) if (fx_list_type[i] == id) return 1 + i; return 0; }
    if (kind == 2) { int k = list_index(kind, id); return k > FX_TYPES ? k - FX_TYPES + 7 : 0; }
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
        if (coop_slot2_set(pos, kind, id, k)) { s_focus2[pos] = kind ? 1 : 0; coop_slot2_save(); g_ui_col2_sets++; }
    } else if (bk == 2) {
        coop_lfo2_set_type(s_col2 ? s_col2 - 1 : 7);
        coop_slot2_save(); g_ui_col2_sets++;
    }
    s_col2_touched = 0;
}

/* from coop_key_event (KeyHandler thread): 1 = consumed */
extern "C" int coop_ui_key(int code, int pressed)
{
    if (coop_settings_is_open()) return coop_settings_key(code, pressed);
    if (coop_random_key(code, pressed)) return 1;                       /* synth held + arrows: randomise / back */
    if (coop_fn_key(code, pressed)) return 1;                           /* Help: a tap on release, a hold is fn */
    if (coop_tape_key(code, pressed)) return 1;                         /* tape: solo, markers */
    if (coop_bend_key(code, pressed)) return 1;                         /* shift + arrows on the synth: the bend */
    if (coop_rename_key(code, pressed)) return 1;                       /* the preset browser: naming */
    uint64_t bit = (uint64_t)1 << (code & 63);
    if (!pressed && (s_swallow & bit)) { s_swallow &= ~bit; return 1; }
    if (s_open && !on_fx_page()) close_page();                         /* the stock screen changed under us */
    if (!pressed) return 0;
    {
        int bk = br_sync();                                            /* a browser is up: the page key applies column 2 */
        if (bk) { if ((bk == 1 && code == KEY_T3) || (bk == 2 && code == KEY_T4)) col2_apply(bk); return 0; }
    }
    if (on_fx_page() && (code == KEY_LIFT || code == KEY_DROP)) {      /* lift: FX 1 in focus; drop: FX 2 */
        int pos = pos_now();
        if (code == KEY_DROP) { s_focus2[pos] = 1; if (!s_open) open_page(); }
        else { s_focus2[pos] = 0; close_page(); }
        s_swallow |= bit;
        return 1;
    }
    if (!s_open) return 0;
    if (code == toggle_key()) {                                        /* FX 2 off / on, as the stock key does for FX 1 */
        coop_slot2_bypass(s_pos, !coop_slot2_bypassed(s_pos));
        s_dirty = 1; g_ui_toggles++;
        s_swallow |= bit;
        return 1;
    }
    if (code == KEY_ENC0) {                                            /* blue push: apply the effect pointed at */
        int kind, id;
        list_entry(s_pick, &kind, &id);
        if (coop_slot2_set(s_pos, kind, id, s_knob)) { s_kind = kind; s_id = id; s_dirty = 1; g_ui_applies++; }
        s_swallow |= bit;
        return 1;
    }
    if (code > KEY_ENC0 && code <= KEY_ENC0 + 3) { s_swallow |= bit; return 1; }   /* the other pushes: nothing */
    if (code >= 0x02 && code <= 0x09) close_page();                    /* a mode key or another T key leaves the page, then acts */
    return 0;                                                          /* the keyboard, Shift and the rest play through */
}

/* encoder step hook (KeyHandler thread): the page takes every turn while it is up */
extern "C" void coop_ui_knob(int knob, int16_t *delta)
{
    int steps = *delta / DETENT;
    if (!steps) steps = *delta > 0 ? 1 : (*delta < 0 ? -1 : 0);
    if (coop_settings_is_open()) { coop_settings_knob(knob, steps); return; }
    if (coop_bend_knob(knob, steps)) return;                           /* shifted white / orange on T2 */
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
    if (!s_open || !on_fx_page()) { if (s_open) close_page(); KNOB_STEP(knob, delta); return; }
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
static const uint8_t c_bg[4] = { 0, 0, 0, 255 }, c_text[4] = { 0xDF, 0xD9, 0xFF, 255 }, c_title[4] = { 0xAE, 0xB1, 0xDC, 255 },
                     c_grey[4] = { 0x62, 0x5D, 0x77, 255 }, c_bar[4] = { 0x38, 0x35, 0x72, 255 };
static const uint8_t c_enc[4][4] = { { 0x69, 0x8E, 0xFF, 255 }, { 0x00, 0xED, 0x95, 255 }, { 0xFF, 0xFF, 0xFF, 255 }, { 0xFF, 0x3A, 0x5D, 255 } };
enum { TX_TITLE_I, TX_TITLE_M, TX_NAME, TX_NAME_BLUE, TX_NAME_GREY, TX_PUSH, TX_OFF, TX_HINT, TX_HINT2,
       TX_CORNER, TX_CORNER_OFF, TX_K0, TX_KD0 = TX_K0 + 4, TX_ROW0 = TX_KD0 + 4, TX_N = TX_ROW0 + 6 };
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
        if (row == s_col2) for (int32_t yy = y; yy < y + 18; yy += 2) fill(r, 185, yy, 300, yy + 4, c_col2_cursor);   /* 2-unit steps: 4-unit ones striped (op1emu) */
        char name[16]; col2_name(bk, row, name);
        coop_text_draw(&s_tx[TX_ROW0 + i], r, 191, y + 2, 12, c_col2, name, 0);
    }
}

extern "C" void coop_ui_frame(void *table)
{
    SCREENS_UPDATE(table);
    { void *rv = op1_raster(); if (rv) coop_save_draw(rv); }            /* "overwriting" over the save clock, any page */
    if (coop_settings_is_open()) { void *rs = op1_raster(); if (rs) coop_settings_draw(rs); return; }
    if (CUR_MODE == 2 && CUR_SCREEN == TAPE_SCREEN) { void *rt = op1_raster(); if (rt) coop_tape_draw(rt); return; }
    if (CUR_MODE == 0 && CUR_SCREEN == SCREEN_PLAYMODE) { void *rp = op1_raster(); if (rp) coop_bend_draw(rp); return; }
    if (CUR_SCREEN == SCREEN_BROWSER && LIST_PTR == 0x01DDD688u) { void *rn = op1_raster(); if (rn) coop_rename_draw(rn); return; }
    {
        int bk = br_sync();
        if (bk) { void *rb = op1_raster(); if (rb) draw_col2(rb, bk); return; }
    }
    if (!on_fx_page()) return;                                         /* closed by the next key or turn */
    if (!s_open && s_focus2[pos_now()]) open_page();                   /* FX 2 was the slot last touched here: back to it */
    if (!s_open) {                                                     /* a slot 2 that runs is always visible: "fx2" in
                                                                          the corner of its position's effect pages */
        uint8_t b[10];
        int pos = pos_now();
        coop_slot2_get(pos, b);
        void *rr = op1_raster();
        if (b[0] && rr) {
            if (coop_slot2_bypassed(pos)) coop_text_draw(&s_tx[TX_CORNER_OFF], rr, 262, 2, 12, c_grey, "fx2 off", 0);
            else coop_text_draw(&s_tx[TX_CORNER], rr, 262, 2, 12, c_enc[0], "fx2", 0);
        }
        return;
    }
    g_ui_frames++;
    void *r = op1_raster();
    if (!r) return;
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
