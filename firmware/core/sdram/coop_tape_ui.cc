/*
 * coop-fw u4 / u7: the tape's solo and markers, and Help as the fn key (docs/ui-features.md §9, §11).
 *
 * Help as fn: the manifesto's deep layer and the solo combo hold Help with another key. Stock enters help mode on
 * the Help press, so coop swallows the press, and on the release sends the stock a press + release only if nothing
 * used Help while it was down: a tap still opens help, a hold is a modifier (coop_fn_held).
 *
 * Solo (u7, the owner's rules): on the tape or the mixer, Shift + Help + T1..T4 toggles that track in the SOLO SET;
 * several keys together solo several tracks (as the stock's Shift + T1 + T2 mutes both). While the set is not empty
 * every track outside it is muted through the stock's own mute flags (u8[4] at 0xFF8010A0, Core::update_mutes
 * 0x01967ED0, the same path as the stock mute; the mixer page shows the flags); the flags before the first solo come
 * back when the set empties. Nothing is drawn: the mutes say it. Mutes are volatile across boots in stock.
 *
 * Markers: four, one per encoder colour. On the tape page Shift + an encoder push places that colour's marker at
 * the playhead ([0xFF802974], 1/64 samples), removes it when the playhead is on it (within 3 px at the current
 * zoom), moves it otherwise; a plain push locates to it (the stock tape_locate 0x01A55024, which suspends the loop
 * as the stock's own jumps do). Drawn as little flags above the tape (u7: a pole and a ">" pennant in the encoder
 * colour, a grey line through the tracks), the tape scrolling under the fixed centre line at x 160.13 (kennel's
 * geometry; zoom = the stock float px/s at 0x018C183C read as bits). Saved to /yaffs2/user/coop_markers.bin ("CMK"
 * 1, four int32 samples, -1 = none) by the main loop (coop_markers_service), read after the tape mounts, cleared by
 * an erase. The pushes are free on the tape page in stock.
 *
 * KeyHandler thread for keys, Gfx thread for drawing, main thread for the files. Fixed point only, no libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 * The marker geometry follows hundekraft's markers.c (GPL-3.0-or-later).
 */
#include <stdint.h>

#define CUR_MODE     (*(volatile int *)0xFF907D58u)
#define CUR_SCREEN   (*(volatile int *)0x018B65C8u)
#define TAPE_SCREEN  21
#define SHIFT_HELD   (*(volatile uint8_t *)0x01DD7F78u)
#define MUTE(t)      (*(volatile uint8_t *)(0xFF8010A0u + (uint32_t)(t)))
#define UPDATE_MUTES ((void (*)(void))0x01967ED0u)
#define TAPE_REDRAW  (*(volatile uint8_t *)0x01B6039Cu)
#define TAPE_POS_Q6  (*(volatile int32_t *)0xFF802974u)
#define TAPE_LOCATE  ((void (*)(int32_t))0x01A55024u)
#define TAPE_MAX     0xF23FA0
#define ZOOM_BITS    (*(volatile uint32_t *)0x018C183Cu)
#define KEY_DISPATCH ((void (*)(int, int))0x019B6C74u)
#define Y_OPEN   ((int (*)(const char *, int, int))0x01A997F4u)
#define Y_CLOSE  ((int (*)(int))0x01A99908u)
#define Y_READ   ((int (*)(int, void *, int))0x01A99C90u)
#define Y_WRITE  ((int (*)(int, const void *, int))0x01A99F84u)
#define KEY_HELP 0x00
#define KEY_T1   0x06
#define KEY_ENC0 0x3B
#define MK_FILE  "/yaffs2/user/coop_markers.bin"
#define CENTRE_X 163973                                                 /* 160.13 px, Q10 (kennel, measured on hardware) */

typedef struct { int32_t x, y; } op1_pt;
#define op1_raster_line(r, p0, p1, w_q10, rgba) \
    (((void (*)(void *, const op1_pt *, const op1_pt *, int32_t, const uint8_t *))0x01A24A4Cu)((r), (p0), (p1), (w_q10), (rgba)))
#include "../../third_party/kennel/synth_sdk/screens/stroke_font.h"

volatile uint32_t g_solo_sets, g_marker_sets, g_marker_jumps, g_fn_taps;   /* stats / tests */
static int s_fn_held, s_fn_used;
static uint8_t s_solo_mask;                                             /* the soloed tracks, bit t */
static uint8_t s_mute_before[4];
static int32_t s_mk[4] = { -1, -1, -1, -1 };
static volatile int s_mk_pending;                                       /* the main loop writes the file */
static uint64_t s_swallow;

/* ---- Help as fn ---- */
extern "C" int coop_fn_held(void) { return s_fn_held; }
extern "C" void coop_fn_use(void) { if (s_fn_held) s_fn_used = 1; }      /* e4: fn was a modifier for another control */
/* e4c (the owner, 2026-11-10): Help with Shift held never reaches the stock. Shift + Help is the deep layer's modifier
 * pair, so shift down, Help down, Help up opened the stock's date and time screen when nothing else was touched. Shift
 * held on the press or on the release keeps the tap. */
volatile uint32_t g_fn_shift_kept;
extern "C" int coop_fn_key(int code, int pressed)                      /* 1 = consumed */
{
    if (code != KEY_HELP) return 0;
    if (pressed) { s_fn_held = 1; s_fn_used = SHIFT_HELD ? 1 : 0; if (SHIFT_HELD) g_fn_shift_kept++; return 1; }
    s_fn_held = 0;
    if (!s_fn_used && SHIFT_HELD) { s_fn_used = 1; g_fn_shift_kept++; }
    if (!s_fn_used) { KEY_DISPATCH(KEY_HELP, 1); KEY_DISPATCH(KEY_HELP, 0); g_fn_taps++; }   /* a tap: help, as stock */
    return 1;
}

/* ---- solo ---- */
static void solo_toggle(int n)
{
    if (!s_solo_mask) for (int t = 0; t < 4; t++) s_mute_before[t] = MUTE(t);
    s_solo_mask ^= (uint8_t)(1u << n);
    if (!s_solo_mask) for (int t = 0; t < 4; t++) MUTE(t) = s_mute_before[t];
    else for (int t = 0; t < 4; t++) MUTE(t) = (uint8_t)!(s_solo_mask & (1u << t));
    UPDATE_MUTES();
    TAPE_REDRAW = 1;
    g_solo_sets++;
}

/* ---- markers ---- */
static int32_t zoom_q10(void)                                           /* float px/s -> Q10, 1..4096 (kennel op1_feature_tape.h) */
{
    uint32_t b = ZOOM_BITS;
    int e = (int)((b >> 23) & 0xFF) - 127;
    if ((b >> 31) || e < 0) return 1 << 10;
    if (e >= 12) return 4096 << 10;
    uint32_t m = (b & 0x7FFFFF) | 0x800000;
    return (int32_t)(m >> (13 - e));
}
static int save_markers(void)                                           /* main thread */
{
    uint8_t b[20] = { 'C', 'M', 'K', 1 };
    for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) b[4 + 4 * i + k] = (uint8_t)((uint32_t)s_mk[i] >> (8 * k));
    int fd = Y_OPEN(MK_FILE, 0x242, 0x180);
    if (fd < 0) return 0;
    int ok = Y_WRITE(fd, b, 20) == 20;
    return (Y_CLOSE(fd) == 0) && ok;
}
extern "C" void coop_markers_service(void)                              /* main loop (coop_fx_service) */
{
    if (!s_mk_pending) return;
    s_mk_pending = 0;
    save_markers();
}
extern "C" void coop_tape_ui_boot(void)                                 /* main thread, after the tape mounts */
{
    uint8_t b[20];
    int fd = Y_OPEN(MK_FILE, 0, 0);
    if (fd < 0) return;
    int got = Y_READ(fd, b, 20);
    Y_CLOSE(fd);
    if (got != 20 || b[0] != 'C' || b[1] != 'M' || b[2] != 'K' || b[3] != 1) return;
    for (int i = 0; i < 4; i++) {
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) v |= (uint32_t)b[4 + 4 * i + k] << (8 * k);
        s_mk[i] = (int32_t)v;
        if (s_mk[i] < 0 || s_mk[i] > TAPE_MAX) s_mk[i] = -1;
    }
}
extern "C" void coop_markers_clear(void)                                /* the erase (tape_edit.cc, main thread) */
{
    for (int i = 0; i < 4; i++) s_mk[i] = -1;
    save_markers();
}
static void marker_push(int e)
{
    int32_t pos = TAPE_POS_Q6 >> 6;
    if (pos < 0) pos = 0;
    if (pos > TAPE_MAX) pos = TAPE_MAX;
    if (SHIFT_HELD) {
        int32_t tol = (3 * 1024 * 44100) / zoom_q10();
        int32_t d = s_mk[e] < 0 ? -1 : (s_mk[e] > pos ? s_mk[e] - pos : pos - s_mk[e]);
        s_mk[e] = (d >= 0 && d <= tol) ? -1 : pos;                      /* on it: remove; elsewhere or none: here */
        s_mk_pending = 1;
        g_marker_sets++;
    } else if (s_mk[e] >= 0) {
        TAPE_LOCATE(s_mk[e]);
        g_marker_jumps++;
    }
    TAPE_REDRAW = 1;
}

/* from coop_ui_key (KeyHandler thread), after coop_fn_key: 1 = consumed */
extern "C" int coop_tape_key(int code, int pressed)
{
    uint64_t bit = (uint64_t)1 << (code & 63);
    if (!pressed) { if (s_swallow & bit) { s_swallow &= ~bit; return 1; } return 0; }
    int mode = CUR_MODE;
    if (code >= KEY_T1 && code <= KEY_T1 + 3 && SHIFT_HELD && s_fn_held && (mode == 2 || mode == 3)) {   /* Shift + Help + Tn: solo */
        s_fn_used = 1;
        solo_toggle(code - KEY_T1);
        s_swallow |= bit;
        return 1;
    }
    if (mode == 2 && code >= KEY_ENC0 && code <= KEY_ENC0 + 3 && CUR_SCREEN == TAPE_SCREEN) {   /* the pushes: markers */
        marker_push(code - KEY_ENC0);
        s_swallow |= bit;
        return 1;
    }
    return 0;
}

/* ---- drawing (Gfx thread), over the stock tape screen ---- */
static const uint8_t c_enc[4][4] = { { 0x69, 0x8E, 0xFF, 255 }, { 0x00, 0xED, 0x95, 255 }, { 0xFF, 0xFF, 0xFF, 255 }, { 0xFF, 0x3A, 0x5D, 255 } };
static const uint8_t c_grey[4] = { 0x62, 0x5D, 0x77, 255 };
static void line(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, const uint8_t *c) { sf_line(r, x0, y0, x1, y1, w, c); }

extern "C" void coop_tape_draw(void *r)
{
    if (CUR_MODE != 2 || CUR_SCREEN != TAPE_SCREEN) return;
    int32_t c = TAPE_POS_Q6 >> 6, z = zoom_q10();
    int32_t k = (int32_t)(((int64_t)z << 16) / 44100);                 /* Q10 px per sample, << 16 */
    for (int i = 0; i < 4; i++) {
        if (s_mk[i] < 0) continue;
        int64_t x = CENTRE_X + (((int64_t)(s_mk[i] - c) * k) >> 16);
        if (x < (3 << 10) || x > (317 << 10)) continue;
        int32_t xi = (int32_t)x;
        line(r, xi, 124 << 10, xi, 146 << 10, 0x400, c_grey);          /* through the tracks */
        line(r, xi, 110 << 10, xi, 122 << 10, 0x600, c_enc[i]);        /* the pole */
        line(r, xi, 110 << 10, xi + (7 << 10), 113 << 10, 0x600, c_enc[i]);   /* the pennant: a ">" */
        line(r, xi + (7 << 10), 113 << 10, xi, 116 << 10, 0x600, c_enc[i]);
    }
}
