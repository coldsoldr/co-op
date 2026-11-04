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
#define STATS_BUILD   "p4a"                                           /* the package tag: proves which image wrote the file */
#define WINDOW_CYCLES 2000000000u                                     /* 5 s at 400 MHz */
#define WINDOWS       12
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
extern volatile uint32_t g_master_cycles, g_fx_cycles;
extern volatile uint32_t g_l1_state, g_l1_used, g_l1_refused, g_l1_sine_copied;
extern volatile uint32_t g_lander_swallowed;   /* lander_off.cc */
extern volatile uint32_t g_plugins_seen, g_plugins_loaded, g_plugins_failed, g_fx_dispatch_mono, g_fx_dispatch_st, g_fx_plugin_blocks, g_fx_stock_stereo_blocks, g_fx_stock_ctor, g_s2_blocks[2], g_s2_changes, g_tank_bound, g_tank_short, g_ui_opens, g_ui_applies;   /* plugin_host.cc */
extern const char *volatile g_plugins_last_error;
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
static char g_text[8192];
static int g_len;

static inline uint32_t now_cycles(void) { uint32_t v; __asm__ __volatile__("%0 = CYCLES;" : "=d"(v)); return v; }

static void put(const char *s) { while (*s && g_len < (int)sizeof g_text - 1) g_text[g_len++] = *s++; }
static void putn(uint32_t v)
{
    char b[12]; int i = 0;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i && g_len < (int)sizeof g_text - 1) g_text[g_len++] = b[--i];
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
static void put_cost(const char *name, int i)               /* name avg max n: cycles per call */
{
    const struct coop_cost *c = &g_coop_cost[i];
    if (!c->n) return;
    put(" "); put(name); putn((uint32_t)(c->sum / c->n)); put("/"); putn(c->max); put("/"); putn(c->n);
}

static void write_file(void)
{
    g_len = 0;
    put("coop_stats 3 build " STATS_BUILD "\n");
    put("cb"); kv("blocks", g_blocks); kv("max", g_max); kv("last", g_last); kv("cycles", now_cycles()); put("\n");
    put("stages"); kv("rec_max", g_tape_rec_cycles_max); kv("mix_max", g_stereo_mix_cycles_max);
    kv("hook_max", g_stereo_hook_cycles_max); kv("stage_max", g_stereo_stage_cycles_max); kv("stage_calls", g_stereo_stage_calls); kv("lfo2_max", g_lfo2_cycles_max); kv("master_max", g_master_cycles);
    kv("fx_max", g_fx_cycles); kv("hooks", g_stereo_hooks); kv("side_blocks", g_stereo_side_blocks); put("\n");
    put("cost avg/max/n");                                   /* per call; slot 1's stock effect by type (stereo class) */
    put_cost("stage=", COOP_COST_STAGE); put_cost("s2_instr=", COOP_COST_S2I); put_cost("s2_master=", COOP_COST_S2M);
    put_cost("master=", COOP_COST_MASTER); put_cost("fx1_plugin=", COOP_COST_PLUG);
    static const char *const st_names[8] = { "fx1_delay=", "fx1_grid=", "fx1_punch=", "fx1_phone=", "fx1_spring=", "fx1_filter=", "fx1_nitro=", "fx1_cwo=" };
    for (int t = 0; t < 8; t++) put_cost(st_names[t], COOP_COST_ST1 + t);
    put("\n");
    put("l1"); kv("state", g_l1_state); kv("used", g_l1_used); kv("refused", g_l1_refused); kv("stereo", g_stereo_l1); kv("tape", g_tape_l1); kv("sine", g_l1_sine_copied); kv("lander_swallowed", g_lander_swallowed); put("\n");
    put("plugins"); kv("seen", g_plugins_seen); kv("loaded", g_plugins_loaded); kv("failed", g_plugins_failed);
    kv("mono_calls", g_fx_dispatch_mono); kv("st_calls", g_fx_dispatch_st); kv("plugin_blocks", g_fx_plugin_blocks); kv("stock_st_blocks", g_fx_stock_stereo_blocks); kv("stock_st_made", g_fx_stock_ctor); kv("s2_instr", g_s2_blocks[0]); kv("s2_master", g_s2_blocks[1]); kv("s2_changes", g_s2_changes); kv("tanks", g_tank_bound); kv("tank_short", g_tank_short); kv("ui_opens", g_ui_opens); kv("ui_applies", g_ui_applies);
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
    for (uint32_t i = 0; i < WINDOWS; i++) {                           /* oldest first */
        const struct window *w = &g_ring[(g_ring_next + i) % WINDOWS];
        if (!w->blocks && !w->io_steps) continue;
        put_window("win", w);
    }
    for (uint32_t i = 0; i < TOP; i++) {                               /* the heaviest windows, any order */
        if (!g_top[i].blocks) continue;
        put_window("top", &g_top[i]);
    }
    g_text[g_len] = 0;
    if (coop_tape_write_tail(g_text, g_len + 1) == 0) g_stats_tail_writes++; else g_stats_tail_errors++;   /* the channel disk mode copies */
    int fd = Y_OPEN(STATS_PATH, 0x242, 0x180);                        /* O_RDWR | O_CREAT | O_TRUNC, as the tape marker (not visible in disk mode) */
    if (fd < 0) { g_stats_errors++; return; }
    int ok = Y_WRITE(fd, g_text, g_len) == g_len;
    if (Y_CLOSE(fd) != 0) ok = 0;
    if (ok) g_stats_writes++; else g_stats_errors++;
}

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
