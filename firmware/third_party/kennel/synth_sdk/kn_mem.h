/*
 * kennel's SDRAM map: the one place every kennel address range lives (ARCHITECTURE.md "Memory map"; plan:
 * OP1-Emulator docs/kennel-memory-plan-2026-10-21.md). C includes this file; Python reads it through
 * synth_sdk/kn_mem.py (a regex over the #defines below, so the two can't drift). The linker scripts (link.ld,
 * harness/tier2/guest.ld) can't include it: make_firmware.py and harness/tier2.py check their literals against it.
 * Format: one `#define KN_<NAME> <value or expression of earlier KN_ names>` per line (kn_mem.py evaluates them).
 * Layout: core window 0x01E00000..0x02000000, undo 0x02800000.., code arena: one block from R246's heap 0
 * (KN_ARENA_SIZE, kn_boot.c, since 2026-10-31) or, if heap 0 can't give it, the fixed range 0x02980000..0x02C00000;
 * pool 0x02C00000..0x03000000 (3 x 1 MiB FX leases + 1 MiB declared buffers; file staging at boot). Hardware SDRAM is
 * 64 MB; the emulator maps 128 MiB. Kennel never uses anything at or above 0x03000000 (R246's DMA
 * page, then heap 1).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifndef KN_MEM_H
#define KN_MEM_H

/* ---- core window 0x01E00000..0x02000000 ---- */
#define KN_CORE_BASE        0x01E00000u     /* core image (link.ld .text) */
#define KN_CORE_CODE_LIMIT  0x01E40000u     /* the loaded image ends below (make_firmware CODE_LIMIT) */
#define KN_FARBSS_LO        0x01E40000u     /* second zero-filled core section (.farbss, an LDR FILL block) for large */
#define KN_FARBSS_HI        0x01EC0000u     /* core tables. Was Maiko #249's module range; kennel never patches Maiko */
#define KN_BSS_LO           0x01EC0000u     /* core .bss (link.ld), an LDR FILL block */
#define KN_BSS_LIMIT        0x01EF0000u     /* .bss ends below; the workshop's boot banner sits here */
#define KN_SDRAM_TOP        0x04000000u     /* hardware: 64 MB. Never address at or above this */

/* ---- plugin memory ---- */
#define KN_UNDO_LO          0x02800000u     /* op1_undo_area: one owner (undo.knl), above R246's last BSS 0x027FFB03 */
#define KN_UNDO_HI          0x02980000u
#define KN_ARENA_LO         0x02980000u     /* the fixed code arena (images + .bss, loader records, art cache; bump, */
#define KN_ARENA_HI         0x02C00000u     /* never freed): used only when heap 0 can't give the arena (below) */
#define KN_ARENA_TOP        KN_ARENA_HI     /* allocations end below this (staging is in the pool since 2026-10-21) */
#define KN_ARENA_SIZE       (KN_ARENA_HI - KN_ARENA_LO)   /* 2.5 MiB: the fixed range (heap 0's block is sized, below) */
/* The arena's usual home (2026-10-31): one malloc from R246's heap 0 at boot, as op1r mallocs each plugin there (op1r
 * caps all plugins at 4 MB of heap 0's ~9 MB free). Heap 0 is SDRAM bank 0 with the thread stacks, data CPLB pages
 * locked and write-back cacheable; the fixed range above shares bank 2 with display framebuffer A (0x02000000), which
 * the PPI reads and MDMA clears every frame. The block starts on a KN_ARENA_ALIGN boundary, as the fixed range does
 * (16 KB: one cache way); the plugins after the first sit a little higher than they did there (the call stubs before
 * them), so their cache sets differ from the fixed range's, as they do whenever a file is added. Heap 0 is more than
 * 16 MiB below the core: plugin calls into the core go through call stubs (kennel/kn_load.h kn_relocate_far). */
#define KN_ARENA_ALIGN      0x00004000u
/* The heap-0 block's size (since 2026-10-31, branch arena-size; kn_boot.c arena_need): what the listed files need,
 * from a look at each enabled file's header before anything is loaded (kn_load.h kn_peek), plus the core's own users,
 * rounded up to KN_ARENA_ALIGN and at most KN_ARENA_MAX. A file kennel.off switches off counts only its per-file
 * bytes. Every term is an upper bound: an estimate that still comes out short refuses the files that don't fit
 * ("plugin memory full"), as a full arena always did. */
#define KN_ARENA_MAX        0x00380000u     /* 3.5 MiB: the ceiling (op1r caps its plugins at 4 MB of heap 0) */
#define KN_ARENA_CORE       0x00001000u     /* boot tables: kn_log's base 512, the image map's head, dispatcher.c's
                                               four screen tables 4 x 64 x 4, kn_input / kn_midi screen objects */
#define KN_ARENA_PER_FILE   0x00000400u     /* per listed file: kn_log 64, inventory record ~100 + author / version /
                                               claims <= 216 + its LIST line ~150, image record 44, name 16 + SVG path 48,
                                               a sequencer's kn_seq_asset copy ~200 */
#define KN_ARENA_RUNTIME    0x00020000u     /* op1_alloc after boot: midiseq ~62 KB, drum synth stacks 16.5 KB,
                                               tapes 8 KB, flash 6.2 KB, sampletools, storage walk */
#define KN_ARENA_MARGIN     0x00010000u     /* for what no term above knows: a plugin's own op1_alloc */
#define KN_HEAP0_LO         0x00000100u     /* R246's heap 0 (linker heap list 0x018E628C): the block must lie inside */
#define KN_HEAP0_HI         0x00FFFFFCu
#define KN_POOL_LO          0x02C00000u     /* big-buffer pool: cleared once at boot, after every file is loaded */
#define KN_POOL_HI          0x03000000u     /* 0x03000000..: R246's DMA page, then heap 1. Not kennel's */
#define KN_FX_BIG_SIZE      0x00100000u     /* one lease per FX position (0 synth, 1 drum, 2 master): op1_fx_big */
#define KN_FX_BIG_COUNT     3
#define KN_DECL_LO          (KN_POOL_LO + KN_FX_BIG_COUNT * KN_FX_BIG_SIZE)   /* buffers plugin files declare */
#define KN_DECL_HI          KN_POOL_HI
#define KN_DECL_MAX         0x00080000u     /* granted per plugin file at most; a larger min is refused */
#define KN_DECL_ALIGN       32u             /* a cache line */
#define KN_FILE_MAX         0x00100000u     /* largest plugin file */
#define KN_STAGE            KN_DECL_LO      /* boot only: the file being read, before the pool is cleared and granted */
#define KN_PRELOAD_LO       KN_POOL_LO      /* boot only: files read whole while the arena is sized (kn_boot.c peek_file), */
#define KN_PRELOAD_HI       KN_STAGE        /* then relocated from here; the rest are staged at KN_STAGE as before */
#define KN_POOL_API         1               /* op1_fx_big / op1_bigbuf_t; a region never changes meaning without it */

/* ---- derived limits ---- */
#define KN_CALL_REACH       0x01000000u     /* PCREL24: +-16 MiB */
#define KN_CALL_CEILING     (KN_CORE_BASE + KN_CALL_REACH)   /* plugin code below this reaches every core export */
#define KN_ARTCACHE_BYTES   (2u * 96u * 1024u)                /* kn_artcache.c: two 96 KiB static-art records */
#define KN_ARTCACHE_HEADROOM 0x00010000u    /* allocated only when this much arena stays free after it */
#define KN_ARENA_BUDGET_PCT 90              /* matrix / builder: the release set must stay below this share */

/* compile-time guards: regions in order, disjoint, inside 64 MB; the arena within call reach */
#define KN_MEM_ASSERT(c, m) typedef char kn_mem_assert_##m[(c) ? 1 : -1]
KN_MEM_ASSERT(KN_CORE_CODE_LIMIT <= KN_FARBSS_LO && KN_FARBSS_HI <= KN_BSS_LO && KN_BSS_LO < KN_BSS_LIMIT, core);
KN_MEM_ASSERT(KN_UNDO_HI <= KN_ARENA_LO && KN_ARENA_TOP <= KN_ARENA_HI && KN_ARENA_HI <= KN_POOL_LO, order);
KN_MEM_ASSERT(KN_POOL_HI <= 0x03000000u && KN_POOL_HI <= KN_SDRAM_TOP, top);
KN_MEM_ASSERT(KN_ARENA_HI <= KN_CALL_CEILING, reach);     /* the fixed arena; heap 0's uses call stubs */
KN_MEM_ASSERT(KN_ARENA_MAX + KN_ARENA_ALIGN < KN_HEAP0_HI - KN_HEAP0_LO && !(KN_ARENA_LO % KN_ARENA_ALIGN), heap0);
KN_MEM_ASSERT(KN_ARENA_SIZE <= KN_ARENA_MAX && !(KN_ARENA_MAX % KN_ARENA_ALIGN), arena_max);
KN_MEM_ASSERT(KN_ARENA_RUNTIME + KN_ARENA_MARGIN >= KN_ARTCACHE_HEADROOM, artcache_room);   /* the cache's headroom */
KN_MEM_ASSERT(KN_DECL_LO < KN_DECL_HI && KN_DECL_HI <= KN_POOL_HI && KN_DECL_MAX <= KN_DECL_HI - KN_DECL_LO, decl);
KN_MEM_ASSERT(KN_STAGE >= KN_POOL_LO && KN_STAGE + KN_FILE_MAX <= KN_POOL_HI, stage);
KN_MEM_ASSERT(KN_PRELOAD_LO == KN_POOL_LO && KN_PRELOAD_HI <= KN_STAGE && !(KN_PRELOAD_LO % 4), preload);

/* Put a large zero-initialised core table in .farbss instead of .bss: `static uint8_t big[N] kn_farbss;`.
 * The input section is named .bss.kn_far so GCC refuses an initialiser ("only zero initializers are allowed");
 * link.ld's .farbss rule comes before .bss's *(.bss*). Host builds (OP1_HOST, clang) get an ordinary static. */
#if defined(__bfin__) && !defined(OP1_HOST)
#define kn_farbss __attribute__((section(".bss.kn_far")))
#else
#define kn_farbss
#endif

/* The audio callback's per-block bookkeeping (screens/sysmon_core.c, dispatcher.c's budget, kennel/kn_shed.c,
 * kennel/kn_diag.c's always-on part): `static uint32_t x kn_hot;`. link.ld puts every .bss.kn_hot input section first
 * in .bss, together, starting and ending on a 32-byte cache line, so a cold block fetches a dozen lines for it instead
 * of one or two lines per variable spread over the core's .bss. Zero-initialised only (GCC refuses an initialiser).
 * Host builds: an ordinary static. */
#if defined(__bfin__) && !defined(OP1_HOST)
#define kn_hot __attribute__((section(".bss.kn_hot")))
#else
#define kn_hot
#endif

#endif
