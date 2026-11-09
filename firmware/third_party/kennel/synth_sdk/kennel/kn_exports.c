/*
 * Core symbols a plugin file may import (docs/plugin-format.md "Imports"), by ELF name (with the C underscore).
 * Add a symbol here when a plugin needs it; never remove or change one a released plugin uses.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include "../op1_synth.h"
#include "../seq/seq_text.h"
#include "../op1_feature.h"
#include "../op1_feature_tape.h"
#include "../op1_feature_seq.h"
#include "../op1_feature_midi.h"
#include "../op1_feature_sampler.h"
#include "kn_load.h"
#include "../op1_feature_drum_synth.h"
#include "../op1_feature_sound.h"
#include "../op1_input.h"
#if KN_DIAG
#include "kn_diag.h"
#endif
#include "kn_l1.h"

int kn_switch_count(void), kn_switch_get(int i);
const char *kn_switch_label(int i);
void kn_switch_set(int i, int on);

void *read_fx_param_safe(int slot, int pos);
/* clock pages (screens/sysmon.c, usbout.c, settings.c as files): their core sides */
extern volatile uint32_t sm_aud_busy, sm_aud_max, sm_aud_max_period, sm_wb[SMC_N + 2], sm_wb_reset, sm_gap_min, sm_wb_fxn;
extern void *volatile sm_idle_obj;
extern volatile uint32_t *const sm_ps_prof, *const sm_dp_prof;
extern volatile int32_t *const sm_db_err;
extern volatile uint8_t usb_tracks_live;
extern uint8_t ci_beats;
void usbout_save(int on), countin_save(int idx);

extern volatile uint32_t kn_loaded,kn_failed,kn_skipped,kn_shed_voices,kn_arena_lo,kn_arena_hi;
int sp_adsr_stock(void *,int16_t *,const int16_t *,const int16_t *);
void dpu_sync_auto(void);
const kn_export kn_exports[] = {
    {"_op1_harmony_context",(uint32_t)op1_harmony_context},
    {"_op1_seq_note_on_quantized",(uint32_t)op1_seq_note_on_quantized},
    {"_op1_input_tuner_active",(uint32_t)op1_input_tuner_active},
    {"_op1_screen_draw_cached",(uint32_t)op1_screen_draw_cached},
    {"_op1_drum_synth_register",(uint32_t)op1_drum_synth_register},
    {"_op1_drum_synth_svg",(uint32_t)op1_drum_synth_svg},
    {"_op1_drum_synth_save_epoch",(uint32_t)op1_drum_synth_save_epoch},
    {"_op1_drum_synth_patch_busy",(uint32_t)op1_drum_synth_patch_busy},
    {"_op1_drum_synth_stock_type",(uint32_t)op1_drum_synth_stock_type},
    {"_op1_drum_synth_stock_normalize",(uint32_t)op1_drum_synth_stock_normalize},
    {"_op1_sampler_preset_count",(uint32_t)op1_sampler_preset_count},
    {"_op1_sampler_preset",(uint32_t)op1_sampler_preset},
    {"_op1_sampler_register",(uint32_t)op1_sampler_register},
    {"_sp_adsr_stock",(uint32_t)sp_adsr_stock},
    {"_sb_open",(uint32_t)sb_open}, {"_sb_close",(uint32_t)sb_close}, {"_sb_is_open",(uint32_t)sb_is_open},   /* sample browser */
    {"_sb_turn",(uint32_t)sb_turn}, {"_sb_pick",(uint32_t)sb_pick}, {"_sb_page",(uint32_t)sb_page},          /* (screens/sbrowse.c) */
    {"_dpu_sync_auto",(uint32_t)dpu_sync_auto},
    {"_sm_cy",(uint32_t)sm_cy},
#if KN_DIAG                                                    /* monitor page 7: the test build only (rows kept in place) */
    {"_kd_view",(uint32_t)kd_view}, {"_kd_reset",(uint32_t)kd_reset}, {"_kd_set_mode",(uint32_t)kd_set_mode},   /* diagnostic */
#endif
    {"_op1_l1_claim",(uint32_t)op1_l1_claim}, {"_op1_l1_claim_state",(uint32_t)op1_l1_claim_state}, {"_op1_l1_sine_quarter",(uint32_t)op1_l1_sine_quarter},   /* L1 pool (kn_l1.c) */
#if KN_DIAG
    {"_kd_set_l1",(uint32_t)kd_set_l1},
    {"_kd_set_switch",(uint32_t)kd_set_switch},                                                                /* page 7 switches */
#endif
    {"_kn_arena_lo",(uint32_t)&kn_arena_lo}, {"_kn_arena_hi",(uint32_t)&kn_arena_hi},                         /* the code arena (scale.c) */
#if KN_DIAG
    {"_kd_live",(uint32_t)kd_live},                                                                            /* page 7 shows: counters on */
    {"_kd_engine_name",(uint32_t)kd_engine_name},                                                              /* counters: page 7 */
#endif
    {"_kn_loaded",(uint32_t)&kn_loaded}, {"_kn_failed",(uint32_t)&kn_failed}, {"_kn_skipped",(uint32_t)&kn_skipped},
    {"_kn_shed_voices",(uint32_t)&kn_shed_voices},   /* soft overload: voices shed (kennel/kn_shed.c) */
    { "_op1_sound_fade", (uint32_t)op1_sound_fade },
    { "_op1_sound_apply", (uint32_t)op1_sound_apply },
    { "_op1_sound_editor_open", (uint32_t)op1_sound_editor_open },
    { "_op1_sound_engine", (uint32_t)op1_sound_engine },
    { "_op1_sound_fx", (uint32_t)op1_sound_fx },
    { "_op1_sound_fx_ready", (uint32_t)op1_sound_fx_ready },
    { "_op1_sound_lfo_ready", (uint32_t)op1_sound_lfo_ready },
    { "_op1_eng_page",        (uint32_t)op1_eng_page },
    { "_op1_dbg",             (uint32_t)op1_dbg },
    { "_op1_fx_page",         (uint32_t)op1_fx_page },
    { "_read_fx_param_safe",  (uint32_t)read_fx_param_safe },
    { "_sq_text",             (uint32_t)sq_text },
    { "_sq_line",             (uint32_t)sq_line },
    { "_sq_num",              (uint32_t)sq_num },
    { "_sq_cat",              (uint32_t)sq_cat },
    { "_sq_node_rotate",      (uint32_t)sq_node_rotate },
    { "_sq_synth_knobs_live", (uint32_t)sq_synth_knobs_live },
    { "_sq_fx_knobs_live",    (uint32_t)sq_fx_knobs_live },
    { "_op1_note_on",         (uint32_t)op1_note_on },        /* feature API (op1_feature.h) */
    { "_op1_note_off",        (uint32_t)op1_note_off },
    { "_op1_music",           (uint32_t)&op1_music },
    { "_op1_music_publish",   (uint32_t)op1_music_publish },  /* feature services */
    { "_op1_alloc",           (uint32_t)op1_alloc },
    { "_op1_toast",           (uint32_t)op1_toast },
    { "_op1_midi_send",       (uint32_t)op1_midi_send },
    { "_op1_setting_get",     (uint32_t)op1_setting_get },
    { "_op1_setting_set",     (uint32_t)op1_setting_set },
    { "_op1_setting_get_str", (uint32_t)op1_setting_get_str },
    { "_op1_setting_set_str", (uint32_t)op1_setting_set_str },
    { "_sm_aud_busy",         (uint32_t)&sm_aud_busy },       /* clock pages: sysmon (screens/sysmon_core.c) */
    { "_sm_aud_max",          (uint32_t)&sm_aud_max },
    { "_sm_aud_max_period",   (uint32_t)&sm_aud_max_period },
    { "_sm_wb",               (uint32_t)&sm_wb },             /* the slowest block's split since the page asked */
    { "_sm_wb_reset",         (uint32_t)&sm_wb_reset },
    { "_sm_gap_min",          (uint32_t)&sm_gap_min },
    { "_sm_wb_fxn",           (uint32_t)&sm_wb_fxn },         /* synth / drum FX renders in the slowest block */
    { "_sm_idle_obj",         (uint32_t)&sm_idle_obj },
    { "_sm_ps_prof",          (uint32_t)&sm_ps_prof },
    { "_sm_dp_prof",          (uint32_t)&sm_dp_prof },
    { "_sm_db_err",           (uint32_t)&sm_db_err },
    { "_usb_tracks_live",     (uint32_t)&usb_tracks_live },   /* usbout (screens/usbtrack.c, settings_store.c) */
    { "_usbout_save",         (uint32_t)usbout_save },
    { "_op1_countin_register", (uint32_t)op1_countin_register },
    { "_op1_countin_start", (uint32_t)op1_countin_start },
    { "_op1_countin_pending", (uint32_t)op1_countin_pending },
    { "_op1_countin_cancel", (uint32_t)op1_countin_cancel },
    { "_ci_beats",            (uint32_t)&ci_beats },          /* settings (tape/countin.c, settings_store.c) */
    { "_countin_save",        (uint32_t)countin_save },
    { "_tm_pos",              (uint32_t)tm_pos },             /* tape features (op1_feature_tape.h) */
    { "_tm_n",                (uint32_t)&tm_n },
    { "_pu_mode",             (uint32_t)&pu_mode },
    { "_op1_db_last_err",     (uint32_t)&op1_db_last_err },   /* op1_db_write (op1_synth.h): tape markers */
    { "_op1_key_down",        (uint32_t)op1_key_down },
    { "_op1_key_up",          (uint32_t)op1_key_up },
    { "_op1_text_entry",       (uint32_t)op1_text_entry },      /* text entry, undo area, tape edits (Maiko rebuild) */
    { "_op1_text_entry_close", (uint32_t)op1_text_entry_close },
    { "_op1_text_entry_open",  (uint32_t)op1_text_entry_open },
    { "_op1_undo_area",        (uint32_t)op1_undo_area },
    { "_op1_fx_big",           (uint32_t)op1_fx_big },
    { "_op1_tape_edit_now",    (uint32_t)op1_tape_edit_now },
    { "_op1_snapshot_defer",   (uint32_t)op1_snapshot_defer },   /* snapshot save hook (kn_save.c) */
    { "_op1_snapshot_save",    (uint32_t)op1_snapshot_save },
    { "_op1_snapshot_name_exists", (uint32_t)op1_snapshot_name_exists },
    { "_op1_seq_note_on",     (uint32_t)op1_seq_note_on },    /* sequencer types (op1_feature_seq.h) */
    { "_op1_seq_note_off",    (uint32_t)op1_seq_note_off },
    { "_op1_seq_press_play",  (uint32_t)op1_seq_press_play },
    { "_sq_dot",              (uint32_t)sq_dot },
    { "_sq_join",             (uint32_t)sq_join },
    { "_sq_split",            (uint32_t)sq_split },
    { "_sq_store_load",       (uint32_t)sq_store_load },
    { "_sq_store_dirty",      (uint32_t)sq_store_dirty },
    { "_op1_storage",          (uint32_t)op1_storage },          /* storage walk (kn_storage.c) */
    { "_op1_storage_refresh",  (uint32_t)op1_storage_refresh },
    { "_op1_flash_open",          (uint32_t)op1_flash_open },   /* flash check (kn_flash.c) */
    { "_op1_flash_close",         (uint32_t)op1_flash_close },
    { "_op1_flash_shown",         (uint32_t)op1_flash_shown },
    { "_op1_flash_draw",          (uint32_t)op1_flash_draw },
    { "_op1_flash_push",          (uint32_t)op1_flash_push },
    { "_op1_flash_counts",        (uint32_t)op1_flash_counts },
    { "_op1_tape_write_now",   (uint32_t)op1_tape_write_now },   /* tape writeback, flush (kn_tape.c) */
    { "_op1_tape_read_old",    (uint32_t)op1_tape_read_old },
    { "_op1_tape_flush",       (uint32_t)op1_tape_flush },
    { "_op1_tape_read",        (uint32_t)op1_tape_read },        /* undo: range read and restore */
    { "_op1_tape_restore",     (uint32_t)op1_tape_restore },
    { "_op1_tape_clips_get",   (uint32_t)op1_tape_clips_get },   /* undo: clip rows (kn_clips.c) */
    { "_op1_tape_clips_set",   (uint32_t)op1_tape_clips_set },
    { "_op1_file_open",          (uint32_t)op1_file_open },   /* files under /yaffs2/user/ (kn_files.c) */
    { "_op1_file_read",          (uint32_t)op1_file_read },
    { "_op1_file_write",         (uint32_t)op1_file_write },
    { "_op1_file_seek",          (uint32_t)op1_file_seek },
    { "_op1_file_close",         (uint32_t)op1_file_close },
    { "_op1_file_unlink",        (uint32_t)op1_file_unlink },
    { "_op1_file_mkdir",         (uint32_t)op1_file_mkdir },
    { "_op1_tape_loop_get",      (uint32_t)op1_tape_loop_get },   /* tape services (kn_tapesvc.c) */
    { "_op1_tape_loop_set",      (uint32_t)op1_tape_loop_set },
    { "_op1_tape_track",         (uint32_t)op1_tape_track },
    { "_op1_tape_pos_get",       (uint32_t)op1_tape_pos_get },
    { "_op1_tape_pos_set",       (uint32_t)op1_tape_pos_set },
    { "_op1_tape_transport",     (uint32_t)op1_tape_transport },
    { "_op1_clipboard_info",     (uint32_t)op1_clipboard_info },
    { "_op1_clipboard_read",     (uint32_t)op1_clipboard_read },
    { "_op1_clipboard_write",    (uint32_t)op1_clipboard_write },
    { "_op1_clipboard_slot",     (uint32_t)op1_clipboard_slot },
    { "_op1_tape_switch",        (uint32_t)op1_tape_switch },  /* four tapes (kn_tapes.c) */
    { "_op1_tape_current",       (uint32_t)op1_tape_current },
    { "_op1_tape_bpm10",         (uint32_t)op1_tape_bpm10 },
    { "_op1_flash_free",         (uint32_t)op1_flash_free },
    { "_op1_note_source",     (uint32_t)op1_note_source },    /* MIDI and synth services (op1_feature_midi.h) */
    { "_op1_param_set",       (uint32_t)op1_param_set },
    { "_op1_midi_take_cc",    (uint32_t)op1_midi_take_cc },
    { "_op1_stock_seq_key",   (uint32_t)op1_stock_seq_key },
    { "_op1_tape_view_open",  (uint32_t)op1_tape_view_open },
    { "_op1_tape_view_close", (uint32_t)op1_tape_view_close },
    { "_op1_tape_view_enable", (uint32_t)op1_tape_view_enable },
    { "_op1_switch",          (uint32_t)op1_switch },
    { "_op1_switch_on",       (uint32_t)op1_switch_on },
    { "_kn_switch_count",     (uint32_t)kn_switch_count },    /* the settings page's switch rows (screens/settings.c) */
    { "_kn_switch_label",     (uint32_t)kn_switch_label },
    { "_kn_switch_get",       (uint32_t)kn_switch_get },
    { "_kn_switch_set",       (uint32_t)kn_switch_set },
    { "_op1_sampler_now",        (uint32_t)op1_sampler_now },   /* sampler tools (kn_sampler.c) */
    { "_op1_sampler_marker",     (uint32_t)op1_sampler_marker },
    { "_op1_sampler_set_marker", (uint32_t)op1_sampler_set_marker },
    { "_op1_sampler_commit",     (uint32_t)op1_sampler_commit },
    { "_op1_sampler_base_freq",  (uint32_t)op1_sampler_base_freq },
    { "_op1_sampler_set_base_freq", (uint32_t)op1_sampler_set_base_freq },
    { "_op1_sampler_job",        (uint32_t)op1_sampler_job },
    { "_op1_sampler_job_running", (uint32_t)op1_sampler_job_running },
};
const int kn_export_count = sizeof kn_exports / sizeof kn_exports[0];
