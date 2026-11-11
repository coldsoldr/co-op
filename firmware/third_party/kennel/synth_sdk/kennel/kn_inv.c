/*
 * Inventory for the workshop (kn_inv.h, docs/inventory.md): records made at boot, read as text by the devupd LIST
 * command (devupd/du.c, du_fw.c). The text is built once, on the first LIST, in the arena.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include "kn_inv.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-tree-loop-distribute-patterns")          /* the firmware has no memcpy / memset */
#endif
#include "kn_load.h"
#include "../kn_mem.h"

void *kn_alloc(uint32_t size);
uint32_t du_crc32(uint32_t crc, const uint8_t *p, uint32_t n);
extern volatile uint32_t kn_next __attribute__((weak));          /* kn_boot.c; absent in host tests */
extern volatile uint32_t kn_arena_lo __attribute__((weak)), kn_arena_hi __attribute__((weak));   /* (with kn_next) */
extern volatile uint32_t kn_decl_used __attribute__((weak));
extern const uint8_t _core_ro_end[] __attribute__((weak));      /* link.ld __core_ro_end (C names get a leading _) */
#define KN_CORE_START 0x01E00000u

/* user synth presets, for the page (an update to a build with a smaller cap would stop R246's boot on the
 * patch.cpp:632 assert): R246's count of user synthesis presets (0x018B610C, preset counts at 0x018B6108, recounted
 * from the DB) and the cap of its save gate 0x019FEEF8, `R1 = cap (X)` at 0x019FEF02 (0xE121 then the immediate:
 * 100 in R246, 300 in the full project's 300 builds). Host tests point these at their own words. */
#ifdef OP1_HOST
const volatile uint32_t *kn_preset_count_at;
const volatile uint16_t *kn_preset_gate_at;
#else
const volatile uint32_t *kn_preset_count_at = (const volatile uint32_t *)0x018B610Cu;
const volatile uint16_t *kn_preset_gate_at = (const volatile uint16_t *)0x019FEF02u;
#endif
#define PRESET_DIGITS 5

kn_inv_rec *kn_inv_cur;
static kn_inv_rec *head, *tail;
static char *text;                                                /* built on the first read */
static uint32_t text_len;

static int same(const char *a, const char *b) { if (!a || !b) return 0; while (*a && *a == *b) { a++; b++; } return *a == *b; }

const char *kn_inv_str(const char *s, int max)
{
    if (!s) return 0;
    int n = 0;
    while (s[n] && n < max - 1) n++;
    char *d = kn_alloc((uint32_t)n + 1);
    if (!d) return 0;
    for (int i = 0; i < n; i++) d[i] = s[i];
    d[n] = 0;
    return d;
}

const char *kn_inv_list(const char *list, int max)
{
    if (!list) return 0;
    int n = 0;
    while (n < max - 1 && (list[n] || (n && list[n - 1]))) n++;   /* up to the empty string that ends it */
    char *d = kn_alloc((uint32_t)n + 2);
    if (!d) return 0;
    for (int i = 0; i < n; i++) d[i] = list[i];
    d[n] = 0; d[n + 1] = 0;
    return d;
}

kn_inv_rec *kn_inv_add(int source, const char *path)
{
    kn_inv_rec *r = kn_alloc(sizeof *r);
    if (!r) return 0;
    r->source = (uint8_t)source;
    r->path = path ? kn_inv_str(path, 64) : 0;
    if (tail) tail->next = r; else head = r;
    tail = r;
    text = 0;                                                     /* (only at boot; rebuilt if read before) */
    return r;
}

static kn_inv_rec *find(const op1_feature_t *f)
{
    for (kn_inv_rec *r = head; r; r = r->next) if (r->f == f) return r;
    return 0;
}

void kn_inv_feature_result(const op1_feature_t *f, int r, int builtin)
{
    kn_inv_rec *rec = builtin ? kn_inv_add(KN_INV_BUILTIN, 0) : kn_inv_cur;
    if (!rec || !f) return;
    rec->kind = KN_KIND_FEATURE;
    rec->f = f;
    if (f->name && !rec->name[0]) for (int i = 0; i < 15 && f->name[i]; i++) rec->name[i] = f->name[i];
    if (!builtin) return;                                         /* a file's state is set by the loader */
    rec->state = r == 0 ? KN_INV_LOADED : r == -1 ? KN_INV_FAILED : KN_INV_OFF;
    rec->reason = r == -1 ? "refused (built for a different core, or too many features)" : r == -2 ? "switched off"
                : r == -3 ? "replaced by a file" : 0;
    if (r == -3)                                                  /* mark the file that took its place */
        for (kn_inv_rec *x = head; x; x = x->next)
            if (x != rec && x->source != KN_INV_BUILTIN && x->kind == KN_KIND_FEATURE && same(x->name, rec->name))
                x->flags |= KN_INV_REPLACES;
}

void kn_inv_lost(const op1_feature_t *loser, const char *claim, const char *winner)
{
    kn_inv_rec *r = find(loser);
    if (!r) return;
    r->state = KN_INV_OFF;
    r->lost_claim = claim; r->lost_to = winner ? winner : "?";
    text = 0;
}

/* ---- text: header line, then one line per record, fields separated by tabs (docs/inventory.md) ---- */
typedef struct { char *o; uint32_t n; } out_t;
static void put_c(out_t *w, char c) { if (w->o) w->o[w->n] = c; w->n++; }
static void put_s(out_t *w, const char *s)                        /* tabs / newlines / non-ASCII -> space */
{
    for (; s && *s; s++) put_c(w, (*s == '\t' || *s == '\n' || *s == '\r' || (unsigned char)*s >= 0x7F) ? ' ' : *s);
}
static void put_u(out_t *w, uint32_t v)
{
    char t[12]; int n = 0;
    do t[n++] = (char)('0' + v % 10); while (v /= 10);
    while (n) put_c(w, t[--n]);
}
static void put_x(out_t *w, uint32_t v) { for (int i = 7; i >= 0; i--) put_c(w, "0123456789abcdef"[v >> (i * 4) & 15]); }

static void put_claim(out_t *w, const kn_inv_rec *r, const char *c, int *first)
{
    if (!*first) put_c(w, ',');
    *first = 0;
    put_c(w, r->lost_claim && same(r->lost_claim, c) ? '!' : r->state == KN_INV_LOADED ? '+' : '~');
    put_s(w, c);
}

static void put_rec(out_t *w, const kn_inv_rec *r)
{
    static const char *const kinds[] = { "-", "synth", "fx", "feature" };
    static const char *const sources[] = { "builtin", "knl", "op1p" };
    static const char *const states[] = { "loaded", "failed", "skipped", "off" };
    put_s(w, kinds[r->kind <= 3 ? r->kind : 0]); put_c(w, '\t');
    put_s(w, r->name); put_c(w, '\t');
    put_s(w, sources[r->source <= 2 ? r->source : 0]); put_c(w, '\t');
    put_s(w, r->path); put_c(w, '\t');
    put_s(w, r->version); put_c(w, '\t');
    put_s(w, r->author); put_c(w, '\t');
    put_u(w, r->size); put_c(w, '\t');
    put_x(w, r->crc); put_c(w, '\t');
    put_s(w, states[r->state <= 3 ? r->state : 1]); put_c(w, '\t');
    int first = 1;                                                /* claims: the file's list, else the feature's */
    if (r->claims) for (const char *c = r->claims; *c; ) { put_claim(w, r, c, &first); while (*c) c++; c++; }
    else if (r->f && r->f->claims) for (const char *const *c = r->f->claims; *c; c++) put_claim(w, r, *c, &first);
    put_c(w, '\t');
    if (r->lost_claim) { put_s(w, "lost "); put_s(w, r->lost_claim); put_s(w, " to "); put_s(w, r->lost_to); }
    else if (r->flags & KN_INV_REPLACES && !r->reason) put_s(w, "replaces the built-in");
    else put_s(w, r->reason);
    put_c(w, '\n');
}

/* the running core's identity: CRC-32 of .text + .rodata as loaded (0x01E00000.. __core_ro_end). The page computes the
 * same over its build's OP1_vdk.ldr blocks: equal = the same core, so plugin files alone can be sent. Once per boot. */
static uint32_t core_len, core_crc;
static uint32_t presets_at;                                       /* offset of the count's digits in the text */
static void put_d5(out_t *w, uint32_t v)                          /* fixed width: rewritten in place (kn_inv_read) */
{
    char d[PRESET_DIGITS];
    for (int i = PRESET_DIGITS - 1; i >= 0; i--) { d[i] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < PRESET_DIGITS; i++) put_c(w, d[i]);
}
static uint32_t preset_count(void) { uint32_t n = *kn_preset_count_at; return n > 99999 ? 99999 : n; }
static void put_all(out_t *w)
{
    if (!core_len && _core_ro_end && (uintptr_t)_core_ro_end > KN_CORE_START) {
        core_len = (uint32_t)((uintptr_t)_core_ro_end - KN_CORE_START);
        core_crc = du_crc32(0, (const uint8_t *)KN_CORE_START, core_len);
    }
    put_s(w, "kennel-inventory 1"); put_c(w, '\t'); put_s(w, "core "); put_x(w, KN_CORE_START); put_c(w, ' '); put_x(w, core_len);
    put_c(w, ' '); put_x(w, core_crc);
    if (kn_preset_count_at && kn_preset_gate_at && kn_preset_gate_at[0] == 0xE121) {   /* "presets 00112 cap 00300" */
        put_c(w, '\t'); put_s(w, "presets "); presets_at = w->n; put_d5(w, preset_count());
        put_s(w, " cap "); put_d5(w, kn_preset_gate_at[1]);
    } else presets_at = 0;
    if (&kn_next) {                                               /* "memory arena <used> <room> decl <used> <room>" */
        put_c(w, '\t'); put_s(w, "memory arena "); put_x(w, kn_next - kn_arena_lo); put_c(w, ' '); put_x(w, kn_arena_hi - kn_arena_lo);
        put_s(w, " decl "); put_x(w, &kn_decl_used ? kn_decl_used : 0); put_c(w, ' '); put_x(w, KN_DECL_HI - KN_DECL_LO);
    }
    put_c(w, '\n');
    for (const kn_inv_rec *r = head; r; r = r->next) put_rec(w, r);
}

int kn_inv_read(uint32_t off, uint8_t *out, int cap, uint32_t *total)
{
    if (!text) {
        out_t w = { 0, 0 };
        put_all(&w);                                              /* measure, then write */
        char *t = kn_alloc(w.n + 1);
        if (!t) { *total = 0; return -1; }
        w.o = t; w.n = 0;
        put_all(&w);
        t[w.n] = 0; text_len = w.n; text = t;
    }
    if (off == 0 && presets_at) {                                 /* a new LIST: today's count (saves change it) */
        out_t w = { text + presets_at, 0 };
        put_d5(&w, preset_count());
    }
    *total = text_len;
    if (off > text_len) return -1;
    int n = text_len - off < (uint32_t)cap ? (int)(text_len - off) : cap;
    for (int i = 0; i < n; i++) out[i] = (uint8_t)text[off + i];
    return n;
}
