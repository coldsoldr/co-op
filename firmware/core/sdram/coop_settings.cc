/*
 * coop-fw u1 / u7: the settings menu (docs/ui-features.md §1).
 *
 * Opened by the four mode keys held together (synth + drum + tape + mixer), the combo that entered the Lander game
 * (lander_off.cc swallows the fourth key and calls coop_settings_open). An overlay drawn from coop_ui_frame over the
 * stock screen (kennel's overlay model, docs/screens.md): no screen object, no SVG, no TE heap.
 *
 * Blue turn moves the cursor; the right arrow or a blue push enters a submenu; the left arrow goes back a level and,
 * at the top, leaves; a mode key leaves and then acts as usual. Green turn changes the selected value (drawn in
 * green); an encoder push on a Plugins row enables / disables that file, applied at the next boot. The keyboard
 * plays through. No hints on screen (the owner, 2026-11-07).
 *
 * Submenus: VOICES (spread 0-100, the only live setting; drum ADSR, master EQ, master compression stay "stock" with
 * "not yet"), UI (tape position bottom; date and time: not yet), PLUGINS (every .coop file in /yaffs2/plugins).
 *
 * Persisted in /yaffs2/user/coop_settings.bin ("CST", version 1: spread %, four choice bytes, the disabled file
 * names). u7: the file is written by the main loop (coop_settings_service from coop_fx_service), never from the
 * KeyHandler thread: a yaffs write from that thread crashed the device (u5, SEQSTAT 0x2a on leaving the menu after a
 * plugin toggle). Read at boot before the plugin scan (coop_plugins_boot), where a file present overrides the image's
 * g_coop_spread word. Threads: keys and encoders in the KeyHandler thread, drawing in the Gfx thread, files on the
 * main thread. Fixed point only, no libc. Drawn in the stock font (coop_text.h) with the manifesto's colour code.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 * The stroke font is hundekraft's (kennel 3.1), GPL-3.0-or-later.
 */
#include <stdint.h>

#define Y_OPEN   ((int (*)(const char *, int, int))0x01A997F4u)         /* yaffs open / close / read / write, as plugin_host.cc */
#define Y_CLOSE  ((int (*)(int))0x01A99908u)
#define Y_READ   ((int (*)(int, void *, int))0x01A99C90u)
#define Y_WRITE  ((int (*)(int, const void *, int))0x01A99F84u)

typedef struct { int32_t x, y; } op1_pt;                               /* Q10, 320 x 160 units */
#define op1_raster_line(r, p0, p1, w_q10, rgba) \
    (((void (*)(void *, const op1_pt *, const op1_pt *, int32_t, const uint8_t *))0x01A24A4Cu)((r), (p0), (p1), (w_q10), (rgba)))
#include "../../third_party/kennel/synth_sdk/screens/stroke_font.h"
#include "coop_text.h"

extern "C" int coop_plugin_file_count(void);                            /* plugin_host.cc: the files the boot scan saw */
extern "C" const char *coop_plugin_file_name(int i);
extern volatile uint32_t g_coop_spread;                                 /* stereo_bus.cc, Q15, read every block */

#define SET_FILE   "/yaffs2/user/coop_settings.bin"
#define DIS_MAX    32                                                   /* e7: was 16; 32 files load (plugin_host.cc MAX_FILES) */
#define NAME_LEN   40                                                   /* p6k: the loader's own limit; v1 files had 16 (a 17-character name could never be disabled) */
#define NAME_LEN_V1 16
#define KEY_LEFT   0x24
#define KEY_RIGHT  0x25
#define KEY_SHIFT  0x26
#define KEY_ENC0   0x3B
#define ROWS_SHOWN 7

static const char *const top_names[3] = { "voices", "ui", "plugins" };          /* lower case: the stock font */
static const char *const voices_rows[4] = { "spread", "drum adsr", "master eq", "master comp" };
static const char *const ui_rows[3] = { "tape position", "date", "time" };

volatile uint32_t g_set_opens, g_set_saves, g_set_loaded;              /* stats / tests */
static volatile int s_open;
static int s_level;                                                     /* 0 the top, 1 a submenu */
static int s_cur[2];                                                    /* cursor at each level */
static int s_pct;                                                       /* spread, 0..100 */
static uint8_t s_choice[4];                                             /* drum adsr, eq, comp, tape position: 0 = stock */
static char s_dis[DIS_MAX][NAME_LEN];                                   /* disabled plugin files */
static int s_ndis;
static int s_dirty;
static volatile int s_save_pending;                                     /* the main loop writes the file */

volatile uint32_t g_set_dropped, g_set_listed;                         /* e4d: names dropped from the list, names left */
static int plausible(const char *s)                                     /* a .coop file name: printable, ends .coop */
{
    int n = 0;
    for (; s[n]; n++) if (s[n] < 0x21 || s[n] > 0x7E || s[n] == '/') return 0;
    return n > 5 && s[n - 5] == '.' && s[n - 4] == 'c' && s[n - 3] == 'o' && s[n - 2] == 'o' && s[n - 1] == 'p';
}
static int is_note(int c)                                               /* IsNoteKey 0x019C316C's 24 codes (screen-interface.md §7) */
{
    return (c >= 0x0D && c <= 0x0F) || (c >= 0x16 && c <= 0x20) || (c >= 0x27 && c <= 0x2A) || (c >= 0x30 && c <= 0x35);
}
static int seq(const char *a, const char *b) { int i = 0; for (; a[i] && a[i] == b[i]; i++) {} return a[i] == b[i]; }
static void scpy(char *d, const char *s, int cap) { int i = 0; for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; }

extern "C" int coop_plugin_disabled(const char *fname)
{
    for (int i = 0; i < s_ndis; i++) if (seq(s_dis[i], fname)) return 1;
    return 0;
}

static int rows(void)
{
    if (!s_level) return 3;
    if (s_cur[0] == 0) return 4;
    if (s_cur[0] == 1) return 3;
    return coop_plugin_file_count();
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---- the file (main thread: boot, and the service pass) ----
 * e3: the buffer is static. Since p6k (40-byte names) it is 650 bytes, and on the stack of the main-loop service pass
 * it was the likeliest cause of the device crash leaving the menu after a plugin toggle (e2, two plugin files; not
 * reproduced in op1emu). It also held 8 + names where the file has 10 + names (2 bytes short with 16 disabled). */
#define FILE_MAX_BYTES (10 + DIS_MAX * NAME_LEN)
static uint8_t s_filebuf[FILE_MAX_BYTES];                               /* save (main loop) and boot (main thread): never at once */
static int save_file(void)
{
    uint8_t *b = s_filebuf;                                             /* zeroed by hand: no libc, so no memset */
    const uint32_t bsize = sizeof s_filebuf;
    for (uint32_t i = 0; i < bsize; i++) b[i] = 0;
    b[0] = 'C'; b[1] = 'S'; b[2] = 'T'; b[3] = 2;                     /* v2: 40-byte names (p6k) */
    b[4] = (uint8_t)s_pct;
    for (int i = 0; i < 4; i++) b[5 + i] = s_choice[i];                /* b[5..8]: the four choices; b[9] the count */
    int n = 9;
    b[n++] = (uint8_t)s_ndis;
    for (int i = 0; i < s_ndis; i++) { for (int k = 0; k < NAME_LEN && s_dis[i][k]; k++) b[n + k] = (uint8_t)s_dis[i][k]; n += NAME_LEN; }   /* up to the NUL: the rest stays zero */
    int fd = Y_OPEN(SET_FILE, 0x242, 0x180);
    if (fd < 0) return 0;
    int ok = Y_WRITE(fd, b, n) == n;
    ok = (Y_CLOSE(fd) == 0) && ok;
    if (ok) g_set_saves++;
    return ok;
}
extern "C" void coop_settings_service(void)                             /* main loop (coop_fx_service) */
{
    if (!s_save_pending) return;
    s_save_pending = 0;
    save_file();
}
extern "C" void coop_settings_boot(void)
{
    uint8_t *b = s_filebuf;
    int fd = Y_OPEN(SET_FILE, 0, 0);
    if (fd < 0) return;
    int got = Y_READ(fd, b, (int)sizeof s_filebuf);
    Y_CLOSE(fd);
    if (got < 10 || b[0] != 'C' || b[1] != 'S' || b[2] != 'T' || (b[3] != 1 && b[3] != 2)) return;
    int nl = b[3] == 1 ? NAME_LEN_V1 : NAME_LEN;                         /* v1 files: 16-byte names (read once, saved back as v2) */
    s_pct = clampi(b[4], 0, 100);
    for (int i = 0; i < 4; i++) s_choice[i] = 0;                        /* no alternative exists yet: stock whatever was saved */
    int n = b[9], at = 10;
    s_ndis = 0;
    for (int i = 0; i < n && i < DIS_MAX && at + nl <= got; i++, at += nl) {
        scpy(s_dis[s_ndis], (const char *)b + at, nl);
        if (plausible(s_dis[s_ndis])) s_ndis++;                         /* e4d: garbage is never a name */
        else g_set_dropped++;
    }
    g_coop_spread = (uint32_t)(s_pct * 32767 / 100);
    g_set_loaded = 1;
}

/* e4d (the owner's device, 2026-11-10: "list full", saw could not be switched off): the boot scan's file names
 * (plugin_host.cc, main thread, right after its directory listing). The list keeps only names of files that are there
 * now, once each; anything else (a file deleted since, a duplicate, the bytes of a damaged file: the e2 crash happened
 * while this file was being written) is dropped and the cleaned list is saved by the main loop. So the 16 places can
 * only ever hold real files (e7: up to 32 of them, plus the named files past that limit). */
extern "C" void coop_settings_prune(const char (*names)[40], int n_names)
{
    int k = 0;
    for (int i = 0; i < s_ndis; i++) {
        int present = 0, dup = 0;
        for (int j = 0; j < n_names && !present; j++) if (seq(s_dis[i], names[j])) present = 1;
        for (int j = 0; j < k && !dup; j++) if (seq(s_dis[i], s_dis[j])) dup = 1;
        if (present && !dup) { if (k != i) scpy(s_dis[k], s_dis[i], NAME_LEN); k++; }
        else g_set_dropped++;
    }
    if (k != s_ndis) { s_ndis = k; s_save_pending = 1; }
    g_set_listed = (uint32_t)s_ndis;
}

/* e3, PLUGIN_SET over SysEx (coop_sysex.cc, main loop): put a plugin file on or off the disabled list and write the
 * settings file now. Applied at the next boot, as the menu's toggle. 1 = saved, 0 = the write failed, -1 = refused
 * (the menu is open on the device: its own edit wins), -2 = the list is full. */
extern "C" int coop_settings_set_plugin(const char *fname, int enable)
{
    if (s_open) return -1;
    int at = -1;
    for (int k = 0; k < s_ndis; k++) if (seq(s_dis[k], fname)) at = k;
    if (enable && at >= 0) {
        for (int k = at; k + 1 < s_ndis; k++) scpy(s_dis[k], s_dis[k + 1], NAME_LEN);
        s_ndis--;
    } else if (!enable && at < 0) {
        if (s_ndis >= DIS_MAX) return -2;
        scpy(s_dis[s_ndis++], fname, NAME_LEN);
    }
    return save_file();
}

/* ---- open / close (KeyHandler thread) ---- */
extern "C" int coop_settings_is_open(void) { return s_open; }
extern "C" int coop_settings_open(void)
{
    if (s_open) return 0;
    uint32_t sp = g_coop_spread > 32767u ? 32767u : g_coop_spread;
    s_pct = (int)((sp * 100u + 16383u) / 32767u);
    s_level = 0; s_cur[0] = 0; s_cur[1] = 0;
    s_open = 1;
    g_set_opens++;
    return 1;
}
static void close_menu(void)
{
    if (!s_open) return;
    s_open = 0;
    if (s_dirty) { s_dirty = 0; s_save_pending = 1; }
}
volatile uint32_t g_set_plugin_turns, g_set_plugin_pushes, g_set_plugin_bounces;
static uint64_t s_last_push;
static inline uint64_t cycles_now(void)                                 /* 64 bits: CYCLES alone wraps every ~10.7 s */
{
    uint32_t lo, hi;
    __asm__ __volatile__("%0 = CYCLES; %1 = CYCLES2;" : "=d"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static void toggle_plugin(int i)
{
    const char *name = coop_plugin_file_name(i);
    if (!name) return;
    for (int k = 0; k < s_ndis; k++)
        if (seq(s_dis[k], name)) {                                     /* disabled: enable */
            for (; k + 1 < s_ndis; k++) scpy(s_dis[k], s_dis[k + 1], NAME_LEN);
            s_ndis--; s_dirty = 1;
            return;
        }
    if (s_ndis < DIS_MAX) { scpy(s_dis[s_ndis++], name, NAME_LEN); s_dirty = 1; }
}
static void enter(void) { if (!s_level) { s_level = 1; s_cur[1] = 0; } }

/* 1 = consumed; a mode key closes the menu and then acts; the keyboard and shift play through */
extern "C" int coop_settings_key(int code, int pressed)
{
    if (!s_open) return 0;
    if (code >= 0x02 && code <= 0x05) { if (pressed) close_menu(); return 0; }
    if (is_note(code) || code == KEY_SHIFT) return 0;
    if (!pressed) return 1;
    if (code == KEY_LEFT) { if (s_level) s_level = 0; else close_menu(); return 1; }
    if (code == KEY_RIGHT) { enter(); return 1; }
    if (code >= KEY_ENC0 && code <= KEY_ENC0 + 3) {                     /* a push: enter at the top, toggle a plugin row */
        if (!s_level) enter();
        else if (s_cur[0] == 2) {                                       /* e4c: a push toggles once: a second press within
                                                                           250 ms is the switch bouncing, which put the row
                                                                           straight back ON on the device */
            uint64_t t = cycles_now();
            if (s_last_push && t - s_last_push < 100000000ull) g_set_plugin_bounces++;   /* 250 ms at 400 MHz */
            else { toggle_plugin(s_cur[1]); g_set_plugin_pushes++; }
            s_last_push = t | 1u;
        }
    }
    return 1;
}

/* steps: detents, signed. 1 = taken */
extern "C" int coop_settings_knob(int knob, int steps)
{
    if (!s_open) return 0;
    if (knob == 0) {
        int n = rows();
        s_cur[s_level] = n ? clampi(s_cur[s_level] + steps, 0, n - 1) : 0;
    } else if (knob == 1 && s_level == 1 && s_cur[0] == 0 && s_cur[1] == 0) {
        s_pct = clampi(s_pct + steps, 0, 100);
        g_coop_spread = (uint32_t)(s_pct * 32767 / 100);
        s_dirty = 1;
    } else if (knob == 1 && s_level == 1 && s_cur[0] == 2 && steps) {   /* e4c: green sets a plugin row, as every value row */
        const char *name = coop_plugin_file_name(s_cur[1]);
        if (name && (steps > 0) == (coop_plugin_disabled(name) != 0)) { toggle_plugin(s_cur[1]); g_set_plugin_turns++; }
    }
    return 1;                                                         /* the other values have no alternative yet */
}

/* ---- drawing (Gfx thread) ---- */
static const uint8_t c_bg[4] = { 0, 0, 0, 255 };
static const uint8_t c_title[4] = { 0xAE, 0xB1, 0xDC, 255 };          /* lavender: the title */
static const uint8_t c_blue[4] = { 0x69, 0x8E, 0xFF, 255 };           /* the cursor row: blue moves it */
static const uint8_t c_green[4] = { 0x00, 0xED, 0x95, 255 };          /* the value: green changes it */
static const uint8_t c_row[4] = { 0xB9, 0xB9, 0xC9, 255 };            /* the other rows and their values */
static const uint8_t c_grey[4] = { 0x62, 0x5D, 0x77, 255 };           /* not yet */
enum { TX_TITLE, TX_EMPTY, TX_ROW0 };
enum { RT_NAME_BLUE, RT_NAME_ROW, RT_VAL_GREEN, RT_VAL_ROW, RT_VAL_GREY, ROW_TX };
static struct coop_text s_tx[TX_ROW0 + ROWS_SHOWN * ROW_TX];        /* one object per place, size and colour */

static void fill(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t *c)
{
    int32_t ym = (y0 + y1) << 9, w = (y1 - y0) << 10;
    sf_line(r, x0 << 10, ym, x1 << 10, ym, w, c);
}
static void num3(char *out, int v)
{
    int i = 0;
    if (v >= 100) out[i++] = (char)('0' + v / 100);
    if (v >= 10) out[i++] = (char)('0' + (v / 10) % 10);
    out[i++] = (char)('0' + v % 10);
    out[i] = 0;
}
static void file_label(const char *fname, char *out)                   /* "stereo_delay.coop" -> "stereo delay" */
{
    int i = 0;
    for (; fname[i] && i < 15; i++) {
        char c = fname[i];
        if (c == '.') break;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c == '_') c = ' ';
        out[i] = c;
    }
    out[i] = 0;
}
static void row_text(void *r, int row, int which, const char *s)
{
    static const int32_t xs[ROW_TX] = { 24, 24, 200, 200, 200 };
    static const uint8_t *const cs[ROW_TX] = { c_blue, c_row, c_green, c_row, c_grey };
    coop_text_draw(&s_tx[TX_ROW0 + row * ROW_TX + which], r, xs[which], 26 + row * 15, 12, cs[which], s, 0);
}

extern "C" void coop_settings_draw(void *r)
{
    if (!s_open) return;
    for (int32_t y = -4; y < 164; y += 4) fill(r, -4, y, 324, y + 8, c_bg);   /* thin overlapping strokes (op1emu: wide ones leave rows) */
    char title[32]; int t = 0;
    for (const char *q = "settings"; *q; q++) title[t++] = *q;
    if (s_level) { title[t++] = ' '; title[t++] = ' '; for (const char *q = top_names[s_cur[0]]; *q; q++) title[t++] = *q; }
    title[t] = 0;
    coop_text_draw(&s_tx[TX_TITLE], r, 12, 8, 12, c_title, title, 0);

    int n = rows(), cur = s_cur[s_level];
    int start = clampi(cur - 3, 0, n > ROWS_SHOWN ? n - ROWS_SHOWN : 0);
    for (int i = start; i < n && i < start + ROWS_SHOWN; i++) {
        int row = i - start, on = i == cur;
        char label[16];
        const char *name = "";
        const char *val = 0; int vgrey = 0;
        if (!s_level) name = top_names[i];
        else if (s_cur[0] == 0) {
            name = voices_rows[i];
            if (i == 0) { num3(label, s_pct); val = label; }
            else val = "stock";
        } else if (s_cur[0] == 1) {
            name = ui_rows[i];
            if (i == 0) val = "bottom"; else { val = "not yet"; vgrey = 1; }
        } else {
            const char *f = coop_plugin_file_name(i);
            file_label(f ? f : "?", label); name = label;
            if (f && coop_plugin_disabled(f)) { val = "off"; vgrey = 1; } else val = "on";
        }
        row_text(r, row, on ? RT_NAME_BLUE : RT_NAME_ROW, name);
        if (val) row_text(r, row, vgrey ? RT_VAL_GREY : (on ? RT_VAL_GREEN : RT_VAL_ROW), val);
    }
    if (s_level && s_cur[0] == 2 && n == 0) coop_text_draw(&s_tx[TX_EMPTY], r, 24, 26, 12, c_grey, "no plugin files", 0);
}
