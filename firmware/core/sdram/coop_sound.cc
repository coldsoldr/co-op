/*
 * coop-fw u5: per-sound coop data inside the stock sound JSON (docs/ui-features.md: bend §2, drum pan §3, and later
 * FX 2 / LFO 2 per sound).
 *
 * The stock's parse_patch_json (0x01A037A8) rebuilds a sound's JSON from the keys it knows and then assigns the
 * fresh Value over the slot's (CALL operator= 0x019BDDC0 at 0x01A044A8): any other member is dropped there. coop
 * retargets that CALL (region-C trampoline +176, tools/l1_stereo_insert.py) at coop_keep, which copies the source's
 * "coop" object member into the fresh Value before the assign, exactly as kennel's dp_keep_op1r keeps "op1r"
 * (hundekraft 3.1.4 kn_drum.c), then reads the member into a per-slot cache. Saves serialise the whole Value, so
 * the member travels with slots, user presets, lift / drop; a sound without it (factory, old) gets the defaults.
 *
 * The jsoncpp-like Value (../te-op1-cfw/docs/screen-interface.md §5; kennel dp_fw.c): 16 bytes, payload at +0, type
 * at +8 (0 null, 1 int, 2 uint, 3 real, 4 string, 5 bool, 6 array, 7 object). Writes go through operator[] (creates),
 * Value(int) into a temp, operator=, and the temp's destructor; then a stock setter call marks the patch dirty so the
 * stock's background writer saves it (Patch::SetAdsr with the live words: nothing else changes).
 *
 * Member layout: "coop": { "bend": -12..12 (semitones; default 12 = the stock bend's range), "bend_t": 0..128
 * (OFF, 1..127, MAX; default 0 = the stock's own slew), "pan": [24 ints, -50..50] (drums; default 0) }.
 *
 * Threads: coop_keep on the main thread (the loaders), the setters on the KeyHandler thread (as the stock setters).
 * No libc, fixed point only.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>

#define JV_KEY      ((void *(*)(void *, const char *))0x019BFEB4u)             /* Value::operator[](const char *): creates */
#define JV_IDX      ((void *(*)(void *, unsigned))0x019BFAF8u)                 /* Value::operator[](ArrayIndex): creates */
#define JV_GET      ((const void *(*)(const void *, const char *))0x019BEB70u) /* const get: the static null when absent */
#define JV_GET_IDX  ((const void *(*)(const void *, unsigned))0x019BEA94u)     /* const index */
#define JV_ASINT    ((int (*)(const void *))0x019BE0C0u)
#define JV_ASSIGN   ((void *(*)(void *, const void *))0x019BDDC0u)             /* Value::operator= */
#define JV_FROM_INT ((void *(*)(void *, int))0x019BD890u)                      /* Value(int) */
#define JV_DTOR     ((void (*)(void *, int))0x019BDD18u)
#define SET_ADSR    ((void (*)(void *, const int16_t *))0x01A00C98u)           /* Patch::SetAdsr: marks the patch dirty */
#define ADSR(i)     ((volatile int16_t *)(0x01B60EF0u + (uint32_t)(i) * 16))
#define SET_DYNA    ((void (*)(void *, const int16_t *))0x01A0005Cu)           /* the drum's dyna_env setter: marks the kit dirty */
#define DYNA(i)     ((volatile int16_t *)(0xFF801544u + (uint32_t)(i) * 16))
#define PATCH_S     0x01DDD7E8u                                                /* the Patch objects: JSON Value at +4 */
#define PATCH_D     0x01DDD9A8u
#define PATCH_SZ    0x38u
#define JV_OBJECT   7
#define JV_ARRAY    6
#define JV_INT      1
#define JV_UINT     2
#define KEY         "coop"
#define PAN_N       24

struct coop_sound { int16_t bend, bend_t; int8_t pan[PAN_N]; uint8_t has; };
static struct coop_sound s_cache[2][8];                                        /* [kind][slot]: 0 synth, 1 drum */
volatile uint32_t g_sound_keeps, g_sound_kept, g_sound_writes;                 /* stats / tests */

static uint8_t jv_type(const void *v) { return ((const uint8_t *)v)[8]; }
static int jv_int(const void *v, int dflt) { uint8_t t = jv_type(v); return t == JV_INT || t == JV_UINT ? JV_ASINT(v) : dflt; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void defaults(struct coop_sound *c) { c->bend = 12; c->bend_t = 0; for (int i = 0; i < PAN_N; i++) c->pan[i] = 0; c->has = 0; }
static int s_inited;
static void init_all(void) { if (s_inited) return; for (int k = 0; k < 2; k++) for (int i = 0; i < 8; i++) defaults(&s_cache[k][i]); s_inited = 1; }
static void read_into(struct coop_sound *c, const void *json)
{
    defaults(c);
    const void *o = JV_GET(json, KEY);
    if (jv_type(o) != JV_OBJECT) return;
    c->has = 1;
    c->bend = (int16_t)clampi(jv_int(JV_GET(o, "bend"), 12), -12, 12);
    c->bend_t = (int16_t)clampi(jv_int(JV_GET(o, "bend_t"), 0), 0, 128);
    const void *a = JV_GET(o, "pan");
    if (jv_type(a) == JV_ARRAY) for (int i = 0; i < PAN_N; i++) c->pan[i] = (int8_t)clampi(jv_int(JV_GET_IDX(a, (unsigned)i), 0), -50, 50);
}
static int slot_of(const void *json, int *kind)                                /* the Value is the Patch object + 4 */
{
    uint32_t p = (uint32_t)(uintptr_t)json;
    if (p >= PATCH_S + 4 && p < PATCH_S + 4 + 8 * PATCH_SZ && (p - PATCH_S - 4) % PATCH_SZ == 0) { *kind = 0; return (int)((p - PATCH_S - 4) / PATCH_SZ); }
    if (p >= PATCH_D + 4 && p < PATCH_D + 4 + 8 * PATCH_SZ && (p - PATCH_D - 4) % PATCH_SZ == 0) { *kind = 1; return (int)((p - PATCH_D - 4) / PATCH_SZ); }
    return -1;
}

/* the retargeted CALL in parse_patch_json: keep "coop", assign, cache (main thread) */
extern "C" void *coop_keep(void *patch, void *fresh)
{
    g_sound_keeps++;
    init_all();
    void *old = JV_KEY(patch, KEY);                                            /* adds a null member if absent: patch is overwritten next */
    if (jv_type(old) == JV_OBJECT) { JV_ASSIGN(JV_KEY(fresh, KEY), old); g_sound_kept++; }
    void *r = JV_ASSIGN(patch, fresh);
    int kind, slot = slot_of(patch, &kind);
    if (slot >= 0) read_into(&s_cache[kind][slot], patch);
    return r;
}

extern "C" const struct coop_sound *coop_sound_get(int kind, int slot)
{
    static struct coop_sound none;
    init_all();                                                                /* a slot never loaded through the hook: the defaults */
    if (kind < 0 || kind > 1 || slot < 0 || slot > 7) { defaults(&none); return &none; }
    return &s_cache[kind][slot];
}
static void put_int(void *obj, const char *name, int v)
{
    uint32_t tmp[4];
    JV_FROM_INT(tmp, v);
    JV_ASSIGN(JV_KEY(obj, name), tmp);
    JV_DTOR(tmp, 2);
}
static void write_out(int kind, int slot)                                      /* KeyHandler thread, as the stock setters */
{
    init_all();
    struct coop_sound *c = &s_cache[kind][slot];
    uint32_t pobj = (kind ? PATCH_D : PATCH_S) + (uint32_t)slot * PATCH_SZ;
    void *json = (void *)(uintptr_t)(pobj + 4);
    void *o = JV_KEY(json, KEY);
    put_int(o, "bend", c->bend);
    put_int(o, "bend_t", c->bend_t);
    void *a = JV_KEY(o, "pan");
    for (int i = 0; i < PAN_N; i++) { uint32_t tmp[4]; JV_FROM_INT(tmp, c->pan[i]); JV_ASSIGN(JV_IDX(a, (unsigned)i), tmp); JV_DTOR(tmp, 2); }
    c->has = 1;
    int16_t v[8];
    if (kind == 0) { for (int k = 0; k < 8; k++) v[k] = ADSR(slot)[k]; SET_ADSR((void *)(uintptr_t)pobj, v); }   /* dirty, nothing changed */
    else { for (int k = 0; k < 8; k++) v[k] = DYNA(slot)[k]; SET_DYNA((void *)(uintptr_t)pobj, v); }
    g_sound_writes++;
}
extern "C" void coop_sound_set_bend(int slot, int bend, int bend_t)
{
    if (slot < 0 || slot > 7) return;
    s_cache[0][slot].bend = (int16_t)clampi(bend, -12, 12);
    s_cache[0][slot].bend_t = (int16_t)clampi(bend_t, 0, 128);
    write_out(0, slot);
}
extern "C" void coop_sound_set_pan(int kit, int voice, int pan)
{
    if (kit < 0 || kit > 7 || voice < 0 || voice >= PAN_N) return;
    s_cache[1][kit].pan[voice] = (int8_t)clampi(pan, -50, 50);
    write_out(1, kit);
}
