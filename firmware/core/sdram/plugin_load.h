/* coop-fw plugin loader header: kennel 3.1 kn_load.h (GPL-3.0-or-later, Hundekraft) as vendored, for files with the
 * magic COOP (docs/plugins.md). Kept byte-compatible with kennel's format so pack.py / reloc.py apply unchanged. */
#ifdef __cplusplus
extern "C" {
#endif
/*
 * kennel plugin loader: checks and relocation of one plugin file (docs/plugin-format.md), the same steps and order
 * as the reference loader tools/kennel/reloc.py. No firmware calls here: kn_boot.c reads the files and registers
 * the result; the host test (harness/kennel_load_host.c) runs this file on the PC against reloc.py.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifndef KN_LOAD_H
#define KN_LOAD_H
#include <stdint.h>

#define KN_FORMAT_VERSION 1
#define KN_HEADER_SIZE    144
#define KN_NONE           0xFFFFFFFFu
#define KN_KIND_SYNTH     1
#define KN_KIND_FX        2
#define KN_KIND_FEATURE   3
#define KN_FLAG_USES_SAMPLE 1u
#define KN_FLAG_PAGE2       2u
#define KN_FLAG_STEREO      4u
#define KN_FLAG_INPUT       8u
#define KN_FLAG_BIGBUF      0x10u        /* the file declares a pool buffer: extension KN_EXT_BIGBUF -> op1_bigbuf_t
                                            (op1_plugin.h). Cores before 2026-10-21 refuse the bit ("unknown flags") */
#define KN_EXT_BIGBUF       4            /* extension tag: u32 image offset of the op1_bigbuf_t descriptor */
#define KN_EXT_COST         5            /* extension tag: u32 x 2, the declared worst-block cost in bcore packets (synth:
                                            per voice, voice start; FX: per call, init). Information for tools only: the
                                            core does not act on it. Any other length is ignored, never refused */
#define KN_BIGBUF_MAGIC     0x42424E4Bu  /* "KNBB" */
#define KN_BIGBUF_VERSION   1
#define KN_BIGBUF_SIZE      24           /* magic, version, min, max, ptr, size */
#define KN_BIGBUF_MAX       0x80000u     /* = kn_mem.h KN_DECL_MAX: a larger min is refused (kn_load.c, reloc.py) */
#define KN_FLAG_COARSE_MASK 0xF0000u     /* synth: bit 16 + k = knob k turns in 8 steps (plugin.json "coarse", op1r 44) */
#define KN_COARSE(flags)    (((flags) >> 16) & 15)

typedef struct { const char *name; uint32_t addr; } kn_export;

/* What a checked file says (pointers point into the file buffer). */
typedef struct {
    uint8_t  kind, legacy_order;
    char     type_name[16];          /* NUL-terminated copy */
    uint32_t flags;
    int16_t  defaults[4], page2[4];
    uint32_t image_size, load_size, image_align, text_size;
    uint32_t entry_offset, screen_offset;
    uint32_t hdr_off;                /* 0, or where the header starts after the SVG prefix */
    const uint8_t *svg;              /* 0 if none; else the file itself starts with the screen SVG */
    uint32_t svg_size;
    uint32_t crc;                    /* the header's crc32 */
    const char *claims, *author, *version;   /* extension table entries (NUL-terminated; claims: a NUL-separated
                                                list ending in an empty string), 0 if absent */
    uint32_t bigbuf_offset;          /* KN_FLAG_BIGBUF: image offset of the op1_bigbuf_t, else KN_NONE */
    uint32_t bigbuf_min, bigbuf_max; /* its declared sizes (bytes) */
    uint32_t cost[2];                /* KN_EXT_COST, {0, 0} if absent or malformed */
    uint32_t n_imports;              /* entries of the import table */
} kn_info;

/* Check `file` (size bytes) and fill `info`. 0 = ok, else *err says why (a static string). */
int kn_parse(const uint8_t *file, uint32_t size, uint16_t api_version, kn_info *info, const char **err);

/* Copy the image to `dst` (which will run at address `base`, `cap` bytes available), zero .bss and apply the
 * relocations; imports are looked up in `exports` (n entries). 0 = ok, else *err. Call kn_parse first. */
/* 1 if the checked file (kn_parse) imports `name` (with Blackfin's underscore, as kn_exports.c lists it), else 0. */
int kn_imports(const uint8_t *file, const kn_info *info, const char *name);

int kn_relocate(const uint8_t *file, const kn_info *info, uint8_t *dst, uint32_t base, uint32_t cap,
                const kn_export *exports, int n, const char **err);

/* Call stubs (docs/plugin-format.md "Call stubs"): kn_relocate with one difference. A CALL / JUMP.L into the core
 * (PCREL24, +-16 MiB) that cannot reach its target from `base` goes to a stub after the image instead, which jumps
 * there absolutely: P1.L = target; P1.H = target; JUMP (P1); NOP (12 bytes, P1 is call-clobbered in the Blackfin
 * ABI). One stub per distinct target, in the order the relocations first need one, from the first 4-byte boundary at or
 * after load_size. *used: the bytes taken (load_size when no stub was needed); at most load_size + kn_stub_room().
 * The plugin arena in heap 0 (kn_boot.c) is more than 16 MiB below the core, so every core call there takes one. */
#define KN_STUB_SIZE 12u
static inline uint32_t kn_stub_room(const kn_info *in) { return 3u + KN_STUB_SIZE * in->n_imports; }
int kn_relocate_far(const uint8_t *file, const kn_info *info, uint8_t *dst, uint32_t base, uint32_t cap,
                    const kn_export *exports, int n, const char **err, uint32_t *used);

/* A look at a file's header before anything is loaded (kn_boot.c sizes the arena from it): no CRC, none of the block
 * checks. `file` holds the file's first `avail` bytes (the reader adds more while the answer is 1: an SVG prefix
 * runs to its NUL, then the header). need: the most the loader takes for this file at a bump pointer on any 4-byte
 * boundary: load_size + kn_stub_room + image_align - 1 (kn_boot.c alloc's pad). str_off / str_size: the string table
 * (the import names). 0 = ok; 1 = more bytes; -1 = kn_parse would refuse it (magic, version, header size, API,
 * kind, alignment, sizes): counts nothing. Whatever kn_parse accepts, kn_peek accepts with the same sizes. */
typedef struct { uint32_t need, load_size, str_off, str_size; } kn_peek_t;
int kn_peek(const uint8_t *file, uint32_t avail, uint16_t api_version, kn_peek_t *p);

#endif

#ifdef __cplusplus
}
#endif
