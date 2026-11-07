/*
 * coop-fw u5: rename a user preset in the preset browser (docs/ui-features.md §6, as OPUS's rename).
 *
 * The preset browser is the stock browser (screen 0x24) on the synth's preset list (the static list 0x01DDD688,
 * refilled on every open from the preset array 0x01DDDB68 + i * 0x38: JSON at +4, folder at +0x14, factory flag at
 * +0x30; research 2026-11-07). The selection accessor 0x01A17560 gives the selected preset's array index. The
 * orange push does nothing there in stock.
 *
 * Orange push on a user preset starts naming: the name is typed with the encoders, blue letters, green digits,
 * white symbols (! # + , - . ; =); a turn picks a character (shown at the cursor, not kept), a push of the same
 * encoder keeps it, a push with nothing picked makes a space; orange anticlockwise deletes the last character;
 * orange push saves; any other key cancels and acts as usual. 13 characters, lower case (the list font draws them
 * as capitals). The save is an UPDATE of the user row: the styled JSON's "name" replaced in place (SQLite's
 * replace(), no re-serialising), matched by folder and the old name; then the preset array is reloaded
 * (0x01A0567C) and the browser's list refilled (0x01A02790) so the new name shows. A factory preset, a sampler
 * preset (its audio file is named after the preset), an empty name or one starting with a dot is refused (the
 * name blinks back). The slot that holds the sound keeps its old name until it is saved again.
 *
 * KeyHandler thread (the db exec waits on the stock's lock, as the stock's own saves do). No libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_text.h"

#define CUR_SCREEN   (*(volatile int *)0x018B65C8u)
#define LIST_PTR     (*(volatile uint32_t *)0x01DE511Cu)
#define LIST_PRESET  0x01DDD688u
#define SCREEN_BROWSER 0x24
#define ROW_DISP     (*(volatile int *)0x01DE4FC0u)                       /* the first row shown */
#define ROW_RAW      (*(volatile int *)0x01DE5138u)                       /* the blue knob: row * 4 */
#define SELECTED     ((int (*)(void))0x01A17560u)                          /* the selected preset's array index */
#define ENTRY(i)     ((const uint8_t *)(0x01DDDB68u + (uint32_t)(i) * 0x38))
#define JV_GET       ((const void *(*)(const void *, const char *))0x019BEB70u)
#define DB_USER      ((void *(*)(void))0x0196DD44u)
#define DB_EXEC      ((int (*)(void *, const char *))0x0196F950u)
#define PRESETS_LOAD ((void (*)(void))0x01A0567Cu)
#define LIST_REFILL  ((void (*)(void))0x01A02790u)
#define KEY_ENC0     0x3B
#define NAME_MAX     13
#define SQL_MAX      1024

typedef struct { int32_t x, y; } op1_pt;
#define op1_raster_line(r, p0, p1, w_q10, rgba) \
    (((void (*)(void *, const op1_pt *, const op1_pt *, int32_t, const uint8_t *))0x01A24A4Cu)((r), (p0), (p1), (w_q10), (rgba)))
#include "../../third_party/kennel/synth_sdk/screens/stroke_font.h"

volatile uint32_t g_rename_starts, g_rename_saves, g_rename_refused, g_rename_pushes;   /* stats / tests */
static volatile int s_sql_pending;                                          /* the main loop runs the UPDATE */
static int s_on;                                                            /* naming */
static int s_entry;                                                         /* the preset's array index */
static char s_buf[NAME_MAX + 1], s_old[NAME_MAX + 1], s_folder[16];
static int s_len, s_set, s_idx;                                             /* the picked set (0 letters, 1 digits, 2 symbols; -1 none) and index */
static char s_pending;
static int s_blink;                                                         /* frames of "refused" blink */
static char s_sql[SQL_MAX];
static const char *const sets[3] = { "abcdefghijklmnopqrstuvwxyz", "0123456789", "!#+,-.;=" };
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int scpy(char *d, const char *s, int cap) { int i = 0; for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; return i; }
static char *scat(char *d, const char *s, const char *end) { while (*s && d < end - 1) *d++ = *s++; *d = 0; return d; }
static const char *std_str(const void *s) { uint32_t cap = *(const uint32_t *)((const uint8_t *)s + 0x18); return cap >= 16 ? *(const char *const *)((const uint8_t *)s + 4) : (const char *)s + 4; }
static const char *jv_str(const void *v) { return ((const uint8_t *)v)[8] == 4 ? *(const char *const *)v : 0; }
static int seq(const char *a, const char *b) { int i = 0; for (; a[i] && a[i] == b[i]; i++) {} return a[i] == b[i]; }

static int on_preset_browser(void) { return CUR_SCREEN == SCREEN_BROWSER && LIST_PTR == LIST_PRESET; }
extern "C" int coop_rename_active(void) { return s_on; }

static int start(void)
{
    int i = SELECTED();
    if (i < 0 || i >= 318) return 0;
    const uint8_t *e = ENTRY(i);
    if (e[0x30]) return 0;                                                  /* factory */
    const void *json = e + 4;
    const char *type = jv_str(JV_GET(json, "type"));
    if (type && seq(type, "sampler")) return 0;                             /* its audio file carries the name: not yet */
    const char *name = jv_str(JV_GET(json, "name"));
    if (!name) return 0;
    scpy(s_old, name, sizeof s_old);
    scpy(s_folder, std_str(e + 0x14), sizeof s_folder);
    s_len = scpy(s_buf, s_old, sizeof s_buf);
    s_entry = i; s_set = -1; s_pending = 0; s_on = 1;
    g_rename_starts++;
    return 1;
}
static int save(void)
{
    if (s_pending && s_len < NAME_MAX) { s_buf[s_len++] = s_pending; s_buf[s_len] = 0; s_pending = 0; }
    while (s_len && s_buf[s_len - 1] == ' ') s_buf[--s_len] = 0;           /* no trailing spaces */
    if (!s_len || s_buf[0] == '.') return 0;
    if (seq(s_buf, s_old)) return 1;
    const char *end = s_sql + SQL_MAX;
    char *p = s_sql;
    /* the three spellings of the member (compact, pretty-printed): nested replace()s, GLOB matches (a % would hang the stock exec) */
    p = scat(p, "UPDATE synth_user SET patch=replace(replace(replace(patch,'\"name\":\"", end); p = scat(p, s_old, end);
    p = scat(p, "\"','\"name\":\"", end); p = scat(p, s_buf, end);
    p = scat(p, "\"'),'\"name\": \"", end); p = scat(p, s_old, end);
    p = scat(p, "\"','\"name\": \"", end); p = scat(p, s_buf, end);
    p = scat(p, "\"'),'\"name\" : \"", end); p = scat(p, s_old, end);
    p = scat(p, "\"','\"name\" : \"", end); p = scat(p, s_buf, end);
    p = scat(p, "\"') WHERE folder='", end); p = scat(p, s_folder, end);
    p = scat(p, "' AND (patch GLOB '*\"name\":\"", end); p = scat(p, s_old, end); p = scat(p, "\"*'", end);
    p = scat(p, " OR patch GLOB '*\"name\": \"", end); p = scat(p, s_old, end); p = scat(p, "\"*'", end);
    p = scat(p, " OR patch GLOB '*\"name\" : \"", end); p = scat(p, s_old, end); p = scat(p, "\"*')", end);
    s_sql_pending = 1;                                                      /* the main loop runs it (coop_rename_service) */
    g_rename_saves++;
    return 1;
}
extern "C" void coop_rename_service(void)                                   /* main loop (coop_fx_service) */
{
    if (!s_sql_pending) return;
    s_sql_pending = 0;
    void *db = DB_USER();
    if (!db) return;
    DB_EXEC(db, s_sql);
    PRESETS_LOAD();
    LIST_REFILL();
}

/* from coop_ui_key: 1 = consumed */
extern "C" int coop_rename_key(int code, int pressed)
{
    if (!pressed) return s_on && code >= KEY_ENC0 && code <= KEY_ENC0 + 3;   /* the pushes' releases while naming */
    if (!s_on) {
        if (code >= KEY_ENC0 && code <= KEY_ENC0 + 3 && on_preset_browser()) g_rename_pushes++;
        if (code == KEY_ENC0 + 3 && on_preset_browser()) { if (!start()) { s_blink = 60; g_rename_refused++; } return 1; }
        return 0;
    }
    if (code >= KEY_ENC0 && code < KEY_ENC0 + 3) {                          /* blue / green / white push: keep or space */
        if (s_pending) { if (s_len < NAME_MAX) { s_buf[s_len++] = s_pending; s_buf[s_len] = 0; } s_pending = 0; s_set = -1; }
        else if (s_len < NAME_MAX) { s_buf[s_len++] = ' '; s_buf[s_len] = 0; }
        return 1;
    }
    if (code == KEY_ENC0 + 3) {                                             /* orange push: save */
        if (save()) s_on = 0; else { s_blink = 60; g_rename_refused++; }
        return 1;
    }
    s_on = 0;                                                               /* any other key: cancel, then it acts */
    return 0;
}
/* from coop_ui_knob while naming: every turn is taken */
extern "C" int coop_rename_knob(int knob, int steps)
{
    if (!s_on) return 0;
    knob &= 3;
    if (knob == 3) {
        if (steps < 0) { if (s_pending) s_pending = 0; else if (s_len) s_buf[--s_len] = 0; }
        return 1;
    }
    const char *set = sets[knob];
    int n = slen(set);
    if (s_set != knob) { s_set = knob; s_idx = steps > 0 ? 0 : n - 1; }
    else s_idx = ((s_idx + steps) % n + n) % n;
    s_pending = set[s_idx];
    return 1;
}

/* drawing (Gfx thread): the typed name over the selected row, a hint below */
static const uint8_t c_bg[4] = { 0, 0, 0, 255 }, c_text[4] = { 0xFF, 0xFF, 0xFF, 255 }, c_grey[4] = { 0x62, 0x5D, 0x77, 255 }, c_blue[4] = { 0x69, 0x8E, 0xFF, 255 };
static struct coop_text s_tx[3];
static void fill(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t *c)
{
    for (int32_t y = y0; y < y1; y += 2) sf_line(r, x0 << 10, (y + 2) << 9, x1 << 10, (y + 2) << 9, 4 << 10, c);
}
extern "C" void coop_rename_draw(void *r)
{
    if (!on_preset_browser()) { s_on = 0; return; }
    if (s_blink > 0) { s_blink--; coop_text_draw(&s_tx[2], r, 190, 146, 12, c_grey, "no", 0); }
    if (!s_on) return;
    int row = ROW_RAW / 4 - ROW_DISP;
    if (row < 0) row = 0;
    if (row > 5) row = 5;
    int32_t y = 6 + 23 * row;
    char shown[NAME_MAX + 3]; int k = scpy(shown, s_buf, sizeof shown);
    if (s_pending && k < NAME_MAX + 1) shown[k++] = s_pending;
    if (k < NAME_MAX + 2) shown[k++] = '_';
    shown[k] = 0;
    fill(r, 60, y, 180, y + 19, c_bg);
    coop_text_draw(&s_tx[0], r, 66, y + 2, 12, s_pending ? c_blue : c_text, shown, 0);
    coop_text_draw(&s_tx[1], r, 12, 146, 12, c_grey, "blue abc  green 123  white sym  orange save", 0);
}
