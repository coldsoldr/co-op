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

#define POSITIONS   3                                                     /* 0 synth, 1 drum, 2 master: an instance each (P2b) */
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
    uint8_t *raws[POSITIONS] = { 0, 0, 0 };
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

/* From the tape mount hook (main thread, boot), once. */
extern "C" void coop_plugins_boot(void)
{
    if (g_plugins_booted) return;
    g_plugins_booted = 1;
    list_dir();
    if (!g_n_names) return;
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
/* Known S1 limit: the stereo grid and spring classes keep their tank at a fixed address (grid 0x01D8C70C, spring
 * 0x01DE1F50 / 0x01DE6248, immediates in their render and init; the mono classes use other ones), so the same one of
 * those two types at an instrument position and on the master at once shares one tank. Bounded (positions live in
 * the object), audible as interference. Fix planned with S3: clones of those two renders retargeted at the unused
 * mono tanks. punch, phone, filter and nitro are dual-mono (their stereo render calls the mono render directly). */
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
static struct st_inst g_st[2];                                            /* [0]: the stock has one instrument FX object for both positions */
static int g_fx_pos;                                                       /* the position of its last init: which instrument bus it processes */
volatile uint32_t g_fx_stock_stereo_blocks, g_fx_stock_ctor;

static int type_of_mono(void *self)
{
    uint32_t vt = *(uint32_t *)self;
    for (int t = 0; t < FX_TYPES; t++) if (g_fx_class[t].mono_vt == vt) return t;
    return -1;
}
static inline uint32_t vt_entry(uint32_t vt, int off) { return *(volatile uint32_t *)(vt + off); }

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
    ((st_init_fn)(uintptr_t)vt_entry(g_fx_class[t].st_vt, 0x1c))(s->obj);
    s->type = t;
    for (int k = 0; k < 8; k++) s->slots[k] = 0;
    g_fx_stock_ctor++;
}

static void st_render(int pos, int16_t *l, int16_t *r, const int16_t *params)
{
    struct st_inst *s = &g_st[pos];
    int t = s->type;
    uint32_t save = LINE_TABLE[5];
    if (t == 0) LINE_TABLE[5] = (uint32_t)(uintptr_t)s->line;              /* the stereo delay reads its line from the table each render */
    uint32_t master[8];
    for (int k = 0; k < 8; k++) { master[k] = FX_SLOTS[8 + k]; FX_SLOTS[8 + k] = s->slots[k]; }
    ((st_render_fn)(uintptr_t)vt_entry(g_fx_class[t].st_vt, 0x14))(s->obj, l, r, params);
    for (int k = 0; k < 8; k++) s->slots[k] = FX_SLOTS[8 + k];
    for (int k = 0; k < 4; k++) FX_SLOTS[4 * (g_fx_pos & 1) + k] = s->slots[k];
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
        int16_t l[BLOCK], r[BLOCK];
        for (int i = 0; i < BLOCK; i++) { l[i] = coop_sat16((int32_t)buf[i] + dL[i]); r[i] = coop_sat16((int32_t)buf[i] + dR[i]); }
        st_render(0, l, r, params);
        for (int i = 0; i < BLOCK; i++) {
            int16_t m = (int16_t)(((int32_t)l[i] + r[i]) >> 1);
            buf[i] = m; dL[i] = (int16_t)(l[i] - m); dR[i] = (int16_t)(r[i] - m);
        }
        return;
    }
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

extern "C" int coop_fx_plugin_loaded(void) { return g_plugins_loaded != 0; }
extern "C" int coop_fx_plugin_render(int32_t *l, int32_t *r, int n) { (void)l; (void)r; (void)n; return 0; }   /* P1's master-insert path: retired */
