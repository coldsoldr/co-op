/*
 * coop-fw per-tape channel strip, milestone CS5: the user interface (docs/channel-strip.md "CS5", layout A of the
 * design canvas, docs/ui-features.md "Channel strip").
 *
 * Entry: with the strip mode on (g_coop_strip_on bit 0, Settings › Voices "channel strip") and the mixer's T1 page up
 * (CUR_MODE 3, screen 0x1F), an encoder push (0x3B..0x3E, which the stock mixer ignores) opens the strip of tape 1..4
 * and is swallowed. While open, coop draws over the mixer screen (the stock still runs it underneath, with its
 * keys withheld) and takes:
 *  - the four turns: the page's four controls (docs/channel-strip.md control table), one table step a detent;
 *    shift + turn: COMP white / orange = release / knee, EQ blue = shelf (ccw) / cut (cw) on bands 1 and 4,
 *    elsewhere the same control without the encoder's acceleration (exactly one step an event: the fine move);
 *  - the pushes: another page's encoder opens that page; the shown page's encoder bypasses its section (EQ, COMP,
 *    SAT; on SEND the two sends) and the tab goes hollow (250 ms debounce, as the settings' plugin toggle);
 *    shift + push resets the value(s) under that encoder to coop_strip_params_default;
 *  - T1-T4 (no shift): another tape's strip on the same page. Shift + T1-T4 (mute) and Shift + Help + T (solo, in
 *    coop_tape_ui.cc before this) still reach their handlers;
 *  - the mixer key closes the strip and goes on to the stock; another mode key closes it too.
 * Every change goes through coop_strip_post (coop_strip.cc), whose staged update applies it within a few blocks.
 * The UI keeps its own copy of the four forms (loaded from coop_strip_params_of at each open); the SEND bypass is
 * the UI's: while it is on, the posted form has FX 1 and FX 2 at 0 and the copy keeps the levels (not saved).
 *
 * Also here: the "off in strip mode" note over the mixer's master EQ (T2) and master compressor (T4) pages.
 *
 * Drawing: the stroke font (kennel, uppercase, no TE heap) for the pages as the mockup's monospace text; the
 * name-and-value popup in the stock font top left, in the encoder's colour for ~1.2 s (engine_ui.cc's popup).
 *
 * Threads: keys and turns on the KeyHandler thread, drawing on the Gfx thread (reads the copy; a torn byte shows for
 * one frame at most). Fixed point only, no libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 * The stroke font is hundekraft's (kennel 3.1), GPL-3.0-or-later.
 */
#include <stdint.h>
#include "coop_strip.h"
#pragma GCC optimize("Os")                                              /* UI code: size over speed (the -O2 build inlined ~9 KB) */

typedef struct { int32_t x, y; } op1_pt;                               /* Q10, 320 x 160 units */
#define op1_raster_line(r, p0, p1, w_q10, rgba) \
    (((void (*)(void *, const op1_pt *, const op1_pt *, int32_t, const uint8_t *))0x01A24A4Cu)((r), (p0), (p1), (w_q10), (rgba)))
#include "../../third_party/kennel/synth_sdk/screens/stroke_font.h"
#include "coop_text.h"

#define CUR_MODE     (*(volatile int *)0xFF907D58u)
#define CUR_SCREEN   (*(volatile int *)0x018B65C8u)
#define MIX_SUB      (*(volatile int *)0x01AAD5F0u)                     /* mixer subpage: 0 T1, 1 T2 EQ, 3 T4 comp (hook map) */
#define SHIFT_HELD   (*(volatile uint8_t *)0x01DD7F78u)
#define FX_TYPE_M    (*(volatile int *)0xFF801090u)                     /* master FX 1's type */
#define FX1_ON_M     (*(volatile uint8_t *)0x01AAAC40u)                 /* master FX 1 on (gate 0xffa01202, hook map) */
#define SCREEN_MIXER 0x1F
#define KEY_MIXER    0x05
#define KEY_T1       0x06
#define KEY_ENC0     0x3B
#define POPUP_CYCLES 480000000u                                         /* ~1.2 s at 400 MHz, as engine_ui.cc */
#define DEBOUNCE     100000000u                                         /* 250 ms */

extern "C" {
extern volatile uint32_t g_coop_strip_on;                              /* coop_strip.cc */
extern volatile uint32_t g_strip_split;
void coop_strip_post(int t, const coop_strip_params *p);
const coop_strip_params *coop_strip_params_of(int t);
int32_t coop_strip_meter(int t, int out);
int32_t coop_strip_gr(int t);                                          /* coop_strip.cc, added for CS5 */
uint32_t coop_strip_blocks(void);
void coop_slot2_get(int pos, uint8_t *out10);                         /* plugin_host.cc */
int coop_slot2_bypassed(int pos);
const char *coop_plugin_name(int n);
}

/* ---- state ---- */
enum { PG_EQ, PG_COMP, PG_SAT, PG_SEND };
enum { C_BAND, C_FREQ, C_GAIN, C_Q, C_CUT, C_THR, C_RATIO, C_ATT, C_REL, C_MAKEUP, C_KNEE,
       C_DRIVE, C_TYPE, C_TONE, C_MIX, C_DRY, C_FX1, C_FX2, C_TAP, C_N };

struct sui_stat { uint32_t opens, closes, turns, clicks, pages, bypasses, resets, tapes, posts, frames, notes, bounces,
                  last_screen, last_sub, last_mode, ignored; };
extern "C" {
volatile struct sui_stat g_sui;                                        /* op1emu --dump-at-exit (docs/channel-strip.md CS5) */
coop_strip_params g_sui_p[4];                                          /* the UI's copy of the four forms (dumped by the gates) */
volatile uint8_t g_sui_send_byp[4];
}
static volatile int s_open;
static volatile int s_tape, s_page;
static int s_band[4];
static uint64_t s_swallow;
static uint64_t s_last_click;
static volatile int s_pop_ctl = -1, s_pop_enc;
static volatile uint32_t s_pop_at;
static volatile uint32_t s_ver;                                        /* bumped on every change: the drawings' caches */

static inline uint32_t now(void) { uint32_t v; __asm__ __volatile__("%0 = CYCLES;" : "=d"(v)); return v; }
static inline uint64_t now64(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("%0 = CYCLES; %1 = CYCLES2;" : "=d"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int strip_on(void) { return (g_coop_strip_on & 1u) != 0; }
static int mixer_t1(void) { return CUR_MODE == 3 && CUR_SCREEN == SCREEN_MIXER; }
extern "C" int coop_strip_ui_active(void) { return s_open && strip_on() && mixer_t1(); }

static void pcopy(coop_strip_params *d, const coop_strip_params *s)   /* no memcpy in the firmware: byte by byte */
{
    const volatile uint8_t *a = (const volatile uint8_t *)s;
    uint8_t *b = (uint8_t *)d;
    for (unsigned i = 0; i < sizeof *d; i++) b[i] = a[i];
}
static void post(int t)
{
    coop_strip_params q; pcopy(&q, &g_sui_p[t]);
    if (g_sui_send_byp[t]) { q.fx1 = 0; q.fx2 = 0; }
    coop_strip_post(t, &q);
    g_sui.posts++;
    s_ver++;
}
static void load_all(void)
{
    for (int t = 0; t < 4; t++) {
        uint8_t f1 = g_sui_p[t].fx1, f2 = g_sui_p[t].fx2;
        pcopy(&g_sui_p[t], coop_strip_params_of(t));
        if (g_sui_send_byp[t]) { g_sui_p[t].fx1 = f1; g_sui_p[t].fx2 = f2; }   /* the levels the bypass holds */
    }
    s_ver++;
}

/* ---- the controls ---- */
static int ctl_for(int page, int enc, int shift)
{
    static const uint8_t m[4][4] = { { C_BAND, C_FREQ, C_GAIN, C_Q }, { C_THR, C_RATIO, C_ATT, C_MAKEUP },
                                     { C_DRIVE, C_TYPE, C_TONE, C_MIX }, { C_DRY, C_FX1, C_FX2, C_TAP } };
    if (shift && page == PG_EQ && enc == 0) return C_CUT;
    if (shift && page == PG_COMP && enc == 2) return C_REL;
    if (shift && page == PG_COMP && enc == 3) return C_KNEE;
    return m[page & 3][enc & 3];
}
static int fine_ok(int c) { return c != C_BAND && c != C_CUT && c != C_TYPE && c != C_TAP && c != C_RATIO; }
static int cget(const coop_strip_params *p, int t, int c)
{
    int b = s_band[t];
    switch (c) {
    case C_BAND: return b;
    case C_FREQ: return p->eq_freq[b];
    case C_GAIN: return p->eq_gain[b];
    case C_Q: return p->eq_q[b];
    case C_CUT: return p->eq_cut[b];
    case C_THR: return p->c_thresh;
    case C_RATIO: return p->c_ratio;
    case C_ATT: return p->c_attack;
    case C_REL: return p->c_release;
    case C_MAKEUP: return p->c_makeup;
    case C_KNEE: return p->c_knee;
    case C_DRIVE: return p->s_drive;
    case C_TYPE: return p->s_type;
    case C_TONE: return p->s_tone;
    case C_MIX: return p->s_mix;
    case C_DRY: return p->dry;
    case C_FX1: return p->fx1;
    case C_FX2: return p->fx2;
    default: return p->tap_post;
    }
}
static void cset(coop_strip_params *p, int t, int c, int v)
{
    int b = s_band[t];
    switch (c) {
    case C_BAND: s_band[t] = clampi(v, 0, 3); break;
    case C_FREQ: p->eq_freq[b] = (uint8_t)clampi(v, 0, STRIP_EQ_NFREQ - 1); break;
    case C_GAIN: p->eq_gain[b] = (uint8_t)clampi(v, 0, STRIP_EQ_NGAIN - 1); break;
    case C_Q: p->eq_q[b] = (uint8_t)clampi(v, 0, STRIP_EQ_NQ - 1); break;
    case C_CUT: if (b == 0 || b == 3) p->eq_cut[b] = (uint8_t)clampi(v, 0, 1); break;
    case C_THR: p->c_thresh = (uint8_t)clampi(v, 0, 96); break;
    case C_RATIO: p->c_ratio = (uint8_t)clampi(v, 0, STRIP_COMP_NRATIO - 1); break;
    case C_ATT: p->c_attack = (uint8_t)clampi(v, 0, STRIP_COMP_NTIME - 1); break;
    case C_REL: p->c_release = (uint8_t)clampi(v, 0, STRIP_COMP_NTIME - 1); break;
    case C_MAKEUP: p->c_makeup = (uint8_t)clampi(v, 0, 48); break;
    case C_KNEE: p->c_knee = (uint8_t)clampi(v, 0, 24); break;
    case C_DRIVE: p->s_drive = (uint8_t)clampi(v, 0, 100); break;
    case C_TYPE: p->s_type = (uint8_t)clampi(v, 0, STRIP_SAT_NTYPE - 1); break;
    case C_TONE: p->s_tone = (int8_t)clampi(v, -50, 50); break;
    case C_MIX: p->s_mix = (uint8_t)clampi(v, 0, 100); break;
    case C_DRY: p->dry = (uint8_t)clampi(v, 0, 100); break;
    case C_FX1: p->fx1 = (uint8_t)clampi(v, 0, 100); break;
    case C_FX2: p->fx2 = (uint8_t)clampi(v, 0, 100); break;
    case C_TAP: p->tap_post = (uint8_t)clampi(v, 0, 1); break;
    }
}
static void reset_enc(int t, int page, int enc)                        /* shift + push: the default(s) under that encoder */
{
    coop_strip_params d; coop_strip_params_default(&d);
    coop_strip_params *p = &g_sui_p[t];
    int b = s_band[t];
    if (page == PG_EQ && enc == 0) {                                   /* blue: the whole band */
        p->eq_cut[b] = d.eq_cut[b]; p->eq_freq[b] = d.eq_freq[b]; p->eq_gain[b] = d.eq_gain[b]; p->eq_q[b] = d.eq_q[b];
        return;
    }
    if (page == PG_EQ) { int c = ctl_for(page, enc, 0); s_band[t] = b; cset(p, t, c, cget(&d, t, c)); return; }
    cset(p, t, ctl_for(page, enc, 0), cget(&d, t, ctl_for(page, enc, 0)));
    if (page == PG_COMP && enc >= 2) cset(p, t, ctl_for(page, enc, 1), cget(&d, t, ctl_for(page, enc, 1)));   /* release / knee too */
}
static void popup(int c, int enc) { s_pop_ctl = c; s_pop_enc = enc; s_pop_at = now() | 1u; }

/* ---- keys (KeyHandler thread): 1 = consumed ---- */
static void close_strip(void) { if (s_open) { s_open = 0; g_sui.closes++; s_pop_at = 0; } }
extern "C" int coop_strip_ui_key(int code, int pressed)
{
    uint64_t bit = (uint64_t)1 << (code & 63);
    if (!pressed) { if (s_swallow & bit) { s_swallow &= ~bit; return 1; } return 0; }
    if (s_open && !coop_strip_ui_active()) close_strip();             /* left by a way the keys did not see */
    if (!s_open) {
        if (!strip_on() || code < KEY_ENC0 || code > KEY_ENC0 + 3) return 0;
        g_sui.last_screen = (uint32_t)CUR_SCREEN; g_sui.last_sub = (uint32_t)MIX_SUB; g_sui.last_mode = (uint32_t)CUR_MODE;
        if (!mixer_t1()) return 0;
        s_tape = code - KEY_ENC0;
        load_all();
        s_open = 1; g_sui.opens++;
        s_swallow |= bit;
        return 1;
    }
    if (code == KEY_MIXER || (code >= 0x02 && code <= 0x04)) { close_strip(); return 0; }   /* closes, then the stock acts */
    if (code >= KEY_T1 && code <= KEY_T1 + 3) {
        if (SHIFT_HELD) return 0;                                      /* shift + T: the stock mute */
        s_tape = code - KEY_T1; s_pop_at = 0; s_ver++;
        g_sui.tapes++;
        s_swallow |= bit;
        return 1;
    }
    if (code >= KEY_ENC0 && code <= KEY_ENC0 + 3) {
        int e = code - KEY_ENC0, t = s_tape;
        s_swallow |= bit;
        g_sui.clicks++;
        if (SHIFT_HELD) {                                              /* reset */
            reset_enc(t, s_page, e);
            post(t); popup(ctl_for(s_page, e, 0), e);
            g_sui.resets++;
            return 1;
        }
        if (e != s_page) { s_page = e; s_pop_at = 0; g_sui.pages++; s_ver++; return 1; }
        uint64_t c = now64();
        if (s_last_click && c - s_last_click < DEBOUNCE) { g_sui.bounces++; return 1; }   /* the switch bouncing */
        s_last_click = c | 1u;
        coop_strip_params *p = &g_sui_p[t];
        if (e == PG_EQ) p->eq_on ^= 1;
        else if (e == PG_COMP) p->comp_on ^= 1;
        else if (e == PG_SAT) p->sat_on ^= 1;
        else g_sui_send_byp[t] ^= 1;
        post(t);
        g_sui.bypasses++;
        return 1;
    }
    return 0;                                                          /* the keyboard, transport, shift play through */
}

/* ---- turns (KeyHandler thread): 1 = taken ---- */
extern "C" int coop_strip_ui_knob(int knob, int steps)
{
    if (!s_open) return 0;
    if (!coop_strip_ui_active()) { close_strip(); return 0; }
    int e = knob & 3, t = s_tape, sh = SHIFT_HELD;
    int c = ctl_for(s_page, e, sh);
    if (!steps) return 1;
    coop_strip_params *p = &g_sui_p[t];
    if (sh && fine_ok(c)) steps = steps > 0 ? 1 : -1;                  /* shift: one step an event, no acceleration */
    if (c == C_CUT) {
        int b = s_band[t];
        if (b != 0 && b != 3) { g_sui.ignored++; popup(C_BAND, 0); return 1; }
        cset(p, t, C_CUT, steps > 0 ? 1 : 0);
    } else if (c == C_BAND) {
        cset(p, t, C_BAND, s_band[t] + steps);
        s_ver++; popup(c, e); g_sui.turns++;
        return 1;                                                      /* nothing to post: the UI's selection */
    } else {
        cset(p, t, c, cget(p, t, c) + steps);
    }
    post(t);
    popup(c, e);
    g_sui.turns++;
    return 1;
}

/* ---- text of a control ---- */
struct sb { char s[24]; int n; };
static void sb_c(struct sb *b, char c) { if (b->n < 23) b->s[b->n++] = c; b->s[b->n] = 0; }
static void sb_s(struct sb *b, const char *s) { while (*s) sb_c(b, *s++); }
static void sb_u(struct sb *b, int v) { char d[8]; int k = 0; if (v < 0) v = 0; do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 8); while (k) sb_c(b, d[--k]); }
static void sb_i(struct sb *b, int v) { if (v < 0) { sb_c(b, '-'); v = -v; } sb_u(b, v); }
static void sb_dec(struct sb *b, int v, int div, int places)          /* v / div with `places` decimals (v >= 0) */
{
    sb_u(b, v / div);
    if (!places) return;
    sb_c(b, '.');
    int f = v % div;
    for (int k = 0; k < places; k++) { f *= 10; sb_c(b, (char)('0' + f / div)); f %= div; }
}
static void sb_halfdb(struct sb *b, int h, int sign)                  /* half dB */
{
    if (h < 0) { sb_c(b, '-'); h = -h; } else if (sign && h > 0) sb_c(b, '+');
    sb_u(b, h / 2);
    if (h & 1) sb_s(b, ".5");
    sb_s(b, "db");
}
static const char *const sat_names[4] = { "tape", "tube", "diode", "fold" };
static const char *ctl_name(int c)
{
    static const char *const n[C_N] = { "band", "freq", "gain", "q", "band", "thresh", "ratio", "attack", "release", "makeup",
                                        "knee", "drive", "type", "tone", "mix", "dry", "fx 1", "fx 2", "tap" };
    return n[c];
}
static void ctl_value(struct sb *b, const coop_strip_params *p, int t, int c)
{
    int v = cget(p, t, c), bd = s_band[t];
    switch (c) {
    case C_BAND: case C_CUT:
        sb_u(b, bd + 1); sb_c(b, ' ');
        sb_s(b, (bd == 1 || bd == 2) ? "peak" : p->eq_cut[bd] ? "cut" : "shelf");
        break;
    case C_FREQ: { int hz = strip_eq_hz[clampi(v, 0, STRIP_EQ_NFREQ - 1)];
        if (hz < 1000) sb_u(b, hz); else if (hz < 10000) { sb_dec(b, hz, 1000, 2); sb_c(b, 'k'); } else { sb_dec(b, hz, 1000, 1); sb_c(b, 'k'); }
        break; }
    case C_GAIN: if ((bd == 0 || bd == 3) && p->eq_cut[bd]) sb_s(b, "--"); else sb_halfdb(b, v - STRIP_EQ_GAIN0, 1); break;
    case C_Q: { int q = strip_eq_q100[clampi(v, 0, STRIP_EQ_NQ - 1)]; if (q < 100) sb_dec(b, q, 100, 2); else sb_dec(b, q, 100, 1); break; }
    case C_THR: sb_halfdb(b, -v, 0); break;
    case C_RATIO: { int r = strip_ratio_x10[clampi(v, 0, STRIP_COMP_NRATIO - 1)];
        if (!r) sb_s(b, "limit"); else { if (r % 10) sb_dec(b, r, 10, 1); else sb_u(b, r / 10); sb_s(b, ":1"); }
        break; }
    case C_ATT: { int a = strip_att_ms10[clampi(v, 0, STRIP_COMP_NTIME - 1)];
        if (a < 100) sb_dec(b, a, 10, 1); else sb_u(b, a / 10);
        sb_s(b, "ms"); break; }
    case C_REL: { int r = strip_rel_ms[clampi(v, 0, STRIP_COMP_NTIME - 1)];
        if (r < 1000) { sb_u(b, r); sb_s(b, "ms"); } else { sb_dec(b, r, 1000, 2); sb_c(b, 's'); }
        break; }
    case C_MAKEUP: sb_halfdb(b, v, 1); break;
    case C_KNEE: sb_halfdb(b, v, 0); break;
    case C_TYPE: sb_s(b, sat_names[v & 3]); break;
    case C_TONE: if (v > 0) sb_c(b, '+'); sb_i(b, v); break;
    case C_TAP: sb_s(b, v ? "post" : "pre"); break;
    default: sb_u(b, v); sb_c(b, '%'); break;                          /* drive, mix, dry, fx 1, fx 2 */
    }
}

/* ---- drawing primitives (Gfx thread) ---- */
static const uint8_t c_bg[4] = { 0, 0, 0, 255 }, c_dim[4] = { 0x62, 0x5D, 0x77, 255 }, c_label[4] = { 0x88, 0x88, 0x90, 255 },
                     c_grid[4] = { 0x26, 0x26, 0x2E, 255 }, c_well[4] = { 0x18, 0x18, 0x1C, 255 }, c_lit[4] = { 0xED, 0xED, 0xED, 255 },
                     c_grfill[4] = { 0x00, 0x5A, 0x38, 255 }, c_off[4] = { 0x40, 0x3C, 0x50, 255 };
static const uint8_t c_enc[4][4] = { { 0x69, 0x8E, 0xFF, 255 }, { 0x00, 0xED, 0x95, 255 }, { 0xFF, 0xFF, 0xFF, 255 }, { 0xFF, 0x3A, 0x5D, 255 } };
#define Q(v) ((int32_t)(v) << 10)
static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t wq, const uint8_t *c) { sf_line(r, x0, y0, x1, y1, wq, c); }
static void fill(void *r, int x0, int y0, int x1, int y1, const uint8_t *c)   /* a box as a thick line */
{
    int32_t ym = (int32_t)(y0 + y1) << 9, w = (int32_t)(y1 - y0) << 10;
    sf_line(r, Q(x0), ym, Q(x1), ym, w, c);
}
static void box(void *r, int x0, int y0, int x1, int y1, const uint8_t *c)
{
    ln(r, Q(x0), Q(y0), Q(x1), Q(y0), 1024, c); ln(r, Q(x1), Q(y0), Q(x1), Q(y1), 1024, c);
    ln(r, Q(x1), Q(y1), Q(x0), Q(y1), 1024, c); ln(r, Q(x0), Q(y1), Q(x0), Q(y0), 1024, c);
}
static void dashed_h(void *r, int x0, int x1, int y, const uint8_t *c) { for (int x = x0; x < x1; x += 4) ln(r, Q(x), Q(y), Q(x + 2), Q(y), 1024, c); }
static void dashed_v(void *r, int x, int y0, int y1, const uint8_t *c) { for (int y = y0; y < y1; y += 4) ln(r, Q(x), Q(y), Q(x), Q(y + 2), 1024, c); }
static void dot(void *r, int x, int y, int d, const uint8_t *c) { ln(r, Q(x) - (d << 9), Q(y), Q(x) + (d << 9), Q(y), d << 10, c); }
static void ring(void *r, int x, int y, int rad, const uint8_t *c)    /* an octagon */
{
    static const int8_t ox[9] = { 10, 7, 0, -7, -10, -7, 0, 7, 10 }, oy[9] = { 0, 7, 10, 7, 0, -7, -10, -7, 0 };
    for (int i = 0; i < 8; i++)
        ln(r, Q(x) + ox[i] * rad * 102, Q(y) + oy[i] * rad * 102, Q(x) + ox[i + 1] * rad * 102, Q(y) + oy[i + 1] * rad * 102, 1024, c);
}
static void upper(char *d, const char *s) { int i = 0; for (; s[i] && i < 31; i++) d[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i]; d[i] = 0; }
static int32_t text_w(const char *s, int32_t sq) { int32_t w = 0; for (; *s; s++) w += (*s == '1' || *s == '.' ? 4 : 6) * sq; return w > 2 * sq ? w - 2 * sq : w; }
/* stroke text, x / y the top left in px, sq the scale (Q10 px a grid unit); align 0 left, 1 centred, 2 right */
static void txt(void *r, int x, int y, int32_t sq, const char *s, const uint8_t *c, int align)
{
    char u[32]; upper(u, s);
    int32_t x0 = Q(x);
    if (align) { int32_t w = text_w(u, sq); x0 -= align == 1 ? w / 2 : w; }
    sf_text(r, x0, Q(y), sq, u, sq + (sq >> 2), c);
}

/* ---- meters ---- */
static int32_t s_hold[2], s_gr_hist[134], s_gr_at;
static uint32_t s_blocks_seen;
static int log2_q8(int32_t v)                                          /* log2(v) Q8 for v >= 1 */
{
    if (v <= 0) return 0;
    int m = 31 - __builtin_clz((uint32_t)v);
    int fr = m >= 8 ? (int)((uint32_t)v >> (m - 8)) & 255 : (int)((uint32_t)v << (8 - m)) & 255;
    return (m << 8) + fr;
}
static int meter_h(int32_t peak, int span)                             /* -48 .. 0 dBFS onto 0 .. span px */
{
    int l = log2_q8(peak) - 7 * 256;                                   /* 2^15 = 0 dB, 8 octaves = 48 dB */
    return clampi(l * span / (8 * 256), 0, span);
}
static void meters(void *r, int t, int32_t *in_peak)
{
    uint32_t bl = coop_strip_blocks();
    int live = ((g_strip_split >> t) & 1) && bl != s_blocks_seen;
    s_blocks_seen = bl;
    for (int k = 0; k < 2; k++) {
        int32_t p = live ? coop_strip_meter(t, k) : 0;
        int32_t h = s_hold[k] - s_hold[k] / 12 - 16;                   /* a falling hold */
        s_hold[k] = p > h ? p : (h > 0 ? h : 0);
        int x = k ? 290 : 268;
        fill(r, x, 36, x + 8, 120, c_well);
        int hh = meter_h(s_hold[k], 84);
        if (hh) fill(r, x, 120 - hh, x + 8, 120, c_lit);
    }
    txt(r, 272, 26, 1024, "in", c_label, 1);
    txt(r, 294, 26, 1024, "out", c_label, 1);
    *in_peak = s_hold[0];
}

/* ---- the common parts ---- */
static int section_on(const coop_strip_params *p, int t, int pg)
{
    if (pg == PG_EQ) return p->eq_on;
    if (pg == PG_COMP) return p->comp_on;
    if (pg == PG_SAT) return p->sat_on;
    return !g_sui_send_byp[t];
}
static void tabs(void *r, const coop_strip_params *p, int t, int page)
{
    static const char *const names[4] = { "eq", "comp", "sat", "send" };
    for (int i = 0; i < 4; i++) {
        int x = 6 + 48 * i, on = section_on(p, t, i);
        if (i == page && on) { for (int y = 5; y < 19; y += 2) fill(r, x, y, x + 44, y + 3, c_enc[i]); txt(r, x + 22, 8, 1300, names[i], c_bg, 1); }
        else { const uint8_t *c = on || i == page ? c_enc[i] : c_dim; box(r, x, 5, x + 44, 19, c); txt(r, x + 22, 8, 1300, names[i], c, 1); }
        if (i == page) fill(r, x, 21, x + 44, 23, c_enc[i]);
    }
    char s[8] = { 't', 'a', 'p', 'e', ' ', (char)('1' + t), 0 };
    txt(r, 314, 8, 1200, s, c_label, 2);
}
static void readouts(void *r, const coop_strip_params *p, int t, int page)
{
    int on = section_on(p, t, page), sh = SHIFT_HELD;
    for (int e = 0; e < 4; e++) {
        int c = ctl_for(page, e, sh && page == PG_COMP);
        struct sb v; v.n = 0; v.s[0] = 0;
        ctl_value(&v, p, t, c);
        int x = 8 + 78 * e;
        const uint8_t *col = on || page == PG_SEND ? c_enc[e] : c_dim;
        if (page == PG_SEND && !on && (e == 1 || e == 2)) col = c_dim;
        txt(r, x, 133, 1024, ctl_name(c), col, 0);
        txt(r, x, 143, 1600, v.s, col, 0);
    }
}

/* ---- EQ ---- */
static int8_t s_eq_y[61];                                             /* the curve, half dB, every 4 px */
static uint32_t s_eq_ver = 0xffffffffu;
static int s_eq_t = -1;
static int band_db(const coop_strip_params *p, int b, int d24)        /* one band's half dB at d24 (1/24 octave) from its freq */
{
    int gh = p->eq_gain[b] - STRIP_EQ_GAIN0;
    if ((b == 0 || b == 3) && p->eq_cut[b]) {
        int d = b == 0 ? d24 : -d24;
        if (d >= 12) return 0;
        if (d >= 0) return -(12 - d) / 2;
        return d - 6;
    }
    if (!gh) return 0;
    if (b == 1 || b == 2) {
        int64_t n = (int64_t)d24 * strip_eq_q100[p->eq_q[b]];
        return (int)((int64_t)gh * 1440000 / (1440000 + n * n));
    }
    int tt = clampi(d24 + 24, 0, 48);
    int s = (int)((int64_t)gh * tt * tt * (144 - 2 * tt) / 110592);  /* gh * smoothstep */
    return b == 0 ? gh - s : s;
}
static void eq_curve(const coop_strip_params *p)
{
    for (int i = 0; i <= 60; i++) {
        int sum = 0;
        for (int b = 0; b < 4; b++) sum += band_db(p, b, i * 4 - 2 * p->eq_freq[b]);
        s_eq_y[i] = (int8_t)clampi(sum, -36, 36);
    }
}
static int eq_y(int h) { return 73 - h * 5 / 4; }
static void page_eq(void *r, const coop_strip_params *p, int t)
{
    if (s_eq_ver != s_ver || s_eq_t != t) { eq_curve(p); s_eq_ver = s_ver; s_eq_t = t; }
    static const uint8_t gx[3] = { 66, 146, 226 };
    static const char *const gl[3] = { "100", "1k", "10k" };
    for (int i = 0; i < 3; i++) { ln(r, Q(gx[i]), Q(26), Q(gx[i]), Q(120), 1024, c_grid); txt(r, gx[i], 123, 900, gl[i], c_label, 1); }
    dashed_h(r, 10, 250, 73, c_grid);
    const uint8_t *cc = p->eq_on ? c_enc[0] : c_dim;
    for (int i = 0; i < 60; i++) ln(r, Q(10 + 4 * i), Q(eq_y(s_eq_y[i])), Q(14 + 4 * i), Q(eq_y(s_eq_y[i + 1])), 1600, cc);
    int sel = s_band[t];
    for (int b = 0; b < 4; b++) {
        int x = 10 + 2 * p->eq_freq[b];
        int h = ((b == 0 || b == 3) && p->eq_cut[b]) ? -6 : p->eq_gain[b] - STRIP_EQ_GAIN0;
        int y = eq_y(h);
        char n[2] = { (char)('1' + b), 0 };
        if (b == sel) { ring(r, x, y, 6, c_lit); dot(r, x, y, 4, c_lit); }
        else dot(r, x, y, 4, cc);
        txt(r, x - 2, y < 50 ? y + 9 : y - 15, 900, n, c_label, 0);
    }
}

/* ---- COMP ---- */
static void page_comp(void *r, const coop_strip_params *p, int t, int32_t in_peak)
{
    box(r, 10, 26, 104, 120, c_grid);
    ln(r, Q(10), Q(120), Q(104), Q(26), 1024, c_grid);
    int thr = -(int)p->c_thresh;                                       /* half dB, <= 0 */
    int tx = 104 + thr * 94 / 96;
    dashed_v(r, tx, 26, 120, p->comp_on ? c_enc[0] : c_dim);
    int rx10 = strip_ratio_x10[p->c_ratio], knee = p->c_knee, mk = p->c_makeup;
    int px = 0, py = 0;
    for (int i = 0; i <= 48; i++) {                                    /* input -48 .. 0 dB, 1 dB a point; half dB inside */
        int in = (i - 48) * 2, out;
        int over = in - thr;
        if (2 * over <= -knee) out = in;
        else if (2 * over >= knee || !knee) out = rx10 ? thr + over * 10 / rx10 : thr;
        else {                                                         /* the soft knee: quadratic */
            int a = over + knee / 2;
            int red = rx10 ? (a * a * (rx10 - 10)) / (2 * knee * rx10) : (a * a) / (2 * knee);
            out = in - red;
        }
        out += mk;
        int x = 10 + i * 94 / 48, y = clampi(26 + (-out) * 94 / 96, 26, 120);
        if (i) ln(r, Q(px), Q(py), Q(x), Q(y), 1600, p->comp_on ? c_enc[1] : c_dim);
        px = x; py = y;
    }
    int32_t gr = coop_strip_gr(t);
    int grx10 = (int)(((int64_t)gr * 60206 >> 16) / 1000);            /* dB x 10 */
    if (p->comp_on && in_peak > 0) {                                   /* the input level on the curve */
        int ih = meter_h(in_peak, 96);                                 /* half dB above -48 */
        int x = 10 + ih * 94 / 96;
        int y = clampi(26 + (96 - ih + grx10 / 5 - mk) * 94 / 96, 26, 120);
        dot(r, x, y, 5, c_lit);
    }
    s_gr_hist[s_gr_at] = p->comp_on ? grx10 : 0;
    s_gr_at = (s_gr_at + 1) % 134;
    txt(r, 116, 27, 1024, "gain reduction", c_label, 0);
    ln(r, Q(116), Q(38), Q(250), Q(38), 1024, c_grid);
    for (int i = 0; i < 134; i++) {
        int v = s_gr_hist[(s_gr_at + i) % 134];
        if (v > 0) ln(r, Q(116 + i), Q(38), Q(116 + i), Q(38 + clampi(v * 2 / 10, 1, 30)), 1024, c_grfill);
    }
    struct sb b; b.n = 0; b.s[0] = 0;
    sb_c(&b, '-'); sb_dec(&b, p->comp_on ? grx10 : 0, 10, 1); sb_s(&b, " db");
    txt(r, 116, 74, 2300, b.s, p->comp_on ? c_enc[1] : c_dim, 0);
    struct sb k; k.n = 0; k.s[0] = 0;
    sb_s(&k, "knee "); sb_halfdb(&k, knee, 0); sb_s(&k, "  rel "); ctl_value(&k, p, t, C_REL);
    txt(r, 116, 96, 900, k.s, c_label, 0);
    txt(r, 116, 108, 900, "shift: release, knee", c_dim, 0);
}

/* ---- SAT ---- */
static const int16_t sin32[32] = { 0, 3196, 6270, 9102, 11585, 13623, 15137, 16069, 16384, 16069, 15137, 13623, 11585, 9102, 6270, 3196,
                                   0, -3196, -6270, -9102, -11585, -13623, -15137, -16069, -16384, -16069, -15137, -13623, -11585, -9102, -6270, -3196 };
static int32_t shape(int type, int32_t x)                              /* Q12 in and out: a display model of the four curves */
{
    const int32_t one = 4096;
    switch (type) {
    case STRIP_SAT_FOLD: {
        int32_t u = x + one;                                           /* a triangle fold into -1 .. 1 */
        int32_t m = u % (4 * one); if (m < 0) m += 4 * one;
        return m < 2 * one ? m - one : 3 * one - m;
    }
    case STRIP_SAT_DIODE: {
        int32_t a = x < 0 ? -x : x, k = one * 7 / 10;
        if (a > k) { int32_t e = a - k; a = k + (int32_t)((int64_t)e * (one - k) / (e + (one - k))); }
        return x < 0 ? -a : a;
    }
    default: {
        int32_t b = type == STRIP_SAT_TUBE ? one / 5 : 0;              /* tube: biased, its offset removed */
        int32_t xs[2] = { x + b, b }, ys[2];
        for (int k = 0; k < 2; k++) {
            int32_t v = clampi(xs[k], -3 * one, 3 * one);
            int64_t v2 = (int64_t)v * v / one;                         /* tanh ~ x (27 + x^2) / (27 + 9 x^2) */
            ys[k] = (int32_t)((int64_t)v * (27 * one + v2) / (27 * one + 9 * v2));
        }
        return ys[0] - ys[1];
    }
    }
}
static int32_t drive_q12(int drive)                                    /* 0 .. +24 dB: 2^(drive * 4 / 100), Q12 */
{
    int e = drive * 4;                                                 /* in 1/100 octave */
    int32_t g = 4096 << (e / 100);
    return g + (int32_t)((int64_t)g * (e % 100) * 69 / 10000);         /* linear between octaves, ~ 2^f */
}
static int32_t sat_out(const coop_strip_params *p, int32_t x)
{
    int32_t y = shape(p->s_type, (int32_t)((int64_t)x * drive_q12(p->s_drive) >> 12));
    return x + (y - x) * p->s_mix / 100;
}
static uint8_t s_harm[6];
static uint32_t s_sat_ver = 0xffffffffu;
static int s_sat_t = -1;
static void sat_harmonics(const coop_strip_params *p)                  /* a 32-point DFT of a full-scale sine through the curve */
{
    int32_t y[32], mag[7];
    for (int i = 0; i < 32; i++) y[i] = sat_out(p, sin32[i] >> 2);   /* Q14 -> Q12 */
    for (int h = 1; h <= 6; h++) {
        int64_t s = 0, c = 0;
        for (int i = 0; i < 32; i++) { s += (int64_t)y[i] * sin32[(i * h) & 31]; c += (int64_t)y[i] * sin32[(i * h + 8) & 31]; }
        s >>= 14; c >>= 14;
        int64_t m2 = s * s + c * c;
        int32_t m = 0; while ((int64_t)(m + 1) * (m + 1) <= m2 && m < 2000000) m += m < 64 ? 1 : (int32_t)(m / 8);   /* rough sqrt */
        mag[h] = m;
    }
    for (int h = 1; h <= 6; h++) {
        int v = mag[1] ? (int)((int64_t)mag[h] * 76 / mag[1]) : 0;
        if (h > 1) { int q = 0; while ((q + 1) * (q + 1) <= v * 76) q++; v = q; }   /* square-root scale for the overtones */
        s_harm[h - 1] = (uint8_t)clampi(v, 0, 76);
    }
}
static void page_sat(void *r, const coop_strip_params *p, int t)
{
    if (s_sat_ver != s_ver || s_sat_t != t) { sat_harmonics(p); s_sat_ver = s_ver; s_sat_t = t; }
    box(r, 10, 26, 104, 120, c_grid);
    ln(r, Q(10), Q(120), Q(104), Q(26), 1024, c_grid);
    const uint8_t *cc = p->sat_on ? c_lit : c_dim;
    int px = 0, py = 0;
    for (int i = 0; i <= 32; i++) {
        int32_t x = (i - 16) * 256;                                    /* -1 .. 1, Q12 */
        int32_t y = sat_out(p, x);
        int sx = 10 + i * 94 / 32, sy = clampi(73 - (int)(y * 47 / 4096), 26, 120);
        if (i) ln(r, Q(px), Q(py), Q(sx), Q(sy), 1600, cc);
        px = sx; py = sy;
    }
    for (int k = 0; k < 4; k++) {
        int y = 32 + 18 * k;
        if (k == p->s_type) { box(r, 118, y - 4, 180, y + 10, p->sat_on ? c_enc[1] : c_dim); txt(r, 124, y, 1300, sat_names[k], p->sat_on ? c_enc[1] : c_dim, 0); }
        else txt(r, 124, y, 1300, sat_names[k], c_off, 0);
    }
    txt(r, 196, 27, 1024, "harmonics", c_label, 0);
    for (int h = 0; h < 6; h++) { int v = s_harm[h]; if (v) fill(r, 196 + 10 * h, 120 - v, 202 + 10 * h, 120, cc); }
}

/* ---- SEND ---- */
static const char *const fx_names[8] = { "delay", "grid", "punch", "phone", "spring", "filter", "nitro", "cwo" };
static void fx_label(char *out, int k)                                 /* "fx 1 delay", "fx 2 off" */
{
    struct sb b; b.n = 0; b.s[0] = 0;
    sb_s(&b, k ? "fx 2 " : "fx 1 ");
    if (!k) { int ty = FX_TYPE_M; if (!FX1_ON_M) sb_s(&b, "off"); else sb_s(&b, ty >= 0 && ty < 8 ? fx_names[ty] : "?"); }
    else {
        uint8_t s[10]; coop_slot2_get(1, s);
        const char *n = s[0] == 1 && s[1] < 8 ? fx_names[s[1]] : s[0] == 2 ? coop_plugin_name(s[1]) : 0;
        if (!n || coop_slot2_bypassed(1)) sb_s(&b, "off"); else for (int i = 0; n[i] && i < 12; i++) sb_c(&b, n[i] == '_' ? ' ' : n[i]);
    }
    for (int i = 0; i <= b.n; i++) out[i] = b.s[i];
}
static void page_send(void *r, const coop_strip_params *p, int t)
{
    int byp = g_sui_send_byp[t];
    box(r, 10, 60, 50, 86, c_label);
    char tn[3] = { 't', (char)('1' + t), 0 };
    txt(r, 30, 69, 1400, tn, c_lit, 1);
    txt(r, 30, 92, 1000, p->tap_post ? "post" : "pre", c_enc[3], 1);
    const uint8_t *cd = p->dry ? c_enc[0] : c_dim, *c1 = p->fx1 && !byp ? c_enc[1] : c_dim, *c2 = p->fx2 && !byp ? c_enc[2] : c_dim;
    ln(r, Q(50), Q(73), Q(70), Q(73), 1024, c_label);
    ln(r, Q(70), Q(73), Q(70), Q(45), 1024, cd); ln(r, Q(70), Q(45), Q(90), Q(45), 1024, cd);
    ln(r, Q(70), Q(73), Q(90), Q(77), 1024, c1);
    ln(r, Q(70), Q(73), Q(70), Q(109), 1024, c2); ln(r, Q(70), Q(109), Q(90), Q(109), 1024, c2);
    char l[24];
    const int ys[3] = { 33, 65, 97 }, lv[3] = { p->dry, p->fx1, p->fx2 };
    for (int k = 0; k < 3; k++) {
        const uint8_t *c = k == 0 ? cd : k == 1 ? c1 : c2;
        if (k == 0) txt(r, 94, ys[k], 1000, "dry to out", c, 0);
        else { fx_label(l, k - 1); txt(r, 94, ys[k], 1000, l, c, 0); }
        fill(r, 94, ys[k] + 9, 244, ys[k] + 16, c_well);
        int w = lv[k] * 150 / 100;
        if (w) fill(r, 94, ys[k] + 9, 94 + w, ys[k] + 16, k && byp ? c_dim : c_enc[k]);
    }
    if (byp) txt(r, 94, 122, 900, "sends bypassed", c_dim, 0);
}

/* ---- the popup (stock font) ---- */
static struct coop_text s_ptx[4], s_note, s_note2;
static void popup_draw(void *r, const coop_strip_params *p, int t)
{
    uint32_t at = s_pop_at;
    if (!at) return;
    if (now() - at > POPUP_CYCLES) { if (s_pop_at == at) s_pop_at = 0; return; }
    int c = s_pop_ctl, e = s_pop_enc & 3;
    if (c < 0 || c >= C_N) return;
    struct sb b; b.n = 0; b.s[0] = 0;
    sb_s(&b, ctl_name(c)); sb_s(&b, "  ");
    ctl_value(&b, p, t, c);
    op1_pt a = { 2 << 10, 12 << 10 }, z = { 250 << 10, 12 << 10 };       /* over the four tabs */
    op1_raster_line(r, &a, &z, 20 << 10, c_bg);
    coop_text_draw(&s_ptx[e], r, 8, 6, 12, c_enc[e], b.s, 0);
}

extern "C" void coop_strip_ui_draw(void *r)
{
    if (!coop_strip_ui_active()) return;
    g_sui.frames++;
    for (int32_t y = -4; y < 164; y += 4) fill(r, -4, y, 324, y + 8, c_bg);   /* thin overlapping strokes (op1emu) */
    int t = s_tape & 3, page = s_page & 3;
    const coop_strip_params *p = &g_sui_p[t];
    int32_t in_peak;
    meters(r, t, &in_peak);
    if (page == PG_EQ) page_eq(r, p, t);
    else if (page == PG_COMP) page_comp(r, p, t, in_peak);
    else if (page == PG_SAT) page_sat(r, p, t);
    else page_send(r, p, t);
    readouts(r, p, t, page);
    tabs(r, p, t, page);
    popup_draw(r, p, t);
}

/* the mixer's master EQ (T2) and master compressor (T4) pages while the strip mode is on: a note, the page kept */
extern "C" void coop_strip_ui_note(void *r)
{
    if (!strip_on() || CUR_MODE != 3) return;
    int s = CUR_SCREEN, sub = MIX_SUB;
    if (s == SCREEN_MIXER || (s >= 8 && s <= 15) || (sub != 1 && sub != 3)) return;
    g_sui.notes++;
    int y = sub == 1 ? 9 : 152;                                         /* EQ: top left is free; comp: the bottom row is */
    op1_pt a = { 2 << 10, y << 10 }, z = { 150 << 10, y << 10 };
    op1_raster_line(r, &a, &z, 16 << 10, c_bg);
    coop_text_draw(sub == 1 ? &s_note : &s_note2, r, 6, y - 6, 12, c_label, "off in strip mode", 0);
}
