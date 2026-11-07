/*
 * coop-fw t1: the tracker's data and state (coop_tracker.cc, coop_tracker_ui.cc). Fixed layout: the song struct is
 * the file. Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#ifndef COOP_TRACKER_H
#define COOP_TRACKER_H
#include <stdint.h>

#define TRK_CH        6
#define TRK_NOTE_OFF  254
enum { TRK_FX_NONE, TRK_FX_ARP, TRK_FX_RTG, TRK_FX_CUT, TRK_FX_DLY, TRK_FX_VOL, TRK_FX_PRB, TRK_FX_N };

struct trk_cell { uint8_t note, inst, vol, fx, fxv, pad; };               /* 6 bytes */
struct trk_song {
    char magic[4]; uint8_t version, on, orient, octave;                   /* orient: 0 landscape, 1 portrait A, 2 portrait B */
    uint16_t bpm; uint8_t speed, order_len;
    uint8_t order[32];
    uint8_t pat_len[16];
    uint8_t ch_inst[TRK_CH], ch_vol[TRK_CH];
    uint8_t reserved[20];
    struct trk_cell pat[16][64][TRK_CH];
};
struct trk_chan { int sounding, note_sounding, inst_sounding; int fx, fxv, arp_step, pending; int note_cell, inst_cell, vol_cell; };
struct trk_state {
    volatile int playing; int row_pending;
    int pos, row, tick;                                                   /* the playhead: order position, row, tick */
    int cur_pos, cur_row, cur_ch, cur_field;                              /* the cursor */
    int view;                                                             /* 0 overview, 1 zoom, 2 song, 3 settings */
    int edit;                                                             /* the keyboard writes notes */
    struct trk_chan ch[TRK_CH];
};
extern struct trk_song g_song;
extern struct trk_state g_trk;

#ifdef __cplusplus
extern "C" {
#endif
struct trk_cell *trk_cell_at(int pat, int row, int ch);
int trk_pat_len(int pat);
void coop_tracker_touch(void);
void coop_tracker_play(int from_start);
void coop_tracker_stop(void);
void coop_tracker_all_off(void);
void coop_tracker_preview(int inst, int note, int on);
int coop_tracker_save(void);
void coop_tracker_request_save(void);
int coop_tracker_on(void);
int coop_tracker_live_family(void);                                       /* the core's play mode: 0 synth, 1 drum */
void coop_tracker_set_on(int on);
#ifdef __cplusplus
}
#endif
#endif
