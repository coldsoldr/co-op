/*
 * kennel feature API, drum type hook point (docs/feature-hooks.md "Drum types"): a feature that runs a kind of
 * sampled drum kit (drum+ first) fills one op1_drum_type and points op1_feature_t.drum at it.
 * The core owns every R246 site (kennel/kn_drum.c): the patch parse keep and load (0x01A044A8, 0x01A04BC0), the
 * drum type browser start row (0x01977900) and job (vtable 0x0190F34C), the DrumSampler vtable slots render /
 * note_on / note_off (0x0190FAC4), the drum_types row and the T1 / T2 screen objects (ids 0x10 / 0x17).
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#ifndef OP1_FEATURE_DRUM_H
#define OP1_FEATURE_DRUM_H
#include "op1_feature.h"

#define OP1_DRUM_API     1               /* bumped when op1_drum_type changes incompatibly */
/* The one drum type row the core adds to drum_types and the patch key it keeps: saved kits depend on both, so
 * they never change meaning. A kit of the type is "type": "drum" (a sampled kit) plus a "op1r" object. */
#define OP1_DRUM_TYPE_ID 2
#define OP1_DRUM_KEY     "op1r"
#define OP1_DRUM_SLOTS   8               /* drum patch slots */
#define OP1_DRUM_PADS    24
#define OP1_DRUM_VOICES  12

/* pad entry: the 8 words of the live pad table (drumplus/dp_voice.h dp_pad is the same) */
typedef struct { int16_t pitch, start_hi, end_hi, playmode, reverse, start_lo, end_lo, volume; } op1_drum_pad;

/* the stock DrumSampler voice functions: a type calls them for the pads it leaves alone */
#define OP1_DRUM_STOCK_RENDER   ((void (*)(void *, int16_t *, const op1_drum_pad *))0x019789EC)
#define OP1_DRUM_STOCK_NOTE_ON  ((void (*)(void *, const op1_drum_pad *))0x01978926)
#define OP1_DRUM_STOCK_NOTE_OFF ((void (*)(void *, const op1_drum_pad *))0x01978898)
/* the patch JSON (a Json::Value) of drum slot s; obj[OP1_DRUM_KEY] holds the type's data */
#define OP1_DRUM_PATCH_JSON(s)  ((void *)(0x01DDD9A8 + (s) * 0x38 + 4))
#define OP1_DRUM_SLOT_CUR       (*(volatile int *)0xFF8016D8 & 7)

struct op1_drum_type {
    uint16_t api;                        /* OP1_DRUM_API */
    uint16_t id;                         /* drum_types id: OP1_DRUM_TYPE_ID */
    const char *name;                    /* drum_types row name and the "engine" member written on pick: "drum+" */
    /* a drum slot was loaded: obj = its OP1_DRUM_KEY object (a Json::Value), 0 = a stock kit (clear the slot).
     * Called from the patch loader, or from the core at boot for kits loaded before the type registered. */
    void (*load)(int slot, void *obj);
    /* DrumSampler vtable, every voice of every kit (audio interrupt for render): handle the type's pads, call the
     * OP1_DRUM_STOCK_* function for the rest */
    void (*render)(void *voice, int16_t *out, const op1_drum_pad *pad);
    void (*note_on)(void *voice, const op1_drum_pad *pad);
    void (*note_off)(void *voice, const op1_drum_pad *pad);
    /* pages T1 (page 0, screen 0x10) and T2 (page 1, 0x17), shown instead of the stock ones while the current drum
     * slot is of the type. The core draws the empty screen, then calls draw on the current raster. */
    void (*page_init)(void);
    void (*page_draw)(int page, int slot, void *raster);
    int  (*page_turn)(int page, int slot, int knob, int delta);   /* 1 = consumed (0: the stock controller's) */
    void (*page_press)(int page, int slot, int knob);             /* encoder press 0..3 (blue..red) */
};

#endif
