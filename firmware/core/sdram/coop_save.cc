/*
 * coop-fw u5: overwrite save (docs/ui-features.md §4).
 *
 * Stock saves a sound by holding a sound key through the save clock (global_controller: the clock pass at
 * 0x019AD090 from the main loop, research 2026-11-07): it reads the slot's row, names the sound with a date stamp
 * (snprintf 0x01A9CE04 at 0x019AD220 for the synth, 0x019AD5A6 for the drum), makes the snapshot .raw marker,
 * INSERTs a new row into synth_user / drum_user (0x01995938 at 0x019AD3DC, 0x01995B80 at 0x019AD762), rewrites the
 * slot row with the new name and reloads the preset array. Stock keeps no "origin": fw_synth_slot_preset
 * (0x01A02030; drum 0x01A01DC8) returns the index into the preset array (0x01DDDB68 + i * 0x38) of the preset
 * whose "type" and "name" equal the slot's, and whether one was found; entry + 0x30 says factory.
 *
 * With the synth (or drum) key held through the clock (the deferred mode key, lander_off.cc; the stock clock does
 * not mind it), coop overwrites: the stamp hook writes the sound's own name instead of the date, and the insert
 * hook runs an UPDATE of the matching user row (same folder, same "name" in the styled JSON) instead of the
 * INSERT. Everything else is the stock's: the slot row, the reload, the green tick. A factory preset, or a sound
 * whose name matches no user preset, saves normally. "overwriting <name>" is drawn over the save clock. The
 * snapshot .raw marker the stock makes (the sound's name, with _n if taken) is a stray empty file; a sampler
 * preset's audio is not re-copied over the original (noted, v1).
 *
 * The four CALL sites are retargeted at region-C trampolines +192 .. +240 (tools/l1_stereo_insert.py), which jump
 * without touching the stack, so the stock arguments arrive unchanged. Main thread (the clock pass). No libc.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_text.h"

#define BANK          (*(volatile int *)0x01DE4C40u)                       /* 0 synth, 1 drum */
#define SYNTH_INST    (*(volatile int *)0xFF8016D4u)
#define DRUM_KIT      (*(volatile int *)0xFF8016D8u)
#define SAVE_SHOWING  (*(volatile uint8_t *)0x01DF4D98u)                   /* the save overlay's state: showing */
#define STOCK_SNPRINTF ((int (*)(char *, int, const char *, int, int, int, int, int))0x01A9CE04u)
#define STOCK_INSERT_S ((int (*)(void *))0x01995938u)
#define STOCK_INSERT_D ((int (*)(void *))0x01995B80u)
#define FIND_S        ((int (*)(int, int *))0x01A02030u)                   /* fw_synth_slot_preset(slot, &found): the preset ARRAY INDEX */
#define FIND_D        ((int (*)(int, int *))0x01A01DC8u)
#define ENTRY(i)      ((const uint8_t *)(0x01DDDB68u + (uint32_t)(i) * 0x38))   /* the preset array: JSON +4, folder +0x14, factory +0x30 */
#define ENTRIES_MAX   318
#define DB_USER       ((void *(*)(void))0x0196DD44u)
#define DB_EXEC       ((int (*)(void *, const char *))0x0196F950u)
#define JV_GET        ((const void *(*)(const void *, const char *))0x019BEB70u)
#define SQL_MAX       8192
#define Y_UNLINK      ((int (*)(const char *))0x01A9A3FCu)
#define SNAPSHOT_DIR  "/yaffs2/user/audio/preset_synth/snapshot/"
#define Y_OPEN        ((int (*)(const char *, int, int))0x01A997F4u)
#define Y_CLOSE       ((int (*)(int))0x01A99908u)
#define Y_LSEEK       ((int (*)(int, int, int))0x01A9A0E4u)                 /* whence 0 / 1 / 2 (kennel) */

extern "C" uint8_t coop_mode_keys_held(void);                              /* lander_off.cc */
extern "C" void coop_mode_key_used(void);

volatile uint32_t g_over_saves, g_over_armed, g_over_stock, g_over_trace;   /* stats / tests; trace: the emulator watches it */
static int s_over;                                                          /* this save overwrites: the origin found */
static char s_name[32], s_folder[16];
static char s_sql[SQL_MAX];

static const char *std_str(const void *s)                                   /* a 0x1c-byte std::string: data inline at +4, or a heap pointer when capacity >= 16 */
{
    uint32_t cap = *(const uint32_t *)((const uint8_t *)s + 0x18);
    return cap >= 16 ? *(const char *const *)((const uint8_t *)s + 4) : (const char *)s + 4;
}
static int scpy(char *d, const char *s, int cap) { int i = 0; for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; return i; }
static char *scat(char *d, const char *s, const char *end) { while (*s && d < end - 1) *d++ = *s++; *d = 0; return d; }

/* the origin of the slot's sound: 1 = a user preset (name and folder copied), 0 = factory / none */
static int origin(int bank)
{
    int found = 0;
    int i = bank ? FIND_D(DRUM_KIT, &found) : FIND_S(SYNTH_INST, &found);
    if (!found || i < 0 || i >= ENTRIES_MAX) return 0;
    const uint8_t *e = ENTRY(i);
    if (e[0x30]) return 0;                                                  /* factory */
    const void *json = e + 4;
    const void *n = JV_GET(json, "name");
    if (((const uint8_t *)n)[8] != 4) return 0;                             /* not a string */
    scpy(s_name, *(const char *const *)n, sizeof s_name);
    scpy(s_folder, std_str(e + 0x14), sizeof s_folder);
    return s_name[0] != 0;                                                  /* any user folder (u7) */
}
static int is_snapshot(void) { const char *q = "snapshot"; for (int k = 0; ; k++) { if (s_folder[k] != q[k]) return 0; if (!q[k]) return 1; } }
static void drop_marker(void)                                               /* the stock makes snapshot/<name>.raw and, when it exists,
                                                                               appends _n to the NAME: remove it first (a sampler's audio
                                                                               is re-copied there by the stock: the overwrite) */
{
    char path[80]; char *p = path; const char *end = path + sizeof path;
    p = scat(p, SNAPSHOT_DIR, end); p = scat(p, s_name, end); p = scat(p, ".raw", end);
    if (is_snapshot()) { Y_UNLINK(path); return; }                        /* the preset's own marker or audio: the stock remakes it */
    int fd = Y_OPEN(path, 0, 0);                                            /* another folder's preset: only an EMPTY marker of the same
                                                                               name may go (a snapshot preset's audio of that name stays) */
    if (fd < 0) return;
    int size = Y_LSEEK(fd, 0, 2);
    Y_CLOSE(fd);
    if (size == 0) Y_UNLINK(path);
}

/* the stamp: the stock's snprintf(buf, 16, "%i%02i%02i_%02i%02i", y, m, d, h, min) */
extern "C" int coop_stamp(char *buf, int n, const char *fmt, int a, int b, int c, int d, int e)
{
    int bank = BANK;
    s_over = 0;
    g_over_trace = 1;
    if (coop_mode_keys_held() & (bank ? 2 : 1)) {
        coop_mode_key_used();
        if (origin(bank)) { s_over = 1; g_over_armed++; g_over_trace = 2; drop_marker(); g_over_trace = 3; return scpy(buf, s_name, n); }
    }
    g_over_stock++;
    return STOCK_SNPRINTF(buf, n, fmt, a, b, c, d, e);
}

static int update_row(const char *table, const void *entry)
{
    const char *text = std_str(entry);                                      /* the styled JSON, the name already ours */
    const char *end = s_sql + SQL_MAX;
    char *p = s_sql;
    p = scat(p, "UPDATE ", end); p = scat(p, table, end); p = scat(p, " SET patch='", end);
    p = scat(p, text, end); p = scat(p, "' WHERE folder='", end); p = scat(p, s_folder, end);
    p = scat(p, "' AND patch GLOB '*\"name\":\"", end); p = scat(p, s_name, end); p = scat(p, "\"*'", end);   /* compact JSON; GLOB, never LIKE:
                                                                                                             the stock exec formats the SQL again, a % hangs it */
    if (p >= end - 1) return 0;                                             /* did not fit: do nothing */
    void *db = DB_USER();
    if (!db) return 0;
    g_over_trace = 5;
    DB_EXEC(db, s_sql);
    g_over_trace = 6;
    return 1;
}
extern "C" int coop_insert_synth(void *entry)
{
    g_over_trace = 4;
    if (s_over) { s_over = 0; g_over_saves++; int r = update_row("synth_user", entry); g_over_trace = 7; return r; }
    return STOCK_INSERT_S(entry);
}
extern "C" int coop_insert_drum(void *entry)
{
    if (s_over) { s_over = 0; g_over_saves++; return update_row("drum_user", entry); }
    return STOCK_INSERT_D(entry);
}

/* drawing (Gfx thread): "overwriting <name>" over the save clock while the mode key is held and the origin is a
 * user preset (checked once when the overlay appears) */
static const uint8_t c_text[4] = { 0xDF, 0xD9, 0xFF, 255 };
static struct coop_text s_tx[2];
static int s_shown, s_will;
extern "C" void coop_save_draw(void *r)
{
    int showing = SAVE_SHOWING != 0;
    if (!showing) { s_shown = 0; return; }
    int bank = BANK;
    if (!s_shown) { s_shown = 1; s_will = (coop_mode_keys_held() & (bank ? 2 : 1)) && origin(bank); }
    if (!s_will) return;
    coop_text_draw(&s_tx[0], r, 12, 8, 12, c_text, "overwriting", 0);
    coop_text_draw(&s_tx[1], r, 12, 24, 12, c_text, s_name, 0);
}
