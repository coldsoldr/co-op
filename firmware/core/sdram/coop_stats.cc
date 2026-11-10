/*
 * coop-fw device statistics (format 3, T4): the counters the coop stages keep (per-block cycle peaks and sums, tape
 * I/O work, edits, state-mismatch detectors, the L1 pool) written as text into the last 8 KiB of track 4's audio in
 * the tape image (the one channel disk mode copies unchanged: tape/track_4.aif, read with tools/tape_tail_stats.py)
 * and to /yaffs2/user/coop_stats.txt (not visible in disk mode, which is a synthesised FAT). Nothing else on the
 * device can read our words (docs/performance.md).
 *
 * Every WINDOW_CYCLES (5 s) coop_stats_tick snapshots the window counters (the *_win words the producers keep:
 * whole audio callback, record tap, mix replacement, voice hook, I/O step / gap / load / flush) and the growth of
 * the cumulative counters (voice hooks, side blocks, record calls, loads, flushes) into a ring of the last WINDOWS
 * windows and, separately, a ring of the TOP heaviest windows by cb_max (so a chord or a take is never pushed out by
 * time); then zeroes them. The file is rewritten after a window closes while nothing is recording (the write is
 * main-loop work we keep out of a take), and once after FIRST_WRITE_PASSES passes so a short session leaves a file
 * that names the build. Format: one "name value" pair list per line.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>

#define Y_OPEN        ((int (*)(const char *, int, int))0x01A997F4u)
#define Y_CLOSE       ((int (*)(int))0x01A99908u)
#define Y_WRITE       ((int (*)(int, const void *, int))0x01A99F84u)
#define STATS_PATH    "/yaffs2/user/coop_stats.txt"
#include "coop_build.h"
#define STATS_BUILD   COOP_BUILD_TAG                                  /* the package tag (one place, p6k): proves which image wrote the file */
#define WINDOW_CYCLES 2000000000u                                     /* 5 s at 400 MHz */
#define WINDOWS       24                                              /* p5a: 2 minutes (was 12) */
#define TOP           8
#define FIRST_WRITE_PASSES 64

extern volatile uint32_t g_last, g_max, g_blocks, g_max_win, g_blocks_win, g_cb_sum_win, g_cb_over_win;   /* coop_cb.cc */
extern volatile uint32_t g_stereo_hooks, g_stereo_hook_cycles_max, g_stereo_mix_cycles_max, g_stereo_mix_cycles_win, g_stereo_side_blocks;
extern volatile uint32_t g_stereo_hook_cycles_win, g_stereo_hook_cycles_sum_win, g_stereo_mix_cycles_sum_win, g_stereo_l1;
extern volatile uint32_t g_stereo_stage_cycles_max, g_stereo_stage_calls;   /* stereo_bus.cc, S3 */
extern volatile uint32_t g_lfo2_cycles_max;
extern volatile uint32_t g_tape_rec_calls, g_tape_rec_cycles_max, g_tape_rec_cycles_win, g_tape_rec_cycles_sum_win, g_tape_rec_side_blocks, g_tape_l1;
extern volatile uint32_t g_tape_state_mismatch, g_tape_state_mismatch2, g_tape_mismatch_word, g_tape_seam_fills;
extern volatile uint32_t g_tape_io_loads, g_tape_io_flushes, g_tape_io_held, g_tape_io_guarded, g_tape_io_errors;
extern volatile uint32_t g_tape_io_step_cycles_max, g_tape_io_load_cycles_max, g_tape_io_flush_cycles_max, g_tape_io_gap_cycles_max;
extern volatile uint32_t g_tape_io_step_cycles_win, g_tape_io_load_cycles_win, g_tape_io_flush_cycles_win, g_tape_io_gap_cycles_win;
extern volatile uint32_t g_tape_io_steps_win;
extern volatile uint32_t g_edit_lifts, g_edit_drops, g_edit_erases, g_edit_errors, g_edit_mono_fallbacks;
extern volatile uint32_t g_master_cycles;
extern volatile uint32_t g_l1_state, g_l1_used, g_l1_refused, g_l1_sine_copied;
extern volatile uint32_t g_l1_leased, g_l1_leases, g_l1_lease_refused;   /* l1_pool.cc (e4) */
extern "C" uint32_t coop_l1_free(void);
extern volatile uint32_t g_lander_swallowed;   /* lander_off.cc */
extern "C" volatile uint32_t g_coop_fast_dsynth, g_coop_fast_stage, g_coop_fast_cwo, g_coop_fast_punch, g_coop_fast_phone,
    g_coop_fast_nitro, g_coop_fast_grid, g_coop_fast_spring, g_coop_fast_delay, g_coop_fast_voltage, g_coop_fast_phase;   /* stereo_bus.cc */
extern volatile uint32_t g_plugins_seen, g_plugins_loaded, g_plugins_failed, g_fx_dispatch_mono, g_fx_dispatch_st, g_fx_plugin_blocks, g_fx_stock_stereo_blocks, g_fx_stock_ctor, g_s2_blocks[2], g_s2_changes, g_tank_bound, g_tank_short, g_ui_opens, g_ui_applies, g_plugins_disabled, g_ui_toggles, g_set_opens, g_set_saves, g_ui_col2_sets, g_rnd_count, g_rnd_backs, g_rnd_refused, g_rnd_slot2, g_solo_sets, g_marker_sets, g_marker_jumps, g_fn_taps, g_mode_key_taps, g_mode_key_holds, g_sound_keeps, g_sound_kept, g_sound_writes, g_bend_presses, g_bend_knobs, g_bend_ramps, g_over_saves, g_over_armed, g_over_stock, g_rename_starts, g_rename_saves, g_rename_refused, g_drumpan_knobs, g_rename_pushes, g_fx2view_params, g_fx2view_disp, g_fx2view_pops, g_fx2view_empty, g_fx2view_size, g_fx2view_ids, g_fx2view_last_post, g_fx2view_cur, g_lfo2view_params, g_lfo2view_pops, g_fx2view_on, g_lfo2view_on, g_ui_knobs2;   /* plugin_host.cc, coop_ui.cc, coop_settings.cc */
extern const char *volatile g_plugins_last_error;
extern volatile uint32_t g_engines_loaded, g_heap_largest;   /* plugin_host.cc (E1, e4) */
extern volatile uint32_t g_ctl_turns, g_ctl_clicks, g_ctl_ignored, g_ctl_cancelled, g_ctl_engines, g_ctl_popup_frames;   /* engine_ui.cc (e4) */                                 /* plugin_host.cc (E1) */
extern volatile uint32_t g_eng_renders, g_eng_noteons, g_eng_iter_renders, g_eng_switches, g_eng_foreign, g_eng_cycles_max;   /* engine_host.cc */
extern "C" void coop_eng_render(void *, int16_t *, const int16_t *);
extern "C" int coop_engine_cost(int n, uint32_t *out6);                   /* engine_host.cc (e4) */
extern "C" const char *coop_engine_name(int n);                           /* plugin_host.cc (e4) */
extern volatile uint32_t g_escr_built, g_escr_failed, g_escr_swaps, g_escr_resets, g_escr_updates;   /* engine_screen.cc (E2) */
extern volatile uint32_t g_st0_builds, g_st0_dry_blocks;           /* plugin_host.cc: slot 1's twin built off the audio path */
extern char g_plugins_last_name[];                                 /* l1_pool.cc */
extern "C" int coop_tape_write_tail(const char *text, int len);   /* tape_io.cc: the last 8 KiB of track 4's audio */

volatile uint32_t g_stats_writes, g_stats_errors, g_stats_windows, g_stats_ticks, g_stats_tail_writes, g_stats_tail_errors;

struct window {
    uint32_t seq, blocks, cb_max, cb_sum, cb_over, rec_max, rec_sum, mix_max, mix_sum, hook_max, hook_sum, hooks, side, recs;
    uint32_t io_steps, io_step_max, io_gap_max, io_load_max, io_flush_max, loads, flushes;
    uint8_t recording;
};
static struct window g_ring[WINDOWS], g_top[TOP];
static uint32_t g_ring_next;
static uint32_t g_window_start;
static uint32_t g_prev_hooks, g_prev_side, g_prev_recs, g_prev_loads, g_prev_flushes;
static int g_started;
static uint8_t g_win_recording;
static char g_text[16384];                                      /* p5a: the whole file over SysEx; the tape tail keeps the first 8 KiB */
static int g_len;

static inline uint32_t now_cycles(void) { uint32_t v; __asm__ __volatile__("%0 = CYCLES;" : "=d"(v)); return v; }

static char *g_out = g_text;                                    /* where put() writes: g_text, or a run-file buffer */
static int g_cap = (int)sizeof g_text;
static void put(const char *s) { while (*s && g_len < g_cap - 1) g_out[g_len++] = *s++; }
static void putn(uint32_t v)
{
    char b[12]; int i = 0;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i && g_len < g_cap - 1) g_out[g_len++] = b[--i];
}
static void kv(const char *k, uint32_t v) { put(" "); put(k); put(" "); putn(v); }

static void put_window(const char *tag, const struct window *w)
{
    put(tag); kv("seq", w->seq); kv("blocks", w->blocks); kv("cb_max", w->cb_max); kv("cb_sum", w->cb_sum); kv("cb_over", w->cb_over);
    kv("rec_max", w->rec_max); kv("rec_sum", w->rec_sum); kv("mix_max", w->mix_max); kv("mix_sum", w->mix_sum);
    kv("hook_max", w->hook_max); kv("hook_sum", w->hook_sum); kv("hooks", w->hooks); kv("side", w->side); kv("recs", w->recs);
    kv("io_steps", w->io_steps); kv("io_step_max", w->io_step_max); kv("io_gap_max", w->io_gap_max);
    kv("io_load_max", w->io_load_max); kv("io_flush_max", w->io_flush_max); kv("loads", w->loads); kv("flushes", w->flushes);
    kv("recording", w->recording); put("\n");
}

#include "coop_cost.h"
struct coop_cost g_coop_cost[COOP_COST_N];
uint32_t g_coop_eng_fn[COOP_ENG_N];
static uint32_t g_eng_blk[2];                                     /* this block's synth / drum render cycles */
static uint32_t g_eng_blk_pf[2][2];                               /* p6e: and the two counters' deltas, summed per block */
static uint32_t g_eng_pf0[2];                                     /* the counters at the render's start (coop_eng_pf_start) */
static uint32_t g_eng_last;                                       /* the entry the last render used */

/* p6e: from the render thunks (stereo_thunks.S) just before the timed render: the counters' start values (the
 * thunk reads CYCLES itself a few instructions later; renders never nest, so one pair of words is enough). */
extern "C" void coop_eng_pf_start(void) { coop_pf_now(g_eng_pf0); }
static void cost_add_d(int i, uint32_t dt, uint32_t d0, uint32_t d1)
{
    struct coop_cost *c = &g_coop_cost[i];
    c->sum += dt; c->n++;
    if (dt > c->max) c->max = dt;
    c->p0 += d0; c->p1 += d1;
}

/* From the render thunks (stereo_thunks.S) after every voice render: the cycles of one render of the engine whose
 * render function is fn. The table fills in the order the engines are first heard; COOP_ENG_N covers every stock
 * synth and drum engine of a session. */
extern "C" void coop_eng_cost(uint32_t fn, uint32_t dt, int drum)
{
    uint32_t now = coop_cost_now();
    uint32_t d[2] = { 0, 0 };
    if (g_coop_pf_live) { coop_pf_now(d); d[0] -= g_eng_pf0[0]; d[1] -= g_eng_pf0[1]; }
    coop_mark_at(0x10 + (drum & 1), now - dt);                     /* the render's start (the thunk read CYCLES there) */
    coop_mark_at(0x18 + (drum & 1), now);
    if (!drum) cost_add_d(g_eng_blk[0] ? COOP_COST_VREST : COOP_COST_VFIRST, dt, d[0], d[1]);   /* p6f: first of the block vs the rest */
    g_eng_blk[drum & 1] += dt;
    g_eng_blk_pf[drum & 1][0] += d[0]; g_eng_blk_pf[drum & 1][1] += d[1];
    uint32_t i = g_eng_last;
    if (g_coop_eng_fn[i] != fn) {
        for (i = 0; i < COOP_ENG_N; i++) {
            if (g_coop_eng_fn[i] == fn) break;
            if (!g_coop_eng_fn[i]) { g_coop_eng_fn[i] = fn; break; }
        }
        if (i == COOP_ENG_N) return;
        g_eng_last = i;
    }
    cost_add_d(COOP_COST_ENG + (int)i, dt, d[0], d[1]);
}

/* From coop_stereo_block_start: the previous block's render sums. */
extern "C" void coop_engine_block_end(void);                              /* engine_host.cc (e4): per-engine block totals */
extern "C" void coop_eng_block_end(void)
{
    coop_engine_block_end();
    for (int k = 0; k < 2; k++) {
        if (!g_eng_blk[k]) continue;
        cost_add_d(k ? COOP_COST_DRUM : COOP_COST_SYNTH, g_eng_blk[k], g_eng_blk_pf[k][0], g_eng_blk_pf[k][1]);
        g_eng_blk[k] = 0; g_eng_blk_pf[k][0] = 0; g_eng_blk_pf[k][1] = 0;
    }
}
/* The slow-block profiler (coop_cost.h): the marks of the current block, and copies of the last SLOW_KEEP blocks
 * that ran over g_coop_slow_th cycles plus the worst one since boot. */
volatile uint32_t g_mk_n, g_mk_lost;
uint32_t g_mk[COOP_MK_N][2];
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_slow_th = 1000000; }   /* ~1.7 blocks */
#define SLOW_KEEP 4
struct slow { uint32_t dt, block, n, lost; uint32_t mk[COOP_MK_N][2]; };
static struct slow g_slow[SLOW_KEEP + 1];                          /* [SLOW_KEEP]: the worst */
static uint32_t g_slow_next, g_slow_count;
static void slow_copy(struct slow *d, uint32_t dt)
{
    uint32_t n = g_mk_n;
    d->dt = dt; d->block = g_blocks; d->n = n; d->lost = g_mk_lost;
    for (uint32_t i = 0; i < n; i++) { d->mk[i][0] = g_mk[i][0]; d->mk[i][1] = g_mk[i][1]; }
}
/* From CoopAudioCb (the audio interrupt) when a block ran over the threshold. */
extern "C" void coop_slow_keep(uint32_t dt)
{
    slow_copy(&g_slow[g_slow_next % SLOW_KEEP], dt);
    g_slow_next++; g_slow_count++;
    if (dt > g_slow[SLOW_KEEP].dt) slow_copy(&g_slow[SLOW_KEEP], dt);
    g_mk_lost = 0;
}

/* The gap profile (coop_cost.h). From CoopAudioCb, every block, when g_coop_gap_prof is set (0 in the packages but the
 * diagnostic one). The marks are in time order but for the render starts (recorded at the render's end), so an
 * insertion sort costs a few compares each. */
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_gap_prof = 0; }
struct coop_gap g_gap[COOP_GAP_N];
uint32_t g_gap_blocks, g_gap_full;
extern "C" void coop_gap_block(void)
{
    uint32_t n = g_mk_n;
    if (n > COOP_MK_N) n = COOP_MK_N;
    for (uint32_t i = 1; i < n; i++) {
        uint32_t id = g_mk[i][0], t = g_mk[i][1], j = i;
        uint32_t t0 = g_mk[0][1];
        while (j > 0 && g_mk[j - 1][1] - t0 > t - t0) { g_mk[j][0] = g_mk[j - 1][0]; g_mk[j][1] = g_mk[j - 1][1]; j--; }
        g_mk[j][0] = id; g_mk[j][1] = t;
    }
    for (uint32_t i = 1; i < n; i++) {
        uint32_t key = (g_mk[i - 1][0] & 0xFF) << 8 | (g_mk[i][0] & 0xFF), dt = g_mk[i][1] - g_mk[i - 1][1];
        uint32_t k = 0;
        while (k < COOP_GAP_N && g_gap[k].n && g_gap[k].key != key) k++;
        if (k == COOP_GAP_N) { g_gap_full++; continue; }
        struct coop_gap *g = &g_gap[k];
        g->key = key; g->n++; g->sum += dt; if (dt > g->max) g->max = dt;
    }
    g_gap_blocks++;
}

/* p6a..p6i: the Blackfin performance monitor (coop_cost.h). PFCTL 0xFFE08000 = PFPWR | PFCEN0 = 3 << 3 | PFMON0 << 5 |
 * PFCEN1 = 3 << 14 | PFMON1 << 16 | PFCNT0 << 24 | PFCNT1 << 25 (kennel's kn_diag.c positions; Linux bfin_pfmon.h's,
 * one bit lower for counter 0, count nothing on this unit: p6g). THE EVENTS ARE THE SWEEP'S, NOT A TABLE'S: p6h ran
 * every code event 0x80..0x8F on counter 0 and every data event 0x90..0x9F on counter 1, as cycles and as
 * occurrences, against known cold / warm workloads on the device (docs/performance.md "p6h"). Counter 0 (written
 * through PFMON0 << 5 as below; only its low two bits select, the pattern repeats every four): 0x83 as cycles =
 * code fetch STALL cycles (26,359 on a 128-line cold pass, 21 warm); 0x81 as occurrences = instruction-cache line
 * fills (133 / 4); 0x82 = 64-bit fetches delivered (~520 either way: what p6a..p6g mistook for stalls). Counter 1
 * follows Linux's numbering: 0x99 "data stalls sent to processor" as cycles = the whole cold-pass wait (52,571 on
 * 256 lines, 0 warm); 0x98 "data stalls" near it (57,077 / 1,227: p6b's column, valid); 0x9A as occurrences = data
 * cache fills (256 / 1); 0x95 / 0x96 partial stall kinds. Modes (g_coop_pf_mode, a --set-word), p0 / p1:
 * 0 = code fetch stall cycles (0x83 s) / data stall cycles (0x99 s); 1 = I-cache line fills (0x81 o) / D-cache
 * fills (0x9A o); 2 = 0x83 s / 0x98 s; 3 = 0x83 s / 0x96 s; 4 = 0x83 s / 0x95 s; 0xFF = off. Programmed once at
 * boot on the main thread (coop_plugins_boot); the counters run free, every cost site takes deltas. op1emu does not
 * model the unit: g_coop_pf_live stays 0 there if the write does not stick. */
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_pf_mode = 0; }
/* p6g: the PFCTL bit layout. kennel's (0): PFCEN0 at 3, PFMON0 at 5, PFCEN1 at 14. Linux bfin_pfmon.h's (1): PFCEN0
 * at 2, PFMON0 at 4, PFCEN1 at 13 (PFMON1 at 16 and PFCNT0 / 1 at 24 / 25 in both). The p6f probe on the device
 * (a 4 KB NOP function: cold pass 27,175 cycles, warm pass 2,058) read ~520 on p0 in BOTH passes under layout 0:
 * one count per 8 bytes of code fetched, not a stall cycle, so layout 0 does not select the event it names. The
 * probe now runs under both layouts and, in auto (2), keeps the one whose cold pass counts thousands and whose
 * warm pass counts near nothing; `pf_layout` on the cost line says which (bit 4 set: neither qualified, layout 1
 * kept unproven). A --set-word forces 0 or 1. */
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_pf_layout = 2; }
volatile uint32_t g_coop_pf_live, g_coop_pf_layout_used;
#define PFCTL (*(volatile uint32_t *)0xFFE08000u)
static uint32_t pf_word(int layout, uint32_t ev0, uint32_t cnt0, uint32_t ev1, uint32_t cnt1)
{
    if (layout == 1) return 1u | 3u << 2 | ev0 << 4 | 3u << 13 | ev1 << 16 | cnt0 << 24 | cnt1 << 25;
    return 1u | 3u << 3 | ev0 << 5 | 3u << 14 | ev1 << 16 | cnt0 << 24 | cnt1 << 25;
}
static void pf_program(int layout)
{
    static const uint8_t ev[5][2] = { { 0x83, 0x99 }, { 0x81, 0x9A }, { 0x83, 0x98 }, { 0x83, 0x96 }, { 0x83, 0x95 } };   /* p6i: the sweep's events */
    static const uint8_t cnt[5][2] = { { 0, 0 }, { 1, 1 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };
    uint32_t m = g_coop_pf_mode;
    if (m > 4) { PFCTL = 0; g_coop_pf_live = 0; return; }
    PFCTL = 0;                                                     /* off, then the new word: no field of the old one survives */
    __asm__ __volatile__("ssync;" ::: "memory");
    PFCTL = pf_word(layout, ev[m][0], cnt[m][0], ev[m][1], cnt[m][1]);
    __asm__ __volatile__("ssync;" ::: "memory");
    g_coop_pf_live = (PFCTL & 1u) ? 1 : 0;                      /* the write stuck: a real monitor (op1emu reads back 0) */
    g_coop_pf_layout_used = (uint32_t)layout;
}
extern "C" void coop_pf_boot(void) { pf_program(g_coop_pf_layout == 1 ? 1 : 0); }   /* auto starts as 0; the probe decides */

/* p6f: the instruction-cache probe (docs/fx-data-placement-plan.md §5, the voice table's open question): a 4 KB
 * straight-line function (2048 NOPs: 128 cache lines, no loads, no branches) run three times from the main thread at
 * boot, with CYCLES and the code-fill-stall counter around each pass. Pass 1 fills the lines from SDRAM: its p0 / 128
 * is the fill stall per line on this unit. Pass 2 immediately after should hit (p0 near 0) if the cache keeps
 * region-A code at all. Pass 3 comes after touching 24 KB of OTHER code (the cold UI functions, called with
 * harmless arguments is not possible, so: a second 24 KB NOP function) and says whether 4 KB survives 24 KB of
 * traffic in a 16 KB 4-way cache (it should: LRU keeps the most recent 16 KB, so no). The stats print the three
 * (cycles, p0) pairs on the `icache` line; the voice question is answered by comparing pass 1's per-line cost with
 * the 12 k per DSynth render. */
__attribute__((noinline, aligned(32))) static void probe_4k(void)  { __asm__ __volatile__(".rept 2048\n nop;\n .endr" ::: "memory"); }
__attribute__((noinline, aligned(32))) static void probe_24k(void) { __asm__ __volatile__(".rept 12288\n nop;\n .endr" ::: "memory"); }
static uint32_t g_icp[2][3][2];                                  /* per layout, per pass: (cycles, p0) */
static void probe_passes(uint32_t out[3][2])
{
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 2) probe_24k();
        uint32_t p0[2], p1[2];
        coop_pf_now(p0);
        uint32_t c0 = coop_cost_now();
        probe_4k();
        uint32_t c1 = coop_cost_now();
        coop_pf_now(p1);
        out[pass][0] = c1 - c0; out[pass][1] = p1[0] - p0[0];
    }
}
static int layout_counts_stalls(const uint32_t icp[3][2])         /* cold pass thousands of counts, warm pass near none */
{
    return icp[0][1] > 1000 && icp[0][1] > 4 * icp[1][1] + 100;
}
extern "C" void coop_icache_probe(void)                          /* main thread, boot, after coop_pf_boot (p6g: both layouts) */
{
    if (!g_coop_pf_live) { probe_passes(g_icp[0]); return; }    /* op1emu: the cycles alone */
    for (int l = 0; l < 2; l++) { pf_program(l); probe_passes(g_icp[l]); }
    uint32_t want = g_coop_pf_layout;
    if (want > 1) {                                              /* auto: the layout the probe proves, else 0 flagged unproven
                                                                    (p6g on the device: layout 1 counts nothing at all) */
        if (layout_counts_stalls(g_icp[0])) want = 0;
        else if (layout_counts_stalls(g_icp[1])) want = 1;
        else want = 0 | 0x10;
    }
    pf_program((int)(want & 1));
    g_coop_pf_layout_used = want;
}

/* p6h: the event sweep. p6g on the device: kennel's field positions are the working ones (Linux's count nothing),
 * but event 0x82 counts instruction fetches (~1 per 8 bytes), not fill stalls. So, once at boot, every code event
 * 0x80..0x8F goes on counter 0 and every data event 0x90..0x9F on counter 1, each as cycles (PFCNT 0, "s") and as
 * occurrences (PFCNT 1, "o"), against known workloads: code = the 4 KB NOP function cold (after 24 KB of other
 * code) and warm; data = an 8 KB array read cold (after a 32 KB walk of another array) and warm. The stats print
 * cold/warm counts per event and mode (`pfsweep_code`, `pfsweep_data`); the event whose cold count is the pass's
 * ~25 k stall cycles and whose warm count is ~0 is the stall event, the one with ~128 cold / 0 warm the miss count.
 * ~40 ms of boot, main thread, before the live programming. */
static uint32_t g_sweep[2][16][2][2];                            /* [side][event][cnt][cold, warm] */
static volatile uint32_t g_probe_data[2048] __attribute__((aligned(32)));   /* 8 KB: 256 data lines */
static volatile uint32_t g_evict_data[8192] __attribute__((aligned(32)));   /* 32 KB: walks the 16 KB data cache out */
static uint32_t data_walk(volatile uint32_t *a, uint32_t n) { uint32_t s = 0; for (uint32_t i = 0; i < n; i += 8) s += a[i]; return s; }
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_pf_sweep = 0; }   /* p6i: the sweep only on request (--set-word): ~40 ms of boot */
extern "C" void coop_pf_sweep(void)
{
    if (!g_coop_pf_live || !g_coop_pf_sweep) return;
    uint32_t m = g_coop_pf_mode;
    if (m > 4) return;
    for (int ev = 0; ev < 16; ev++)
        for (int cnt = 0; cnt < 2; cnt++) {
            /* code side: counter 0 = 0x80 + ev */
            PFCTL = 0; __asm__ __volatile__("ssync;" ::: "memory");
            PFCTL = pf_word(0, 0x80u + (uint32_t)ev, (uint32_t)cnt, 0x98u, 0); __asm__ __volatile__("ssync;" ::: "memory");
            probe_24k();
            uint32_t a[2], b[2];
            coop_pf_now(a); probe_4k(); coop_pf_now(b); g_sweep[0][ev][cnt][0] = b[0] - a[0];
            coop_pf_now(a); probe_4k(); coop_pf_now(b); g_sweep[0][ev][cnt][1] = b[0] - a[0];
            /* data side: counter 1 = 0x90 + ev */
            PFCTL = 0; __asm__ __volatile__("ssync;" ::: "memory");
            PFCTL = pf_word(0, 0x82u, 0, 0x90u + (uint32_t)ev, (uint32_t)cnt); __asm__ __volatile__("ssync;" ::: "memory");
            (void)data_walk(g_evict_data, 8192);
            coop_pf_now(a); (void)data_walk(g_probe_data, 2048); coop_pf_now(b); g_sweep[1][ev][cnt][0] = b[1] - a[1];
            coop_pf_now(a); (void)data_walk(g_probe_data, 2048); coop_pf_now(b); g_sweep[1][ev][cnt][1] = b[1] - a[1];
        }
    pf_program((int)(g_coop_pf_layout_used & 1));                /* back to the live programming */
}

static const struct coop_cost *g_cost_src = g_coop_cost;          /* the live table, or a run section's copy */
static void put_cost(const char *name, int i)               /* name avg/max/n[/p0/p1]: cycles per call, the counters' averages */
{
    const struct coop_cost *c = &g_cost_src[i];
    if (!c->n) return;
    put(" "); put(name); putn((uint32_t)(c->sum / c->n)); put("/"); putn(c->max); put("/"); putn(c->n);
    if (c->p0 || c->p1) { put("/"); putn((uint32_t)(c->p0 / c->n)); put("/"); putn((uint32_t)(c->p1 / c->n)); }
}

/* The stock engines' render functions (synth: vtable + 0x14, read from the constructors in the stock disassembly,
 * docs/performance.md "Per-engine cost"); the other drum engines and iter print as r_ + their address. */
static const char *eng_name(uint32_t fn)
{
    static const struct { uint32_t fn; const char *name; } k[] = {
        { 0x019A23D4u, "fm=" }, { 0x01A51F7Au, "digital=" }, { 0x0197C870u, "drwave=" }, { 0x01A4F098u, "string=" },
        { 0x01960288u, "cluster=" }, { 0xFFA04B88u, "pulse=" }, { 0x01A51A7Cu, "sampler=" }, { 0x01A0BC5Cu, "phase=" },
        { 0x019D33A0u, "ls=" }, { 0x019B48D8u, "dna=" }, { 0x0197E1D8u, "dsynth=" }, { 0x01A49BF4u, "voltage=" },
        { 0x019788B0u, "drumsampler=" },                                /* DrumSampler.cpp, vtable 0x0190FAC4 + 0x24 (op1emu det_drum) */
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) if (k[i].fn == fn) return k[i].name;
    if (fn == (uint32_t)(uintptr_t)&coop_eng_render) return "plugin_engine=";   /* E1: every plugin engine (and iter) */
    static char hex[COOP_ENG_N][12];                              /* "r_" + 8 hex digits + "=", one per table entry */
    for (int i = 0; i < COOP_ENG_N; i++) {
        if (g_coop_eng_fn[i] != fn) continue;
        char *h = hex[i]; h[0] = 'r'; h[1] = '_';
        for (int d = 0; d < 8; d++) h[2 + d] = "0123456789abcdef"[(fn >> (28 - 4 * d)) & 15];
        h[10] = '='; h[11] = 0; return h;
    }
    return "?";
}

static void put_cost_lines(void)                             /* the "cost" and "engines" lines from g_cost_src */
{
    put("cost avg/max/n/p0/p1"); put(" pf_mode "); putn(g_coop_pf_mode); put(" pf_live "); putn(g_coop_pf_live); put(" pf_layout "); putn(g_coop_pf_layout_used);   /* per call; slot 1's stock effect by type (stereo class); p0 / p1 the monitor's averages (coop_pf_boot) */
    put_cost("stage=", COOP_COST_STAGE); put_cost("s2_instr=", COOP_COST_S2I); put_cost("s2_master=", COOP_COST_S2M);
    put_cost("master=", COOP_COST_MASTER); put_cost("fx1_plugin=", COOP_COST_PLUG);
    static const char *const st_names[8] = { "fx1_delay=", "fx1_grid=", "fx1_punch=", "fx1_phone=", "fx1_spring=", "fx1_filter=", "fx1_nitro=", "fx1_cwo=" };
    for (int t = 0; t < 8; t++) put_cost(st_names[t], COOP_COST_ST1 + t);
    put("\n");
    put("engines avg/max/n");                                /* per voice render, by engine; synth= / drum= per block */
    put_cost("synth=", COOP_COST_SYNTH); put_cost("drum=", COOP_COST_DRUM);
    put_cost("synth_first=", COOP_COST_VFIRST); put_cost("synth_rest=", COOP_COST_VREST);   /* p6f: per render, the block's first synth voice vs the others */
    for (int i = 0; i < COOP_ENG_N; i++) {
        if (!g_coop_eng_fn[i]) break;
        put_cost(eng_name(g_coop_eng_fn[i]), COOP_COST_ENG + i);
    }
    for (int n = 1; n < 64; n++) {                                     /* e4: each plugin engine, per voice render and per block */
        uint32_t c[6];
        if (!coop_engine_cost(n, c)) continue;
        const char *nm = coop_engine_name(n);
        put(" eng_"); put(nm ? nm : "?"); put("="); putn(c[0]); put("/"); putn(c[1]); put("/"); putn(c[2]);
        put(" eng_"); put(nm ? nm : "?"); put("_block="); putn(c[3]); put("/"); putn(c[4]); put("/"); putn(c[5]);
    }
    put("\n");
}

static void puthex2(uint32_t v) { char b[3] = { "0123456789abcdef"[(v >> 4) & 15], "0123456789abcdef"[v & 15], 0 }; put(b); }
static void put_slow(const char *tag, const struct slow *w)        /* marks in time order, cycles after the callback start */
{
    if (!w->dt) return;
    put(tag); kv("cb", w->dt); kv("block", w->block); kv("marks", w->n); kv("lost", w->lost); put(":");
    uint32_t t0 = w->n ? w->mk[0][1] : 0, last = 0;
    for (uint32_t i = 0; i < w->n; i++) if (w->mk[i][0] == 0x01) t0 = w->mk[i][1];
    static uint8_t done[COOP_MK_N];
    for (uint32_t i = 0; i < w->n; i++) done[i] = 0;
    for (uint32_t k = 0; k < w->n; k++) {                          /* selection by time: render starts are recorded late */
        uint32_t best = COOP_MK_N, bt = 0;
        for (uint32_t i = 0; i < w->n; i++)
            if (!done[i] && (best == COOP_MK_N || w->mk[i][1] - t0 < bt)) { best = i; bt = w->mk[i][1] - t0; }
        done[best] = 1;
        put(" "); puthex2(w->mk[best][0]); put("@"); putn(bt);
        if (bt - last > 200000u) put("!");                         /* a gap of more than 200 k cycles before this mark */
        last = bt;
    }
    put("\n");
}

/* p5e: the cache set-up, to judge the memory-bound effects. BF52x core MMRs (Blackfin HRM, memory chapter):
 * DMEM_CONTROL 0xFFE00004, DCPLB_ADDR0..15 0xFFE00100, DCPLB_DATA0..15 0xFFE00200; IMEM_CONTROL 0xFFE01004,
 * ICPLB_ADDR0..15 0xFFE01100, ICPLB_DATA0..15 0xFFE01200. A snapshot: a CPLB miss handler may swap entries. Also where
 * the big buffers sit (slot 1's delay / cwo line and spring tank: plugin_host.cc). */
extern "C" uint32_t coop_fx_buffers(uint32_t *out4);               /* plugin_host.cc */
static void puthex8(uint32_t v) { char b[9]; for (int d = 0; d < 8; d++) b[d] = "0123456789abcdef"[(v >> (28 - 4 * d)) & 15]; b[8] = 0; put(b); }
static void put_cplb(void)
{
    volatile uint32_t *const dmem = (volatile uint32_t *)0xFFE00004u, *const imem = (volatile uint32_t *)0xFFE01004u;
    put("cplb dmem "); puthex8(*dmem); put(" imem "); puthex8(*imem); put("\n");
    put("dcplb");
    for (int i = 0; i < 16; i++) { put(" "); puthex8(((volatile uint32_t *)0xFFE00100u)[i]); put("/"); puthex8(((volatile uint32_t *)0xFFE00200u)[i]); }
    put("\nicplb");
    for (int i = 0; i < 16; i++) { put(" "); puthex8(((volatile uint32_t *)0xFFE01100u)[i]); put("/"); puthex8(((volatile uint32_t *)0xFFE01200u)[i]); }
    uint32_t b[4] = { 0, 0, 0, 0 };
    coop_fx_buffers(b);
    put("\nbuffers line1 "); puthex8(b[0]); put(" tank_spring1 "); puthex8(b[1]); put(" st_obj1 "); puthex8(b[2]); put(" text "); puthex8(b[3]); put("\n");
}

static void put_switches(void)
{
    put("switches"); kv("dsynth", g_coop_fast_dsynth); kv("stage", g_coop_fast_stage); kv("cwo", g_coop_fast_cwo);   /* the image's g_coop_fast_* words */
    kv("punch", g_coop_fast_punch); kv("phone", g_coop_fast_phone); kv("nitro", g_coop_fast_nitro); kv("grid", g_coop_fast_grid);
    kv("spring", g_coop_fast_spring); kv("delay", g_coop_fast_delay); kv("voltage", g_coop_fast_voltage); kv("phase", g_coop_fast_phase); put("\n");
}

/* The run file (coop_runner.cc, main loop): its header, then one block of lines per section. */
extern "C" int coop_stats_run_header(char *out, int cap, const char *script, uint32_t events, uint32_t blocks, uint32_t state, uint32_t lost)
{
    g_out = out; g_cap = cap; g_len = 0;
    put("coop_run 1 build " STATS_BUILD " script "); put(script); put("\n");
    put("run"); kv("events", events); kv("blocks", blocks); kv("state", state); kv("lost_sections", lost); put("\n");
    put_switches();
    int n = g_len;
    g_out = g_text; g_cap = (int)sizeof g_text; g_len = 0;
    return n;
}

/* "gaps" line: the pairs by their share of the block, largest first: prev>id=per-block/avg/max/n (cycles). */
static void put_gaps(const struct coop_gap *g, uint32_t blocks)
{
    if (!blocks || !g[0].n) return;
    static uint8_t done[COOP_GAP_N];
    for (int i = 0; i < COOP_GAP_N; i++) done[i] = 0;
    put("gaps per-block/avg/max/n"); kv("blocks", blocks);
    for (int k = 0; k < 24; k++) {
        int best = -1;
        for (int i = 0; i < COOP_GAP_N; i++) if (g[i].n && !done[i] && (best < 0 || g[i].sum > g[best].sum)) best = i;
        if (best < 0) break;
        done[best] = 1;
        put(" "); puthex2(g[best].key >> 8); put(">"); puthex2(g[best].key); put("=");
        putn((uint32_t)(g[best].sum / blocks)); put("/"); putn((uint32_t)(g[best].sum / g[best].n)); put("/"); putn(g[best].max); put("/"); putn(g[best].n);
    }
    put("\n");
}

extern "C" int coop_stats_run_section(char *out, int cap, const char *name, const struct coop_cost *cost, uint32_t blocks, uint64_t cb_sum, uint32_t cb_max, const struct coop_gap *gaps, uint32_t gap_blocks)
{
    g_out = out; g_cap = cap; g_len = 0; g_cost_src = cost;
    put("section "); put(name); kv("blocks", blocks); kv("cb_avg", blocks ? (uint32_t)(cb_sum / blocks) : 0); kv("cb_max", cb_max); put("\n");
    put_cost_lines();
    put_gaps(gaps, gap_blocks);
    int n = g_len;
    g_out = g_text; g_cap = (int)sizeof g_text; g_len = 0; g_cost_src = g_coop_cost;
    return n;
}

static void write_file(void)
{
    g_out = g_text; g_cap = (int)sizeof g_text; g_len = 0; g_cost_src = g_coop_cost;
    put("coop_stats 3 build " STATS_BUILD "\n");
    put("cb"); kv("blocks", g_blocks); kv("max", g_max); kv("last", g_last); kv("cycles", now_cycles()); put("\n");
    put("stages"); kv("rec_max", g_tape_rec_cycles_max); kv("mix_max", g_stereo_mix_cycles_max);
    kv("hook_max", g_stereo_hook_cycles_max); kv("stage_max", g_stereo_stage_cycles_max); kv("stage_calls", g_stereo_stage_calls); kv("lfo2_max", g_lfo2_cycles_max); kv("master_max", g_master_cycles);
    kv("hooks", g_stereo_hooks); kv("side_blocks", g_stereo_side_blocks); put("\n");
    put_cost_lines();
    for (int l = 0; l < 2; l++) {                                /* p6f / p6g: the 4 KB probe under each PFCTL layout, three passes (cycles, p0) */
        put(l ? "icache_l1" : "icache_l0"); kv("c1", g_icp[l][0][0]); kv("p1", g_icp[l][0][1]); kv("c2", g_icp[l][1][0]); kv("p2", g_icp[l][1][1]); kv("c3", g_icp[l][2][0]); kv("p3", g_icp[l][2][1]); put("\n");
    }
    for (int side = 0; side < 2; side++) {                       /* p6h: the event sweep, "ev s cold/warm" (cycles) and "ev o cold/warm" (occurrences) */
        put(side ? "pfsweep_data" : "pfsweep_code");
        for (int ev = 0; ev < 16; ev++)
            for (int cnt = 0; cnt < 2; cnt++) {
                char k[8]; k[0] = ' '; k[1] = side ? '9' : '8'; k[2] = "0123456789abcdef"[ev]; k[3] = cnt ? 'o' : 's'; k[4] = 0;
                put(k); putn(g_sweep[side][ev][cnt][0]); put("/"); putn(g_sweep[side][ev][cnt][1]);
            }
        put("\n");
    }
    put("l1"); kv("state", g_l1_state); kv("used", g_l1_used); kv("refused", g_l1_refused); kv("stereo", g_stereo_l1); kv("tape", g_tape_l1); kv("sine", g_l1_sine_copied); kv("lander_swallowed", g_lander_swallowed); kv("leased", g_l1_leased); kv("leases", g_l1_leases); kv("lease_refused", g_l1_lease_refused); kv("free", coop_l1_free()); put("\n");
    put_switches();
    put_cplb();
    put("slow"); kv("count", g_slow_count); kv("threshold", g_coop_slow_th); kv("gap_prof", g_coop_gap_prof); kv("gap_full", g_gap_full); put("\n");
    put_gaps(g_gap, g_gap_blocks);
    put_slow("slow_worst", &g_slow[SLOW_KEEP]);
    for (uint32_t i = 0; i < SLOW_KEEP; i++) put_slow("slow_recent", &g_slow[(g_slow_next + i) % SLOW_KEEP]);
#ifdef COOP_PROBE_ITER
    put("iter_ranges");                                                   /* probe: ENG_RANGE(14, k) {min, max, scale, mode} */
    for (int k = 0; k < 8; k++) { const volatile int16_t *rg = (const volatile int16_t *)(0x01DE5890u + 14u * 64u + (uint32_t)k * 8u);
        put(" ["); for (int j = 0; j < 4; j++) { if (j) put(","); int v = rg[j]; if (v < 0) { put("-"); v = -v; } putn((uint32_t)v); } put("]"); }
    put("\n");
#endif
    put("plugins"); kv("seen", g_plugins_seen); kv("loaded", g_plugins_loaded); kv("failed", g_plugins_failed); kv("disabled", g_plugins_disabled);
    kv("mono_calls", g_fx_dispatch_mono); kv("st_calls", g_fx_dispatch_st); kv("plugin_blocks", g_fx_plugin_blocks); kv("stock_st_blocks", g_fx_stock_stereo_blocks); kv("stock_st_made", g_fx_stock_ctor); kv("s2_instr", g_s2_blocks[0]); kv("s2_master", g_s2_blocks[1]); kv("s2_changes", g_s2_changes); kv("tanks", g_tank_bound); kv("tank_short", g_tank_short); kv("ui_opens", g_ui_opens); kv("ui_applies", g_ui_applies); kv("ui_toggles", g_ui_toggles); kv("ui_knobs2", g_ui_knobs2); kv("ui_col2_sets", g_ui_col2_sets); kv("rnd", g_rnd_count); kv("rnd_back", g_rnd_backs); kv("rnd_refused", g_rnd_refused); kv("rnd_slot2", g_rnd_slot2); kv("solo", g_solo_sets); kv("markers", g_marker_sets); kv("jumps", g_marker_jumps); kv("help_taps", g_fn_taps); kv("mode_taps", g_mode_key_taps); kv("mode_holds", g_mode_key_holds); kv("keeps", g_sound_keeps); kv("kept", g_sound_kept); kv("sound_writes", g_sound_writes); kv("bend", g_bend_presses); kv("bend_knobs", g_bend_knobs); kv("bend_ramps", g_bend_ramps); kv("overwrites", g_over_saves); kv("over_armed", g_over_armed); kv("saves", g_over_stock); kv("renames", g_rename_saves); kv("rename_starts", g_rename_starts); kv("rename_refused", g_rename_refused); kv("drumpan", g_drumpan_knobs); kv("rename_pushes", g_rename_pushes); kv("fx2view", g_fx2view_params); kv("fx2view_disp", g_fx2view_disp); kv("fx2view_pops", g_fx2view_pops); kv("fx2view_empty", g_fx2view_empty); kv("fx2view_size", g_fx2view_size); kv("fx2view_ids", g_fx2view_ids); kv("lfo2view", g_lfo2view_params); kv("lfo2view_pops", g_lfo2view_pops); kv("fx2view_on", g_fx2view_on); kv("lfo2view_on", g_lfo2view_on); kv("fx2view_post", g_fx2view_last_post); kv("fx2view_cur", g_fx2view_cur); kv("set_opens", g_set_opens); kv("set_saves", g_set_saves); kv("st0_builds", g_st0_builds); kv("st0_dry", g_st0_dry_blocks);
    kv("engines", g_engines_loaded); kv("eng_renders", g_eng_renders); kv("eng_noteons", g_eng_noteons); kv("eng_iter", g_eng_iter_renders);
    kv("heap_largest", g_heap_largest); kv("ctl_turns", g_ctl_turns); kv("ctl_clicks", g_ctl_clicks); kv("ctl_ignored", g_ctl_ignored); kv("ctl_cancelled", g_ctl_cancelled); kv("ctl_engines", g_ctl_engines); kv("ctl_popup_frames", g_ctl_popup_frames);
    kv("eng_switches", g_eng_switches); kv("eng_foreign", g_eng_foreign); kv("eng_max", g_eng_cycles_max);
    kv("scr_built", g_escr_built); kv("scr_failed", g_escr_failed); kv("scr_swaps", g_escr_swaps); kv("scr_resets", g_escr_resets); kv("scr_updates", g_escr_updates);
    if (g_plugins_last_error) { put(" last "); put(g_plugins_last_name); put(": "); put(g_plugins_last_error); }
    put("\n");
    put("tape"); kv("rec_calls", g_tape_rec_calls); kv("rec_side_blocks", g_tape_rec_side_blocks);
    kv("mismatch", g_tape_state_mismatch); kv("mismatch2", g_tape_state_mismatch2); kv("mismatch_word", g_tape_mismatch_word);
    kv("seam_fills", g_tape_seam_fills); put("\n");
    put("io"); kv("loads", g_tape_io_loads); kv("flushes", g_tape_io_flushes); kv("held", g_tape_io_held); kv("guarded", g_tape_io_guarded); kv("errors", g_tape_io_errors);
    kv("step_max", g_tape_io_step_cycles_max); kv("gap_max", g_tape_io_gap_cycles_max);
    kv("load_max", g_tape_io_load_cycles_max); kv("flush_max", g_tape_io_flush_cycles_max); put("\n");
    put("edits"); kv("lifts", g_edit_lifts); kv("drops", g_edit_drops); kv("erases", g_edit_erases); kv("errors", g_edit_errors);
    kv("mono_fallbacks", g_edit_mono_fallbacks); put("\n");
    put("stats"); kv("writes", g_stats_writes); kv("errors", g_stats_errors); kv("tail_writes", g_stats_tail_writes);
    kv("tail_errors", g_stats_tail_errors); kv("windows", g_stats_windows); kv("ticks", g_stats_ticks); put("\n");
    for (uint32_t i = 0; i < TOP; i++) {                               /* the heaviest windows, any order */
        if (!g_top[i].blocks) continue;
        put_window("top", &g_top[i]);
    }
    for (uint32_t i = 0; i < WINDOWS; i++) {                           /* oldest first */
        const struct window *w = &g_ring[(g_ring_next + i) % WINDOWS];
        if (!w->blocks && !w->io_steps) continue;
        put_window("win", w);
    }
    g_text[g_len] = 0;
    if (coop_tape_write_tail(g_text, g_len + 1) == 0) g_stats_tail_writes++; else g_stats_tail_errors++;   /* the channel disk mode copies */
    int fd = Y_OPEN(STATS_PATH, 0x242, 0x180);                        /* O_RDWR | O_CREAT | O_TRUNC, as the tape marker (not visible in disk mode) */
    if (fd < 0) { g_stats_errors++; return; }
    int ok = Y_WRITE(fd, g_text, g_len) == g_len;
    if (Y_CLOSE(fd) != 0) ok = 0;
    if (ok) g_stats_writes++; else g_stats_errors++;
}

extern "C" void coop_stats_write_now(void) { write_file(); }    /* coop_runner.cc: after a run (main loop) */

static void keep_if_heavy(const struct window *w)
{
    uint32_t imin = 0;
    for (uint32_t i = 1; i < TOP; i++) if (g_top[i].cb_max < g_top[imin].cb_max) imin = i;
    if (w->cb_max > g_top[imin].cb_max || !g_top[imin].blocks) g_top[imin] = *w;
}

/* From coop_tape_io_step (main loop, mode 0) on every pass. */
extern "C" void coop_stats_tick(int recording)
{
    uint32_t now = now_cycles();
    if (!g_started) { g_started = 1; g_window_start = now; }
    g_stats_ticks++;
    if (recording) g_win_recording = 1;
    if (g_stats_ticks == FIRST_WRITE_PASSES && !recording) write_file();
    if (now - g_window_start < WINDOW_CYCLES) return;
    g_window_start = now;
    struct window *w = &g_ring[g_ring_next];
    g_ring_next = (g_ring_next + 1) % WINDOWS;
    w->seq = g_stats_windows;
    w->blocks = g_blocks_win;            g_blocks_win = 0;
    w->cb_max = g_max_win;               g_max_win = 0;
    w->cb_sum = g_cb_sum_win;            g_cb_sum_win = 0;
    w->cb_over = g_cb_over_win;          g_cb_over_win = 0;
    w->rec_max = g_tape_rec_cycles_win;  g_tape_rec_cycles_win = 0;
    w->rec_sum = g_tape_rec_cycles_sum_win; g_tape_rec_cycles_sum_win = 0;
    w->mix_max = g_stereo_mix_cycles_win; g_stereo_mix_cycles_win = 0;
    w->mix_sum = g_stereo_mix_cycles_sum_win; g_stereo_mix_cycles_sum_win = 0;
    w->hook_max = g_stereo_hook_cycles_win; g_stereo_hook_cycles_win = 0;
    w->hook_sum = g_stereo_hook_cycles_sum_win; g_stereo_hook_cycles_sum_win = 0;
    w->hooks = g_stereo_hooks - g_prev_hooks;              g_prev_hooks = g_stereo_hooks;
    w->side = g_tape_rec_side_blocks - g_prev_side;        g_prev_side = g_tape_rec_side_blocks;
    w->recs = g_tape_rec_calls - g_prev_recs;              g_prev_recs = g_tape_rec_calls;
    w->io_steps = g_tape_io_steps_win;   g_tape_io_steps_win = 0;
    w->io_step_max = g_tape_io_step_cycles_win;   g_tape_io_step_cycles_win = 0;
    w->io_gap_max = g_tape_io_gap_cycles_win;     g_tape_io_gap_cycles_win = 0;
    w->io_load_max = g_tape_io_load_cycles_win;   g_tape_io_load_cycles_win = 0;
    w->io_flush_max = g_tape_io_flush_cycles_win; g_tape_io_flush_cycles_win = 0;
    w->loads = g_tape_io_loads - g_prev_loads;             g_prev_loads = g_tape_io_loads;
    w->flushes = g_tape_io_flushes - g_prev_flushes;       g_prev_flushes = g_tape_io_flushes;
    w->recording = g_win_recording;      g_win_recording = 0;
    g_stats_windows++;
    keep_if_heavy(w);
    if (!recording) write_file();
}
