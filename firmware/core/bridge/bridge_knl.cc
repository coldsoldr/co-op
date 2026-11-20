/*
 * coop's bridge, kennel side (bridge.h, docs/bridge.md): the import table, a .knl's header made coop's dialect, the
 * verdict on its kind, flags and imports, the export table the loader relocates it against, and the audit of the
 * firmware addresses its code carries. No firmware calls: the host test runs this file as it is.
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include "bridge.h"
#include "../../sdk/coop_plugin.h"
extern "C" {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-tree-loop-distribute-patterns")   /* the firmware has no memcpy / memset */
#endif

#define BR_ADDR(a, n, note)
#define BR_IMPORT(d, n, loads, full, kind, shim, note) { n, BR_##d, loads, full, BR_K_##kind, (uintptr_t)(shim), note },
const br_import br_imports[] = {
#include "bridge_imports.def"
};
#undef BR_IMPORT
#undef BR_ADDR
const int br_imports_count = (int)(sizeof br_imports / sizeof br_imports[0]);

#define BR_IMPORT(d, n, loads, full, kind, shim, note)
#define BR_ADDR(a, n, note) { a, n },
const br_addr br_addrs[] = {
#include "bridge_imports.def"
};
#undef BR_IMPORT
#undef BR_ADDR
const int br_addrs_count = (int)(sizeof br_addrs / sizeof br_addrs[0]);

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void copy(char *d, const char *s, int cap) { int i = 0; for (; s && s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; }

static uint32_t crc32_file(const uint8_t *d, uint32_t n, uint32_t crc_at)   /* plugin_load.cc's: the CRC word as 0 */
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= (i >= crc_at && i < crc_at + 4) ? 0 : d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

const br_import *br_import_find(int dialect, const char *name)
{
    for (int i = 0; i < br_imports_count; i++)
        if (br_imports[i].dialect == dialect && streq(br_imports[i].name, name)) return &br_imports[i];
    return 0;
}

int br_addr_known(uint32_t addr)
{
    for (int i = 0; i < br_addrs_count; i++) if (br_addrs[i].addr == addr) return 1;
    return 0;
}

/* header offsets (plugin_load.cc's enum; third_party/kennel/docs/plugin-format.md) */
enum { O_FILE_SIZE = 8, O_CRC = 12, O_API = 16, O_KIND = 18, O_NAME = 20, O_FLAGS = 36, O_TEXT = 56, O_BSS_OFF = 76,
       O_BSS_SIZE = 80, O_IMAGE_SIZE = 84, O_IMAGE_FILE = 100, O_RELOC = 104, O_RELOC_N = 108, O_IMPORT = 112,
       O_IMPORT_N = 116, O_STR = 120, O_STR_SIZE = 124, O_EXT = 140 };

static int is_knpl(const uint8_t *p) { return p[0] == 'K' && p[1] == 'N' && p[2] == 'P' && p[3] == 'L'; }

static int64_t knl_header(const uint8_t *file, uint32_t size)   /* plugin_load.cc header_offset, kennel's magic */
{
    if (size >= 8 && is_knpl(file)) return 0;
    if (!size || file[0] != '<') return -1;
    uint32_t end = 0;
    while (end < size && file[end]) end++;
    if (end == size) return -1;
    uint64_t h = ((uint64_t)end + 4) & ~3ull;
    return h + 8 <= size && is_knpl(file + h) ? (int64_t)h : -1;
}

static void later(br_verdict *v, int phase, const char *reason, const char *needs)   /* the latest phase wins; never wins over all */
{
    if (v->phase == BR_PHASE_NEVER && v->reason) return;
    if (phase == BR_PHASE_NEVER || phase > v->phase) {
        v->phase = phase;
        v->reason = reason;
        copy(v->needs, needs, (int)sizeof v->needs);
    }
}

static const char *const R_FEATURE = "a kennel feature: it hooks kennel's own core, which coop does not have";
static const char *const R_INPUT = "an input engine: coop has no input engine path";
static const char *const R_ENGINE_EXT = "kennel engine extension (own envelope, voice-sum block, own pages, input, service)";
static const char *const R_SAMPLE = "an engine that plays the slot's sample";
static const char *const R_BIGBUF = "a declared pool buffer";
static const char *const R_FLAGS = "flags this bridge does not know";
static const char *const R_IMPORT = "needs an import the bridge does not provide";
static const char *const R_IMPORT_LATER = "needs an import a later bridge phase provides";

int br_knl_normalise(uint8_t *file, uint32_t size, br_verdict *v, const char **err)
{
    v->dialect = BR_KNL; v->kind = 0; v->phase = 0; v->reason = 0; v->needs[0] = 0; v->name[0] = 0;
    v->load_size = 0; v->flags = 0;
    int64_t ho = knl_header(file, size);
    if (ho < 0) { *err = "not a kennel plugin (magic)"; return -1; }
    uint32_t h = (uint32_t)ho;
    if ((uint64_t)h + KN_HEADER_SIZE > size || rd16(file + h + 4) != KN_FORMAT_VERSION || rd16(file + h + 6) < KN_HEADER_SIZE) {
        *err = "format version not supported"; return -1;
    }
    if (rd32(file + h + O_FILE_SIZE) != size) { *err = "file size does not match the header"; return -1; }
    if (crc32_file(file, size, h + O_CRC) != rd32(file + h + O_CRC)) { *err = "CRC mismatch"; return -1; }
    if (rd16(file + h + O_API) != BR_KNL_API) { *err = "built for a different kennel core"; return -1; }
    const int kind = file[h + O_KIND];
    const uint32_t flags = rd32(file + h + O_FLAGS);
    v->kind = kind; v->flags = flags;
    v->load_size = rd32(file + h + O_BSS_OFF) + rd32(file + h + O_BSS_SIZE);
    for (int i = 0; i < 15 && file[h + O_NAME + i]; i++) { v->name[i] = (char)file[h + O_NAME + i]; v->name[i + 1] = 0; }

    /* kind and flags: what coop has a place for, and from which phase */
    if (kind == KN_KIND_FEATURE) later(v, BR_PHASE_NEVER, R_FEATURE, "kind feature");
    else if (kind == KN_KIND_FX) {
        later(v, 2, 0, "");
        if (flags & KN_FLAG_BIGBUF) later(v, 8, R_BIGBUF, "flag bigbuf");
        if (flags & ~(KN_FLAG_PAGE2 | KN_FLAG_STEREO | KN_FLAG_BIGBUF)) later(v, BR_PHASE_NEVER, R_FLAGS, "flags");
    } else if (kind == KN_KIND_SYNTH) {
        later(v, 4, 0, "");
        if (flags & KN_FLAG_USES_SAMPLE) later(v, 8, R_SAMPLE, "flag uses_sample");
        if (flags & BR_KNF_ENGINE_EXT) later(v, 8, R_ENGINE_EXT, "flag engine extension");
        if (flags & KN_FLAG_BIGBUF) later(v, 8, R_BIGBUF, "flag bigbuf");
        if (flags & KN_FLAG_INPUT) later(v, BR_PHASE_NEVER, R_INPUT, "flag input");
        if (flags & ~(KN_FLAG_USES_SAMPLE | KN_FLAG_PAGE2 | KN_FLAG_INPUT | KN_FLAG_BIGBUF | BR_KNF_ENGINE_EXT | KN_FLAG_COARSE_MASK))
            later(v, BR_PHASE_NEVER, R_FLAGS, "flags");
    } else { *err = "unknown kind"; return -1; }
    /* a flag coop's loader would read as its own (0x20 = scratch) or refuse: the file stops here, bytes untouched */
    if (flags & ~(KN_FLAG_USES_SAMPLE | KN_FLAG_PAGE2 | KN_FLAG_STEREO | KN_FLAG_INPUT | KN_FLAG_BIGBUF | KN_FLAG_COARSE_MASK))
        return 1;
    if (v->phase == BR_PHASE_NEVER) return 1;

    /* extension table: kennel's tag 7 is its settings rows, coop's is the scratch size */
    uint32_t eo = rd32(file + h + O_EXT);
    if (eo) {
        if (eo % 4 || eo >= size) { *err = "extension table outside the file or misaligned"; return -1; }
        for (;;) {
            if ((uint64_t)eo + 4 > size) { *err = "extension table runs past the file"; return -1; }
            uint32_t tag = rd16(file + eo), n = rd16(file + eo + 2);
            if (!tag) break;
            if ((uint64_t)eo + 4 + n > size) { *err = "extension entry runs past the file"; return -1; }
            if (tag == BR_KN_EXT_SETTINGS) wr16(file + eo, BR_KN_EXT_PARKED);
            eo += 4 + ((n + 3) & ~3u);
        }
    }
    file[h] = 'C'; file[h + 1] = 'O'; file[h + 2] = 'O'; file[h + 3] = 'P';
    wr16(file + h + O_API, COOP_PLUGIN_API);
    wr32(file + h + O_CRC, crc32_file(file, size, h + O_CRC));
    return 0;
}

int br_knl_parse(const uint8_t *file, uint32_t size, kn_info *in, const char **err)
{
#ifdef KN_HAVE_PARSE_AS
    static const uint8_t knl_entry[4] = { 0, 16, 12, 84 };           /* op1_engine_t, op1_fx_t (kennel op1_plugin.h) */
    return kn_parse_as(file, size, COOP_PLUGIN_API, in, err, knl_entry);
#else
    return kn_parse(file, size, COOP_PLUGIN_API, in, err);
#endif
}

int br_knl_parse_exact(void)
{
#ifdef KN_HAVE_PARSE_AS
    return 1;
#else
    return 0;
#endif
}

int br_knl_imports(const uint8_t *file, const kn_info *in, br_verdict *v)
{
    const uint32_t h = in->hdr_off, io = rd32(file + h + O_IMPORT), n = rd32(file + h + O_IMPORT_N);
    const char *str = (const char *)file + rd32(file + h + O_STR);
    for (uint32_t i = 0; i < n; i++) {
        const char *name = str + rd32(file + io + 4 * i);            /* kn_parse checked every name */
        const br_import *e = br_import_find(BR_KNL, name);
        if (!e || e->loads == BR_PHASE_NEVER) later(v, BR_PHASE_NEVER, R_IMPORT, name);
        else if (e->loads > v->phase) later(v, e->loads, R_IMPORT_LATER, name);
    }
    return v->phase;
}

int br_knl_exports(int level, kn_export *out, int cap)
{
    int n = 0;
    for (int i = 0; i < br_imports_count && n < cap; i++) {
        const br_import *e = &br_imports[i];
        if (e->dialect != BR_KNL || !e->shim || e->loads == BR_PHASE_NEVER || e->loads > level) continue;
        out[n].name = e->name;
        out[n].addr = (uint32_t)e->shim;
        n++;
    }
    return n;
}

/* ---- the addresses a file's code carries ----
 * kennel's SDK reaches R246 through static inlines (the knob words, the position buffer table, the voice objects, the
 * screen calls by address), which are immediates in the file's code, not imports. Blackfin has no literal pools: a
 * 32-bit constant is reg.H = imm16 with reg.L = imm16 (LDIMMhalf, 0xE1xx + the immediate), so one walk of the text
 * finds them all. Instruction lengths as binutils' bfin-dis.c: 16 bits below 0xC000, else 32, and a 32-bit DSP
 * instruction with the multi-issue bit starts a 64-bit group. */
static uint32_t insn_len(uint32_t w) { return w < 0xC000 ? 2 : ((w & 0x0800) && (w & 0xE800) != 0xE800) ? 8 : 4; }

static int tight(uint32_t a)       /* R246's L1 data A, scratchpad and L1 code: a number is unlikely to land here */
{
    return (a >= 0xFF800000u && a <= 0xFF807FFFu) || (a >= 0xFFB00000u && a <= 0xFFB00FFFu) ||
           (a >= 0xFFA00000u && a <= 0xFFA13FFFu);
}

static int relocated(const uint8_t *file, const kn_info *in, uint32_t site)
{
    const uint32_t ro = rd32(file + in->hdr_off + O_RELOC), rn = rd32(file + in->hdr_off + O_RELOC_N);
    for (uint32_t i = 0; i < rn; i++) if (rd32(file + ro + 12 * i) == site) return 1;
    return 0;
}

void br_knl_audit(const uint8_t *file, const kn_info *in, br_audit *a)
{
    a->pairs = a->known = a->unlisted = a->first_unlisted = 0;
    const uint8_t *img = file + rd32(file + in->hdr_off + O_IMAGE_FILE);
    const uint32_t n = (in->text_size < in->image_size ? in->text_size : in->image_size) & ~1u;
    struct { uint8_t have, hi; uint16_t imm; uint32_t site; } half[32];
    for (int i = 0; i < 32; i++) half[i].have = 0;
    for (uint32_t off = 0; off + 2 <= n;) {
        const uint32_t w = rd16(img + off), len = insn_len(w);
        if (len >= 4 && (w & 0xFF00) == 0xE100 && off + 4 <= n) {
            const uint32_t imm = rd16(img + off + 2), hi = (w >> 6) & 1, key = w & 31;
            if (w & 0xA0) half[key].have = 0;                        /* (Z) / (X): a whole register under 64 K */
            else if (half[key].have && half[key].hi != hi) {
                const uint32_t v = hi ? imm << 16 | half[key].imm : (uint32_t)half[key].imm << 16 | imm;
                const uint32_t other = half[key].site;
                half[key].have = 0;
                if (br_addr_known(v) || tight(v)) {
                    if (!relocated(file, in, off + 2) && !relocated(file, in, other)) {
                        a->pairs++;
                        if (br_addr_known(v)) a->known++;
                        else if (!a->unlisted++) a->first_unlisted = v;
                    }
                }
            } else { half[key].have = 1; half[key].hi = (uint8_t)hi; half[key].imm = (uint16_t)imm; half[key].site = off + 2; }
        }
        off += len;
    }
}
}   /* extern "C" */
