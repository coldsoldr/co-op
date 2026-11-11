/* Native sampler hook point; descriptor from pcppcp/op1r-src 7cf11afc (GPL-3.0-or-later + firmware permission).
 * Kennel registration Copyright (C) 2026 Hundekraft, same terms. */
#ifndef OP1_FEATURE_SAMPLERPLUS_H
#define OP1_FEATURE_SAMPLERPLUS_H
#include "op1_plugin.h"
typedef struct {
    void (*render)(void *self, int16_t *out, const int16_t *knobs);   /* SynthSampler voice (audio interrupt) */
    void (*note_on)(void *self, const int16_t *knobs);
    void (*note_off)(void *self);
    int  (*adsr)(void *adsr, int16_t *env, const int16_t *words, const int16_t *vel);   /* 1 = voice done */
    void (*slot_loaded)(int slot, void *op1r);       /* synth patch loaded: its "op1r" object (Json::Value) or 0 */
    int  (*is_plus)(int slot);
    void (*type_job)(void *job);                      /* synth type browser job (stock job for other rows) */
    void (*ui_build)(void);                           /* boot: its page objects for screens 6 / 0x16 */
    void (*ui_load)(void);
    void *(*screen_for)(int id);                      /* object for 6 / 0x16 (stock when the slot is not sampler+) */
    void (*sync)(int editor_open_on_6);
    int  (*knob)(int knob, int delta);                /* 1 = consumed */
    int  (*key)(int code, int pressed);
} op1_samplerplus_t;
#define OP1_SAMPLER_ROW 100                           /* synth_types id of the 'sampler+' row */

int op1_sampler_register(const op1_samplerplus_t *type); /* OP1_EV_REGISTER only; 0 success */
/* Read-only preset JSON; follows the builder-relocated stock table, not a fixed RAM address. */
int op1_sampler_preset_count(void);
const void *op1_sampler_preset(int index);
#endif
