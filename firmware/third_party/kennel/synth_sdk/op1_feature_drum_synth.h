/* Native synthesized drum services. Core owns R246 hooks; plugins own instruments.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with OP-1 firmware permission. */
#ifndef OP1_FEATURE_DRUM_SYNTH_H
#define OP1_FEATURE_DRUM_SYNTH_H
#include "op1_feature.h"
#define OP1_DRUM_SYNTH_API 1
/* All callbacks use the normal C ABI. Audio callbacks run on guarded core-owned
 * 4KiB stacks with native DAG/loop/register state preserved. Never allocate/save there. */
typedef struct op1_drum_synth {
    uint16_t api, size;
    const char *name;
    const char *state_key;
    uint32_t (*type)(const void *,uint32_t,const void *);
    uint32_t (*factory)(uint32_t);
    uint32_t (*route)(const void *);
    uint32_t (*trigger)(void *,const int16_t *);
    uint32_t (*render)(void *,int16_t *,const int16_t *);
    uint32_t (*status)(const void *);
    uint32_t (*release)(void *,const int16_t *);
    void (*screen_ctor)(void);
    void (*screen_load)(void *);
    uint32_t (*screen_select)(void);
    int (*key)(int,int);
    int (*knob)(int,const int16_t *);
    uint32_t (*json_equal)(const void *,const void *);
    uint32_t (*preset_query)(void *,void *,const char *);
    uint32_t (*preset_copy)(void *,uint32_t,const void *);
    uint32_t (*snapshot_read)(void *,uint32_t);
    void (*snapshot_save)(uint32_t,const void *);
    uint32_t (*normalize)(void *,uint32_t);
    void (*service)(void);
    void (*reset)(void);
} op1_drum_synth_t;
#if UINTPTR_MAX == UINT32_MAX
typedef char op1_drum_synth_descriptor_size[(sizeof(op1_drum_synth_t)==92)?1:-1];
#endif
/* One provider per name; bounded registry. Register only during OP1_EV_REGISTER.
 * A nonzero result leaves the provider inactive. Descriptor lifetime is the boot. */
int op1_drum_synth_register(const op1_drum_synth_t *);
/* Embedded screen path of an accepted file provider; NULL without an asset. */
const char *op1_drum_synth_svg(const char *name);
void kn_ds_asset(const op1_drum_synth_t *,const char *);
uint32_t op1_drum_synth_stock_type(const void *);
uint32_t op1_drum_synth_stock_normalize(void *,uint32_t);
/* Core-private composition, also exercised by host/Blackfin harnesses. */
int kn_ds_name(const char *,uint32_t);
/* An explicit native type change discards the old engine payload. Absence does not. */
void kn_ds_type_changing(void *json);
void kn_ds_type_changed(void);
/* Save identity: zero during a type/preset transition; changes on replacement. */
uint32_t op1_drum_synth_save_epoch(unsigned slot);
/* Nonzero while R246's background worker runs a job that serializes live slot patch JSON. A provider must not
 * assign, replace or remove members of a live patch then (defer and retry); kn_ds_service is skipped meanwhile. */
uint32_t op1_drum_synth_patch_busy(void);
void kn_ds_patch_job_begin(void),kn_ds_patch_job_end(void);
void kn_ds_slot_save_job(void *),kn_ds_patch_job_a(void *),kn_ds_patch_job_b(void *);
uint32_t kn_ds_type(const void *,uint32_t,const void *);
uint32_t kn_ds_factory(uint32_t);
uint32_t kn_ds_route(const void *);
uint32_t kn_ds_trigger(void *,const int16_t *);
uint32_t kn_ds_render(void *,int16_t *,const int16_t *);
uint32_t kn_ds_status(const void *);
uint32_t kn_ds_release(void *,const int16_t *);
void kn_ds_ctor(void),kn_ds_load(void *);
uint32_t kn_ds_screen(void);
int kn_ds_key(int,int),kn_ds_knob(int,const int16_t *);
uint32_t kn_ds_json_equal(const void *,const void *);
uint32_t kn_ds_preset_query(void *,void *,const char *);
uint32_t kn_ds_preset_copy(void *,uint32_t,const void *);
uint32_t kn_ds_snapshot_read(void *,uint32_t);
void kn_ds_snapshot_save(uint32_t,const void *);
uint32_t kn_ds_normalize(void *,uint32_t);
void kn_ds_service(void),kn_ds_reset(void);
#endif
