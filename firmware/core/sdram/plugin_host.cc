/*
 * coop-fw plugin host (Phase 4, milestone P1): loads the `.coop` files of /yaffs2/plugins at boot and hosts the first
 * FX plugin in the master insert slot (master_suite.cc) instead of the built-in stereo delay.
 *
 * Boot (main thread, from the tape mount hook: the filesystem is up, the audio may already run): the directory is
 * listed first (names only, no I/O while it is open: kennel kn_boot.c), then each file is read whole into a staging
 * buffer from heap 0, parsed and checked (plugin_load.cc, kennel's loader with the magic COOP), relocated into its
 * own heap-0 block (never freed: plugins live until power-off), its entry object checked (magic, api) and its
 * init called. A file that fails any step is skipped with a reason kept for the stats; the boot goes on. The
 * audio interrupt sees the plugin only once `g_fx_plugin` is set, after init.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_plugin.h"
#include "plugin_load.h"
#include "coop_plugin_host.h"

#define Y_OPENDIR  ((void *(*)(const char *))0x01A9B450u)               /* kennel kn_boot.c */
#define Y_READDIR  ((const uint8_t *(*)(void *))0x01A9B648u)             /* dirent: +0x0B d_name */
#define Y_CLOSEDIR ((int (*)(void *))0x01A9B74Cu)
#define Y_OPEN     ((int (*)(const char *, int, int))0x01A997F4u)
#define Y_CLOSE    ((int (*)(int))0x01A99908u)
#define Y_READ     ((int (*)(int, void *, int))0x01A99C90u)
#define MALLOC     ((void *(*)(uint32_t))0x01A9F348u)                   /* heap 0 (kennel kn_boot.c) */
#define FREE       ((void (*)(void *))0x01AA0040u)

#define DIR_PLUGINS "/yaffs2/plugins"
#define MAX_FILES   8
#define NAME_MAX    40
#define FILE_MAX    0x00100000u                                           /* 1 MiB, as kennel */
#define ARENA_ALIGN 32u

extern "C" const kn_export coop_exports[];                                /* sdk/coop_exports.cc */
extern "C" const int coop_exports_count;

volatile uint32_t g_plugins_seen, g_plugins_loaded, g_plugins_failed, g_plugins_booted;
const char *volatile g_plugins_last_error;                                /* the last refused file's reason */
char g_plugins_last_name[NAME_MAX];
const coop_fx_t *volatile g_fx_plugin;                                   /* the master slot's FX, 0 = built-in delay */
int16_t g_fx_plugin_params[4];

static char g_names[MAX_FILES][NAME_MAX];
static int g_n_names;

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int ends_with(const char *s, const char *e) { int a = slen(s), b = slen(e); if (a <= b) return 0; for (int i = 0; i < b; i++) if (s[a - b + i] != e[i]) return 0; return 1; }
static void scopy(char *d, const char *s, int cap) { int i = 0; for (; i < cap - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static char *scat(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }

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

static const char *load_one(const char *name, uint8_t *stage)
{
    char path[64];
    scat(scat(scat(path, DIR_PLUGINS), "/"), name);
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
    if (in.kind != KN_KIND_FX) return "not an FX plugin (P1 hosts effects only)";
    if (in.flags) return "plugin flags not supported yet";
    if (g_fx_plugin) return "the master slot already has a plugin";
    uint32_t cap = in.load_size + 3u + KN_STUB_SIZE * in.n_imports + ARENA_ALIGN;
    uint8_t *raw = (uint8_t *)MALLOC(cap);
    if (!raw) return "plugin memory full";
    uint32_t base = ((uint32_t)(uintptr_t)raw + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1);
    uint32_t room = cap - (base - (uint32_t)(uintptr_t)raw);
    uint32_t used = 0;
    if (kn_relocate_far(stage, &in, (uint8_t *)(uintptr_t)base, base, room, coop_exports, coop_exports_count, &err, &used)) {
        FREE(raw);
        return err;
    }
    const coop_fx_t *fx = (const coop_fx_t *)(uintptr_t)(base + in.entry_offset);
    if (fx->magic != COOP_FX_MAGIC) { FREE(raw); return "entry object is not a coop FX (magic)"; }
    if (fx->api != COOP_PLUGIN_API) { FREE(raw); return "entry object built for another API"; }
    uint32_t text_end = base + in.text_size;
    uint32_t pi = (uint32_t)(uintptr_t)fx->init, pr = (uint32_t)(uintptr_t)fx->render;
    if (pi < base || pi >= text_end || pr < base || pr >= text_end || (pi & 1) || (pr & 1)) { FREE(raw); return "callbacks outside the plugin's code"; }
    for (int i = 0; i < 4; i++) g_fx_plugin_params[i] = in.defaults[i];
    if (fx->init(0) != 0) { FREE(raw); return "init refused"; }
    __asm__ __volatile__("ssync;" ::: "memory");
    g_fx_plugin = fx;                                                     /* the audio thread may use it from here */
    return 0;
}

/* From the tape mount hook (main thread, boot), once. */
extern "C" void coop_plugins_boot(void)
{
    if (g_plugins_booted) return;
    g_plugins_booted = 1;
    list_dir();
    if (!g_n_names) return;
    uint8_t *stage = (uint8_t *)MALLOC(FILE_MAX);
    if (!stage) { g_plugins_last_error = "no staging memory"; return; }
    for (int i = 0; i < g_n_names; i++) {
        g_plugins_seen++;
        const char *err = load_one(g_names[i], stage);
        if (err) { g_plugins_failed++; g_plugins_last_error = err; scopy(g_plugins_last_name, g_names[i], NAME_MAX); }
        else g_plugins_loaded++;
    }
    FREE(stage);
}

extern "C" int coop_fx_plugin_loaded(void) { return g_fx_plugin != 0; }

/* From the master insert slot (audio interrupt): 1 if a plugin rendered the block. */
extern "C" int coop_fx_plugin_render(int32_t *l, int32_t *r, int n)
{
    const coop_fx_t *fx = g_fx_plugin;
    if (!fx) return 0;
    fx->render(0, l, r, g_fx_plugin_params, n);
    return 1;
}
