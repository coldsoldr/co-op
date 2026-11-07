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
#define STATS_BUILD   "t1"                                           /* the package tag: proves which image wrote the file */
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
extern volatile uint32_t g_master_cycles, g_fx_cycles;
extern volatile uint32_t g_l1_state, g_l1_used, g_l1_refused, g_l1_sine_copied;
extern volatile uint32_t g_lander_swallowed;   /* lander_off.cc */
extern "C" volatile uint32_t g_coop_fast_dsynth, g_coop_fast_stage, g_coop_fast_cwo, g_coop_fast_punch, g_coop_fast_phone,
    g_coop_fast_nitro, g_coop_fast_grid, g_coop_fast_spring, g_coop_fast_delay, g_coop_fast_voltage, g_coop_fast_phase;   /* stereo_bus.cc */
extern volatile uint32_t g_plugins_seen, g_plugins_loaded, g_plugins_failed, g_fx_dispatch_mono, g_fx_dispatch_st, g_fx_plugin_blocks, g_fx_stock_stereo_blocks, g_fx_stock_ctor, g_s2_blocks[2], g_s2_changes, g_tank_bound, g_tank_short, g_ui_opens, g_ui_applies, g_plugins_disabled, g_ui_toggles, g_set_opens, g_set_saves, g_ui_col2_sets, g_rnd_count, g_rnd_backs, g_rnd_refused, g_solo_sets, g_marker_sets, g_marker_jumps, g_fn_taps, g_mode_key_taps, g_mode_key_holds, g_sound_keeps, g_sound_kept, g_sound_writes, g_bend_presses, g_bend_knobs, g_bend_ramps, g_over_saves, g_over_armed, g_over_stock, g_rename_starts, g_rename_saves, g_rename_refused, g_drumpan_knobs, g_rename_pushes, g_fx2view_params, g_fx2view_disp, g_fx2view_pops, g_trk_toggles, g_trk_written, g_trk_plays, g_trk_frames, g_trk_knobs, g_trk_rows, g_trk_notes, g_trk_renders;   /* plugin_host.cc, coop_ui.cc, coop_settings.cc, coop_tracker*.cc */
extern const char *volatile g_plugins_last_error;
extern volatile uint32_t g_xvoice_cycles_max, g_xvoice_cycles_sum, g_xvoice_blocks, g_xvoice_notes, g_xvoice_max_sounding, g_xvoice_l1, g_xvoice_peak[5];   /* coop_xvoice.cc (F1) */
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
static uint32_t g_eng_last;                                       /* the entry the last render used */

/* From the render thunks (stereo_thunks.S) after every voice render: the cycles of one render of the engine whose
 * render function is fn. The table fills in the order the engines are first heard; COOP_ENG_N covers every stock
 * synth and drum engine of a session. */
extern "C" void coop_eng_cost(uint32_t fn, uint32_t dt, int drum)
{
    uint32_t now = coop_cost_now();
    coop_mark_at(0x10 + (drum & 1), now - dt);                     /* the render's start (the thunk read CYCLES there) */
    coop_mark_at(0x18 + (drum & 1), now);
    g_eng_blk[drum & 1] += dt;
    uint32_t i = g_eng_last;
    if (g_coop_eng_fn[i] != fn) {
        for (i = 0; i < COOP_ENG_N; i++) {
            if (g_coop_eng_fn[i] == fn) break;
            if (!g_coop_eng_fn[i]) { g_coop_eng_fn[i] = fn; break; }
        }
        if (i == COOP_ENG_N) return;
        g_eng_last = i;
    }
    coop_cost_add(COOP_COST_ENG + (int)i, dt);
}

/* From coop_stereo_block_start: the previous block's render sums. */
extern "C" void coop_eng_block_end(void)
{
    if (g_eng_blk[0]) { coop_cost_add(COOP_COST_SYNTH, g_eng_blk[0]); g_eng_blk[0] = 0; }
    if (g_eng_blk[1]) { coop_cost_add(COOP_COST_DRUM, g_eng_blk[1]); g_eng_blk[1] = 0; }
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

static const struct coop_cost *g_cost_src = g_coop_cost;          /* the live table, or a run section's copy */
static void put_cost(const char *name, int i)               /* name avg max n: cycles per call */
{
    const struct coop_cost *c = &g_cost_src[i];
    if (!c->n) return;
    put(" "); put(name); putn((uint32_t)(c->sum / c->n)); put("/"); putn(c->max); put("/"); putn(c->n);
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
    put("cost avg/max/n");                                   /* per call; slot 1's stock effect by type (stereo class) */
    put_cost("stage=", COOP_COST_STAGE); put_cost("s2_instr=", COOP_COST_S2I); put_cost("s2_master=", COOP_COST_S2M);
    put_cost("master=", COOP_COST_MASTER); put_cost("fx1_plugin=", COOP_COST_PLUG);
    static const char *const st_names[8] = { "fx1_delay=", "fx1_grid=", "fx1_punch=", "fx1_phone=", "fx1_spring=", "fx1_filter=", "fx1_nitro=", "fx1_cwo=" };
    for (int t = 0; t < 8; t++) put_cost(st_names[t], COOP_COST_ST1 + t);
    put("\n");
    put("engines avg/max/n");                                /* per voice render, by engine; synth= / drum= per block */
    put_cost("synth=", COOP_COST_SYNTH); put_cost("drum=", COOP_COST_DRUM);
    for (int i = 0; i < COOP_ENG_N; i++) {
        if (!g_coop_eng_fn[i]) break;
        put_cost(eng_name(g_coop_eng_fn[i]), COOP_COST_ENG + i);
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
    kv("hook_max", g_stereo_hook_cycles_max); kv("stage_max", g_stereo_stage_cycles_max); kv("stage_calls", g_stereo_stage_calls); kv("lfo2_max", g_lfo2_cycles_max); kv("master_max", g_master_cycles); kv("xv_max", g_xvoice_cycles_max); kv("xv_sum", g_xvoice_cycles_sum); kv("xv_blocks", g_xvoice_blocks); kv("xv_notes", g_xvoice_notes); kv("xv_sounding", g_xvoice_max_sounding); kv("xv_l1", g_xvoice_l1); kv("xv_peak_fm", g_xvoice_peak[0]); kv("xv_peak_str", g_xvoice_peak[1]); kv("xv_peak_dig", g_xvoice_peak[2]); kv("xv_peak_dsy", g_xvoice_peak[3]); kv("xv_peak_pha", g_xvoice_peak[4]);
    kv("fx_max", g_fx_cycles); kv("hooks", g_stereo_hooks); kv("side_blocks", g_stereo_side_blocks); put("\n");
    put_cost_lines();
    put("l1"); kv("state", g_l1_state); kv("used", g_l1_used); kv("refused", g_l1_refused); kv("stereo", g_stereo_l1); kv("tape", g_tape_l1); kv("sine", g_l1_sine_copied); kv("lander_swallowed", g_lander_swallowed); put("\n");
    put_switches();
    put_cplb();
    put("slow"); kv("count", g_slow_count); kv("threshold", g_coop_slow_th); kv("gap_prof", g_coop_gap_prof); kv("gap_full", g_gap_full); put("\n");
    put_gaps(g_gap, g_gap_blocks);
    put_slow("slow_worst", &g_slow[SLOW_KEEP]);
    for (uint32_t i = 0; i < SLOW_KEEP; i++) put_slow("slow_recent", &g_slow[(g_slow_next + i) % SLOW_KEEP]);
    put("plugins"); kv("seen", g_plugins_seen); kv("loaded", g_plugins_loaded); kv("failed", g_plugins_failed); kv("disabled", g_plugins_disabled);
    kv("mono_calls", g_fx_dispatch_mono); kv("st_calls", g_fx_dispatch_st); kv("plugin_blocks", g_fx_plugin_blocks); kv("stock_st_blocks", g_fx_stock_stereo_blocks); kv("stock_st_made", g_fx_stock_ctor); kv("s2_instr", g_s2_blocks[0]); kv("s2_master", g_s2_blocks[1]); kv("s2_changes", g_s2_changes); kv("tanks", g_tank_bound); kv("tank_short", g_tank_short); kv("ui_opens", g_ui_opens); kv("ui_applies", g_ui_applies); kv("ui_toggles", g_ui_toggles); kv("ui_col2_sets", g_ui_col2_sets); kv("rnd", g_rnd_count); kv("rnd_back", g_rnd_backs); kv("rnd_refused", g_rnd_refused); kv("solo", g_solo_sets); kv("markers", g_marker_sets); kv("jumps", g_marker_jumps); kv("help_taps", g_fn_taps); kv("mode_taps", g_mode_key_taps); kv("mode_holds", g_mode_key_holds); kv("keeps", g_sound_keeps); kv("kept", g_sound_kept); kv("sound_writes", g_sound_writes); kv("bend", g_bend_presses); kv("bend_knobs", g_bend_knobs); kv("bend_ramps", g_bend_ramps); kv("overwrites", g_over_saves); kv("over_armed", g_over_armed); kv("saves", g_over_stock); kv("renames", g_rename_saves); kv("rename_starts", g_rename_starts); kv("rename_refused", g_rename_refused); kv("drumpan", g_drumpan_knobs); kv("rename_pushes", g_rename_pushes); kv("fx2view", g_fx2view_params); kv("fx2view_disp", g_fx2view_disp); kv("fx2view_pops", g_fx2view_pops); kv("trk_toggles", g_trk_toggles); kv("trk_written", g_trk_written); kv("trk_plays", g_trk_plays); kv("trk_frames", g_trk_frames); kv("trk_knobs", g_trk_knobs); kv("trk_rows", g_trk_rows); kv("trk_notes", g_trk_notes); kv("trk_renders", g_trk_renders); kv("set_opens", g_set_opens); kv("set_saves", g_set_saves); kv("st0_builds", g_st0_builds); kv("st0_dry", g_st0_dry_blocks);
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
