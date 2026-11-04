/*
 * coop-fw plugin host (Phase 4: P1 loader, P2 FX positions). Loads the `.coop` files of /yaffs2/plugins at boot and
 * hosts FX plugins at the stock FX positions (0 synth, 1 drum, 2 master) the way kennel 3.1 does (dispatcher.c,
 * kn_boot.c): the stock "filter" FX type (type 5) is hijacked, so every plugin appears in the stock FX list under its
 * own name (a `fx_types` row in the factory database, written at boot), resolves to the filter type (the name-compare
 * hook), and carries its number in the slot's fifth knob word (`params[4] = 0x5300 + n`). The filter object's vtable
 * entries (render, init; mono 0x01910C94, stereo master 0x01910CB4) point at the dispatchers below, which run the
 * stock filter when the selector names no plugin.
 *
 * Numbers are stable across boots: a name -> number map persisted in the user database (table coopplugins, as
 * kennel's op1rplugins), files take 32..63. Positions 0/1 are mono in stock: a plugin there gets L = R = the
 * instrument's mono sum and may return a stereo result, whose mono goes on down the stock path and whose
 * difference is added to coop's side buses for the mix (docs/plugins.md, P2 limits). Position 2 is true stereo.
 *
 * Boot (main thread, from the tape mount hook): list the directory (names first, no I/O while it is open), then for
 * each file read it whole into a staging block from heap 0, parse and check it (plugin_load.cc), relocate it into its
 * own heap-0 block (never freed), check the entry object, number it, register it. A failure skips the file with a
 * reason kept for the stats; the boot goes on.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_plugin.h"
#include "plugin_load.h"
#include "coop_plugin_host.h"
#include "coop_fixed.h"
#include "coop_cost.h"

#define Y_OPENDIR  ((void *(*)(const char *))0x01A9B450u)               /* kennel kn_boot.c */
#define Y_READDIR  ((const uint8_t *(*)(void *))0x01A9B648u)             /* dirent: +0x0B d_name */
#define Y_CLOSEDIR ((int (*)(void *))0x01A9B74Cu)
#define Y_OPEN     ((int (*)(const char *, int, int))0x01A997F4u)
#define Y_CLOSE    ((int (*)(int))0x01A99908u)
#define Y_READ     ((int (*)(int, void *, int))0x01A99C90u)
#define MALLOC     ((void *(*)(uint32_t))0x01A9F348u)                   /* heap 0 */
#define FREE       ((void (*)(void *))0x01AA0040u)
#define OP_DELETE  ((void (*)(void *))0x01A5E310u)                      /* operator delete (dstring heap text) */
#define DB_HANDLE  ((void *(*)(void))0x0196DD44u)                       /* user/op1.db: the number map */
#define DB_FACTORY ((void *(*)(void))0x0196DD38u)                       /* content/op1_factory.db: fx_types */
#define DB_EXEC    ((void (*)(void *, const char *))0x0196F950u)
#define DB_GET     ((void (*)(void *, void *, const char *))0x0196FBD0u) /* (dstr out, db, sql): first column as text */
#define SQLITE_EXEC ((int (*)(void *, const char *, void *, void *, char **))0x019C9460u)
#define DB_SEM     (*(void *volatile *)0x01BAB718u)                      /* db2 statement lock */
#define SEM_TAKE   ((void (*)(void *, int))0xFFA0B6E8u)
#define SEM_GIVE   ((void (*)(void *))0xFFA0B96Cu)
#define FILTER_RENDER    ((void (*)(void *, int16_t *, const int16_t *))0x019E7934u)          /* stock filter, mono */
#define FILTER_INIT      ((void (*)(void *, int))0x019E780Au)
#define FILTER_ST_RENDER ((void (*)(void *, int16_t *, int16_t *, const int16_t *))0x019E7EE4u)   /* master, stereo */
#define FILTER_ST_INIT   ((void (*)(void *))0x019E78D0u)
#define MASTER_FX_TYPE   (*(volatile int *)0xFF801090u)                 /* 5 = the master runs the filter type */
#define RANGE_TABLE      ((const int16_t *)(0x01DE5C50u + 5 * 0x30))    /* fx type 5 knob ranges: min, max, default x 4 */

#define DIR_PLUGINS "/yaffs2/plugins"
#define MAX_FILES   8
#define NAME_MAX    40
#define FILE_MAX    0x00100000u
#define ARENA_ALIGN 32u
#define MAGIC       0x5300                                                /* OP1_ENGINE_MAGIC: params[4] = MAGIC + n */
#define SLOTS       64                                                    /* numbers 1..63 */
#define FILE_FIRST  32
#define MAP_MAX     64
#define BLOCK       64

extern "C" const kn_export coop_exports[];                                /* sdk/coop_exports.cc */
extern "C" const int coop_exports_count;
extern "C" int coop_stereo_side_rw(int instr, int16_t **l, int16_t **r);   /* stereo_bus.cc: one instrument's side pair, live */

volatile uint32_t g_plugins_seen, g_plugins_loaded, g_plugins_failed, g_plugins_booted, g_plugins_map_n;
volatile uint32_t g_fx_dispatch_mono, g_fx_dispatch_st, g_fx_plugin_blocks;
const char *volatile g_plugins_last_error;
char g_plugins_last_name[NAME_MAX];

#define POSITIONS   5                                                     /* 0 synth, 1 drum, 2 master: an instance each (P2b);
                                                                             S2: 3 instrument slot 2, 4 master slot 2 */
struct plug { const coop_fx_t *fx; const coop_fx_t *inst[POSITIONS]; char name[16]; int16_t def[4]; };
static struct plug g_plug[SLOTS];                                          /* by number */
struct mapent { char name[16]; uint8_t n, present; };
static struct mapent g_map[MAP_MAX];
static int g_map_n, g_map_dirty;
static char g_names[MAX_FILES][NAME_MAX];
static int g_n_names;
static int g_cur[3] = { -1, -1, -1 };                                       /* the plugin each position initialised last */
static void *g_self[2];                                                    /* the mono filter objects of positions 0 / 1 */
static int16_t g_params_z[8];

typedef struct { uint32_t alloc; union { char buf[16]; char *ptr; } u; uint32_t size, cap; uint32_t pad; } dstr;

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int ends_with(const char *s, const char *e) { int a = slen(s), b = slen(e); if (a <= b) return 0; for (int i = 0; i < b; i++) if (s[a - b + i] != e[i]) return 0; return 1; }
static void scopy(char *d, const char *s, int cap) { int i = 0; for (; i < cap - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static char *scat(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *snum(char *d, int v)
{
    char b[12]; int i = 0;
    if (v < 0) { *d++ = '-'; v = -v; }
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) *d++ = b[--i];
    *d = 0; return d;
}
static void dstr_init(dstr *s) { volatile uint32_t *w = (volatile uint32_t *)s; for (unsigned i = 0; i < sizeof *s / 4; i++) w[i] = 0; }
static const char *dstr_p(const dstr *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }
static void dstr_free(dstr *s) { if (s->cap > 15 && s->u.ptr) OP_DELETE(s->u.ptr); }

static int db_write(void *db, const char *sql)                            /* kennel op1_synth.h op1_db_write */
{
    void *sem = DB_SEM;
    if (sem) SEM_TAKE(sem, 0);
    int rc = SQLITE_EXEC(*(void **)db, sql, 0, 0, 0);
    if (sem) SEM_GIVE(sem);
    return rc;
}

/* ---- the number map (user db, table coopplugins id 0: "f:name=n,-f:gone=n,...") ---- */
static void map_load(void *db)
{
    DB_EXEC(db, "CREATE TABLE IF NOT EXISTS coopplugins (id INTEGER PRIMARY KEY, data TEXT NOT NULL)");
    dstr s; dstr_init(&s);
    DB_GET(&s, db, "SELECT data FROM coopplugins WHERE id=0");
    const char *p = dstr_p(&s), *e = p + s.size;
    while (p < e && g_map_n < MAP_MAX) {
        int present = 1;
        if (*p == '-') { present = 0; p++; }
        if (e - p < 4 || p[0] != 'f' || p[1] != ':') break;
        p += 2;
        int i = 0;
        while (p < e && *p != '=' && i < 15) g_map[g_map_n].name[i++] = *p++;
        g_map[g_map_n].name[i] = 0;
        if (p >= e || *p != '=') break;
        p++;
        int v = 0;
        while (p < e && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        if (p < e && *p == ',') p++;
        if (v < 1 || v >= SLOTS) continue;
        g_map[g_map_n].present = (uint8_t)present; g_map[g_map_n].n = (uint8_t)v; g_map_n++;
    }
    dstr_free(&s);
    g_plugins_map_n = (uint32_t)g_map_n;
}
static void map_save(void *db)
{
    static char sql[80 + MAP_MAX * 24];
    char *o = scat(sql, "INSERT OR REPLACE INTO coopplugins VALUES (0,'");
    for (int i = 0; i < g_map_n; i++) {
        if (!g_map[i].present) *o++ = '-';
        *o++ = 'f'; *o++ = ':';
        o = scat(o, g_map[i].name); *o++ = '=';
        o = snum(o, g_map[i].n); *o++ = ',';
    }
    scat(o, "')");
    db_write(db, sql);
}
static int number_for(const char *name)
{
    int me = -1;
    for (int i = 0; i < g_map_n; i++) if (seq(g_map[i].name, name)) me = i;
    if (me >= 0) {
        if (!g_map[me].present) { g_map[me].present = 1; g_map_dirty = 1; }
        return g_plug[g_map[me].n].fx ? 0 : g_map[me].n;
    }
    if (g_map_n >= MAP_MAX) return 0;
    for (int n = FILE_FIRST; n < SLOTS; n++) {
        int used = g_plug[n].fx != 0;
        for (int i = 0; i < g_map_n; i++) if (g_map[i].n == n) used = 1;
        if (used) continue;
        g_map[g_map_n].present = 1; g_map[g_map_n].n = (uint8_t)n;
        scopy(g_map[g_map_n].name, name, 16); g_map_n++; g_map_dirty = 1;
        return n;
    }
    return 0;
}

/* ---- the plugin's fx_types row (factory db): the stock FX list shows it, selecting it copies these defaults ---- */
static const int16_t fx_rng[4][2] = { { 64, 14528 }, { 0, 24576 }, { 0, 22528 }, { 8192, 32767 } };
static void row_ensure(void *db, const char *name, const int16_t *def, int n)
{
    int v[8];
    for (int i = 0; i < 4; i++) { int d = def[i] < 0 ? 0 : def[i]; v[i] = fx_rng[i][0] + d * (fx_rng[i][1] - fx_rng[i][0]) / 32767; }
    v[4] = MAGIC + n; v[5] = v[6] = v[7] = 0;
    char want[96], sql[256], *o = want;
    *o++ = '[';
    for (int i = 0; i < 8; i++) { if (i) { *o++ = ','; *o++ = ' '; } o = snum(o, v[i]); }
    *o++ = ']'; *o = 0;
    o = scat(scat(sql, "SELECT default_params FROM fx_types WHERE type='"), name); scat(o, "'");
    dstr s; dstr_init(&s);
    DB_GET(&s, db, sql);
    const char *have = dstr_p(&s);
    int exists = s.size != 0, same = exists && s.size == (uint32_t)slen(want);
    for (uint32_t i = 0; same && i < s.size; i++) same = have[i] == want[i];
    dstr_free(&s);
    if (same) return;
    if (exists) { o = scat(scat(sql, "UPDATE fx_types SET default_params='"), want); o = scat(scat(o, "' WHERE type='"), name); scat(o, "'"); }
    else { o = scat(scat(sql, "INSERT INTO fx_types SELECT MAX(id)+1,'"), name); o = scat(scat(o, "','"), want); scat(o, "' FROM fx_types"); }
    db_write(db, sql);
}

/* ---- loading ---- */
static void list_dir(void)
{
    g_n_names = 0;
    void *d = Y_OPENDIR(DIR_PLUGINS);
    if (!d) return;
    const uint8_t *e;
    while ((e = Y_READDIR(d)) && g_n_names < MAX_FILES) {
        const char *s = (const char *)e + 11;
        if (s[0] == '.' || slen(s) >= NAME_MAX || !ends_with(s, ".coop")) continue;
        scopy(g_names[g_n_names++], s, NAME_MAX);
    }
    Y_CLOSEDIR(d);
}

static const char *load_one(const char *fname, uint8_t *stage)
{
    char path[64];
    scat(scat(scat(path, DIR_PLUGINS), "/"), fname);
    int fd = Y_OPEN(path, 0, 0);
    if (fd < 0) return "cannot open";
    int got = Y_READ(fd, stage, (int)FILE_MAX), more = 0;
    if (got == (int)FILE_MAX) { uint8_t b; more = Y_READ(fd, &b, 1) > 0; }
    Y_CLOSE(fd);
    if (got <= 0) return "cannot read";
    if (more) return "file larger than 1 MiB";
    kn_info in;
    const char *err = 0;
    if (kn_parse(stage, (uint32_t)got, COOP_PLUGIN_API, &in, &err)) return err;
    if (in.kind != KN_KIND_FX) return "not an FX plugin (effects only so far)";
    if (in.flags) return "plugin flags not supported yet";
    for (int k = 1; k < SLOTS; k++) if (g_plug[k].fx && seq(g_plug[k].name, in.type_name)) return "another file has the same name";
    int n = number_for(in.type_name);
    if (!n) return "no free plugin number";
    /* one relocated instance per position (P2b): a plugin selected at two positions at once keeps two states */
    const coop_fx_t *inst[POSITIONS];
    uint8_t *raws[POSITIONS] = { 0 };
    for (int k = 0; k < POSITIONS; k++) {
        uint32_t cap = in.load_size + 3u + KN_STUB_SIZE * in.n_imports + ARENA_ALIGN;
        uint8_t *raw = (uint8_t *)MALLOC(cap);
        if (!raw) err = "plugin memory full";
        else {
            raws[k] = raw;
            uint32_t base = ((uint32_t)(uintptr_t)raw + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1);
            uint32_t room = cap - (base - (uint32_t)(uintptr_t)raw), used = 0;
            if (!kn_relocate_far(stage, &in, (uint8_t *)(uintptr_t)base, base, room, coop_exports, coop_exports_count, &err, &used)) {
                const coop_fx_t *fx = (const coop_fx_t *)(uintptr_t)(base + in.entry_offset);
                uint32_t text_end = base + in.text_size;
                uint32_t pi = (uint32_t)(uintptr_t)fx->init, pr = (uint32_t)(uintptr_t)fx->render;
                if (fx->magic != COOP_FX_MAGIC) err = "entry object is not a coop FX (magic)";
                else if (fx->api != COOP_PLUGIN_API) err = "entry object built for another API";
                else if (pi < base || pi >= text_end || pr < base || pr >= text_end || (pi & 1) || (pr & 1)) err = "callbacks outside the plugin's code";
                else inst[k] = fx;
            }
        }
        if (err) { for (int j = 0; j <= k; j++) if (raws[j]) FREE(raws[j]); return err; }
    }
    scopy(g_plug[n].name, in.type_name, 16);
    for (int i = 0; i < 4; i++) g_plug[n].def[i] = in.defaults[i];
    for (int k = 0; k < POSITIONS; k++) g_plug[n].inst[k] = inst[k];
    __asm__ __volatile__("ssync;" ::: "memory");
    g_plug[n].fx = inst[0];                                               /* selectable from here */
    row_ensure(DB_FACTORY(), g_plug[n].name, g_plug[n].def, n);
    return 0;
}

static void slot2_load(void);                                              /* S2, below */
static void tanks_boot(void);                                              /* below */

/* From the tape mount hook (main thread, boot), once. */
extern "C" void coop_plugins_boot(void)
{
    if (g_plugins_booted) return;
    g_plugins_booted = 1;
    tanks_boot();                                                          /* the instrument spring's own tank */
    list_dir();
    if (!g_n_names) { slot2_load(); return; }
    void *udb = DB_HANDLE();
    if (udb) map_load(udb);
    uint8_t *stage = (uint8_t *)MALLOC(FILE_MAX);
    if (!stage) { g_plugins_last_error = "no staging memory"; return; }
    for (int i = 0; i < g_n_names; i++) {
        g_plugins_seen++;
        const char *err = load_one(g_names[i], stage);
        if (err) { g_plugins_failed++; g_plugins_last_error = err; scopy(g_plugins_last_name, g_names[i], NAME_MAX); }
        else g_plugins_loaded++;
    }
    FREE(stage);
    for (int i = 0; i < g_map_n; i++)                                      /* files that went away keep their numbers, marked absent */
        if (g_map[i].present && !g_plug[g_map[i].n].fx) { g_map[i].present = 0; g_map_dirty = 1; }
    if (udb && g_map_dirty) map_save(udb);
    slot2_load();                                                          /* S2: after the plugins it may name */
}

/* ---- the stock FX list: every plugin name resolves to the filter type (replaces the fx_types name compare) ---- */
static int streq_n(const char *a, const char *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i] || !b[i]) return 0;
    return b[n] == 0;
}
static const char *const stock_fx_names[] = { "grid", "punch", "delay", "phone", "spring", "cwo", "nitro", "filter", 0 };
extern "C" int coop_fx_name_eq(const char *name, const dstr *s)
{
    const char *p = dstr_p(s);
    if (streq_n(p, name, s->size)) return 1;
    if (seq(name, "filter")) {                                            /* a plugin, an old plugin name, an unknown name */
        if (!s->size) return 0;
        for (const char *const *q = stock_fx_names; *q; q++) if (streq_n(p, *q, s->size)) return 0;
        return 1;
    }
    return 0;
}

/* ---- the stock effects as stereo objects at the instrument positions (stereo bus v2, S1) ----
 * Every stock mono FX vtable's render / init entries point at coop_fx_render_mono / init_mono (the package tool), so
 * the stock mono effect never runs on the mono bus. When a stock type is selected at an instrument position, coop
 * constructs that type's *stereo* object (the master's class: dual-mono inside, or native stereo) in its own memory
 * and runs it on the instrument's stereo pair. Classes and entries from the stock disassembly (docs/stereo-bus-v2.md,
 * RE 2026-11-03): type index = the stock name table order (delay 0, grid 1, punch 2, phone 3, spring 4, filter 5,
 * nitro 6, cwo 7); vtable entries are 8 bytes apart, render at +0x14, init at +0x1c. */
#define FX_TYPES 8
#define OBJ_BYTES 0x400                                                   /* every stereo object fits the 0x270 placement gap */
#define LINE_TABLE ((volatile uint32_t *)0x01AB8B94u)                     /* delay / cwo line buffers: [+0xc] mono delay, [+0x14] stereo delay, [+0] mono cwo, [+8] stereo cwo */
#define LINE_BYTES 0x2B14Cu                                               /* one line buffer (0x1b0ee48 - 0x1ae3cfc) */
/* The stereo grid and spring classes keep their tank at a fixed address (grid 0x01D8C70C, spring 0x01DE6252); every
 * coop object of those two types gets its own (tank_bind below; the S1 note's 0x01DE1F50 was a misread L1 table).
 * punch, phone, filter and nitro are dual-mono (their stereo render calls the mono render directly). */
struct fx_class { uint32_t mono_vt, st_ctor, st_vt; };
static const struct fx_class g_fx_class[FX_TYPES] = {
    { 0x0190FB6Cu, 0x0197D880u, 0x0190FB8Cu },   /* delay  */
    { 0x01910204u, 0x019AF25Cu, 0x01910224u },   /* grid   */
    { 0x01910C1Cu, 0x019E61BCu, 0x01910C3Cu },   /* punch  */
    { 0x019112F4u, 0x01A1C144u, 0x01911314u },   /* phone  */
    { 0x019113BCu, 0x01A32F94u, 0x019113DCu },   /* spring */
    { 0x01910C94u, 0x019E784Cu, 0x01910CB4u },   /* filter */
    { 0x01910014u, 0x019A6200u, 0x01910034u },   /* nitro  */
    { 0x0190F574u, 0x0194425Cu, 0x0190F594u },   /* cwo    */
};
typedef void (*st_render_fn)(void *, int16_t *, int16_t *, const int16_t *);
typedef void (*st_init_fn)(void *);
struct st_inst { void *obj; int type; uint8_t *line; uint32_t slots[8]; };
/* Display slots. Every stock effect writes its page state (levels, the animated dials) into a table in L1 at
 * 0xFF801774, 16 bytes per slot: a mono object into slot [obj + index field] = its position (0 synth, 1 drum, set by
 * the mono init), a stereo object into slots 2 and 3 (the master's; dual-mono classes set them in their init, native
 * stereo ones hard-code 0xFF801794). The twin therefore runs with slots 2-3 swapped for its own copy, and its left
 * slot is copied into the instrument's slot afterwards, so the instrument page follows the knobs and the master's
 * page is untouched (nitro's dials, found by the owner on the device, 2026-11-03). */
#define FX_SLOTS ((volatile uint32_t *)0xFF801774u)
static struct st_inst g_st[3];                                            /* [0]: the stock has one instrument FX object for both
                                                                             positions; S2: [1] instrument slot 2, [2] master slot 2 */
static int g_fx_pos;                                                       /* the position of its last init: which instrument bus it processes */
volatile uint32_t g_fx_stock_stereo_blocks, g_fx_stock_ctor;

static int type_of_mono(void *self)
{
    uint32_t vt = *(uint32_t *)self;
    for (int t = 0; t < FX_TYPES; t++) if (g_fx_class[t].mono_vt == vt) return t;
    return -1;
}
static inline uint32_t vt_entry(uint32_t vt, int off) { return *(volatile uint32_t *)(vt + off); }

/* Own reverb memory for every coop grid / spring object (clean-up after S2). The stereo classes load their tank as an
 * immediate in render and init; the package points those four loads at coop_tank_* (stereo_thunks.S), which look the
 * object up here: { obj0, tank0, obj1, tank1, obj2, tank2, 0, stock tank }, index = g_st index. Any other object (the
 * stock master's) gets the stock tank. Tanks, from the stock static constructors (RE 2026-11-04):
 * - grid stereo: 0x42040 bytes at 0x01D8C70C. The mono class keeps two of the same size just below it at 0x01D0868C
 *   (two positions); since S1 no mono grid runs (its vtable points at coop), so they serve g_st[0] and g_st[1].
 *   g_st[2] (master slot 2) gets one from the heap when grid is put there.
 * - spring stereo: 0xE3A0 bytes at 0x01DE6252 (halfword aligned, 2 mod 8). The mono ones (0x01001EA8, two) are word
 *   aligned, so every coop spring tank comes from the heap at the stock alignment: g_st[0]'s at boot, slot 2's when
 *   spring is put there. All tanks are zero when bound (the stock init clears them anyway).
 * A tank that cannot be had leaves the object on the stock tank (the old shared behaviour), counted in g_tank_short. */
#define GRID_TANK        0x42040u
#define GRID_MONO_TANKS  0x01D0868Cu
#define SPRING_TANK      0xE3A0u
extern "C" {
__attribute__((section(".data"))) volatile uint32_t g_coop_tank_grid[8] = { 0, 0, 0, 0, 0, 0, 0, 0x01D8C70Cu };
__attribute__((section(".data"))) volatile uint32_t g_coop_tank_spring[8] = { 0, 0, 0, 0, 0, 0, 0, 0x01DE6252u };
}
static uint32_t g_tank_grid_heap, g_tank_spring_own[3];
volatile uint32_t g_tank_short, g_tank_bound;                              /* g_tank_bound: bit i grid, bit 4 + i spring */

static uint32_t tank_alloc(uint32_t size, uint32_t rem8)                  /* main thread only; zeroed, addr % 8 == rem8 */
{
    uint8_t *raw = (uint8_t *)MALLOC(size + 16);
    if (!raw) { g_tank_short++; return 0; }
    uint32_t a = (((uint32_t)(uintptr_t)raw + 7u) & ~7u) + rem8;
    for (uint32_t i = 0; i < size; i += 2) *(volatile uint16_t *)(uintptr_t)(a + i) = 0;
    return a;
}

static void tanks_boot(void) { if (!g_tank_spring_own[0]) g_tank_spring_own[0] = tank_alloc(SPRING_TANK, 2); }

static void tank_bind(int i, void *obj, int t)
{
    volatile uint32_t *map = t == 1 ? g_coop_tank_grid : t == 4 ? g_coop_tank_spring : 0;
    if (!map || !obj || map[2 * i] == (uint32_t)(uintptr_t)obj) return;
    uint32_t a = t == 1 ? (i < 2 ? GRID_MONO_TANKS + (uint32_t)i * GRID_TANK : g_tank_grid_heap) : g_tank_spring_own[i];
    if (!a) return;
    map[2 * i + 1] = a;
    __asm__ __volatile__("ssync;" ::: "memory");
    map[2 * i] = (uint32_t)(uintptr_t)obj;
    g_tank_bound |= 1u << (t == 1 ? i : 4 + i);
}

/* construct (or reconstruct) position pos's stereo object of stock type t */
static void st_make(int pos, int t)
{
    struct st_inst *s = &g_st[pos];
    if (!s->obj) s->obj = MALLOC(OBJ_BYTES);
    if (!s->obj) { s->type = -1; return; }
    if ((t == 0 || t == 7) && !s->line) s->line = (uint8_t *)MALLOC(LINE_BYTES);   /* its own delay line */
    if ((t == 0 || t == 7) && !s->line) { s->type = -1; return; }
    uint8_t *o = (uint8_t *)s->obj;
    for (uint32_t i = 0; i < OBJ_BYTES; i += 4) *(volatile uint32_t *)(o + i) = 0;
    uint32_t save8 = LINE_TABLE[2];
    if (t == 7) LINE_TABLE[2] = (uint32_t)(uintptr_t)s->line;              /* cwo caches its line at construction */
    ((void (*)(void *))(uintptr_t)g_fx_class[t].st_ctor)(s->obj);
    if (t == 7) LINE_TABLE[2] = save8;
    tank_bind(pos, s->obj, t);                                            /* before the init, which clears the tank */
    ((st_init_fn)(uintptr_t)vt_entry(g_fx_class[t].st_vt, 0x1c))(s->obj);
    s->type = t;
    for (int k = 0; k < 8; k++) s->slots[k] = 0;
    g_fx_stock_ctor++;
}

#ifdef COOP_PHONE_CHECK
/* Test builds only (EXTRA_CXXFLAGS=-DCOOP_PHONE_CHECK): phone_fast.S in lockstep with the stock loop. Each mono half
 * runs first on a copy of its object and I/O block with g_coop_fast_phone = 0, then for real with 1; any difference in
 * the object (0x8c bytes) or the output counts in g_phone_check_bad. The capture buffer is shared by both runs, which
 * write the same values to it. */
extern "C" volatile uint32_t g_coop_fast_phone;
extern "C" { volatile uint32_t g_phone_check_calls, g_phone_check_bad, g_phone_check_first; }
extern "C" { volatile uint32_t g_phone_hit_slow, g_phone_hit_end, g_phone_hit_swap, g_phone_hit_capa, g_phone_hit_rise,
                                g_phone_hit_capb; }   /* phone_fast.S's rare paths */
static uint32_t g_phone_shadow[0x90 / 4];
static int16_t g_phone_buf[64];
typedef void (*phone_mono_fn)(void *, int16_t *, const int16_t *);
static void phone_check(uint8_t *obj, int16_t *io, const int16_t *params)
{
    const uint32_t *o = (const uint32_t *)obj;
    for (int k = 0; k < 0x8c / 4; k++) g_phone_shadow[k] = o[k];
    for (int k = 0; k < 64; k++) g_phone_buf[k] = io[k];
    g_coop_fast_phone = 0;
    ((phone_mono_fn)0x01A1BB74u)(g_phone_shadow, g_phone_buf, params);
    g_coop_fast_phone = 1;
    ((phone_mono_fn)0x01A1BB74u)(obj, io, params);
    int bad = 0;
    for (int k = 0; k < 0x8c / 4; k++) if (g_phone_shadow[k] != o[k]) bad = 1;
    for (int k = 0; k < 64; k++) if (g_phone_buf[k] != io[k]) bad = 1;
    g_phone_check_calls++;
    if (bad) { if (!g_phone_check_bad) g_phone_check_first = g_phone_check_calls; g_phone_check_bad++; }
}
#endif

#ifdef COOP_NITRO_CHECK
/* Test builds only (EXTRA_CXXFLAGS=-DCOOP_NITRO_CHECK): nitro_fast.S in lockstep with the stock loops, as phone_check:
 * each mono half (0x3c bytes) runs first on a copy of its object and I/O block with g_coop_fast_nitro = 0, then for
 * real with 1; any difference in the object or the output counts in g_nitro_check_bad. Both runs write the same values
 * to the L1 buffer 0xFF801B50 and the display slots. g_nitro_hit_calls counts the rewrite's runs (nitro_fast.S). */
extern "C" volatile uint32_t g_coop_fast_nitro;
extern "C" { volatile uint32_t g_nitro_check_calls, g_nitro_check_bad, g_nitro_check_first, g_nitro_hit_calls; }
static uint32_t g_nitro_shadow[0x3c / 4];
static int16_t g_nitro_buf[64];
typedef void (*nitro_mono_fn)(void *, int16_t *, const int16_t *);
static void nitro_check(uint8_t *obj, int16_t *io, const int16_t *params)
{
    const uint32_t *o = (const uint32_t *)obj;
    for (int k = 0; k < 0x3c / 4; k++) g_nitro_shadow[k] = o[k];
    for (int k = 0; k < 64; k++) g_nitro_buf[k] = io[k];
    g_coop_fast_nitro = 0;
    ((nitro_mono_fn)0x019A62A4u)(g_nitro_shadow, g_nitro_buf, params);
    g_coop_fast_nitro = 1;
    ((nitro_mono_fn)0x019A62A4u)(obj, io, params);
    int bad = 0;
    for (int k = 0; k < 0x3c / 4; k++) if (g_nitro_shadow[k] != o[k]) bad = 1;
    for (int k = 0; k < 64; k++) if (g_nitro_buf[k] != io[k]) bad = 1;
    g_nitro_check_calls++;
    if (bad) { if (!g_nitro_check_bad) g_nitro_check_first = g_nitro_check_calls; g_nitro_check_bad++; }
}
#endif

static void st_render(int pos, int16_t *l, int16_t *r, const int16_t *params)
{
    struct st_inst *s = &g_st[pos];
    int t = s->type;
    tank_bind(pos, s->obj, t);                                            /* a tank that came after the construction (zeroed) */
    uint32_t save = LINE_TABLE[5];
    if (t == 0) LINE_TABLE[5] = (uint32_t)(uintptr_t)s->line;             /* the stereo delay reads its line from the table each render */
    uint32_t master[8];
    for (int k = 0; k < 8; k++) { master[k] = FX_SLOTS[8 + k]; FX_SLOTS[8 + k] = s->slots[k]; }
#ifdef COOP_PHONE_CHECK
    if (t == 3) { phone_check((uint8_t *)s->obj + 4, l, params); phone_check((uint8_t *)s->obj + 0x90, r, params); } else
#endif
#ifdef COOP_NITRO_CHECK
    if (t == 6) { nitro_check((uint8_t *)s->obj + 4, l, params); nitro_check((uint8_t *)s->obj + 0x40, r, params); } else
#endif
    ((st_render_fn)(uintptr_t)vt_entry(g_fx_class[t].st_vt, 0x14))(s->obj, l, r, params);
    for (int k = 0; k < 8; k++) s->slots[k] = FX_SLOTS[8 + k];
    if (pos == 0) for (int k = 0; k < 4; k++) FX_SLOTS[4 * (g_fx_pos & 1) + k] = s->slots[k];   /* slot 2 has no stock page */
    for (int k = 0; k < 8; k++) FX_SLOTS[8 + k] = master[k];
    if (t == 0) LINE_TABLE[5] = save;
    g_fx_stock_stereo_blocks++;
}

/* ---- dispatch at the three positions (the mono vtables of every stock type point here) ---- */
static int fx_index(int n) { return (n > 0 && n < SLOTS && g_plug[n].fx) ? n : 0; }

/* kennel's guard: keep the raw knob words of every slot running a plugin inside the filter type's ranges; the stock
 * read_fx_param asserts on anything outside and the OP-1 locks up (at every boot while such a value is stored). */
static void fx_sanitize(void)
{
    for (int pos = 0; pos < 3; pos++)
        for (int slot = 0; slot < (pos == 2 ? 1 : 8); slot++) {
            int16_t *p = (int16_t *)(pos == 2 ? 0xFF8016C4u : (pos ? 0xFF8015C4u : 0xFF801444u) + slot * 16);
            if (!fx_index(p[4] - MAGIC)) continue;
            if (pos == 2 && MASTER_FX_TYPE != 5) continue;
            for (int i = 0; i < 4; i++) {
                int lo = RANGE_TABLE[i * 3], hi = RANGE_TABLE[i * 3 + 1];
                if (hi <= lo) { lo = fx_rng[i][0]; hi = fx_rng[i][1]; }
                if (p[i] < lo) p[i] = (int16_t)lo; else if (p[i] > hi) p[i] = (int16_t)hi;
            }
        }
}

extern "C" void coop_fx_init_mono(void *self, int pos)
{
    g_self[pos & 1] = self;
    g_cur[pos & 1] = -1;
    g_fx_pos = pos & 1;
    int t = type_of_mono(self);                                           /* the stock just constructed this mono object: */
    if (t >= 0) st_make(0, t);                                            /* its stereo twin (one object serves both positions) */
}

extern "C" void coop_fx_render_mono(void *self, int16_t *buf, const int16_t *params)
{
    g_fx_dispatch_mono++;
    fx_sanitize();
    int pos = g_fx_pos;
    int e = fx_index(params[4] - MAGIC);
    int16_t *dL, *dR;
    coop_stereo_side_rw(pos, &dL, &dR);                                  /* this instrument's own side pair (P2b) */
    if (!e) {                                                             /* a stock type: its stereo object on the pair (S1) */
        g_cur[pos] = 0;
        int t = type_of_mono(self);
        if (t < 0 || g_st[0].type != t) return;                          /* no twin (construction failed): pass through dry */
        uint32_t c0 = coop_cost_now();
        int16_t l[BLOCK], r[BLOCK];
        for (int i = 0; i < BLOCK; i++) { l[i] = coop_sat16((int32_t)buf[i] + dL[i]); r[i] = coop_sat16((int32_t)buf[i] + dR[i]); }
        st_render(0, l, r, params);
        for (int i = 0; i < BLOCK; i++) {
            int16_t m = (int16_t)(((int32_t)l[i] + r[i]) >> 1);
            buf[i] = m; dL[i] = (int16_t)(l[i] - m); dR[i] = (int16_t)(r[i] - m);
        }
        coop_cost_add(COOP_COST_ST1 + t, coop_cost_now() - c0);
        return;
    }
    uint32_t c0 = coop_cost_now();
    const coop_fx_t *fx = g_plug[e].inst[pos];
    if (e != g_cur[pos]) { g_cur[pos] = e; if (fx->init(pos) != 0) { g_plug[e].fx = 0; return; } }
    int32_t L[BLOCK], R[BLOCK];
    for (int i = 0; i < BLOCK; i++) {                                     /* the instrument in stereo: mono + its difference */
        L[i] = (int32_t)coop_sat16((int32_t)buf[i] + dL[i]) << 16;
        R[i] = (int32_t)coop_sat16((int32_t)buf[i] + dR[i]) << 16;
    }
    fx->render(pos, L, R, params, BLOCK);
    for (int i = 0; i < BLOCK; i++) {
        int16_t l = coop_sat16(L[i] >> 16), r = coop_sat16(R[i] >> 16);
        int16_t m = (int16_t)(((int32_t)l + r) >> 1);
        buf[i] = m;                                                       /* down the stock mono path */
        dL[i] = (int16_t)(l - m); dR[i] = (int16_t)(r - m);               /* the new side of this instrument */
    }
    coop_cost_add(COOP_COST_PLUG, coop_cost_now() - c0);
    g_fx_plugin_blocks++;
}

extern "C" void coop_fx_init_st(void *self)
{
    g_cur[2] = -1;
    FILTER_ST_INIT(self);
}

extern "C" void coop_fx_render_st(void *self, int16_t *l, int16_t *r, const int16_t *params)
{
    g_fx_dispatch_st++;
    fx_sanitize();
    int e = fx_index(params[4] - MAGIC);
    if (!e) { g_cur[2] = 0; FILTER_ST_RENDER(self, l, r, params); return; }
    const coop_fx_t *fx = g_plug[e].inst[2];
    if (e != g_cur[2]) { g_cur[2] = e; if (fx->init(2) != 0) { g_plug[e].fx = 0; FILTER_ST_RENDER(self, l, r, params); return; } }
    int32_t L[BLOCK], R[BLOCK];
    for (int i = 0; i < BLOCK; i++) { L[i] = (int32_t)l[i] << 16; R[i] = (int32_t)r[i] << 16; }
    fx->render(2, L, R, params, BLOCK);
    for (int i = 0; i < BLOCK; i++) { l[i] = coop_sat16(L[i] >> 16); r[i] = coop_sat16(R[i] >> 16); }
    g_fx_plugin_blocks++;
}

/* ---- S2: the second FX slot, one after the instrument effect and one after the master effect ----
 * Each holds nothing, a stock type (its own stereo object, g_st[1 + pos]) or a coop plugin (its own relocated
 * instance, inst[3 + pos]), with four knobs as Q15 positions mapped into the type's ranges. It runs every block,
 * whether or not the stock slot (slot 1) is on: the instrument one from the mix entry on the stereo pair
 * (stereo_bus.cc), the master one in the master stand-in before the stock level stage (master_suite.cc).
 * Chosen from the SysEx host page (coop_sysex.cc, command 0x26), kept in /yaffs2/user/coop_slot2.bin; an on-device
 * page comes with the screens (P3). A change is posted by the main thread and taken by the audio thread, which
 * constructs the object (as the stock constructs its effects in the audio thread). */
#define RANGE_ROW(t) ((const int16_t *)(0x01DE5C50u + (uint32_t)(t) * 0x30u))   /* knob i: min, max, default at 3i */
#define Y_WRITE      ((int (*)(int, const void *, int))0x01A99F84u)
#define S2_FILE      "/yaffs2/user/coop_slot2.bin"
struct slot2 { volatile int kind, id; volatile uint16_t knob[4]; volatile uint32_t req; uint32_t took; int kind_on, id_on; };
static struct slot2 g_s2[2];
volatile uint32_t g_s2_blocks[2], g_s2_changes, g_s2_trace;   /* g_s2_trace: debug progress marker */

extern "C" int coop_slot2_set(int pos, int kind, int id, const uint16_t *knob)
{
    if (pos < 0 || pos > 1) return 0;
    if (kind == 1 && (id < 0 || id >= FX_TYPES)) return 0;
    if (kind == 2 && !fx_index(id)) return 0;
    if (kind < 0 || kind > 2) return 0;
    if (kind == 1) {                                                      /* the heap only from here (main thread): a
                                                                             MALLOC in the audio interrupt hung op1emu */
        struct st_inst *t = &g_st[1 + pos];
        if (!t->obj) t->obj = MALLOC(OBJ_BYTES);
        if ((id == 0 || id == 7) && !t->line) t->line = (uint8_t *)MALLOC(LINE_BYTES);
        if (!t->obj || ((id == 0 || id == 7) && !t->line)) return 0;
        if (id == 4 && !g_tank_spring_own[1 + pos]) g_tank_spring_own[1 + pos] = tank_alloc(SPRING_TANK, 2);
        if (id == 1 && pos == 1 && !g_tank_grid_heap) g_tank_grid_heap = tank_alloc(GRID_TANK, 4);   /* the instrument's is a mono one */
    }
    struct slot2 *s = &g_s2[pos];
    for (int i = 0; i < 4; i++) s->knob[i] = knob[i] > 32767 ? 32767 : knob[i];
    s->kind = kind; s->id = kind ? id : 0;
    __asm__ __volatile__("ssync;" ::: "memory");
    s->req++;
    return 1;
}

extern "C" void coop_slot2_get(int pos, uint8_t *out10)
{
    struct slot2 *s = &g_s2[pos & 1];
    out10[0] = (uint8_t)s->kind; out10[1] = (uint8_t)s->id;
    for (int i = 0; i < 4; i++) { out10[2 + 2 * i] = (uint8_t)s->knob[i]; out10[3 + 2 * i] = (uint8_t)(s->knob[i] >> 8); }
}

/* the slot 2 page (coop_ui.cc): a loaded plugin's name by number, 0 if none */
extern "C" const char *coop_plugin_name(int n) { return fx_index(n) ? g_plug[n].name : 0; }

/* the plugin list for the host page: "n=name;" entries */
extern "C" int coop_plugin_list(char *out, int cap)
{
    char *p = out, *end = out + cap - 1;
    for (int n = 1; n < SLOTS; n++) {
        if (!g_plug[n].fx) continue;
        char tmp[24];
        char *t = snum(tmp, n); *t++ = '='; t = scat(t, g_plug[n].name); *t++ = ';'; *t = 0;
        int l = slen(tmp);
        if (p + l > end) break;
        for (int i = 0; i < l; i++) *p++ = tmp[i];
    }
    *p = 0;
    return (int)(p - out);
}

/* main thread: save / load the two slots (magic "CS2", version 1, then 10 bytes per slot) */
extern "C" int coop_slot2_save(void)
{
    uint8_t b[24] = { 'C', 'S', '2', 1 };
    coop_slot2_get(0, b + 4); coop_slot2_get(1, b + 14);
    int fd = Y_OPEN(S2_FILE, 0x242, 0x180);
    if (fd < 0) return 0;
    int ok = Y_WRITE(fd, b, 24) == 24;
    return (Y_CLOSE(fd) == 0) && ok;
}

static void slot2_load(void)
{
    uint8_t b[24];
    int fd = Y_OPEN(S2_FILE, 0, 0);
    if (fd < 0) return;
    int got = Y_READ(fd, b, 24);
    Y_CLOSE(fd);
    if (got != 24 || b[0] != 'C' || b[1] != 'S' || b[2] != '2' || b[3] != 1) return;
    for (int pos = 0; pos < 2; pos++) {
        const uint8_t *e = b + 4 + 10 * pos;
        uint16_t k[4];
        for (int i = 0; i < 4; i++) k[i] = (uint16_t)(e[2 + 2 * i] | (e[3 + 2 * i] << 8));
        coop_slot2_set(pos, e[0], e[1], k);
    }
}

static int16_t knob_map(int lo, int hi, uint16_t k)
{
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    return (int16_t)(lo + (int32_t)(((uint32_t)(hi - lo) * (uint32_t)k) >> 15));   /* <= 65535 * 32767: fits 32 bits */
}

/* audio thread: take a posted change (construct / init), then run the slot on l / r (int16, 64, in place) */
static void slot2_render(int pos, int16_t *l, int16_t *r);
extern "C" void coop_slot2_render(int pos, int16_t *l, int16_t *r)
{
    uint32_t c0 = coop_cost_now();
    slot2_render(pos, l, r);
    if (g_s2[pos & 1].kind_on) coop_cost_add(pos ? COOP_COST_S2M : COOP_COST_S2I, coop_cost_now() - c0);
}
static void slot2_render(int pos, int16_t *l, int16_t *r)
{
    struct slot2 *s = &g_s2[pos & 1];
    uint32_t req = s->req;
    /* The stock fills the knob range table from the factory db during boot (~0.2 s after the audio starts). A stock
     * type must not run before its row is there: zero ranges give the delay a zero length, and its render's
     * wrap loop (subtract the length until below it) never ends (op1emu, 2026-11-03: the audio stopped at 2 s). */
    if (s->kind == 1 && req != s->took) {
        const int16_t *rg = RANGE_ROW(s->id);
        if (rg[1] <= rg[0]) return;                                       /* not loaded yet: take the change later */
    }
    if (req != s->took) {
        s->took = req;
        int kind = s->kind, id = s->id;
        if (kind != s->kind_on || id != s->id_on) {
            s->kind_on = 0;
            if (kind == 1) { g_s2_trace = 2; st_make(1 + pos, id); g_s2_trace = 3; if (g_st[1 + pos].type == id) s->kind_on = 1; }
            else if (kind == 2 && fx_index(id)) { if (g_plug[id].inst[3 + pos]->init(3 + pos) == 0) s->kind_on = 2; }
            s->id_on = id;
            g_s2_changes++;
        }
    }
    g_s2_trace = 4; if (!s->kind_on) return;
    int16_t p[8];
    if (s->kind_on == 1) {
        const int16_t *rg = RANGE_ROW(s->id_on);
        for (int i = 0; i < 8; i++) p[i] = i < 4 ? knob_map(rg[3 * i], rg[3 * i + 1], s->knob[i]) : rg[3 * i + 2];
        g_s2_trace = 5; st_render(1 + pos, l, r, p); g_s2_trace = 6;
    } else {
        const coop_fx_t *fx = g_plug[s->id_on].inst[3 + pos];
        for (int i = 0; i < 4; i++) p[i] = knob_map(fx_rng[i][0], fx_rng[i][1], s->knob[i]);
        p[4] = (int16_t)(MAGIC + s->id_on); p[5] = p[6] = p[7] = 0;
        static int32_t L[BLOCK], R[BLOCK];                                /* audio thread only, never nested: off the interrupt stack */
        for (int i = 0; i < BLOCK; i++) { L[i] = (int32_t)l[i] << 16; R[i] = (int32_t)r[i] << 16; }
        fx->render(3 + pos, L, R, p, BLOCK);
        for (int i = 0; i < BLOCK; i++) { l[i] = coop_sat16(L[i] >> 16); r[i] = coop_sat16(R[i] >> 16); }
    }
    g_s2_blocks[pos & 1]++; g_s2_trace = 7;
}

/* the instrument slot 2 on the carrier (mid in the stock bus buffer, side in coop's): from stereo_bus.cc's mix entry */
extern "C" void coop_slot2_instr(int16_t *mid, int16_t *sl, int16_t *sr)
{
    if (!g_s2[0].kind_on && g_s2[0].req == g_s2[0].took) return;
    static int16_t l[BLOCK], r[BLOCK];                                    /* off the interrupt stack (the mix context is deep) */
    for (int i = 0; i < BLOCK; i++) { l[i] = coop_sat16((int32_t)mid[i] + sl[i]); r[i] = coop_sat16((int32_t)mid[i] + sr[i]); }
    g_s2_trace = 1; coop_slot2_render(0, l, r); g_s2_trace = 9;
    for (int i = 0; i < BLOCK; i++) {
        int32_t m = ((int32_t)l[i] + r[i]) >> 1;
        mid[i] = (int16_t)m; sl[i] = coop_sat16(l[i] - m); sr[i] = coop_sat16(r[i] - m);
    }
}

extern "C" int coop_fx_plugin_loaded(void) { return g_plugins_loaded != 0; }
extern "C" int coop_fx_plugin_render(int32_t *l, int32_t *r, int n) { (void)l; (void)r; (void)n; return 0; }   /* P1's master-insert path: retired */
