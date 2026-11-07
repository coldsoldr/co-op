/*
 * coop-fw t1: the tracker's screens and controls (docs/tracker.md). The data and the sequencer are in coop_tracker.cc.
 *
 * The tracker replaces the tape view (the owner's decision 4): while it is on, every frame of tape mode is painted
 * black and one of four views is drawn over it (T1 pattern overview, T2 one channel zoomed, T3 the song's order list,
 * T4 setup), and the tape's keys and encoders are taken. Switching: Shift + tape on the tape page, or the settings
 * menu's UI page ("tape view"). Entry via settings keeps the tape reachable even if Shift + tape ever clashes.
 *
 * Controls (first pass, the owner's draft in docs/tracker-handover.md section 5 with guesses filled in):
 *   blue    the note at the cursor (Shift: an octave); push: note off marker / clear
 *   green   the cell's instrument: synth or drum family (the stock loads one synth and one kit: see coop_tracker.cc)
 *   white   the cell's volume 0..99 (Shift: tens); push: back to the channel default
 *   orange  the effect word (Shift: its value); push: clear it
 *   left / right    a row up / down (Shift: a channel left / right)   keyboard  writes the note, moves down a row
 *   play  play from the cursor / stop (Shift: from the song start)    stop  stop; stopped: the cursor to row 0
 *   rec   edit on / off (off: the keyboard only previews)             lift / drop  copy / paste a cell   scissors clear
 *   song view: blue the position, green its pattern, white the song length, orange that pattern's length
 *   setup view: blue bpm, green speed (ticks a row), white the keyboard octave, orange the orientation
 *
 * Orientation: landscape, or portrait with the view's top at the screen's right edge (a: the device turned
 * anticlockwise, the keyboard down the left) or its left edge (b: clockwise). All drawing goes
 * through one transform: lines by their endpoints; landscape text is the stock font (Text objects bake place, size
 * and colour, so a pool holds one per (view, row, field, colour)); portrait text is kennel's stroke font with every
 * segment transformed, because the stock Text object takes only the translation of its matrix (op1emu, t1).
 * Gfx thread draws, the KeyHandler thread edits; fixed point only, no libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 * The stroke font (fills and dots) is hundekraft's (kennel 3.1), GPL-3.0-or-later.
 */
#include <stdint.h>
#include "coop_tracker.h"

#define CUR_MODE     (*(volatile int *)0xFF907D58u)
#define SHIFT_HELD   (*(volatile uint8_t *)0x01DD7F78u)
typedef struct { int32_t x, y; } op1_pt;
#define op1_raster_line(r, p0, p1, w_q10, rgba) \
    (((void (*)(void *, const op1_pt *, const op1_pt *, int32_t, const uint8_t *))0x01A24A4Cu)((r), (p0), (p1), (w_q10), (rgba)))
#include "../../third_party/kennel/synth_sdk/screens/stroke_font.h"
#include "coop_text.h"

#define KEY_TAPE 0x04
#define KEY_T1 0x06
#define KEY_LIFT 0x0A
#define KEY_DROP 0x0B
#define KEY_SCIS 0x0C
#define KEY_REC 0x21
#define KEY_PLAY 0x22
#define KEY_STOP 0x23
#define KEY_LEFT 0x24
#define KEY_RIGHT 0x25
#define KEY_SHIFT 0x26
#define KEY_ENC0 0x3B
#define ROWS_MAX 21                                                        /* portrait rows on screen; landscape shows 9 */
#define PITCH 14

volatile uint32_t g_trk_toggles, g_trk_written, g_trk_plays, g_trk_frames, g_trk_knobs;   /* stats / tests */
static uint64_t s_swallow;
static struct trk_cell s_clip;                                              /* lift / drop */
static int s_clip_full;
static int s_or;                                                            /* the orientation drawn (0, 1, 2) */

/* the 24 note keys -> key index (f3 = 0 .. e5 = 23); -1 other keys (screen-interface.md section 7, op1emu ui.json) */
static const int8_t key_idx[64] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 1, 3, 5,                      /* 0x0d f#3, 0x0e g#3, 0x0f a#3 */
    -1,-1,-1,-1,-1,-1,10,13,15,17,20,22, 7, 9,11,12,                      /* 0x16 d#4 .. 0x1b d#5, 0x1c c4 d4 e4 f4 */
     8,-1,-1,-1,-1,-1,-1, 0, 2, 4, 6,-1,-1,-1,-1,-1,                      /* 0x20 c#4, 0x27 f3 g3 a3 b3 */
    14,16,18,19,21,23,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };                    /* 0x30 g4 a4 b4 c5 d5 e5 */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int cur_pat(void) { return g_song.order[g_trk.cur_pos & 31] & 15; }
static struct trk_cell *cur_cell(void) { return trk_cell_at(cur_pat(), g_trk.cur_row, g_trk.cur_ch); }
static int wrapi(int v, int n) { return n <= 0 ? 0 : ((v % n) + n) % n; }
static void cell_copy(struct trk_cell *d, const struct trk_cell *s) { d->note = s->note; d->inst = s->inst; d->vol = s->vol; d->fx = s->fx; d->fxv = s->fxv; }   /* no libc memcpy */

/* ---- the switch ---- */
static void set_on(int on)
{
    coop_tracker_set_on(on);
    g_trk_toggles++;
}

/* ---- editing (KeyHandler thread) ---- */
static int note_of(int inst, int idx) { return inst == 1 ? 53 + idx : clampi(41 + 12 * clampi(g_song.octave, 0, 6) + idx, 1, 127); }
static void write_note(int idx)
{
    struct trk_cell *x = cur_cell();
    x->note = (uint8_t)note_of(x->inst, idx);
    coop_tracker_touch();
    g_trk_written++;
    g_trk.cur_row = wrapi(g_trk.cur_row + 1, trk_pat_len(cur_pat()));
}
static void preview(int idx, int on)
{
    struct trk_cell *x = cur_cell();
    coop_tracker_preview(x->inst, note_of(x->inst, idx), on);
}
static void move_row(int d) { g_trk.cur_row = wrapi(g_trk.cur_row + d, trk_pat_len(cur_pat())); }
static void move_ch(int d) { g_trk.cur_ch = wrapi(g_trk.cur_ch + d, TRK_CH); }

/* from coop_ui_key, before the tape's own handlers: 1 = consumed */
extern "C" int coop_tracker_key(int code, int pressed)
{
    uint64_t bit = (uint64_t)1 << (code & 63);
    if (!pressed) {
        if (s_swallow & bit) {
            s_swallow &= ~bit;
            if (key_idx[code & 63] >= 0) preview(key_idx[code & 63], 0);
            return 1;
        }
        return 0;
    }
    if (code == KEY_TAPE && SHIFT_HELD && CUR_MODE == 2) { set_on(!g_song.on); s_swallow |= bit; return 1; }   /* Shift + tape: tracker / tape */
    if (!g_song.on || CUR_MODE != 2) return 0;
    if (code == KEY_SHIFT || code == KEY_TAPE || code <= 0x03 || code == 0x05 || code == 0x2B || code == 0x2C) return 0;   /* the mode keys, shift, help, metronome, mic, com: stock */
    int idx = key_idx[code & 63];
    if (idx >= 0) {
        if (g_trk.edit && g_trk.view <= 1) write_note(idx);
        preview(idx, 1);
        s_swallow |= bit;
        return 1;
    }
    s_swallow |= bit;
    switch (code) {
    case KEY_T1: case KEY_T1 + 1: case KEY_T1 + 2: case KEY_T1 + 3: g_trk.view = code - KEY_T1; break;
    case KEY_LEFT: if (SHIFT_HELD) move_ch(-1); else if (g_trk.view == 2) g_trk.cur_pos = wrapi(g_trk.cur_pos - 1, g_song.order_len); else move_row(-1); break;
    case KEY_RIGHT: if (SHIFT_HELD) move_ch(1); else if (g_trk.view == 2) g_trk.cur_pos = wrapi(g_trk.cur_pos + 1, g_song.order_len); else move_row(1); break;
    case KEY_PLAY:
        if (g_trk.playing && !SHIFT_HELD) coop_tracker_stop();
        else { coop_tracker_play(SHIFT_HELD); g_trk_plays++; }
        break;
    case KEY_STOP: if (g_trk.playing) coop_tracker_stop(); else g_trk.cur_row = 0; break;
    case KEY_REC: g_trk.edit = !g_trk.edit; break;
    case KEY_LIFT: cell_copy(&s_clip, cur_cell()); s_clip_full = 1; break;
    case KEY_DROP: if (s_clip_full) { cell_copy(cur_cell(), &s_clip); coop_tracker_touch(); } break;
    case KEY_SCIS: { struct trk_cell *x = cur_cell(); x->note = 0; x->fx = 0; x->fxv = 0; coop_tracker_touch(); } break;
    case KEY_ENC0: { struct trk_cell *x = cur_cell(); x->note = x->note == TRK_NOTE_OFF ? 0 : (x->note ? TRK_NOTE_OFF : 0); coop_tracker_touch(); } break;   /* note -> off -> none */
    case KEY_ENC0 + 2: { struct trk_cell *x = cur_cell(); x->vol = 0; coop_tracker_touch(); } break;
    case KEY_ENC0 + 3: { struct trk_cell *x = cur_cell(); x->fx = 0; x->fxv = 0; coop_tracker_touch(); } break;
    default: break;
    }
    return 1;
}

/* from coop_ui_knob: steps = detents. 1 = taken */
static void knob_cell(int knob, int steps, int shift)
{
    struct trk_cell *x = cur_cell();
    switch (knob) {
    case 0:
        if (x->note == 0 || x->note == TRK_NOTE_OFF) x->note = (uint8_t)(x->inst ? 53 : 60);   /* the first turn places a note */
        else x->note = (uint8_t)clampi(x->note + steps * (shift ? 12 : 1), 1, 127);
        break;
    case 1: x->inst = (uint8_t)clampi(x->inst + steps, 0, 2); g_song.ch_inst[g_trk.cur_ch] = x->inst; break;   /* syn, drm, fm (F1) */
    case 2: x->vol = (uint8_t)clampi(x->vol + steps * (shift ? 10 : 1), 0, 99); break;
    case 3:
        if (!shift) {
            x->fx = (uint8_t)clampi(x->fx + steps, 0, TRK_FX_N - 1);
            if (x->fx == TRK_FX_PRB && !x->fxv) x->fxv = 50;
            if (x->fx == TRK_FX_VOL && !x->fxv) x->fxv = 80;
            if (x->fx == TRK_FX_ARP && !x->fxv) x->fxv = 37;
            if ((x->fx == TRK_FX_RTG || x->fx == TRK_FX_CUT || x->fx == TRK_FX_DLY) && (!x->fxv || x->fxv > 16)) x->fxv = x->fx == TRK_FX_RTG ? 2 : 3;
        } else {
            int hi = x->fx == TRK_FX_ARP ? 99 : (x->fx == TRK_FX_PRB ? 100 : (x->fx == TRK_FX_VOL ? 99 : 16));
            x->fxv = (uint8_t)clampi(x->fxv + steps, 0, hi);
        }
        break;
    }
    coop_tracker_touch();
}
static void knob_song(int knob, int steps, int shift)
{
    switch (knob) {
    case 0: g_trk.cur_pos = clampi(g_trk.cur_pos + steps, 0, g_song.order_len - 1); return;
    case 1: g_song.order[g_trk.cur_pos & 31] = (uint8_t)clampi(g_song.order[g_trk.cur_pos & 31] + steps, 0, 15); break;
    case 2: g_song.order_len = (uint8_t)clampi(g_song.order_len + steps, 1, 32); if (g_trk.cur_pos >= g_song.order_len) g_trk.cur_pos = g_song.order_len - 1; break;
    case 3: { int p = cur_pat(); g_song.pat_len[p] = (uint8_t)clampi(g_song.pat_len[p] + steps * (shift ? 16 : 1), 1, 64); if (g_trk.cur_row >= g_song.pat_len[p]) g_trk.cur_row = g_song.pat_len[p] - 1; break; }
    }
    coop_tracker_touch();
}
static void knob_setup(int knob, int steps, int shift)
{
    switch (knob) {
    case 0: g_song.bpm = (uint16_t)clampi(g_song.bpm + steps * (shift ? 10 : 1), 40, 300); break;
    case 1: g_song.speed = (uint8_t)clampi(g_song.speed + steps, 1, 16); break;
    case 2: g_song.octave = (uint8_t)clampi(g_song.octave + steps, 0, 6); break;
    case 3: g_song.orient = (uint8_t)clampi(g_song.orient + steps, 0, 2); break;
    }
    coop_tracker_touch();
}
extern "C" int coop_tracker_knob(int knob, int steps)
{
    if (!g_song.on || CUR_MODE != 2) return 0;
    if (knob < 0 || knob > 3 || !steps) return 1;
    int shift = SHIFT_HELD;
    g_trk_knobs++;
    if (g_trk.view == 2) knob_song(knob, steps, shift);
    else if (g_trk.view == 3) knob_setup(knob, steps, shift);
    else knob_cell(knob, steps, shift);
    return 1;
}

/* ---- drawing (Gfx thread) ---- */
static const uint8_t c_bg[4] = { 0, 0, 0, 255 };
static const uint8_t c_head[4] = { 0xAE, 0xB1, 0xDC, 255 };
static const uint8_t c_row[4] = { 0xB9, 0xB9, 0xC9, 255 };
static const uint8_t c_dim[4] = { 0x62, 0x5D, 0x77, 255 };
static const uint8_t c_blue[4] = { 0x69, 0x8E, 0xFF, 255 };
static const uint8_t c_green[4] = { 0x00, 0xED, 0x95, 255 };
static const uint8_t c_white[4] = { 0xFF, 0xFF, 0xFF, 255 };
static const uint8_t c_orange[4] = { 0xFF, 0x3A, 0x5D, 255 };
static const uint8_t c_bar_cur[4] = { 0x24, 0x26, 0x34, 255 };             /* the cursor row */
static const uint8_t c_bar_play[4] = { 0x0C, 0x3A, 0x2C, 255 };            /* the playhead row */
static const uint8_t *const c_knob[4] = { c_blue, c_green, c_white, c_orange };

enum { OV_NUM, OV_NUM_CUR, OV_NOTE, OV_NOTE_DRM = OV_NOTE + TRK_CH, OV_NOTE_CUR = OV_NOTE_DRM + TRK_CH, OV_FX = OV_NOTE_CUR + TRK_CH, OV_N = OV_FX + TRK_CH };   /* a colour is baked in its object */
enum { ZM_NUM, ZM_NUM_CUR, ZM_NOTE, ZM_NOTE_CUR, ZM_INST, ZM_INST_CUR, ZM_VOL, ZM_VOL_CUR, ZM_FX, ZM_FX_CUR, ZM_FXV, ZM_FXV_CUR, ZM_N };
enum { SG_NUM, SG_NUM_CUR, SG_PAT, SG_PAT_CUR, SG_LEN, SG_LEN_CUR, SG_N };
static struct coop_text s_tx_over[ROWS_MAX][OV_N];
static struct coop_text s_tx_zoom[ROWS_MAX][ZM_N];
static struct coop_text s_tx_song[ROWS_MAX][SG_N];
static struct coop_text s_tx_setup[4][2];
static struct coop_text s_tx_head[4];

static int VW(void) { return s_or ? 160 : 320; }
static int VH(void) { return s_or ? 320 : 160; }
static void xf(int32_t x, int32_t y, int32_t *X, int32_t *Y)
{
    if (!s_or) { *X = x; *Y = y; }
    else if (s_or == 1) { *X = 320 - y; *Y = x; }                           /* portrait a: the view's top at the screen's right edge (the device turned anticlockwise) */
    else { *X = y; *Y = 160 - x; }                                          /* portrait b: at the left edge (turned clockwise) */
}
static void vfill(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t *c)
{
    int32_t ax, ay, bx, by;
    xf(x0, y0, &ax, &ay); xf(x1, y1, &bx, &by);
    if (ax > bx) { int32_t t = ax; ax = bx; bx = t; }
    if (ay > by) { int32_t t = ay; ay = by; by = t; }
    for (int32_t y = ay; y < by; y += 4) {                                  /* thin overlapping strokes (op1emu: wide ones leave rows) */
        int32_t h = by - y < 8 ? by - y : 8;
        sf_line(r, ax << 10, (y << 10) + (h << 9), bx << 10, (y << 10) + (h << 9), h << 10, c);
    }
}
static void vdot(void *r, int32_t x, int32_t y, int32_t d, const uint8_t *c)
{
    int32_t X, Y; xf(x, y, &X, &Y);
    sf_line(r, X << 10, Y << 10, (X << 10) + 1, Y << 10, d << 10, c);
}
/* Portrait text: the stock Text object takes only the TRANSLATION of its matrix (op1emu, t1: the glyphs stayed
 * upright at the rotated place), so the turned views draw kennel's stroke font with every segment put through the
 * transform. Scale s = Q10 px per grid unit (a glyph is 4 x 6 units, the advance 6 units; '1' and '.' 4). */
static void xfq(int32_t x, int32_t y, int32_t *X, int32_t *Y)             /* Q10 in, Q10 out */
{
    if (s_or == 1) { *X = (320 << 10) - y; *Y = x; }
    else { *X = y; *Y = (160 << 10) - x; }
}
static void ptext(void *r, int32_t x, int32_t y, int32_t s, const char *t, const uint8_t *c)
{
    int32_t xq = x << 10, yq = y << 10;
    for (; *t; t++) {
        char ch = *t;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);                   /* the stroke font is capitals */
        for (unsigned k = 0; k < sizeof sf_font / sizeof sf_font[0]; k++) if (sf_font[k].c == ch) {
            for (int j = 0; j < sf_font[k].n; j++) {
                const uint8_t *g = sf_font[k].seg[j];
                int32_t ax, ay, bx, by;
                xfq(xq + g[0] * s, yq + g[1] * s, &ax, &ay);
                xfq(xq + g[2] * s, yq + g[3] * s, &bx, &by);
                sf_line(r, ax, ay, bx, by, 1024, c);
            }
            break;
        }
        xq += (ch == '1' || ch == '.' ? 4 : 6) * s;
    }
}
static void vtext(struct coop_text *t, void *r, int32_t x, int32_t y, int32_t size, const uint8_t *c, const char *s)
{
    if (!s_or) { coop_text_draw(t, r, x, y, size, c, s, 0); return; }     /* landscape: the stock font, the pooled object */
    ptext(r, x, y, size >= 12 ? 1536 : 1280, s, c);                        /* portrait: 9 / 7.5 px a character */
}

static void num2(char *o, int v) { v = clampi(v, 0, 99); o[0] = (char)('0' + v / 10); o[1] = (char)('0' + v % 10); o[2] = 0; }
static void num3(char *o, int v)
{
    int i = 0;
    if (v >= 100) o[i++] = (char)('0' + v / 100);
    if (v >= 10) o[i++] = (char)('0' + (v / 10) % 10);
    o[i++] = (char)('0' + v % 10); o[i] = 0;
}
static void note_name(char *o, int note)                                    /* "c-4", "c#4"; "off", "---" */
{
    static const char names[12][2] = { {'c','-'},{'c','#'},{'d','-'},{'d','#'},{'e','-'},{'f','-'},{'f','#'},{'g','-'},{'g','#'},{'a','-'},{'a','#'},{'b','-'} };
    if (note == TRK_NOTE_OFF) { o[0] = 'o'; o[1] = 'f'; o[2] = 'f'; o[3] = 0; return; }
    if (note <= 0 || note > 127) { o[0] = '-'; o[1] = '-'; o[2] = '-'; o[3] = 0; return; }
    int oct = note / 12 - 1;
    o[0] = names[note % 12][0]; o[1] = names[note % 12][1]; o[2] = (char)('0' + clampi(oct, 0, 9)); o[3] = 0;
}
static const char *const inst_names[3] = { "syn", "drm", "fm" };
static const char *const fx_names[TRK_FX_N] = { "---", "arp", "rtg", "cut", "dly", "vol", "prb" };
static const char fx_letter[TRK_FX_N] = { ' ', 'a', 'r', 'c', 'd', 'v', 'p' };
static void scat(char *d, const char *s) { int i = 0; while (d[i]) i++; while (*s && i < 30) d[i++] = *s++; d[i] = 0; }

static int rows_shown(void) { return (VH() - 26) / PITCH; }
static int first_row(int len, int focus)
{
    int n = rows_shown();
    return clampi(focus - n / 2, 0, len > n ? len - n : 0);
}
static void draw_header(void *r)
{
    char l[32], rt[16], b[4];
    int pos = g_trk.playing ? g_trk.pos : g_trk.cur_pos;
    l[0] = 0;                                                               /* "trk p00 01/01 120 syn": 21 chars (coop_text keeps 23) */
    if (!s_or) scat(l, "trk ");
    scat(l, "p"); num2(b, g_song.order[pos & 31]); scat(l, b);
    scat(l, " "); num2(b, pos + 1); scat(l, b); scat(l, "/"); num2(b, g_song.order_len); scat(l, b);
    scat(l, " "); num3(b, g_song.bpm); scat(l, b);
    scat(l, coop_tracker_live_family() ? " drm" : " syn");                 /* the family the core plays (t1: one at a time) */
    vtext(&s_tx_head[0], r, 4, 4, s_or ? 10 : 12, c_head, l);
    rt[0] = 0;
    scat(rt, g_trk.playing ? "play" : "stop");
    if (g_trk.edit && !s_or) scat(rt, "  edit");
    vtext(&s_tx_head[g_trk.playing ? 1 : 2], r, VW() - (s_or ? 32 : 80), 4, s_or ? 10 : 12, g_trk.playing ? c_green : c_dim, rt);
    if (g_trk.edit && s_or) vdot(r, 155, 9, 3, c_white);                   /* portrait: edit is a white dot */
    vfill(r, 0, 19, VW(), 20, c_dim);
}
static void row_bars(void *r, int i, int row, int pat_shown)
{
    int y = 22 + i * PITCH;
    int play_here = g_trk.playing && (g_song.order[g_trk.pos & 31] & 15) == pat_shown && row == g_trk.row;
    if (play_here) vfill(r, 0, y - 1, VW(), y + PITCH - 2, c_bar_play);
    else if (row == g_trk.cur_row) vfill(r, 0, y - 1, VW(), y + PITCH - 2, c_bar_cur);
}
static void draw_overview(void *r)
{
    int pat = cur_pat(), len = trk_pat_len(pat);
    int focus = g_trk.playing && (g_song.order[g_trk.pos & 31] & 15) == pat ? g_trk.row : g_trk.cur_row;
    int f = first_row(len, focus), n = rows_shown();
    int x_num = s_or ? 0 : 6, x0 = s_or ? 20 : 36, step = s_or ? 23 : 47, size = s_or ? 10 : 12;
    for (int i = 0; i < n && f + i < len; i++) {
        int row = f + i, y = 22 + i * PITCH;
        row_bars(r, i, row, pat);
        char b[4]; num2(b, row);
        vtext(&s_tx_over[i][row == g_trk.cur_row ? OV_NUM_CUR : OV_NUM], r, x_num, y, size, row == g_trk.cur_row ? c_blue : c_dim, b);
        for (int c = 0; c < TRK_CH; c++) {
            struct trk_cell *x = trk_cell_at(pat, row, c);
            int on = row == g_trk.cur_row && c == g_trk.cur_ch;
            char nn[4]; note_name(nn, x->note);
            if (!x->note && !on) { nn[0] = '.'; nn[1] = 0; }
            vtext(&s_tx_over[i][(on ? OV_NOTE_CUR : (x->inst == 1 ? OV_NOTE_DRM : OV_NOTE)) + c], r, x0 + c * step, y, size, on ? c_blue : (x->inst == 1 ? c_green : c_row), nn);
            if (x->fx && !s_or) { char fl[2]; fl[0] = fx_letter[x->fx]; fl[1] = 0; vtext(&s_tx_over[i][OV_FX + c], r, x0 + c * step + 26, y, size, c_orange, fl); }
            else if (x->fx) vdot(r, x0 + c * step + 20, y + 10, 2, c_orange);
        }
    }
    for (int c = 0; c < TRK_CH; c++) vdot(r, x0 + c * step + 2, 17, 2, c == g_trk.cur_ch ? c_blue : c_dim);   /* the channel marks over the columns */
}
static void draw_zoom(void *r)
{
    int pat = cur_pat(), len = trk_pat_len(pat), ch = g_trk.cur_ch;
    int focus = g_trk.playing && (g_song.order[g_trk.pos & 31] & 15) == pat ? g_trk.row : g_trk.cur_row;
    int f = first_row(len, focus), n = rows_shown();
    static const int xl[6] = { 6, 34, 78, 118, 158, 196 }, xp[6] = { 2, 22, 52, 84, 108, 136 };
    const int *xs = s_or ? xp : xl;
    for (int i = 0; i < n && f + i < len; i++) {
        int row = f + i, y = 22 + i * PITCH, on = row == g_trk.cur_row;
        struct trk_cell *x = trk_cell_at(pat, row, ch);
        row_bars(r, i, row, pat);
        char b[4]; num2(b, row);
        vtext(&s_tx_zoom[i][on ? ZM_NUM_CUR : ZM_NUM], r, xs[0], y, 12, on ? c_blue : c_dim, b);
        char nn[4]; note_name(nn, x->note);
        vtext(&s_tx_zoom[i][on ? ZM_NOTE_CUR : ZM_NOTE], r, xs[1], y, 12, on ? c_blue : c_row, nn);
        if (x->note || on) {
            vtext(&s_tx_zoom[i][on ? ZM_INST_CUR : ZM_INST], r, xs[2], y, 12, on ? c_green : c_dim, inst_names[x->inst > 2 ? 0 : x->inst]);
            if (x->vol) num2(b, x->vol); else { b[0] = '-'; b[1] = '-'; b[2] = 0; }
            vtext(&s_tx_zoom[i][on ? ZM_VOL_CUR : ZM_VOL], r, xs[3], y, 12, on ? c_white : c_dim, b);
        }
        if (x->fx || on) {
            vtext(&s_tx_zoom[i][on ? ZM_FX_CUR : ZM_FX], r, xs[4], y, 12, on ? c_orange : c_dim, fx_names[x->fx]);
            if (x->fx) { num3(b, x->fxv); vtext(&s_tx_zoom[i][on ? ZM_FXV_CUR : ZM_FXV], r, xs[5], y, 12, on ? c_orange : c_dim, b); }
        }
        if (!s_or) for (int c = 0; c < TRK_CH; c++) {                      /* the strip: every channel's note on this row */
            struct trk_cell *o = trk_cell_at(pat, row, c);
            vdot(r, 246 + c * 12, y + 6, o->note ? 4 : 1, c == ch ? c_blue : (o->note ? c_row : c_dim));
        }
    }
}
static void draw_song(void *r)
{
    int len = g_song.order_len, f = first_row(len, g_trk.cur_pos), n = rows_shown();
    static const int xl[3] = { 6, 70, 150 }, xp[3] = { 4, 50, 100 };
    const int *xs = s_or ? xp : xl;
    for (int i = 0; i < n && f + i < len; i++) {
        int pos = f + i, y = 22 + i * PITCH, on = pos == g_trk.cur_pos;
        int pat = g_song.order[pos & 31] & 15;
        if (g_trk.playing && pos == g_trk.pos) vfill(r, 0, y - 1, VW(), y + PITCH - 2, c_bar_play);
        else if (on) vfill(r, 0, y - 1, VW(), y + PITCH - 2, c_bar_cur);
        char b[12]; num2(b, pos + 1);
        vtext(&s_tx_song[i][on ? SG_NUM_CUR : SG_NUM], r, xs[0], y, 12, on ? c_blue : c_dim, b);
        b[0] = 'p'; num2(b + 1, pat);
        vtext(&s_tx_song[i][on ? SG_PAT_CUR : SG_PAT], r, xs[1], y, 12, on ? c_green : c_row, b);
        num2(b, g_song.pat_len[pat]); if (!s_or) scat(b, " rows");
        vtext(&s_tx_song[i][on ? SG_LEN_CUR : SG_LEN], r, xs[2], y, 12, on ? c_orange : c_dim, b);
    }
}
static void draw_setup(void *r)
{
    static const char *const names[4] = { "bpm", "speed", "octave", "view" };
    static const char *const views[3] = { "landscape", "portrait a", "portrait b" };
    int xn = s_or ? 6 : 24, xv = s_or ? 66 : 180;
    for (int i = 0; i < 4; i++) {
        int y = 30 + i * 20;
        char b[12];
        vtext(&s_tx_setup[i][0], r, xn, y, 12, c_row, names[i]);
        const char *v = b;
        if (i == 0) num3(b, g_song.bpm);
        else if (i == 1) num2(b, g_song.speed);
        else if (i == 2) num2(b, g_song.octave);
        else v = views[clampi(g_song.orient, 0, 2)];
        vtext(&s_tx_setup[i][1], r, xv, y, 12, c_knob[i], v);
    }
}
extern "C" void coop_tracker_draw(void *r)
{
    int o = clampi(g_song.orient, 0, 2);
    s_or = o;
    g_trk_frames++;
    for (int32_t y = -4; y < 164; y += 4) sf_line(r, -4096, (y + 4) << 10, 324 << 10, (y + 4) << 10, 8 << 10, c_bg);   /* black over the tape */
    draw_header(r);
    switch (g_trk.view) {
    case 1: draw_zoom(r); break;
    case 2: draw_song(r); break;
    case 3: draw_setup(r); break;
    default: draw_overview(r); break;
    }
}
