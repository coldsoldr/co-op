/*
 * coop's plugin bridge (docs/bridge.md): other firmwares' plugin files, unmodified, on coop. Phase B1: what can be
 * decided and proven on the host. Nothing here is linked into the core yet (core/sdram/Makefile does not list this
 * folder); tests/host/bridge builds and runs it, tools/bridge/survey.py is its Python twin.
 *
 *   kennel (.knl)  the file format is coop's own (plugin_load.cc is kennel's loader), so the bridge rewrites the
 *                  header in the staging block into coop's dialect (br_knl_normalise) and the ONE loader checks and
 *                  relocates it, against the bridge's export table (br_knl_exports) instead of coop_exports.
 *   Opus (.opmod)  its own format: br_opmod_check (every table and relocation checked before a byte is written).
 * A file is refused at load with a named reason whenever it imports a name the bridge does not provide, carries a flag
 * it does not serve or is of a kind coop has no place for: br_verdict.
 *
 * Freestanding as the loader is: no libc, no float, no static constructors.
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef COOP_BRIDGE_H
#define COOP_BRIDGE_H
#include <stdint.h>
#include "../sdram/plugin_load.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BR_LEVEL      1            /* the bridge phases this tree has built (docs/bridge.md "Plan"); B1 wires nothing */
#define BR_KNL        1
#define BR_OPMOD      2
#define BR_K_FN       0
#define BR_K_DATA     1
#define BR_KNL_API    1            /* kennel's OP1_PLUGIN_API (op1_plugin.h: "stays 1 until the first release") */
#define BR_PHASE_NEVER 0

/* kennel's header flags where they differ from coop's (hundekraft 3.3.1 kn_load.h). 0x20 is coop's KN_FLAG_SCRATCH and
 * extension tag 7 coop's KN_EXT_SCRATCH: a .knl must never reach kn_parse with either as it stands. */
#define BR_KNF_OWN_ENV     0x20u
#define BR_KNF_SUM         0x40u
#define BR_KNF_T2_PAGE     0x80u
#define BR_KNF_T2_SWALLOW  0x100u
#define BR_KNF_ENG_INPUT   0x200u
#define BR_KNF_T4_PAGE     0x400u
#define BR_KNF_ENG_SERVICE 0x800u
#define BR_KNF_COLD        0x1000u
#define BR_KNF_ENGINE_EXT  (BR_KNF_OWN_ENV | BR_KNF_SUM | BR_KNF_T2_PAGE | BR_KNF_T2_SWALLOW | BR_KNF_ENG_INPUT | \
                            BR_KNF_T4_PAGE | BR_KNF_ENG_SERVICE)
#define BR_KN_EXT_SETTINGS 7       /* kennel: the settings page's rows */
#define BR_KN_EXT_PARKED   0x4B07  /* what br_knl_normalise makes of it: a tag coop's parser skips */

typedef struct {
    const char *name;              /* as the file's import table spells it */
    uint8_t dialect, loads, full, kind;
    uintptr_t shim;                /* the bridge's function or object, 0 = needs the core */
    const char *note;
} br_import;
extern const br_import br_imports[];
extern const int br_imports_count;
const br_import *br_import_find(int dialect, const char *name);

typedef struct { uint32_t addr; const char *name; } br_addr;
extern const br_addr br_addrs[];
extern const int br_addrs_count;
int br_addr_known(uint32_t addr);

/* What the bridge makes of one file. phase: BR_PHASE_NEVER, or the phase from which it loads. */
typedef struct {
    int dialect, kind;             /* kind: KN_KIND_* (an .opmod: FX when it adds an effect, SYNTH an engine, else 0) */
    int phase;
    const char *reason;            /* 0 = loads at `phase`; else why not before (static text) */
    char needs[40];                /* the import, flag or line the reason is about ("" if none) */
    char name[40];                 /* the type name (.knl) or the manifest's id (.opmod) */
    uint32_t load_size;            /* image and zero fill */
    uint32_t flags;                /* the file's own flag word */
} br_verdict;

/* ---- kennel ---- */
/* The .knl in `file` (size bytes, writable: the staging block) becomes a file kn_parse accepts: kennel's size, CRC and
 * API checked first, then the magic, the API word and extension tag 7 rewritten and the CRC redone. Flags are checked,
 * not translated: a file with a kennel-only flag is refused here (v->reason), so bit 0x20 never reaches kn_parse.
 * 0 = normalised (v->phase is the phase its kind and flags allow), -1 = not a .knl or damaged (*err), 1 = refused
 * (v->phase and v->reason say why; the bytes are untouched). */
int br_knl_normalise(uint8_t *file, uint32_t size, br_verdict *v, const char **err);
/* The one loader's parse for a normalised .knl. With plugin_load's kn_parse_as (docs/bridge.md "Loader change") the
 * entry object is checked at kennel's sizes (op1_engine_t 16 bytes, op1_fx_t 12); before it, at coop's, so a .knl
 * engine whose 16-byte entry object sits in the image's last 36 bytes is refused ("entry object truncated"). */
int br_knl_parse(const uint8_t *file, uint32_t size, kn_info *in, const char **err);
int br_knl_parse_exact(void);      /* 1 when kn_parse_as is there */
/* After br_knl_parse: every import against the table. Updates v (the latest phase wins, never wins over all); returns the
 * phase (0 = never). */
int br_knl_imports(const uint8_t *file, const kn_info *in, br_verdict *v);
/* The export table kn_relocate_far resolves a .knl against at `level`: every KNL entry with loads <= level and a shim.
 * Returns the count (<= cap). A name left out makes the loader refuse the file ("an import is not exported"). */
int br_knl_exports(int level, kn_export *out, int cap);
/* The firmware addresses the file's code builds (.H / .L immediate pairs with no relocation): known = on BR_ADDR's
 * list, unlisted = others inside R246's L1 data, scratchpad or L1 code (first_unlisted: the first of them). */
typedef struct { uint32_t pairs, known, unlisted, first_unlisted; } br_audit;
void br_knl_audit(const uint8_t *file, const kn_info *in, br_audit *a);

/* ---- kennel effects: the adapter (bridge_fx.cc) ---- */
#define BR_FX_BLOCK 64             /* kennel's OP1_FX_BLOCK: its render takes exactly this many samples */
typedef struct {                   /* kennel's op1_fx_stereo_t; a mono file ends after press */
    void (*init)(int pos);
    void (*render)(int pos, int16_t *buf, const int16_t *params);
    uint8_t press[4];
    void (*render_stereo)(int pos, int16_t *left, int16_t *right, const int16_t *params);
} br_knl_fx;
typedef struct {
    const br_knl_fx *left, *right; /* two relocations of the file: independent state a channel (docs/fx-abi.md) */
    int stereo;                    /* the file's stereo flag: the master position calls left->render_stereo once */
    void **scratch_tab;            /* R246's position buffer table (0x01AB8BA0 on the OP-1), 0 = the file uses none */
    void *right_scratch[3];        /* the right instance's buffer per position, swapped in around its calls */
    uint32_t dry_blocks;           /* blocks passed dry: a block that is not 64 samples, a position past the master */
} br_fx_inst;
int br_fx_init(br_fx_inst *x, int slot);
void br_fx_render(br_fx_inst *x, int slot, int32_t *l, int32_t *r, const int16_t *params, int n);

/* ---- Opus (.opmod format 1) ---- */
typedef struct {
    const uint8_t *d;
    uint32_t size, crc;
    uint32_t man_off, man_len, img_off, img_len, zero, entry, imp_off, imp_n, rel_off, rel_n, str_off, str_len;
    uint32_t flags, exp_off, exp_n, pl_n;
} br_opmod;
const char *br_opmod_check(const uint8_t *d, uint32_t size, br_opmod *f);       /* 0, or the reason */
const char *br_opmod_import(const br_opmod *f, uint32_t i);
const char *br_opmod_line(const br_opmod *f, const char *key, int n, uint32_t *len);   /* a manifest line's value */
int br_opmod_verdict(const br_opmod *f, br_verdict *v);                          /* the phase (0 = never) */

/* ---- the stand-ins a .knl import resolves to (bridge_shims.cc) ---- */
extern volatile uint8_t br_op1_fx_page[3][4], br_op1_fx_wrap[3][4], br_op1_eng_page[4];
void *br_op1_l1_claim(int part, const void *owner, uint32_t bytes, int *fresh);
void *br_op1_l1_claim_state(int part, void *home, uint32_t state_bytes, uint32_t bytes, int *fresh);
const int16_t *br_op1_l1_sine_quarter(void);
void br_op1_dbg(const char *text, int a, int b);
void *br_op1_fx_fast(int pos, uint32_t *size);
int br_op1_sc_source(int src, int16_t *out);
int br_stub0(void);

#ifdef __cplusplus
}
#endif
#endif
