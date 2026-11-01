/*
 * Loaded-image map for crash reports: where each plugin file's code and data sit after this boot's relocation.
 * kn_boot.c appends one record per file it registered (.knl engine / FX / feature, .op1p engine / FX), in load order;
 * a file that failed, was switched off or was shadowed (its memory given back) leaves none. The map lives in the
 * plugin arena (one record per listed file that kennel.off leaves on, allocated before the first file loads; no .bss
 * beyond one pointer). Read from memory by a debugger or the emulator: the word at the core symbol _kn_images
 * (build/synth.sym) points at it (0: no plugin files this boot, or every one switched off); dump 16 + cap * 44 bytes. tools/kennel/symbolicate.py turns a program
 * counter into file + offset (+ symbol). Not part of the inventory (LIST) or any wire format.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifndef KN_IMAGES_H
#define KN_IMAGES_H
#include <stdint.h>

#define KN_IMG_MAGIC   0x474D494Bu         /* "KIMG" */
#define KN_IMG_VERSION 1
enum { KN_IMG_KNL = 1, KN_IMG_OP1P = 2 };  /* = KN_INV_KNL / KN_INV_OP1P */

typedef struct {
    uint32_t base;                         /* load address: image at base, then .bss */
    uint32_t load_size;                    /* bytes taken from the arena (image + bss) */
    uint32_t image_size;                   /* initialized bytes (code + rodata + data) */
    uint32_t text_size;                    /* code bytes from base (.op1p: no boundary, = image_size) */
    uint32_t crc;                          /* the file's CRC (.knl header, .op1p trailer), as the inventory */
    uint32_t file_size;
    uint8_t  format, kind, dir, pad;       /* KN_IMG_*, KN_KIND_* (kn_load.h), 0 plugins/ 1 content/ */
    char     id[16];                       /* the file id: its name without .knl / .op1p */
} kn_img_rec;                              /* 44 bytes */

typedef struct {
    uint32_t magic;                        /* KN_IMG_MAGIC once the loader ran */
    uint16_t version, rec_size;            /* KN_IMG_VERSION, sizeof(kn_img_rec) */
    uint32_t cap, count;                   /* records allocated (the files listed, <= 128), records in use */
    kn_img_rec rec[];
} kn_img_map;                              /* 16 bytes + cap records */

extern kn_img_map *kn_images;
#endif
