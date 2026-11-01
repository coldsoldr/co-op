/*
 * Text entry service (op1_feature.h op1_text_entry): a modal screen for typing a short name. Three threads:
 *   kn_text_input    KeyHandler: first in kn_features_input (kn_core.c), before every feature; takes every key press
 *                    and encoder turn while the entry is open
 *   kn_text_draw     Gfx: kn_screens_update, after the features' overlays (over every screen), before the toast
 *   kn_text_service  main loop: kn_core_service; runs the opener's done(text, ok) once
 * Controls (ours; the naming spec's grid, docs/maiko-rebuild.md): blue turn one character, green one row (both wrap),
 * blue or white push adds the character, left deletes the last one, red push finishes with ok 1; a mode key finishes
 * with ok 0 and goes on to the stock code. Releases of keys that were already down when the entry opened (the sound
 * key a naming feature opens it from, Shift) go to the stock code too, so its held flags clear.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#pragma GCC optimize("no-tree-loop-distribute-patterns")          /* the firmware has no memcpy / memset */
#include "../op1_synth.h"
#include "../op1_feature.h"
#include "../screens/stroke_font.h"

#define CS_MAX   64
#define ROWS     4                                    /* grid rows shown */
#define DETENT   1024
static const char default_cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";

static struct {
    volatile uint8_t open;                            /* taking input, drawn */
    volatile uint8_t pending;                         /* done still to run (main loop) */
    uint8_t ok, max, cols, n, len;
    int sel;
    int32_t acc[2];                                   /* encoder remainders, blue / green */
    uint64_t swallow;                                 /* presses taken while open: their releases are taken too */
    void (*done)(const char *text, int ok);
    int (*key)(int code, int pressed, int shift);
    char cs[CS_MAX + 1], buf[OP1_TEXT_MAX + 1], title[OP1_TEXT_MAX + 1];
} te;

static int allowed(char c)                            /* op1_setting_set_str's set without ':' (not in FAT names) */
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
           c == '_' || c == ',' || c == '-';
}

int op1_text_entry_open(void) { return te.open || te.pending; }

int op1_text_entry(const op1_text_entry_t *t)
{
    if (!t || !t->done || t->max < 1 || t->max > OP1_TEXT_MAX || op1_text_entry_open()) return -1;
    const char *cs = t->charset ? t->charset : default_cs;
    int n = 0;
    for (; cs[n]; n++) if (n >= CS_MAX || !allowed(cs[n])) return -1;
    if (!n) return -1;
    for (int i = 0; i <= n; i++) te.cs[i] = cs[i];
    te.n = (uint8_t)n;
    te.max = t->max;
    te.cols = (uint8_t)(t->cols >= 1 && t->cols <= 16 ? t->cols : 9);   /* the grid spreads them over x 18..302 */
    int k = 0;
    for (const char *s = t->title; s && *s && k < OP1_TEXT_MAX; s++) te.title[k++] = *s;
    te.title[k] = 0;
    k = 0;
    for (const char *s = t->initial; s && *s && k < te.max; s++) if (allowed(*s)) te.buf[k++] = *s;
    te.buf[k] = 0;
    te.len = (uint8_t)k;
    te.sel = 0; te.acc[0] = te.acc[1] = 0; te.ok = 0;
    te.done = t->done; te.key = t->key;
    te.open = 1;                                      /* last: the other threads start here */
    return 0;
}

void op1_text_entry_close(int ok)
{
    if (!te.open) return;
    te.ok = (uint8_t)(ok != 0);
    te.open = 0;
    te.pending = 1;
}

/* main loop: the opener's done, once (cleared first, so done may open a new entry) */
void kn_text_service(void)
{
    if (!te.pending || te.open) return;
    char text[OP1_TEXT_MAX + 1];
    int n = te.len;
    for (int i = 0; i < n; i++) text[i] = te.buf[i];
    while (n && (text[n - 1] == ' ' || text[n - 1] == '.')) n--;   /* not at the end of a file name */
    text[n] = 0;
    void (*done)(const char *, int) = te.done;
    int ok = te.ok;
    te.pending = 0;
    done(text, ok);
}

static int mode_key(int code)                         /* help, tempo, synth, drum, tape, mixer, sequencer, mic, COM */
{
    return code <= 0x05 || code == 0x15 || code == 0x2B || code == 0x2C;
}

static void step(int which, int32_t delta)            /* which 0: one character, 1: one row */
{
    te.acc[which] += delta;
    int k = 0;
    while (te.acc[which] >= DETENT) { te.acc[which] -= DETENT; k++; }
    while (te.acc[which] <= -DETENT) { te.acc[which] += DETENT; k--; }
    int s = te.sel + k * (which ? te.cols : 1);
    while (s < 0) s += te.n;
    while (s >= te.n) s -= te.n;
    te.sel = s;
}

/* KeyHandler (kn_features_input): 1 = taken */
int kn_text_input(int type, int code, int pressed, int delta, int shift)
{
    uint64_t bit = code >= 0 && code < 64 ? (uint64_t)1 << code : 0;
    if (!te.open) {                                   /* closed: still take the releases of presses it took */
        if (type != OP1_IN_KEY || pressed || !(te.swallow & bit)) return 0;
        te.swallow &= ~bit;
        return 1;
    }
    if (type == OP1_IN_KNOB) {
        if (code == 0 || code == 1) step(code, delta);
        return 1;                                     /* every turn while open */
    }
    if (te.key && te.key(code, pressed, shift)) {     /* the opener's keys first */
        if (pressed) te.swallow |= bit;
        else te.swallow &= ~bit;
        return 1;
    }
    if (!pressed) {                                   /* a release: taken only if its press was */
        int mine = (te.swallow & bit) != 0;
        te.swallow &= ~bit;
        return mine;
    }
    if (code == 0x26) return 0;                       /* Shift: stock keeps its flag */
    if (code == 0x38 || code == 0x39 || code == 0x3F) return 0;   /* R246's own events: jacks (0x019EFE40), eject */
    if (mode_key(code)) { op1_text_entry_close(0); return 0; }   /* finish, then the mode key acts as usual */
    te.swallow |= bit;
    if (code == 0x3B || code == 0x3D) {               /* blue / white push: add */
        if (te.len < te.max) { te.buf[te.len] = te.cs[te.sel]; te.buf[++te.len] = 0; }
    } else if (code == 0x24) {                        /* left: delete */
        if (te.len) te.buf[--te.len] = 0;
    } else if (code == 0x3E) {                        /* red push: done */
        op1_text_entry_close(1);
    }
    return 1;
}

/* ---- the screen (Gfx thread), design A: black; the title and a len/max counter in grey; the text in white with a
 * white underline cursor; a grey rule; the grid across x 18..302 in `cols` columns, rows 21 px apart, the current row
 * white with a green bar at the left edge, the selected character in a blue box; a legend in the knob colours (blue
 * LETTER, green ROW, white ADD, red OK) and DEL for the left key; a space in the grid is an open box. Colours as the
 * chord page's. ---- */
static const uint8_t c_black[4] = { 0, 0, 0, 255 }, c_white[4] = { 0xff, 0xff, 0xff, 255 },
    c_grey[4] = { 0x7c, 0x79, 0x90, 255 }, c_red[4] = { 0xff, 0x3a, 0x5d, 255 },
    c_blue[4] = { 0x69, 0x8e, 0xff, 255 }, c_green[4] = { 0x00, 0xed, 0x95, 255 };
#define STROKE 0x600                                  /* 1.5 px */
#define GRID_X0 18
#define GRID_X1 302
#define ROW_Y0  60                                    /* the first row's top; rows 21 px apart (centres 66, 87, 108) */
#define ROW_Y0_4 52                                   /* four rows: start higher, clear of the legend */
#define ROW_DY  21

static char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static void ln(void *r, int x0, int y0, int x1, int y1, int32_t w, const uint8_t *c)   /* px */
{
    sf_line(r, x0 << 10, y0 << 10, x1 << 10, y1 << 10, w, c);
}
static int32_t text(void *r, int32_t x, int y, int32_t s, const char *t, const uint8_t *col)   /* x Q10, y px */
{
    char b[2] = { 0, 0 };
    for (; *t; t++) { b[0] = up(*t); x = sf_text(r, x, y << 10, s, b, STROKE, col); }
    return x;
}
static int32_t width(const char *t, int32_t s)        /* Q10, as sf_text advances */
{
    int32_t w = 0;
    for (; *t; t++) w += (*t == '1' || *t == '.' ? 4 : 6) * s;
    return w;
}
static void box(void *r, int x0, int y0, int x1, int y1, const uint8_t *c)
{
    ln(r, x0, y0, x1, y0, STROKE, c); ln(r, x1, y0, x1, y1, STROKE, c);
    ln(r, x1, y1, x0, y1, STROKE, c); ln(r, x0, y1, x0, y0, STROKE, c);
}
static void ring(void *r, int cx, int cy, const uint8_t *c)   /* a 3 px ring of 8 segments, centre in px */
{
    static const int8_t px[9] = { 3, 2, 0, -2, -3, -2, 0, 2, 3 }, py[9] = { 0, 2, 3, 2, 0, -2, -3, -2, 0 };
    for (int i = 0; i < 8; i++) ln(r, cx + px[i], cy + py[i], cx + px[i + 1], cy + py[i + 1], 0x400, c);
}
static void legend(void *r)
{
    static const char *const label[4] = { "LETTER", "ROW", "ADD", "OK" };
    static const uint8_t *const col[4] = { c_blue, c_green, c_white, c_red };
    int x = 14;
    for (int i = 0; i < 4; i++) {
        ring(r, x, 140, col[i]);
        x = (int)(text(r, (x + 6) << 10, 137, 1024, label[i], c_grey) >> 10) + 14;
    }
    int32_t w = width("DEL", 1024);                  /* right-aligned at 308, a left-pointing triangle before it */
    int x0 = 308 - (int)(w >> 10);
    text(r, x0 << 10, 137, 1024, "DEL", c_grey);
    ln(r, x0 - 4, 137, x0 - 4, 143, 0x400, c_grey); ln(r, x0 - 4, 137, x0 - 9, 140, 0x400, c_grey);
    ln(r, x0 - 9, 140, x0 - 4, 143, 0x400, c_grey);
}

void kn_text_draw(void *r)
{
    if (!te.open) return;
    for (int y = 4; y <= 156; y += 8) ln(r, 0, y, 320, y, 12 << 10, c_black);   /* wide lines are capped */
    text(r, 12 << 10, 5, 1536, te.title, c_grey);
    char cnt[8], *o = cnt;                            /* len/max, right-aligned */
    if (te.len >= 10) *o++ = (char)('0' + te.len / 10);
    *o++ = (char)('0' + te.len % 10); *o++ = '/';
    if (te.max >= 10) *o++ = (char)('0' + te.max / 10);
    *o++ = (char)('0' + te.max % 10); *o = 0;
    text(r, (308 << 10) - width(cnt, 1536), 5, 1536, cnt, c_grey);
    int32_t s = 2731;                                 /* 16 px tall; smaller when max would not fit */
    if (te.max * 6 * s > (296 << 10)) s = (296 << 10) / (te.max * 6);
    int32_t x = text(r, 12 << 10, 21, s, te.buf, c_white);
    if (te.len < te.max) sf_line(r, x, 40 << 10, x + 4 * s, 40 << 10, STROKE, c_white);   /* cursor */
    ln(r, 12, 45, 308, 45, 0x400, c_grey);
    int rows = (te.n + te.cols - 1) / te.cols, row = te.sel / te.cols;
    int top = row < ROWS ? 0 : row - ROWS + 1;
    if (top > rows - ROWS) top = rows > ROWS ? rows - ROWS : 0;
    int32_t cell = ((GRID_X1 - GRID_X0) << 10) / te.cols;
    int y0 = rows - top >= ROWS ? ROW_Y0_4 : ROW_Y0;
    for (int rr = 0; rr < ROWS && top + rr < rows; rr++) {
        int cy = y0 + rr * ROW_DY, cur = top + rr == row;
        if (cur) ln(r, 8, cy - 3, 8, cy + 15, 3 << 10, c_green);
        for (int c = 0; c < te.cols; c++) {
            int i = (top + rr) * te.cols + c;
            if (i >= te.n) break;
            int cx = (int)(((GRID_X0 << 10) + cell * c + cell / 2) >> 10);   /* the cell's centre */
            const uint8_t *col = cur ? c_white : c_grey;
            if (i == te.sel) box(r, cx - 8, cy - 4, cx + 8, cy + 16, c_blue);
            if (te.cs[i] == ' ') {                    /* space: an open box (a baseline with upticks), unlike '_' */
                ln(r, cx - 4, cy + 12, cx + 4, cy + 12, STROKE, col);
                ln(r, cx - 4, cy + 12, cx - 4, cy + 8, STROKE, col);
                ln(r, cx + 4, cy + 12, cx + 4, cy + 8, STROKE, col);
            }
            else { char t[2] = { up(te.cs[i]), 0 }; sf_text(r, (cx - 4) << 10, cy << 10, 2048, t, STROKE, col); }
        }
    }
    legend(r);
}
