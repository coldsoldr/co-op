/*
 * coop's bridge, Opus side (bridge.h, docs/bridge.md): an .opmod (Opus 2's plugin file, format 1) checked whole before
 * anything is written, and the verdict on what it adds and imports. The checks are those of kennel's bridge loader
 * (hundekraft 3.3.1 synth_sdk/opus/opus_load.c ol_check, Copyright (C) 2026 Hundekraft, GPL-3.0-or-later with the OP-1
 * firmware permission), itself written from the Opus plugin kit's documented format; adapted here: coop's names and
 * verdict, no placement yet (B6). Places (patches of R246 code) are refused, as there. No libc.
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include "bridge.h"
extern "C" {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-tree-loop-distribute-patterns")
#endif

#define OPMOD_MAGIC       0x444D504Fu      /* "OPMD" */
#define OPMOD_HDR         0x60u
#define OPMOD_MAX_IMAGE   (1u << 20)        /* image and zero fill, each */
#define OPMOD_F_CODE      1u
#define OPMOD_F_APART     8u
#define OPMOD_MAX_IMPORTS 96u
#define REL_ZERO          (1u << 30)

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int inside(uint32_t off, uint32_t n, uint32_t size) { return off <= size && n <= size - off; }

static uint32_t crc32_skip(const uint8_t *d, uint32_t n, uint32_t skip)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= (i >= skip && i < skip + 4) ? 0 : d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static int name_ok(const br_opmod *f, uint32_t o)             /* a NUL-terminated, non-empty name inside the strings */
{
    const uint8_t *s = f->d + f->str_off;
    if (o >= f->str_len) return 0;
    for (uint32_t e = o; e < f->str_len; e++) if (!s[e]) return e > o;
    return 0;
}

const char *br_opmod_check(const uint8_t *d, uint32_t size, br_opmod *f)
{
    if (size < OPMOD_HDR || rd32(d) != OPMOD_MAGIC) return "not an Opus plugin file (magic)";
    if (rd16(d + 4) != 1 || rd16(d + 6) < OPMOD_HDR) return "a newer file format";
    if (rd32(d + 8) != size) return "damaged: size";
    f->crc = rd32(d + 12);
    if (crc32_skip(d, size, 12) != f->crc) return "damaged: crc";
    uint32_t w[20];
    for (int i = 0; i < 20; i++) w[i] = rd32(d + 0x10 + 4 * i);
    f->d = d; f->size = size;
    f->man_off = w[0]; f->man_len = w[1]; f->img_off = w[2]; f->img_len = w[3]; f->zero = w[4]; f->entry = w[5];
    f->imp_off = w[6]; f->imp_n = w[7]; f->rel_off = w[8]; f->rel_n = w[9];
    f->str_off = w[12]; f->str_len = w[13]; f->flags = w[14]; f->exp_off = w[16]; f->exp_n = w[17]; f->pl_n = w[19];
    if (!inside(w[0], w[1], size) || !inside(w[2], w[3], size) || w[7] > size / 4 || !inside(w[6], 4 * w[7], size) ||
        w[9] > size / 12 || !inside(w[8], 12 * w[9], size) || w[11] > size / 16 || !inside(w[10], 16 * w[11], size) ||
        !inside(w[12], w[13], size) || w[17] > size / 8 || !inside(w[16], 8 * w[17], size) || w[19] > size / 24 ||
        !inside(w[18], 24 * w[19], size))
        return "damaged: a table outside the file";
    if (!f->img_len || !(f->flags & OPMOD_F_CODE)) return "has no code";
    if (f->img_len > OPMOD_MAX_IMAGE || f->zero > OPMOD_MAX_IMAGE || f->img_len < 4 || f->entry > f->img_len - 4 ||
        (f->entry & 1))
        return "damaged: image";
    if (f->imp_n > OPMOD_MAX_IMPORTS) return "too many imports";
    for (uint32_t i = 0; i < f->imp_n; i++) if (!name_ok(f, rd32(d + f->imp_off + 4 * i))) return "damaged: names";
    for (uint32_t i = 0; i < f->exp_n; i++)
        if (!name_ok(f, rd32(d + f->exp_off + 8 * i)) || rd32(d + f->exp_off + 8 * i + 4) >= f->img_len)
            return "damaged: exports";
    for (uint32_t i = 0; i < f->rel_n; i++) {                   /* every relocation before any byte is written */
        const uint8_t *r = d + f->rel_off + 12 * i;
        const uint32_t word = rd32(r), tgt = rd32(r + 4), val = rd32(r + 8), site = word & 0x0FFFFFFFu, kind = (word >> 28) & 3;
        const int imported = (int)(word >> 31), inz = !!(word & REL_ZERO);
        const uint32_t width = kind == 0 ? 4 : 2;
        if (site + width > f->img_len || (kind == 3 && site < 2) || (site & 1)) return "damaged: a relocation";
        if (imported ? tgt >= f->imp_n : (int32_t)val < 0 || val > f->img_len + f->zero) return "damaged: a relocation";
        if (inz && (imported || kind == 3 || !(f->flags & OPMOD_F_APART))) return "damaged: a zero-fill mark";
        if (kind == 3) return "a branch relocation (this core places images anywhere)";
    }
    return 0;
}

const char *br_opmod_import(const br_opmod *f, uint32_t i)
{
    return (const char *)f->d + f->str_off + rd32(f->d + f->imp_off + 4 * i);
}

const char *br_opmod_line(const br_opmod *f, const char *key, int n, uint32_t *len)
{
    const char *m = (const char *)f->d + f->man_off, *end = m + f->man_len;
    int kl = 0;
    while (key[kl]) kl++;
    for (const char *p = m; p < end;) {
        const char *e = p;
        while (e < end && *e != '\n') e++;
        int k = 0;
        while (k < kl && p + k < e && p[k] == key[k]) k++;
        if (k == kl && p + k < e && (p[k] == ' ' || p[k] == '\t')) {
            const char *v = p + k;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            const char *ve = e;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r')) ve--;
            if (!n--) { *len = (uint32_t)(ve - v); return v; }
        }
        p = e + 1;
    }
    *len = 0;
    return 0;
}

static void set(br_verdict *v, int phase, const char *reason, const char *needs, uint32_t nlen)
{
    if (v->phase == BR_PHASE_NEVER && v->reason) return;
    if (phase != BR_PHASE_NEVER && phase <= v->phase) return;
    v->phase = phase; v->reason = reason;
    uint32_t i = 0;
    for (; needs && i < nlen && i < sizeof v->needs - 1; i++) v->needs[i] = needs[i];
    v->needs[i] = 0;
}
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static int starts(const char *v, uint32_t len, const char *word)
{
    uint32_t n = slen(word);
    if (len < n) return 0;
    for (uint32_t i = 0; i < n; i++) if (v[i] != word[i]) return 0;
    return len == n || v[n] == ' ' || v[n] == '\t';
}

int br_opmod_verdict(const br_opmod *f, br_verdict *v)
{
    uint32_t len;
    v->dialect = BR_OPMOD; v->kind = 0; v->phase = 0; v->reason = 0; v->needs[0] = 0; v->name[0] = 0;
    v->load_size = f->img_len + f->zero; v->flags = f->flags;
    const char *id = br_opmod_line(f, "id", 0, &len);
    for (uint32_t i = 0; id && i < len && i < sizeof v->name - 1; i++) { v->name[i] = id[i]; v->name[i + 1] = 0; }
    if (!br_opmod_line(f, "core", 0, &len)) { set(v, BR_PHASE_NEVER, "its manifest has no core line", "core", 4); return 0; }
    if (f->pl_n) { set(v, BR_PHASE_NEVER, "it patches the firmware's code (places)", "place", 5); return 0; }
    if (f->exp_n) { set(v, BR_PHASE_NEVER, "a library other files link against", "exports", 7); return 0; }
    if (br_opmod_line(f, "needs", 0, &len)) {
        const char *lib = br_opmod_line(f, "needs", 0, &len);
        set(v, BR_PHASE_NEVER, "needs another plugin file (a library)", lib, len);
        return 0;
    }
    int adds = 0;
    for (int n = 0;; n++) {
        const char *a = br_opmod_line(f, "adds", n, &len);
        if (!a) break;
        adds++;
        if (starts(a, len, "effect")) { if (!v->kind) v->kind = KN_KIND_FX; set(v, 6, 0, "", 0); }
        else if (starts(a, len, "engine")) { v->kind = KN_KIND_SYNTH; set(v, 7, 0, "", 0); }
        else { set(v, BR_PHASE_NEVER, "adds something coop has no place for", a, len); return 0; }
    }
    if (!adds) { set(v, BR_PHASE_NEVER, "adds no effect or engine (a feature of Opus's own core)", "adds", 4); return 0; }
    for (uint32_t i = 0; i < f->imp_n; i++) {
        const char *name = br_opmod_import(f, i);
        const br_import *e = br_import_find(BR_OPMOD, name);
        if (!e || e->loads == BR_PHASE_NEVER) { set(v, BR_PHASE_NEVER, "needs an import the bridge does not provide", name, slen(name)); return 0; }
        if (e->loads > v->phase) set(v, e->loads, "needs an import a later bridge phase provides", name, slen(name));
    }
    return v->phase;
}
}   /* extern "C" */
