/* coop-fw plugin loader: kennel 3.1 kn_load.c (GPL-3.0-or-later, Hundekraft) with the magic COOP; see
 * plugin_load.h. Compiled as C++ inside extern "C". */
/*
 * kennel plugin loader, file part: the checks and the relocation of tools/kennel/reloc.py, step for step
 * (docs/plugin-format.md "Loader steps"). Freestanding: no libc, no firmware calls.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include "plugin_load.h"
extern "C" {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-tree-loop-distribute-patterns")   /* the firmware has no memcpy / memset */
#endif

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }

/* CRC-32 (zlib) over the file with the 4 CRC bytes at offset 12 taken as zero; bitwise, runs once per file at boot */
static uint32_t crc32_file(const uint8_t *d, uint32_t n, uint32_t crc_at)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= (i >= crc_at && i < crc_at + 4) ? 0 : d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

#define H32(o) rd32(file + hoff + (o))
enum { O_FILE_SIZE = 8, O_CRC = 12, O_API = 16, O_KIND = 18, O_ORDER = 19, O_NAME = 20, O_FLAGS = 36, O_DEF = 40,
       O_PAGE2 = 48, O_TEXT = 56, O_RODATA_OFF = 60, O_RODATA_SIZE = 64, O_DATA_OFF = 68, O_DATA_SIZE = 72,
       O_BSS_OFF = 76, O_BSS_SIZE = 80, O_IMAGE_SIZE = 84, O_ALIGN = 88, O_ENTRY = 92, O_SCREEN = 96,
       O_IMAGE_FILE = 100, O_RELOC = 104, O_RELOC_N = 108, O_IMPORT = 112, O_IMPORT_N = 116, O_STR = 120,
       O_STR_SIZE = 124, O_SVG = 128, O_SVG_SIZE = 132, O_SVG_NAME = 136, O_EXT = 140 };
enum { R_ABS32 = 1, R_HI16 = 2, R_LO16 = 3, R_PCREL24 = 4 };
#define RELOC_SIZE 12
#define SYM_LOCAL  0xFFFF

/* a block inside the file: off % align == 0, header_size <= off, off + size <= file size */
static int span_ok(uint32_t off, uint32_t size, uint32_t align, uint32_t hsize, uint32_t fsize)
{
    return !(off % align) && off >= hsize && (uint64_t)off + size <= fsize;
}

/* string at `off` of the table ends inside it; ASCII only */
static const char *cstr(const uint8_t *s, uint32_t n, uint32_t off)
{
    if (off >= n) return 0;
    for (uint32_t i = off; i < n; i++) {
        if (!s[i]) return (const char *)s + off;
        if (s[i] & 0x80) return 0;
    }
    return 0;
}

#define FAIL(msg) do { *err = (msg); return -1; } while (0)

static int is_magic(const uint8_t *p) { return p[0] == 'C' && p[1] == 'O' && p[2] == 'O' && p[3] == 'P'; }   /* coop: COOP */

/* 0, or for a file that starts with its screen SVG: the 4-byte boundary after the SVG's NUL; -1 = no header */
static int64_t header_offset(const uint8_t *file, uint32_t size)
{
    if (size >= 4 && is_magic(file)) return size >= 8 ? 0 : -1;   /* magic, version and header size: 8 bytes */
    if (!size || file[0] != '<') return -1;
    uint32_t end = 0;
    while (end < size && file[end]) end++;
    if (end == size) return -1;
    uint64_t h = ((uint64_t)end + 4) & ~3ull;
    return h + 8 <= size && is_magic(file + h) ? (int64_t)h : -1;
}

int kn_parse(const uint8_t *file, uint32_t size, uint16_t api_version, kn_info *in, const char **err)
{
    int64_t ho = header_offset(file, size);
    if (ho < 0) FAIL("not a coop plugin (magic)");
    uint32_t hoff = (uint32_t)ho, ver = rd16(file + hoff + 4);
    if (ver != KN_FORMAT_VERSION) FAIL("format version not supported");
    uint32_t hs = rd16(file + hoff + 6);
    if (hs < KN_HEADER_SIZE || (uint64_t)hoff + hs > size) FAIL("header too short");
    if (H32(O_FILE_SIZE) != size) FAIL("file size does not match the header");
    if (crc32_file(file, size, hoff + 12) != H32(O_CRC)) FAIL("CRC mismatch");
    in->hdr_off = hoff;
    in->crc = H32(O_CRC);
    in->claims = in->author = in->version = 0;
    in->controls = 0;
    in->bigbuf_offset = KN_NONE; in->bigbuf_min = in->bigbuf_max = 0; in->cost[0] = in->cost[1] = 0;
    int has_bigbuf = 0;                              /* extension 4 seen (its value may be any word) */
    hs += hoff;                                      /* blocks after the header start here */
    if (rd16(file + hoff + O_API) != api_version) FAIL("built for a different coop core");
    in->kind = file[hoff + O_KIND];
    if (in->kind < KN_KIND_SYNTH || in->kind > KN_KIND_FEATURE) FAIL("unknown kind");
    in->legacy_order = file[hoff + O_ORDER];
    if (in->legacy_order > 31) FAIL("legacy order > 31");
    const uint8_t *nm = file + hoff + O_NAME;
    int len = 0;
    while (len < 16 && nm[len]) { if (nm[len] & 0x80) FAIL("type name not ASCII"); len++; }
    if (len == 16 || len == 0) FAIL("type name empty or not NUL-terminated");
    for (int i = 0; i < 16; i++) in->type_name[i] = i < len ? (char)nm[i] : 0;
    in->flags = H32(O_FLAGS);
    if (in->flags & ~(KN_FLAG_USES_SAMPLE | KN_FLAG_PAGE2 | KN_FLAG_STEREO | KN_FLAG_INPUT | KN_FLAG_BIGBUF | KN_FLAG_COARSE_MASK))
        FAIL("unknown plugin flags");
    if ((in->flags & KN_FLAG_COARSE_MASK) && in->kind != KN_KIND_SYNTH) FAIL("coarse knobs require synth kind");
    if ((in->flags & KN_FLAG_STEREO) && in->kind != KN_KIND_FX) FAIL("stereo flag requires FX kind");
    if ((in->flags & KN_FLAG_INPUT) && (in->kind != KN_KIND_SYNTH || (in->flags & KN_FLAG_USES_SAMPLE))) FAIL("input flag requires nonsampler synth");
    for (int i = 0; i < 4; i++) {
        in->defaults[i] = (int16_t)rd16(file + hoff + O_DEF + 2 * i);
        in->page2[i] = (int16_t)rd16(file + hoff + O_PAGE2 + 2 * i);
    }

    uint32_t a = H32(O_ALIGN);
    if (a < 4 || (a & (a - 1))) FAIL("image alignment not a power of two >= 4");
    uint32_t text = H32(O_TEXT), ro = H32(O_RODATA_OFF), ros = H32(O_RODATA_SIZE), da = H32(O_DATA_OFF),
             das = H32(O_DATA_SIZE), bo = H32(O_BSS_OFF), bs = H32(O_BSS_SIZE), is = H32(O_IMAGE_SIZE);
    if (ro % 4 || da % 4 || bo % 4) FAIL("section offset misaligned");
    if (!(text <= ro && (uint64_t)ro <= (uint64_t)ro + ros && (uint64_t)ro + ros <= da &&
          (uint64_t)da + das == is && is <= bo)) FAIL("section layout out of order");
    uint64_t total = (uint64_t)bo + bs;
    if (total > 0xFFFFFFFFu) FAIL("plugin too large");
    in->text_size = text;
    in->image_align = a;
    in->image_size = is;
    in->load_size = (uint32_t)total;
    if (!span_ok(H32(O_IMAGE_FILE), is, 4, hs, size)) FAIL("image outside the file or misaligned");
    uint32_t so = H32(O_STR), sn = H32(O_STR_SIZE);
    if (!span_ok(so, sn, 1, hs, size)) FAIL("strings outside the file");
    const uint8_t *str = file + so;
    if (sn && str[sn - 1]) FAIL("string table not NUL-terminated");
    uint32_t io = H32(O_IMPORT), inn = H32(O_IMPORT_N);
    in->n_imports = inn;
    if ((uint64_t)inn * 4 > size || !span_ok(io, inn * 4, 4, hs, size)) FAIL("imports outside the file or misaligned");
    for (uint32_t i = 0; i < inn; i++)
        if (!cstr(str, sn, rd32(file + io + 4 * i))) FAIL("import name runs past the string table");
    uint32_t ro_ = H32(O_RELOC), rn = H32(O_RELOC_N);
    if ((uint64_t)rn * RELOC_SIZE > size || !span_ok(ro_, rn * RELOC_SIZE, 4, hs, size))
        FAIL("relocations outside the file or misaligned");
    for (uint32_t i = 0; i < rn; i++) {
        const uint8_t *r = file + ro_ + RELOC_SIZE * i;
        uint32_t off = rd32(r), typ = r[4], sym = rd16(r + 6);
        int32_t addend = (int32_t)rd32(r + 8);
        if (typ < R_ABS32 || typ > R_PCREL24 || r[5]) FAIL("relocation of unknown type");
        int64_t start = (int64_t)off + (typ == R_PCREL24 ? -2 : 0);
        uint32_t fs = (typ == R_ABS32 || typ == R_PCREL24) ? 4 : 2;
        if (start < 0 || start + fs > is || (start & 1)) FAIL("relocation outside the image");
        if (sym == SYM_LOCAL) {
            if (typ == R_PCREL24) FAIL("local PCREL24 (must be resolved by the packer)");
            if (addend < 0 || (uint64_t)addend > total) FAIL("relocation target outside the plugin");
        } else if (sym >= inn) FAIL("relocation import out of range");
    }
    in->entry_offset = H32(O_ENTRY);
    in->screen_offset = H32(O_SCREEN);
    if (in->entry_offset != KN_NONE && (in->entry_offset >= total || in->entry_offset % 4)) FAIL("entry outside the plugin");
    if (in->screen_offset != KN_NONE && (in->screen_offset >= total || in->screen_offset % 4)) FAIL("screen outside the plugin");
    if (in->entry_offset == KN_NONE) FAIL("no entry object");
    uint32_t entry_size = in->kind == KN_KIND_FEATURE ? 84 : in->kind == KN_KIND_SYNTH ? 36 : 12;   /* coop: coop_engine_t (sdk/coop_plugin.h) */
    if ((uint64_t)in->entry_offset + entry_size > is) FAIL("entry object truncated");
    if (in->screen_offset != KN_NONE && (uint64_t)in->screen_offset + 8 > is) FAIL("screen object truncated");
    if ((in->flags & KN_FLAG_INPUT) && (uint64_t)in->entry_offset + 32 > is) FAIL("input entry object truncated");
    if ((in->flags & KN_FLAG_STEREO) && (uint64_t)in->entry_offset + 16 > is) FAIL("stereo entry object truncated");
    if (H32(O_SVG) || H32(O_SVG_NAME) != KN_NONE) FAIL("reserved svg fields not 0 / none");
    in->svg_size = H32(O_SVG_SIZE);                  /* the file starts with the SVG text and its NUL */
    if (!hoff != !in->svg_size || (hoff && (in->svg_size > hoff || file[in->svg_size - 1]))) FAIL("svg prefix does not match the header");
    in->svg = hoff ? file : 0;
    uint32_t eo = H32(O_EXT);                        /* extension table (claims, author, version): bounds only here */
    if (eo) {
        if (eo % 4 || eo < hs || eo >= size) FAIL("extension table outside the file or misaligned");
        for (;;) {
            if ((uint64_t)eo + 4 > size) FAIL("extension table runs past the file");
            uint32_t tag = rd16(file + eo), n = rd16(file + eo + 2);
            if (!tag) break;
            if ((uint64_t)eo + 4 + n > size) FAIL("extension entry runs past the file");
            if (tag >= 1 && tag <= 3) {
                if (!n || file[eo + 4 + n - 1]) FAIL("extension entry not NUL-terminated ASCII");
                if (tag == 1 && n > 1 && file[eo + 4 + n - 2]) FAIL("claims list not double-NUL-terminated");
                for (uint32_t i = 0; i < n; i++) if (file[eo + 4 + i] & 0x80) FAIL("extension entry not NUL-terminated ASCII");
                const char *v = (const char *)file + eo + 4;
                if (tag == 1) in->claims = v; else if (tag == 2) in->author = v; else in->version = v;
            } else if (tag == KN_EXT_CONTROLS) {                 /* coop e4 */
                if (!n || file[eo + 4 + n - 1]) FAIL("controls entry not NUL-terminated");
                for (uint32_t i = 0; i < n; i++) if (file[eo + 4 + i] & 0x80) FAIL("controls entry not ASCII");
                in->controls = (const char *)file + eo + 4;
            } else if (tag == KN_EXT_COST) {
                if (n == 8) { in->cost[0] = rd32(file + eo + 4); in->cost[1] = rd32(file + eo + 8); }
            } else if (tag == KN_EXT_BIGBUF) {
                if (n != 4 || has_bigbuf) FAIL("bigbuf entry malformed");
                in->bigbuf_offset = rd32(file + eo + 4); has_bigbuf = 1;
            }
            eo += 4 + ((n + 3) & ~3u);
        }
    }
    /* declared pool buffer: flag and extension together, the descriptor inside .data (written by the loader later) */
    if (!(in->flags & KN_FLAG_BIGBUF) != !has_bigbuf) FAIL("bigbuf flag and descriptor disagree");
    if (in->flags & KN_FLAG_BIGBUF) {
        uint32_t bd = in->bigbuf_offset;
        if (bd % 4 || bd < da || (uint64_t)bd + KN_BIGBUF_SIZE > is) FAIL("bigbuf descriptor outside .data or misaligned");
        const uint8_t *d = file + H32(O_IMAGE_FILE) + bd;
        if (rd32(d) != KN_BIGBUF_MAGIC || rd32(d + 4) != KN_BIGBUF_VERSION) FAIL("bigbuf descriptor magic or version");
        in->bigbuf_min = rd32(d + 8); in->bigbuf_max = rd32(d + 12);
        if (in->bigbuf_min > in->bigbuf_max || !in->bigbuf_max) FAIL("bigbuf min > max or max 0");
        if (in->bigbuf_min > KN_BIGBUF_MAX) FAIL("bigbuf min larger than 512 KB");
        if (rd32(d + 16) || rd32(d + 20)) FAIL("bigbuf ptr / size not 0 in the file");
        for (uint32_t i = 0; i < rn; i++) {          /* nothing relocates into the descriptor */
            uint32_t off = rd32(file + ro_ + RELOC_SIZE * i);
            if (off + 4 > bd && off < bd + KN_BIGBUF_SIZE + 2) FAIL("relocation inside the bigbuf descriptor");
        }
    }
    return 0;
}

static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

int kn_imports(const uint8_t *file, const kn_info *in, const char *name)
{
    uint32_t hoff = in->hdr_off;
    uint32_t inn = H32(O_IMPORT_N), io = H32(O_IMPORT);
    const uint8_t *str = file + H32(O_STR);
    uint32_t sn = H32(O_STR_SIZE);
    for (uint32_t i = 0; i < inn; i++) {
        const char *s = cstr(str, sn, rd32(file + io + 4 * i));
        if (s && streq(s, name)) return 1;
    }
    return 0;
}

static int relocate(const uint8_t *file, const kn_info *in, uint8_t *dst, uint32_t base, uint32_t cap,
                    const kn_export *exports, int n, const char **err, int stubs, uint32_t *used)
{
    uint32_t hoff = in->hdr_off;
    if (base % in->image_align) FAIL("load address misaligned");
    if (cap < in->load_size) FAIL("no room for the plugin");
    const uint32_t s0 = (in->load_size + 3u) & ~3u;   /* call stubs from here (kn_relocate_far) */
    uint32_t ns = 0;
    uint32_t inn = H32(O_IMPORT_N), io = H32(O_IMPORT);
    const uint8_t *str = file + H32(O_STR);
    uint32_t sn = H32(O_STR_SIZE);
    for (uint32_t i = 0; i < inn; i++) {             /* every import must exist before anything is written */
        const char *name = cstr(str, sn, rd32(file + io + 4 * i));
        int k = 0;
        while (k < n && !streq(exports[k].name, name)) k++;
        if (k == n) FAIL("an import is not exported by the core");
    }
    const uint8_t *img = file + H32(O_IMAGE_FILE);
    for (uint32_t i = 0; i < in->image_size; i++) dst[i] = img[i];
    uint32_t z = in->image_size;                     /* .bss: word stores (gin alone has ~178 KB of it) */
    for (; z < in->load_size && ((uintptr_t)(dst + z) & 3); z++) dst[z] = 0;
    for (; z + 4 <= in->load_size; z += 4) *(volatile uint32_t *)(dst + z) = 0;
    for (; z < in->load_size; z++) dst[z] = 0;
    uint32_t ro_ = H32(O_RELOC), rn = H32(O_RELOC_N);
    for (uint32_t i = 0; i < rn; i++) {
        const uint8_t *r = file + ro_ + RELOC_SIZE * i;
        uint32_t off = rd32(r), typ = r[4], sym = rd16(r + 6), addend = rd32(r + 8), v;
        if (sym == SYM_LOCAL) v = base + addend;
        else {
            const char *name = cstr(str, sn, rd32(file + io + 4 * sym));
            int k = 0;
            while (!streq(exports[k].name, name)) k++;
            v = exports[k].addr + addend;
        }
        if (typ == R_ABS32) wr32(dst + off, v);
        else if (typ == R_HI16) wr16(dst + off, v >> 16);
        else if (typ == R_LO16) wr16(dst + off, v & 0xFFFF);
        else {                                       /* off = second halfword of a 32-bit CALL / JUMP.L */
            int64_t d = (int64_t)v - ((int64_t)base + off - 2);
            if (stubs && !(d & 1) && (d < -(1 << 24) || d > (1 << 24) - 2)) {   /* through a stub (kn_load.h) */
                uint32_t k = 0;
                for (; k < ns; k++) {
                    const uint8_t *t = dst + s0 + KN_STUB_SIZE * k;
                    if ((rd16(t + 2) | (uint32_t)rd16(t + 6) << 16) == v) break;
                }
                if (k == ns) {
                    if ((uint64_t)s0 + KN_STUB_SIZE * (ns + 1) > cap) FAIL("no room for call stubs");
                    if (!ns) for (uint32_t z = in->load_size; z < s0; z++) dst[z] = 0;   /* the pad before them */
                    uint8_t *t = dst + s0 + KN_STUB_SIZE * ns++;
                    wr16(t, 0xE109); wr16(t + 2, v & 0xFFFF);        /* P1.L = target */
                    wr16(t + 4, 0xE149); wr16(t + 6, v >> 16);       /* P1.H = target */
                    wr16(t + 8, 0x0051); wr16(t + 10, 0x0000);       /* JUMP (P1); NOP */
                }
                d = ((int64_t)base + s0 + KN_STUB_SIZE * k) - ((int64_t)base + off - 2);
            }
            if ((d & 1) || d < -(1 << 24) || d > (1 << 24) - 2) FAIL("call into the core out of reach");
            uint32_t u = (uint32_t)(d >> 1) & 0xFFFFFF;
            wr16(dst + off - 2, (rd16(dst + off - 2) & 0xFF00) | (u >> 16));
            wr16(dst + off, u & 0xFFFF);
        }
    }
    if (in->flags & KN_FLAG_STEREO) {
        // Blackfin ABI: mono prefix init/render/press[4] (12 bytes), stereo at +12.
        // Only plugin-local executable callbacks are accepted, never imports/data.
        const uint32_t fields[] = { 0, 4, 12 };
        const uint64_t end = (uint64_t)base + in->text_size;
        if (end > 0x100000000ULL) FAIL("stereo text address overflow");
        for (unsigned i = 0; i < 3; i++) {
            uint32_t ptr = rd32(dst + in->entry_offset + fields[i]);
            if (!ptr || (ptr & 1) || ptr < base || (uint64_t)ptr + 2 > end)
                FAIL("stereo callback outside plugin text");
        }
    }
    if (used) *used = ns ? s0 + KN_STUB_SIZE * ns : in->load_size;
    return 0;
}

int kn_relocate(const uint8_t *file, const kn_info *in, uint8_t *dst, uint32_t base, uint32_t cap,
                const kn_export *exports, int n, const char **err)
{
    return relocate(file, in, dst, base, cap, exports, n, err, 0, 0);
}

int kn_relocate_far(const uint8_t *file, const kn_info *in, uint8_t *dst, uint32_t base, uint32_t cap,
                    const kn_export *exports, int n, const char **err, uint32_t *used)
{
    return relocate(file, in, dst, base, cap, exports, n, err, 1, used);
}

/* kn_load.h: the header's sizes without the CRC or the block checks (arena sizing before loading). The checks kept
 * are the ones on the header words alone, in kn_parse's order, so a file kn_parse accepts is accepted here too. */
int kn_peek(const uint8_t *file, uint32_t avail, uint16_t api_version, kn_peek_t *p)
{
    uint32_t hoff = 0;
    if (avail < 4) return 1;
    if (!is_magic(file)) {
        if (file[0] != '<') return -1;
        uint32_t end = 0;
        while (end < avail && file[end]) end++;
        if (end == avail) return 1;                  /* the SVG goes on */
        hoff = (end + 4) & ~3u;
        if ((uint64_t)hoff + 8 > avail) return 1;
        if (!is_magic(file + hoff)) return -1;
    }
    if ((uint64_t)hoff + KN_HEADER_SIZE > avail) return 1;
    if (rd16(file + hoff + 4) != KN_FORMAT_VERSION || rd16(file + hoff + 6) < KN_HEADER_SIZE) return -1;
    if (rd16(file + hoff + O_API) != api_version) return -1;
    if (file[hoff + O_KIND] < KN_KIND_SYNTH || file[hoff + O_KIND] > KN_KIND_FEATURE) return -1;
    uint32_t a = H32(O_ALIGN), bo = H32(O_BSS_OFF), inn = H32(O_IMPORT_N);
    if (a < 4 || (a & (a - 1)) || bo % 4) return -1;
    uint64_t total = (uint64_t)bo + H32(O_BSS_SIZE);
    if (total > 0xFFFFFFFFu || inn > 0x40000u) return -1;   /* kn_parse: 4 bytes per import inside a <= 1 MiB file */
    uint64_t need = total + 3u + (uint64_t)KN_STUB_SIZE * inn + (a - 1u);
    if (need > 0xFFFFFFFFu) return -1;
    p->need = (uint32_t)need;
    p->load_size = (uint32_t)total;
    p->str_off = H32(O_STR);
    p->str_size = H32(O_STR_SIZE);
    return 0;
}
}   /* extern "C" */
