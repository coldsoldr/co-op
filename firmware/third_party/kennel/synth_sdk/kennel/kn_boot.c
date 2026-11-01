/*
 * kennel plugin loader, firmware part (ARCHITECTURE.md "Plugin loader", docs/plugin-format.md).
 * At boot, every plugin file (kennel .knl and op1r .op1p) in /yaffs2/plugins and /yaffs2/content (files shipped with
 * the firmware) becomes a synth engine, effect or feature. Several pieces follow op1r 34's loader (op1r-src a04e3bc,
 * plugload/plugload.c, GPL) so both firmwares treat a device the same way:
 *   - runs from the boot's CALL Clips::load at 0x019F02EE: databases open, staged updates applied, before sounds,
 *     the master FX, audio and screens;
 *   - plugin numbers come from one name -> number map in op1.db (table op1rplugins, the same row format as op1r):
 *     a file keeps its legacy order if free, else gets 32..63; numbers are never reused;
 *   - a file named like a built-in replaces it (same number);
 *   - its synth_types / fx_types row in op1_factory.db is written at boot; rows of file plugins that are gone are
 *     deleted;
 *   - a file that starts with its screen SVG is loaded as that SVG ("../../plugins/<file>" from content/display);
 *   - crash marker plugins/.boot: written before loading, removed once the UI has run a while (kn_boot_service, main
 *     loop) or at a normal power-off or restart after the UI started (kn_boot_unmount); found at boot -> all files
 *     are skipped this boot;
 *   - the master FX (loaded from the database, not through the patch parser) gets its selector by name.
 * A file that fails a check is skipped and logged (kn_log); boot always goes on.
 * Memory (kn_mem.h): a bump allocator in the code arena kn_arena_lo..kn_arena_hi: one block from R246's heap 0,
 * malloc'd at the start of the boot hook (op1r mallocs its plugins there too) and sized to the listed files
 * (arena_need: each enabled file's header, at most KN_ARENA_MAX), or the fixed range KN_ARENA_LO..KN_ARENA_TOP
 * (KN_ARENA_SIZE) when heap 0 can't give it. Heap 0 is out of PCREL24 reach of the core: a plugin's calls
 * into the core go through call stubs after its image (kn_load.h kn_relocate_far). Files are read one at a time into
 * KN_STAGE, inside the big-buffer pool, which is cleared once after every file is loaded; then the buffers files
 * declare (KN_FLAG_BIGBUF) get their pointers, in load order.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#pragma GCC optimize("no-tree-loop-distribute-patterns")          /* the firmware has no memcpy / memset */
#include "../op1_synth.h"
#include "../op1_feature.h"
#include "../op1_feature_drum_synth.h"
#include "../kn_mem.h"
#include "kn_load.h"
#include "kn_input.h"
#include "kn_validate.h"
#include "kn_seq_asset.h"
#include "kn_seq_rows.h"
#include "kn_inv.h"
#include "kn_images.h"
#include "../plugload/op1p_load.h"

#define Y_OPEN     ((int (*)(const char *, int, int))0x01A997F4)
#define Y_CLOSE    ((int (*)(int))0x01A99908)
#define Y_READ     ((int (*)(int, void *, unsigned))0x01A99C90)
#define Y_LSEEK    ((int32_t (*)(int, int32_t, int))0x01A9A0E4)   /* yaffs_lseek(fd, off, whence 0 set / 2 end) */
#define Y_UNLINK   ((int (*)(const char *))0x01A9A3FC)
#define Y_ACCESS   ((int (*)(const char *, int))0x01A9A6B0)
#define Y_MKDIR    ((int (*)(const char *, int))0x01A9A888)
#define Y_OPENDIR  ((void *(*)(const char *))0x01A9B450)
#define Y_READDIR  ((const uint8_t *(*)(void *))0x01A9B648)     /* dirent: +0x0B d_name (checked in yaffs_readdir) */
#define Y_CLOSEDIR ((int (*)(void *))0x01A9B74C)
#define O_CREATE   0x241
typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } kn_dstr;
#define CLIPS_LOAD ((void (*)(void *))0x0195CE88)
#define MASTER_LOAD ((void (*)(void))0x019D79BC)
#define MASTER_FX_TYPE ((void (*)(kn_dstr *))0x0196EF20)          /* db2::get_master_fx_type */
#define OP_DELETE  ((void (*)(void *))0x01A5E310)
#define DB_HANDLE  ((void *(*)(void))0x0196DD44)                  /* user/op1.db: the number map */
#define DB_FACTORY ((void *(*)(void))0x0196DD38)                  /* content/op1_factory.db: synth_types / fx_types */
#define DB_EXEC    ((void (*)(void *, const char *))0x0196F950)
#define DB_GET     ((void (*)(void *, void *, const char *))0x0196FBD0)

#define DIR_PLUGINS   "/yaffs2/plugins"
#define DIR_CONTENT   "/yaffs2/content"
#define MARKER        "/yaffs2/plugins/.boot"
#define MAX_FILES     128                                       /* every plugin as a file (about 45) + room for the user's own */

/* generated tables (make_firmware.gen_table), writable with room for every number */
extern const op1_engine_t *op1_engines[];
extern const char *op1_engine_names[];
extern const char *op1_sample_engine_names[];
extern const op1_screen_t *op1_engine_screens[];
extern const char *op1_engine_svgs[];
extern const op1_fx_t *op1_fxs[];
extern op1_fx_stereo_render_t op1_fx_stereo_renders[];
extern uint8_t op1_engine_coarse[];                             /* by number: knobs (bit k) in 8 steps ("coarse") */
void op1_fx_stereo_init(void);
extern const char *op1_fx_order_names[];
extern const char *op1_fx_names[];
extern const op1_screen_t *op1_fx_screens[];
extern const char *op1_fx_svgs[];
extern const kn_export kn_exports[];
extern const int kn_export_count;
void kn_features_finish(void);
int kn_feature_count(void);
const op1_feature_t *kn_feature_at(int);
int kn_feature_register(const op1_feature_t *f);
void *kn_alloc(uint32_t size);

/* state for a debugger / the emulator: what was loaded and why something was not */
volatile uint32_t kn_loaded, kn_failed, kn_skipped, kn_next = KN_ARENA_LO;
/* the arena (kn_mem.h): [kn_arena_lo, kn_arena_hi), the fixed range until arena_init moves it into heap 0. Exported:
 * scale.c tells a plugin's screen object from a stock one by it, the page 7 switch ic finds the arena's CPLB page */
volatile uint32_t kn_arena_lo = KN_ARENA_LO, kn_arena_hi = KN_ARENA_TOP;
volatile uint32_t kn_arena_heap0;                               /* 1: the arena is the heap-0 block */
volatile uint32_t kn_heap0_free_boot, kn_heap0_free_loaded;     /* heap 0's free bytes before the arena / after loading */
volatile uint32_t kn_arena_need, kn_arena_peek_bytes;           /* arena_need: the size asked for; bytes it read that
                                                                   load_file reads again (files not preloaded) */
/* kn_log: "who: what" lines for a debugger / the emulator (the word at _kn_log points at the text, _kn_log_cap is its
 * size). A small .bss buffer until load_all knows how many files there are; then KN_LOG_BASE + KN_LOG_PER_FILE bytes
 * per listed file in the arena (128 files: 8.7 KB), the early lines copied over. */
#define KN_LOG_BASE 512u                                        /* sampler / sound repair / claim lines */
#define KN_LOG_PER_FILE 64u                                     /* "id.knl: loaded" ~20, a refusal up to ~90 */
static char kn_log_boot[256];
char *kn_log = kn_log_boot;
uint32_t kn_log_cap = sizeof kn_log_boot;
static uint32_t kn_log_pos;
static uint8_t img_cache_user;                                  /* the file being loaded imports op1_screen_draw_cached */
static int artcache_users;                                      /* loaded files that do */
static uint8_t from_file[2][OP1_SLOTS];                         /* registered by a file this boot */
kn_img_map *kn_images;                                          /* kn_images.h: loaded files, for crash reports */
static kn_img_rec img_cur;                                      /* the file being loaded; kept only if it registers */
static int marker_set, ui_ran;                                  /* ui_ran: the main loop has run (kn_boot_service) */
static uint32_t marker_c2;
/* big-buffer pool (kn_mem.h KN_POOL_LO..HI): declared buffers reserved while loading, granted after the clear */
KN_MEM_ASSERT(KN_BIGBUF_MAX == KN_DECL_MAX, bigbuf_max);
typedef struct { uint32_t desc, off, size; } kn_grant;           /* desc: the op1_bigbuf_t in the plugin's image */
kn_grant kn_pool_grants[128] kn_farbss;                         /* one per file at most (MAX_FILES); debugger, matrix */
int kn_pool_ngrants;
#define grants kn_pool_grants
#define n_grants kn_pool_ngrants
static int rs_n;
static uint32_t decl_used, rs_used;                             /* rs_: the state before the file being loaded */
volatile uint32_t kn_pool_ready, kn_pool_clear_cycles, kn_decl_used;   /* debugger, inventory, op1_fx_big */

/* ---- small string helpers (no libc) ---- */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static char *scat(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }

/* R246's SVG parser panics on a number with too many decimals (Parser.cpp:116 "unsupported decimal count", seen
 * with 16 from Python float noise) and on a path command it lacks (Parser.cpp:525: Q and A in Amp's voice box,
 * 2026-10-29), and a panic at boot hangs the OP-1. Stock screens use at most 4 decimals and only M L H V C S Z. */
#define SVG_MAX_DECIMALS 4
static const char *svg_err(const uint8_t *s, uint32_t len)  /* 0 = fine, else why it is refused */
{
    for (uint32_t i = 1; i < len && s[i]; i++) {
        if (s[i] == 'd' && i + 2 < len && s[i + 1] == '=' && s[i + 2] == '"' && (s[i - 1] == ' ' || s[i - 1] == '\n' || s[i - 1] == '\t')) {
            for (i += 3; i < len && s[i] && s[i] != '"'; i++) {
                uint8_t c = s[i] | 0x20;
                if (c >= 'a' && c <= 'z' && c != 'm' && c != 'l' && c != 'h' && c != 'v' && c != 'c' && c != 's' && c != 'z')
                    return "screen SVG path uses a command R246 lacks (only M L H V C S Z)";
            }
            continue;
        }
        if (s[i] != '.' || s[i - 1] < '0' || s[i - 1] > '9') continue;
        uint32_t n = 0;
        while (i + 1 < len && s[i + 1] >= '0' && s[i + 1] <= '9') { i++; n++; }
        if (n > SVG_MAX_DECIMALS) return "screen SVG has a number with more than 4 decimals";
    }
    return 0;
}
static char *snum(char *d, int v)
{
    char t[12]; int n = 0;
    if (v < 0) { *d++ = '-'; v = -v; }
    do t[n++] = (char)('0' + v % 10); while (v /= 10);
    while (n) *d++ = t[--n];
    *d = 0;
    return d;
}
static void copy_str(char *d, const char *s, int n) { int i = 0; for (; i < n - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static void dstr_init(kn_dstr *s) { volatile uint32_t *w = (volatile uint32_t *)s; for (unsigned i = 0; i < sizeof *s / 4; i++) w[i] = 0; }
static int ends_with(const char *s, const char *e) { int a = slen(s), b = slen(e); return a > b && seq(s + a - b, e); }
void kn_log_line(const char *file, const char *msg);
#define log_line kn_log_line
void kn_log_line(const char *file, const char *msg)
{
    const char *p[4] = { file, ": ", msg, "\n" };
    for (int k = 0; k < 4; k++)
        for (const char *s = p[k]; *s && kn_log_pos < kn_log_cap - 1; s++) kn_log[kn_log_pos++] = *s;
    kn_log[kn_log_pos] = 0;                                     /* the arena copy is not zeroed */
}

/* ---- name -> number map, one text row in op1.db, op1r's format: "s:name=n,f:name=n," (- prefix: plugin gone) ---- */
#define MAP_MAX 128                                             /* both kinds: up to 63 numbers each */
static struct { char kind, present; char name[16]; int n; } map[MAP_MAX] kn_farbss;
static int map_n, map_dirty;
static void map_load(void *db)
{
    DB_EXEC(db, "CREATE TABLE IF NOT EXISTS op1rplugins (id INTEGER PRIMARY KEY, data TEXT NOT NULL)");
    kn_dstr s; dstr_init(&s);
    DB_GET(&s, db, "SELECT data FROM op1rplugins WHERE id=0");
    const char *p = s.cap > 15 ? s.u.ptr : s.u.buf, *e = p + s.size;
    while (p < e && map_n < MAP_MAX) {
        int present = 1;
        if (*p == '-') { present = 0; p++; }
        if (e - p < 4 || (p[0] != 's' && p[0] != 'f') || p[1] != ':') break;
        char k = p[0]; p += 2;
        int i = 0;
        while (p < e && *p != '=' && i < 15) map[map_n].name[i++] = *p++;
        map[map_n].name[i] = 0;
        if (p >= e || *p != '=') break;
        p++;
        int v = 0;
        while (p < e && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        if (p < e && *p == ',') p++;
        if (v < 1 || v > OP1_MAX_ORDER) continue;
        map[map_n].kind = k; map[map_n].present = (char)present; map[map_n].n = v; map_n++;
    }
    if (s.cap > 15 && s.u.ptr) OP_DELETE(s.u.ptr);
}
static void map_save(void *db)
{
    static char sql[64 + MAP_MAX * 24];
    char *o = scat(sql, "INSERT OR REPLACE INTO op1rplugins VALUES (0,'");
    for (int i = 0; i < map_n; i++) {
        if (!map[i].present) *o++ = '-';
        *o++ = map[i].kind; *o++ = ':';
        o = scat(o, map[i].name); *o++ = '=';
        o = snum(o, map[i].n); *o++ = ',';
    }
    scat(o, "')");
    op1_db_write(db, sql);
}

static int taken(int fx, int n) { return fx ? op1_fxs[n] != 0 : op1_engines[n] != 0; }
static int builtin_number(int fx, const char *name)                 /* a compiled-in plugin with this name */
{
    for (int n = 1; n < OP1_SLOTS; n++) {
        const char *nm = fx ? op1_fx_order_names[n - 1] : op1_engine_names[n - 1];
        if (taken(fx, n) && !from_file[fx][n] && nm && nm[0] && seq(nm, name)) return n;
    }
    return 0;
}
static int number_for(int fx, const char *name, int pref)
{
    char k = fx ? 'f' : 's';
    int me = -1;
    for (int i = 0; i < map_n; i++)
        if (map[i].kind == k && seq(map[i].name, name)) me = i;
    if (me >= 0) {
        if (!map[me].present) { map[me].present = 1; map_dirty = 1; }
        if (pref && map[me].n >= OP1_FILE_FIRST) {               /* a former built-in: back to its own number if free */
            int used = taken(fx, pref);
            for (int i = 0; i < map_n; i++) if (i != me && map[i].kind == k && map[i].n == pref) used = 1;
            if (!used) { map[me].n = pref; map_dirty = 1; }
        }
        return taken(fx, map[me].n) ? 0 : map[me].n;
    }
    if (map_n >= MAP_MAX) return 0;
    for (int n = pref ? pref : OP1_FILE_FIRST; n <= OP1_MAX_ORDER; n = n == pref ? OP1_FILE_FIRST : n + 1) {
        int used = taken(fx, n);
        for (int i = 0; i < map_n; i++) if (map[i].kind == k && map[i].n == n) used = 1;
        if (used) continue;
        map[map_n].kind = k; map[map_n].present = 1; map[map_n].n = n;
        copy_str(map[map_n].name, name, 16); map_n++; map_dirty = 1;
        return n;
    }
    return 0;
}

/* ---- the plugin's synth_types / fx_types row (as op1r: defaults, selector, page-2 words) ---- */
static const int16_t fx_rng[4][2] = { { 64, 14528 }, { 0, 24576 }, { 0, 22528 }, { 8192, 32767 } };
static void row_ensure(void *db, int fx, const char *name, const int16_t *def, const int16_t *page2, int n)
{
    int v[8];
    for (int i = 0; i < 4; i++) {
        int d = def[i] < 0 ? 0 : def[i];
        v[i] = fx ? fx_rng[i][0] + d * (fx_rng[i][1] - fx_rng[i][0]) / 32767 : d;
    }
    v[4] = OP1_ENGINE_MAGIC + n;
    for (int i = 5; i < 8; i++) v[i] = fx && page2 ? (page2[i - 5] < 0 ? 0 : page2[i - 5]) : 0;
    char want[96], sql[256], *o = want;
    *o++ = '[';
    for (int i = 0; i < 8; i++) { if (i) { *o++ = ','; *o++ = ' '; } o = snum(o, v[i]); }
    *o++ = ']'; *o = 0;
    const char *tbl = fx ? "fx_types" : "synth_types";
    o = scat(scat(scat(scat(sql, "SELECT default_params FROM "), tbl), " WHERE type='"), name); scat(o, "'");
    kn_dstr s; dstr_init(&s);
    DB_GET(&s, db, sql);
    const char *have = s.cap > 15 ? s.u.ptr : s.u.buf;
    int exists = s.size != 0, same = exists && s.size == (uint32_t)slen(want);
    for (uint32_t i = 0; same && i < s.size; i++) same = have[i] == want[i];
    if (s.cap > 15 && s.u.ptr) OP_DELETE(s.u.ptr);
    if (same) return;
    if (exists) {
        o = scat(scat(scat(scat(sql, "UPDATE "), tbl), " SET default_params='"), want);
        o = scat(scat(o, "' WHERE type='"), name); scat(o, "'");
    } else {
        o = scat(scat(scat(sql, "INSERT INTO "), tbl), " SELECT MAX(id)+1,'");
        o = scat(scat(scat(o, name), "','"), want);
        o = scat(scat(o, "' FROM "), tbl);
    }
    op1_db_write(db, sql);
}
static void row_delete(void *db, char kind, const char *name)
{
    char sql[96];
    char *o = scat(scat(sql, kind == 'f' ? "DELETE FROM fx_types" : "DELETE FROM synth_types"), " WHERE type='");
    scat(scat(o, name), "'");
    op1_db_write(db, sql);
}

static void sync_caches(uint32_t lo, uint32_t hi)
{
    for (uint32_t a = lo & ~31u; a < hi; a += 32) __asm__ volatile("flush [%0];" : : "a"(a) : "memory");
    __asm__ volatile("ssync;" : : : "memory");
    for (uint32_t a = lo & ~31u; a < hi; a += 32) __asm__ volatile("iflush [%0];" : : "a"(a) : "memory");
    __asm__ volatile("csync;" : : : "memory");
}

static int append(const char **list, const char *name)          /* 0-terminated list with room for every number */
{
    int i = 0;
    while (list[i]) { if (seq(list[i], name)) return 0; i++; }
    if (i >= OP1_MAX_ORDER) return -1;
    list[i] = name; list[i + 1] = 0;
    return 0;
}

/* register a relocated engine / FX at its number; svg: screen file path relative to content/display, or 0 */
int kn_plugin_off(const char *name);
/* a plugin's id is its file name without .knl / .op1p (docs/plugin-format.md "Names"): kennel.off and the inventory
 * go by it, never by the saved (type) name */
static void file_id(const char *file, char id[40])
{
    copy_str(id, file, 40);
    int n = slen(id), e = ends_with(id, ".op1p") ? 5 : ends_with(id, ".knl") ? 4 : 0;
    id[n - e] = 0;
}
static int off_by_file(const char *file) { char id[40]; file_id(file, id); return kn_plugin_off(id); }
static int valid_file(const char *file)
{
    char id[40]; file_id(file, id);
    int n = slen(id);
    if (n < 1 || n > 15) return 0;
    for (int i = 0; i < n; i++)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') || id[i] == '_' || id[i] == '-')) return 0;
    return 1;
}
static int svg_path(char out[48], int plugins, const char *file)
{
    const char *prefix = plugins ? "../../plugins/" : "../";
    if (slen(prefix) + slen(file) >= 48) return 0;
    scat(scat(out, prefix), file);
    return 1;
}
static const char *register_plugin(int fx, const char *type_name, uint32_t entry, uint32_t screen, const char *svg,
                                   int uses_sample, int pref, const int16_t *def, const int16_t *page2, op1_fx_stereo_render_t stereo,
                                   int coarse)
{
    for (int k = 1; k < OP1_SLOTS; k++) {                      /* a file with this name is already registered */
        const char *nm = fx ? op1_fx_order_names[k - 1] : op1_engine_names[k - 1];
        if (from_file[fx][k] && nm && seq(nm, type_name)) return "another file has the same name";
    }
    int n = builtin_number(fx, type_name), replaces = n != 0;
    if (replaces && kn_inv_cur) kn_inv_cur->flags |= KN_INV_REPLACES;
    if (!n) n = number_for(fx, type_name, pref);
    if (!n) return "no free plugin number";
    if (from_file[fx][n]) return "another file has the same name";
    char *name = kn_alloc(16), *path = svg ? kn_alloc(48) : 0;  /* in the arena: no .bss for 64 numbers x 2 */
    if (!name || (svg && !path)) return "plugin memory full";
    copy_str(name, type_name, 16);
    if (svg) copy_str(path, svg, 48);
    const op1_screen_t *scr = svg && screen != KN_NONE ? (const op1_screen_t *)screen : 0;
    if (fx) {
        if (append(op1_fx_names, name)) return "FX name list full";
        op1_fx_order_names[n - 1] = name;
        op1_fx_screens[n] = scr;
        op1_fx_svgs[n] = scr ? path : 0;
        op1_fxs[n] = (const op1_fx_t *)entry;
        op1_fx_stereo_renders[n] = stereo;             /* replacement by mono/.op1p must clear it */
    } else {
        if (uses_sample && append(op1_sample_engine_names, name)) return "sample engine list full";
        op1_engine_names[n - 1] = name;
        op1_engine_screens[n] = scr;
        op1_engine_svgs[n] = scr ? path : 0;
        op1_engines[n] = (const op1_engine_t *)entry;
        op1_engine_coarse[n] = (uint8_t)coarse;        /* a replacement without "coarse" must clear it */
    }
    from_file[fx][n] = 1;
    if (!replaces) row_ensure(DB_FACTORY(), fx, name, def, page2, n);   /* a replaced built-in keeps its row */
    return 0;
}

static uint32_t alloc(uint32_t size, uint32_t align)
{
    uint32_t base = (kn_next + align - 1) & ~(align - 1);
    if (base < kn_next || base >= kn_arena_hi || kn_arena_hi - base < size) return 0;
    kn_next = base + size;
    return base;
}

/* R246's heap 0 (docs/firmware/clocks-memory.md 5.4): malloc / free (heap index 0; op1r's plugload uses the same two),
 * and the free bytes the way the resource monitor's RAM row counts them (screens/sysmon.c sample_ram): the heap's
 * size minus what its bump pointer has handed out, plus its free list; under the scheduler lock malloc and free take */
#define MALLOC       ((void *(*)(uint32_t))0x01A9F348)
#define FREE         ((void (*)(void *))0x01AA0040)
#define HEAP_LOOKUP  ((uint32_t *(*)(int))0x01A9F2E2)            /* heap descriptor: base, size, ?, bump; free list +0x18 */
#define LOCK_SCHED   ((void (*)(void))0x01A9F76C)
#define UNLOCK_SCHED ((void (*)(void))0x01A9F78E)
uint32_t kn_heap0_free(void)
{
    LOCK_SCHED();
    const uint32_t *h = HEAP_LOOKUP(0);
    uint32_t fr = 0;
    if (h) {
        uint32_t used = h[3] - h[0], fl = 0;
        const uint32_t *b = (const uint32_t *)h[0x18 / 4];
        for (int n = 0; b && n < 8192; n++) { fl += b[0]; b = (const uint32_t *)b[2]; }   /* {size, heap, next} */
        used = used > fl ? used - fl : 0;
        fr = h[1] > used ? h[1] - used : 0;
    }
    UNLOCK_SCHED();
    return fr;
}

/* the arena into heap 0 (kn_mem.h): `size` bytes (arena_need) on a KN_ARENA_ALIGN boundary, taken once and never
 * given back (the plugins live until power-off). The fixed range stays when malloc fails, returns memory outside heap
 * 0, or something took arena memory before (never so far). Either way the boot goes on. */
static void arena_init(uint32_t size)
{
    kn_heap0_free_boot = kn_heap0_free();
    if (kn_next != kn_arena_lo || !size || size > KN_ARENA_MAX) return;
    const uint32_t raw = (uint32_t)MALLOC(size + KN_ARENA_ALIGN);
    if (!raw) return;
    const uint32_t lo = (raw + KN_ARENA_ALIGN - 1) & ~(KN_ARENA_ALIGN - 1);
    if (raw < KN_HEAP0_LO || lo < raw || lo > KN_HEAP0_HI - size) { FREE((void *)raw); return; }
    kn_arena_lo = kn_next = lo;
    kn_arena_hi = lo + size;
    kn_arena_heap0 = 1;
}

/* zeroed memory from the arena, for core tables that grow with the plugin numbers (dispatcher.c screens) */
void *kn_alloc(uint32_t size)
{
    if (!size || size > kn_arena_hi - kn_arena_lo - 3) return 0;
    uint32_t a = alloc((size + 3) & ~3u, 4);
    if (a) for (uint32_t i = 0; i < size; i += 4) *(volatile uint32_t *)(a + i) = 0;
    return (void *)a;
}

/* a refused file gives back its arena memory and its pool reservation */
static void release(uint32_t save) { kn_next = save; decl_used = rs_used; n_grants = rs_n; }

/* reserve a declared buffer (granted after the pool is cleared): min..max, at most KN_DECL_MAX, first come first
 * served in load order. 0 = ok (size may be 0 when min is 0: the plugin goes without), else the refusal */
static const char *reserve(uint32_t desc, uint32_t min, uint32_t max)
{
    uint32_t room = KN_DECL_HI - KN_DECL_LO - decl_used, want = max > KN_DECL_MAX ? KN_DECL_MAX : max;
    uint32_t got = want <= room ? want : room & ~(KN_DECL_ALIGN - 1);
    if (got < min) return "plugin buffer memory full";
    if (n_grants >= (int)(sizeof grants / sizeof grants[0])) return "plugin buffer memory full";
    grants[n_grants].desc = desc; grants[n_grants].off = decl_used; grants[n_grants].size = got; n_grants++;
    decl_used += (got + KN_DECL_ALIGN - 1) & ~(KN_DECL_ALIGN - 1);
    return 0;
}

/* .knl (kennel): engines, effects, features */
static const char *load_knl(int in_plugins, const char *file, const uint8_t *buf, uint32_t len)
{
    kn_info in;
    const char *err = 0;
    rs_used = decl_used; rs_n = n_grants;
    if (kn_parse(buf, len, OP1_PLUGIN_API, &in, &err)) return err;
    char canonical[40], saved_name[16]; file_id(file, canonical);
    if (seq(canonical, "drumplus")) copy_str(saved_name, "drum+", 16);
    else {
        int i = 0;
        for (; canonical[i] && i < 15; i++) saved_name[i] = canonical[i] == '_' ? ' ' : canonical[i];
        saved_name[i] = 0;
    }
    if (!seq(saved_name, in.type_name)) return "saved name differs from plugin id";
    kn_inv_rec *rec = kn_inv_cur;                                 /* inventory: what the file says */
    if (rec) {
        rec->kind = in.kind; rec->crc = in.crc;
        rec->author = kn_inv_str(in.author, 32); rec->version = kn_inv_str(in.version, 24);
        rec->claims = kn_inv_list(in.claims, 160);
    }
    if (in.svg && svg_err(buf, len)) return svg_err(buf, len);
    if (off_by_file(file)) return "switched off";                /* by id, before the memory is taken: no arena */
    /* image + .bss, then room for its call stubs into the core (kn_load.h); what the stubs did not need goes back */
    uint32_t save = kn_next, room = in.load_size + kn_stub_room(&in), used = 0;
    uint32_t base = room < in.load_size ? 0 : alloc(room, in.image_align);
    if (!base) return "plugin memory full";
    if (kn_relocate_far(buf, &in, (uint8_t *)base, base, room, kn_exports, kn_export_count, &err, &used)) { release(save); return err; }
    kn_next = base + used;
    if (kn_validate((const uint8_t *)base, base, in.image_size, in.text_size, in.kind,
                    (in.flags & KN_FLAG_STEREO ? 1 : 0) | ((in.flags & KN_FLAG_INPUT) ? 2 : 0), in.entry_offset, in.screen_offset, &err)) { release(save); return err; }
    sync_caches(base, base + used);
    if ((in.flags & KN_FLAG_BIGBUF) && (err = reserve(base + in.bigbuf_offset, in.bigbuf_min, in.bigbuf_max))) {
        release(save); return err;
    }
    img_cache_user = (uint8_t)kn_imports(buf, &in, "_op1_screen_draw_cached");
    img_cur.base = base; img_cur.load_size = in.load_size; img_cur.image_size = in.image_size;
    img_cur.text_size = in.text_size; img_cur.crc = in.crc; img_cur.kind = in.kind;
    if (in.kind == KN_KIND_FEATURE) {
        const op1_feature_t *f = (const op1_feature_t *)(base + in.entry_offset);
        char id[40]; file_id(file, id);
        if (!seq(f->name, id)) { release(save); return "feature name differs from plugin id"; }
        if (f->api == OP1_FEATURE_API && f->seq) {
            if (!f->seq->name || !seq(f->seq->name, in.type_name)) { release(save); return "sequencer name differs from plugin id"; }
            if (in.svg) {
                char svg[48];
                if (!svg_path(svg, in_plugins, file)) { release(save); return "plugin filename too long"; }
                f = kn_seq_asset(f, svg, kn_alloc);
                if (!f) { release(save); return "plugin memory full"; }
            }
        }
        int r = f->api != OP1_FEATURE_API ? 1 : kn_feature_register(f);
        if (!r && f->drum_synth && in.svg) {
            char path[48];
            if (svg_path(path, in_plugins, file)) kn_ds_asset(f->drum_synth, path);
        }
        if (!r && f->seq) kn_seq_row_ensure(DB_FACTORY(), in.type_name, DB_EXEC, op1_db_has);
        if (r) {                                                  /* not registered: its memory back, and the */
            release(save);                                        /* inventory forgets the pointer into it */
            if (kn_inv_cur && kn_inv_cur->f == f) kn_inv_cur->f = 0;
        }
        return f->api != OP1_FEATURE_API ? "built for a different kennel core" : r == -2 ? "switched off"
               : r == -4 ? "shadowed by earlier provider"
               : r ? "too many features" : 0;
    }
    char svg[48];
    const char *path = 0;
    if (in.svg) {
        if (!svg_path(svg, in_plugins, file)) { release(save); return "plugin filename too long"; }
        path = svg;
    }
    err = register_plugin(in.kind == KN_KIND_FX, in.type_name, base + in.entry_offset,
                          in.screen_offset == KN_NONE ? KN_NONE : base + in.screen_offset, path,
                          in.flags & KN_FLAG_USES_SAMPLE, in.legacy_order, in.defaults,
                          in.flags & KN_FLAG_PAGE2 ? in.page2 : 0,
                          in.flags & KN_FLAG_STEREO ? ((const op1_fx_stereo_t *)(base + in.entry_offset))->render_stereo : 0,
                          KN_COARSE(in.flags));
    if (!err && in.kind == KN_KIND_SYNTH) {
        for(int n=1;n<OP1_SLOTS;n++) if(op1_engines[n]==(const op1_engine_t *)(base+in.entry_offset))
            op1_input_engines[n]=(in.flags & KN_FLAG_INPUT)?(const op1_input_engine_t *)(base+in.entry_offset):0;
    }
    if (err) release(save);                                       /* refused (no number, same name): memory back */
    return err;
}

/* .op1p (op1r 34): engines and effects; imports by name without the C underscore; absolute calls (any address).
 * op1r 42's OP1P_F_STEREO FX: the 16-byte op1_fx_t has render_stereo at +12, the layout of kennel's op1_fx_stereo_t;
 * checked like a .knl stereo FX (every callback inside the image) and registered into op1_fx_stereo_renders. */
static const char *load_op1p(int in_plugins, const char *file, const uint8_t *buf, uint32_t len)
{
    static const char *const perr[] = { "unreadable", "size", "magic", "CRC mismatch", "version", "header",
                                        "relocation", "import", "unknown or misplaced header flags" };
    op1p_view v;
    int r = op1p_parse(buf, len, &v);
    if (r) return perr[r < 9 ? r : 0];
    if (kn_inv_cur) {                                             /* inventory: name, kind, the trailer's CRC */
        kn_inv_cur->kind = v.hdr->kind == OP1P_FX ? KN_KIND_FX : KN_KIND_SYNTH;
        const uint8_t *t = buf + len - sizeof(op1p_trailer);
        kn_inv_cur->crc = (uint32_t)t[12] | (uint32_t)t[13] << 8 | (uint32_t)t[14] << 16 | (uint32_t)t[15] << 24;
    }
    if (v.svg_len && svg_err(buf, v.svg_len)) return svg_err(buf, v.svg_len);
    uint32_t imp[OP1P_MAX_IMPORTS];
    for (uint32_t i = 0; i < v.hdr->n_imports; i++) {
        imp[i] = 0;
        for (int k = 0; k < kn_export_count; k++)
            if (seq(kn_exports[k].name + 1, v.import[i])) imp[i] = kn_exports[k].addr;
        if (!imp[i]) return "an import is not exported by the core";
    }
    if (off_by_file(file)) return "switched off";                /* by id, before the memory is taken */
    uint32_t save = kn_next, base = alloc(v.mem_size, 4);
    if (!base) return "plugin memory full";
    op1p_relocate(&v, (uint8_t *)base, base, imp);
    const char *validation_error = 0;
    uint32_t stereo;                                              /* op1r 42 stereo FX: relocated render_stereo */
    if (op1p_check(&v, (const uint8_t *)base, base, &stereo, &validation_error)) { kn_next = save; return validation_error; }
    sync_caches(base, base + v.mem_size);
    const uint8_t *t = buf + len - sizeof(op1p_trailer);
    img_cur.base = base; img_cur.load_size = v.mem_size; img_cur.image_size = img_cur.text_size = v.hdr->image_size;
    img_cur.crc = (uint32_t)t[12] | (uint32_t)t[13] << 8 | (uint32_t)t[14] << 16 | (uint32_t)t[15] << 24;
    img_cur.kind = v.hdr->kind == OP1P_FX ? KN_KIND_FX : KN_KIND_SYNTH;
    char svg[48];
    if (v.svg_len && !svg_path(svg, in_plugins, file)) { kn_next = save; return "plugin filename too long"; }
    int pref = OP1P_PREF(v.hdr->flags);
    const char *err = register_plugin(v.hdr->kind == OP1P_FX, v.hdr->name, base + v.hdr->entry_off,
                                      v.hdr->screen_off == OP1P_NO_SCREEN ? KN_NONE : base + v.hdr->screen_off,
                                      v.svg_len ? svg : 0, v.hdr->flags & OP1P_F_USES_SAMPLE,
                                      pref < OP1_FILE_FIRST ? pref : 0, v.hdr->defaults, v.hdr->page2_defaults,
                                      (op1_fx_stereo_render_t)stereo,
                                      v.hdr->kind == OP1P_SYNTH ? OP1P_COARSE(v.hdr->flags) : 0);   /* FX: ignored, as op1r */
    if (err) kn_next = save;
    return err;
}

static char files[MAX_FILES][40] kn_farbss;
static uint8_t file_in_plugins[MAX_FILES];
static int n_files, n_over;                                      /* listed; past MAX_FILES (inventory only, one log line) */
/* Files read whole into the pool's preload area (kn_mem.h KN_PRELOAD_LO..HI) while the arena is sized: load_file then
 * takes them from there instead of reading them again. 0 length: not preloaded (switched off, too big, no room, a
 * failed read): load_file reads it into KN_STAGE as before. */
static uint32_t preload_at[MAX_FILES] kn_farbss, preload_len[MAX_FILES] kn_farbss;
static uint32_t preload_next = KN_PRELOAD_LO;

static void load_file(int i)
{
    const char *file = files[i];
    char path[64];
    scat(scat(scat(path, file_in_plugins[i] ? DIR_PLUGINS : DIR_CONTENT), "/"), file);
    const char *err = 0;
    uint8_t *buf = (uint8_t *)KN_STAGE;
    { volatile uint8_t *z = (volatile uint8_t *)&img_cur; for (unsigned k = 0; k < sizeof img_cur; k++) z[k] = 0; }
    img_cache_user = 0;
    scat(scat(scat(path, file_in_plugins[i] ? "plugins" : "content"), "/"), file);
    kn_inv_cur = kn_inv_add(ends_with(file, ".op1p") ? KN_INV_OP1P : KN_INV_KNL, path);
    if (kn_inv_cur) { char id[40]; file_id(file, id); copy_str(kn_inv_cur->name, id, 16); }   /* the inventory name: the id */
    scat(scat(scat(path, file_in_plugins[i] ? DIR_PLUGINS : DIR_CONTENT), "/"), file);
    int fd = valid_file(file) && !preload_len[i] ? Y_OPEN(path, 0, 0) : -1;
    if (!valid_file(file)) err = "invalid plugin filename (id must be 1..15 lowercase letters, digits, _ or -)";
    else if (preload_len[i]) {                                   /* read while the arena was sized (peek_file) */
        buf = (uint8_t *)preload_at[i];
        if (kn_inv_cur) kn_inv_cur->size = preload_len[i];
        img_cur.file_size = preload_len[i];
        err = ends_with(file, ".op1p") ? load_op1p(file_in_plugins[i], file, buf, preload_len[i])
                                       : load_knl(file_in_plugins[i], file, buf, preload_len[i]);
    }
    else if (fd < 0) err = "cannot open";
    else {
        int got = Y_READ(fd, buf, KN_FILE_MAX), more = 0;
        if (got == (int)KN_FILE_MAX) { uint8_t b; more = Y_READ(fd, &b, 1) > 0; }
        Y_CLOSE(fd);
        if (kn_inv_cur && got > 0) kn_inv_cur->size = (uint32_t)got;
        if (got > 0) img_cur.file_size = (uint32_t)got;
        if (got <= 0) err = "cannot read";
        else if (more) err = "file larger than 1 MB";
        else err = ends_with(file, ".op1p") ? load_op1p(file_in_plugins[i], file, buf, (uint32_t)got)
                                            : load_knl(file_in_plugins[i], file, buf, (uint32_t)got);
    }
    if (err && !seq(err, "switched off") && off_by_file(file))    /* broken, but switched off by the user: off, */
        err = "switched off";                                     /* not a failure, whatever is wrong with it */
    int shadowed = err && (seq(err, "shadowed by earlier provider") || seq(err, "another file has the same name"));
    if (err) {                                                    /* switched off or shadowed: not a broken file */
        if (!seq(err, "switched off") && !shadowed) kn_failed++;
        log_line(file, err);
    }
    else {
        kn_loaded++; log_line(file, "loaded");
        if (img_cache_user) artcache_users++;
        if (img_cur.base && kn_images && kn_images->count < kn_images->cap) {   /* registered: memory and record stay */
            img_cur.format = ends_with(file, ".op1p") ? KN_IMG_OP1P : KN_IMG_KNL;
            img_cur.dir = file_in_plugins[i] ? 0 : 1;
            char id[40]; file_id(file, id); copy_str(img_cur.id, id, 16);
            const uint32_t *src = (const uint32_t *)&img_cur;    /* word copy: no memcpy in the firmware */
            volatile uint32_t *dst = (volatile uint32_t *)&kn_images->rec[kn_images->count];
            for (unsigned k = 0; k < sizeof img_cur / 4; k++) dst[k] = src[k];
            kn_images->count++;
        }
    }
    if (kn_inv_cur) {
        kn_inv_cur->state = !err ? KN_INV_LOADED : (seq(err, "switched off") || shadowed) ? KN_INV_OFF : KN_INV_FAILED;
        kn_inv_cur->reason = err;
    }
    kn_inv_cur = 0;
}

static void list_dir(const char *dir, int plugins)                 /* .knl and .op1p */
{
    void *d = Y_OPENDIR(dir);
    if (!d) return;
    const uint8_t *e;
    while ((e = Y_READDIR(d))) {                                  /* names first: no file I/O while the dir is open */
        const char *s = (const char *)e + 11;
        if (s[0] == '.' || slen(s) >= 40 || !(ends_with(s, ".knl") || ends_with(s, ".op1p"))) continue;
        if (n_files >= MAX_FILES) {                               /* never silent: logged, in the inventory */
            char p[64];
            scat(scat(scat(p, plugins ? "plugins" : "content"), "/"), s);
            kn_inv_rec *r = kn_inv_add(ends_with(s, ".op1p") ? KN_INV_OP1P : KN_INV_KNL, p);
            if (r) { r->state = KN_INV_FAILED; r->reason = "more than 128 plugin files"; }
            n_over++;                                             /* one log line for all of them (load_all) */
            kn_failed++;
            continue;
        }
        copy_str(files[n_files], s, 40);
        file_in_plugins[n_files++] = (uint8_t)plugins;
    }
    Y_CLOSEDIR(d);
}

static void list_files(void)
{
    list_dir(DIR_PLUGINS, 1);                                     /* user files first: they win over shipped ones */
    list_dir(DIR_CONTENT, 0);
    /* Stable provider order within each directory; user providers always precede shipped ones. */
    for (int i = 1; i < n_files; i++)
        for (int j = i; j > 0 && file_in_plugins[j] == file_in_plugins[j - 1]; j--) {
            const char *a = files[j - 1], *b = files[j];
            while (*a && *a == *b) { a++; b++; }
            if ((unsigned char)*a <= (unsigned char)*b) break;
            char tmp[40]; copy_str(tmp, files[j], 40); copy_str(files[j], files[j - 1], 40); copy_str(files[j - 1], tmp, 40);
        }
}

/* ---- the arena's size, before anything is loaded (kn_mem.h KN_ARENA_MAX and the terms before it) ---- */
static int str_has(const uint8_t *s, uint32_t n, const char *name)  /* `name` is one of the table's strings */
{
    for (uint32_t i = 0; i < n; ) {
        const char *a = name; uint32_t k = i;
        while (k < n && *a && s[k] == (uint8_t)*a) { k++; a++; }
        if (!*a && k < n && !s[k]) return 1;
        while (i < n && s[i]) i++;
        i++;
    }
    return 0;
}
static uint32_t rd_le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
#define PEEK_FIRST 512u                                         /* the header (144 bytes) when there is no SVG */
#define PEEK_CHUNK 8192u                                        /* then through an SVG to its NUL */
static uint32_t op1p_need(const uint8_t *hdr)                   /* the .op1p header (4-byte aligned) */
{
    const op1p_hdr *h = (const op1p_hdr *)hdr;
    return h->magic == OP1P_MAGIC && h->image_size <= OP1P_MAX_IMAGE && h->bss_size <= OP1P_MAX_IMAGE
           ? h->image_size + h->bss_size + 3u : 0;   /* alloc(mem_size, 4) */
}
/* the most the loader takes for file i (0: it would be refused); *cache: it imports op1_screen_draw_cached. The file is
 * read whole into the preload area when it fits there; otherwise only its header (after a screen SVG: up to its NUL)
 * and string table are read, into KN_STAGE, and kn_arena_peek_bytes counts them (the loader reads them again). No
 * CRC, no relocation. A file whose header is refused or unreadable counts 0: load_file refuses it anyway. */
static uint32_t peek_file(int i, int *cache)
{
    char path[64];
    scat(scat(scat(path, file_in_plugins[i] ? DIR_PLUGINS : DIR_CONTENT), "/"), files[i]);
    int fd = Y_OPEN(path, 0, 0);
    if (fd < 0) return 0;
    const int op1p = ends_with(files[i], ".op1p");
    const int32_t size = Y_LSEEK(fd, 0, 2);
    uint8_t *buf = (uint8_t *)KN_STAGE;
    uint32_t need = 0, got = 0;
    if (size > 0 && (uint32_t)size <= KN_FILE_MAX && (uint32_t)size <= KN_PRELOAD_HI - preload_next &&
        Y_LSEEK(fd, 0, 0) == 0) {                                 /* whole, into the preload area */
        uint8_t *at = (uint8_t *)preload_next;
        int n = Y_READ(fd, at, (uint32_t)size);
        if (n == size) {
            preload_at[i] = preload_next; preload_len[i] = (uint32_t)size;
            preload_next += ((uint32_t)size + 3u) & ~3u;
            buf = at; got = (uint32_t)size;
        } else if (n > 0) kn_arena_peek_bytes += (uint32_t)n;
    }
    if (op1p) {                                                   /* trailer -> header: image + .bss */
        const uint32_t ts = sizeof(op1p_trailer), hs = sizeof(op1p_hdr);
        if (size >= (int32_t)(ts + hs) && (uint32_t)size <= KN_FILE_MAX) {
            if (got) {
                uint32_t off = rd_le32(buf + size - ts + 4);
                if (rd_le32(buf + size - ts) == OP1P_TMAGIC && !(off & 3) && off <= (uint32_t)size - ts - hs)
                    need = op1p_need(buf + off);
            } else if (Y_LSEEK(fd, size - (int32_t)ts, 0) >= 0 && Y_READ(fd, buf, ts) == (int)ts &&
                       rd_le32(buf) == OP1P_TMAGIC) {
                uint32_t off = rd_le32(buf + 4);
                kn_arena_peek_bytes += ts;
                if (!(off & 3) && off <= (uint32_t)size - ts - hs &&
                    Y_LSEEK(fd, (int32_t)off, 0) == (int32_t)off && Y_READ(fd, buf + ts, hs) == (int)hs) {
                    kn_arena_peek_bytes += hs;
                    need = op1p_need(buf + ts);
                }
            }
        }
        Y_CLOSE(fd);
        return need;
    }
    kn_peek_t pk;
    int r = got ? kn_peek(buf, got, OP1_PLUGIN_API, &pk) : 1;
    if (!got && Y_LSEEK(fd, 0, 0) == 0)
        while (r == 1 && got < KN_FILE_MAX) {                     /* the header, after the SVG if there is one */
            uint32_t want = got ? PEEK_CHUNK : PEEK_FIRST;
            int n = Y_READ(fd, buf + got, want < KN_FILE_MAX - got ? want : KN_FILE_MAX - got);
            if (n <= 0) break;
            got += (uint32_t)n;
            kn_arena_peek_bytes += (uint32_t)n;
            r = kn_peek(buf, got, OP1_PLUGIN_API, &pk);
        }
    if (!r) {
        need = pk.need;
        uint32_t so = pk.str_off, sn = pk.str_size;              /* import names: the static-art cache's users */
        if (sn && sn <= KN_FILE_MAX && so <= KN_FILE_MAX - sn) {
            if ((uint64_t)so + sn <= got) *cache |= str_has(buf + so, sn, "_op1_screen_draw_cached");
            else if (Y_LSEEK(fd, (int32_t)so, 0) == (int32_t)so && Y_READ(fd, (uint8_t *)KN_STAGE, sn) == (int)sn) {
                kn_arena_peek_bytes += sn;
                *cache |= str_has((const uint8_t *)KN_STAGE, sn, "_op1_screen_draw_cached");
            }
        }
    }
    Y_CLOSE(fd);
    return need;
}
/* the heap-0 block's size: the core's terms, KN_ARENA_PER_FILE per listed file, each enabled file's peek, the
 * static-art cache when a user is enabled (or compiled in), rounded up to KN_ARENA_ALIGN, at most KN_ARENA_MAX. A
 * file switched off (kennel.off), with a bad name, or all of them after a boot that did not reach the UI: only
 * KN_ARENA_PER_FILE (their log line and inventory record) */
static uint32_t arena_need(void)
{
    extern const uint8_t kn_artcache_builtin;
    uint32_t need = KN_ARENA_CORE + KN_ARENA_RUNTIME + KN_ARENA_MARGIN + (uint32_t)n_files * KN_ARENA_PER_FILE;
    int cache = kn_artcache_builtin;
    if (n_files && Y_ACCESS(MARKER, 0) != 0)                      /* (the marker: load_all skips every file) */
        for (int i = 0; i < n_files && need < KN_ARENA_MAX; i++) {
            if (!valid_file(files[i]) || off_by_file(files[i])) continue;
            uint32_t f = peek_file(i, &cache);
            need = f < KN_ARENA_MAX - need ? need + f : KN_ARENA_MAX;
        }
    if (cache && need < KN_ARENA_MAX) need += KN_ARTCACHE_BYTES;
    need = (need + KN_ARENA_ALIGN - 1) & ~(KN_ARENA_ALIGN - 1);
    return need < KN_ARENA_MAX ? need : KN_ARENA_MAX;
}

static void load_all(void)
{
    if (!n_files) return;
    {                                                             /* the log, sized to the files listed */
        uint32_t cap = KN_LOG_BASE + (uint32_t)n_files * KN_LOG_PER_FILE;
        char *p = kn_alloc(cap);
        if (p) {
            for (uint32_t k = 0; k <= kn_log_pos; k++) p[k] = kn_log[k];
            kn_log = p; kn_log_cap = cap;
        }
    }
    if (n_over) {
        char m[64];
        scat(snum(m, n_over), " files not loaded: more than 128 plugin files");
        log_line("plugin files", m);
    }
    if (Y_ACCESS(MARKER, 0) == 0) {                               /* the last boot with plugins did not reach the UI */
        Y_UNLINK(MARKER);
        kn_skipped = (uint32_t)n_files;
        log_line("all plugin files", "skipped: the last boot did not reach the UI");
        for (int i = 0; i < n_files; i++) {                       /* inventory: listed, not opened */
            char p[64];
            scat(scat(scat(p, file_in_plugins[i] ? "plugins" : "content"), "/"), files[i]);
            kn_inv_rec *r = kn_inv_add(ends_with(files[i], ".op1p") ? KN_INV_OP1P : KN_INV_KNL, p);
            if (r) { r->state = KN_INV_SKIPPED; r->reason = "the last boot did not reach the UI"; }
        }
        return;
    }
    Y_MKDIR(DIR_PLUGINS, 0x1FF);
    int fd = Y_OPEN(MARKER, O_CREATE, 0x180);
    if (fd >= 0) { Y_CLOSE(fd); marker_set = 1; }
    uint32_t c1;
    __asm__ volatile("%0 = CYCLES; %1 = CYCLES2;" : "=d"(c1), "=d"(marker_c2));
    (void)c1;
    int n_on = 0;                                                 /* a record per file kennel.off leaves on */
    for (int i = 0; i < n_files; i++) n_on += !off_by_file(files[i]);
    kn_images = n_on ? kn_alloc(sizeof(kn_img_map) + (uint32_t)n_on * sizeof(kn_img_rec)) : 0;   /* 0: no map */
    if (kn_images) {
        kn_images->magic = KN_IMG_MAGIC; kn_images->version = KN_IMG_VERSION;
        kn_images->rec_size = sizeof(kn_img_rec); kn_images->cap = (uint32_t)n_on;
    }
    void *db = DB_HANDLE();
    map_load(db);
    for (int i = 0; i < n_files; i++) load_file(i);
    for (int i = 0; i < map_n; i++) {                             /* file plugins gone since the last boot: drop rows */
        if (!map[i].present) continue;
        int fx = map[i].kind == 'f';
        if (!from_file[fx][map[i].n]) { row_delete(DB_FACTORY(), map[i].kind, map[i].name); map[i].present = 0; map_dirty = 1; }
    }
    if (map_dirty) map_save(db);
}

/* the pool, once per boot, after the last file was staged in it and before anything is handed out: word stores
 * (SDRAM outside the LDR keeps its contents over a watchdog reset; the emulator starts zeroed and can't tell) */
static void pool_clear(void)
{
    uint32_t c0, c1, t;
    __asm__ volatile("%0 = CYCLES; %1 = CYCLES2;" : "=d"(c0), "=d"(t));
    for (volatile uint32_t *p = (volatile uint32_t *)KN_POOL_LO; p < (volatile uint32_t *)KN_POOL_HI; p += 8) {
        p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0; p[6] = 0; p[7] = 0;
    }
    __asm__ volatile("%0 = CYCLES; %1 = CYCLES2;" : "=d"(c1), "=d"(t));
    kn_pool_clear_cycles = c1 - c0;
    for (int i = 0; i < n_grants; i++) {                          /* declared buffers: pointer and size, load order */
        volatile uint32_t *d = (volatile uint32_t *)grants[i].desc;
        d[4] = grants[i].size ? KN_DECL_LO + grants[i].off : 0;
        d[5] = grants[i].size;
    }
    kn_decl_used = decl_used;
    kn_pool_ready = 1;
}

/* boot: CALL Clips::load at 0x019F02EE (hook.S hook_knboot) */
/* boot hook timing for a debugger / the emulator: core cycles (CYCLES2:CYCLES) from the hook's start to after the arena
 * is sized and taken, and to its end (the emulator's cycles are its own, not the unit's) */
volatile uint64_t kn_boot_cycles_sized, kn_boot_cycles_total;
static uint64_t cycles64(void)
{
    uint32_t lo, hi;
    __asm__ volatile("%0 = CYCLES; %1 = CYCLES2;" : "=d"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}
void kn_boot_hook(void *clips)
{
    const uint64_t t0 = cycles64();
    list_files();                                                 /* plugins/ and content/, names only */
    arena_init(kn_arena_need = arena_need());                     /* the code arena into heap 0, sized (kn_mem.h) */
    kn_boot_cycles_sized = cycles64() - t0;
#if KN_DIAG
    extern void kd_init(void);
    kd_init();                                                    /* diagnostic counters on (kn_diag.c, monitor page 7, test build) */
#endif
    extern void kn_l1_init(void);
    kn_l1_init();                                                 /* the L1 pool from the Lander's sounds (kn_l1.c) */
    CLIPS_LOAD(clips);
    op1_fx_stereo_init();
    load_all();
    pool_clear();                                                 /* before any plugin code runs (features, audio) */
    for (int fx = 0; fx < 2; fx++)                                /* inventory: the compiled-in engines and effects */
        for (int n = 1; n < OP1_SLOTS; n++) {
            const char *nm = fx ? op1_fx_order_names[n - 1] : op1_engine_names[n - 1];
            if (!taken(fx, n) || from_file[fx][n] || !nm) continue;
            kn_inv_rec *r = kn_inv_add(KN_INV_BUILTIN, 0);
            if (r) { r->kind = fx ? KN_KIND_FX : KN_KIND_SYNTH; copy_str(r->name, nm, 16); }
        }
    kn_features_finish();                                         /* built-in and loaded features: order fixed now */
    const char *seq_names[48]; unsigned seq_count = 0;
    for (int i = 0; i < kn_feature_count() && seq_count < 48; i++) {
        const op1_feature_t *f = kn_feature_at(i);
        if (f->seq) seq_names[seq_count++] = f->seq->name;
    }
    kn_seq_rows_cleanup(DB_FACTORY(), seq_names, seq_count, DB_EXEC, op1_db_has);
    extern void kn_artcache_init(int users);
    kn_artcache_init(artcache_users);
    kn_heap0_free_loaded = kn_heap0_free();
    kn_boot_cycles_total = cycles64() - t0;
}

/* main loop (ci_service): remove the crash marker once the UI has run two CYCLES2 periods, as op1r. CYCLES2 counts
 * 2^32 core cycles, 10.7 s at 400 MHz, so that is 10.7..21.5 s after loading, depending on where CYCLES stood. */
void kn_boot_service(void)
{
    ui_ran = 1;
    if (!marker_set) return;
    uint32_t c1, c2;
    __asm__ volatile("%0 = CYCLES; %1 = CYCLES2;" : "=d"(c1), "=d"(c2));
    (void)c1;
    if (c2 - marker_c2 >= 2) { Y_UNLINK(MARKER); marker_set = 0; }
}

/* main loop: R246 calls the yaffs background GC (0x01A9AC48, R0 = the device; its result is not used) on every pass that
 * did no other work (CALL at 0x019F0C46, hook.S hook_gcrate), a few hundred times a second. op1r 49 runs it at most once
 * per 40,000,000 core cycles (0.1 s at 400 MHz): the same here, which keeps the GC's code and data out of the caches the
 * audio callback shares. CYCLES is 32 bits; the unsigned difference survives its wrap (10.7 s). The first call after
 * boot runs the GC. */
#define YAFFS_GC ((int (*)(void *))0x01A9AC48)
#define GC_PERIOD 40000000u
static uint32_t gc_last;
static uint8_t gc_ran;
void kn_gc_rate(void *dev)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    if (gc_ran && c - gc_last < GC_PERIOD) return;
    gc_last = c; gc_ran = 1;
    YAFFS_GC(dev);
}

/* power-off and restart: R246's shutdown CALL yaffs unmount 0x019964B4 at 0x019F0BEA (hook.S hook_knunmount), after
 * the tape write-back. A start that reached the UI and is switched off or restarted normally is no crash, so the
 * marker goes before the unmount; switching off soon after the tape screen appears no longer skips every file on
 * the next boot. A hang before the main loop never gets here and keeps the marker. R0 is 0 here (the write-back
 * loop ends on R0 == 0); it is passed on unchanged. */
#define YAFFS_UNMOUNT ((int (*)(int))0x019964B4)
int kn_boot_unmount(int arg)
{
    extern void bgs_flush_sync(void);
    bgs_flush_sync();                                             /* patch saves noted after the worker stopped (bgsave.c) */
    if (marker_set && ui_ran) { Y_UNLINK(MARKER); marker_set = 0; }
    return YAFFS_UNMOUNT(arg);
}

/* boot: CALL MasterController::load at 0x019F054A (hook.S hook_knmaster). The master FX comes from the database, not
 * through the patch parser: after the stock load, set its selector word by the FX's type name (as op1r). */
void kn_master_load(void)
{
    MASTER_LOAD();
    kn_dstr s; dstr_init(&s);
    MASTER_FX_TYPE(&s);
    const char *p = s.cap > 15 ? s.u.ptr : s.u.buf;
    int n = 0;
    if (s.size && s.size < 16) {
        char nm[16];
        for (uint32_t i = 0; i < s.size; i++) nm[i] = p[i];
        nm[s.size] = 0;
        for (int k = 1; k < OP1_SLOTS && !n; k++)
            if (op1_fxs[k] && op1_fx_order_names[k - 1] && seq(op1_fx_order_names[k - 1], nm)) n = k;
    }
    if (s.cap > 15 && s.u.ptr) OP_DELETE(s.u.ptr);
    if (n) ((volatile int16_t *)0xFF8016C4)[4] = (int16_t)(OP1_ENGINE_MAGIC + n);
}
