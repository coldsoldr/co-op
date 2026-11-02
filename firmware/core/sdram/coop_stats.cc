/*
 * coop-fw device statistics file: the counters the coop stages keep (per-block cycle peaks, tape I/O work, edits,
 * state-mismatch detectors) written as text to /yaffs2/user/coop_stats.txt by the main loop, so the owner can copy
 * it in disk mode. Nothing else on the device can read our words (docs/performance.md: "a reader for the
 * measurement words").
 *
 * Every WINDOW_CYCLES (5 s) coop_stats_tick snapshots the window peaks (the *_win words the producers keep: whole
 * audio callback, record tap, mix replacement, I/O step / load / flush) into a ring of the last WINDOWS windows and
 * zeroes them; the file is rewritten after a window closes while nothing is recording (the write itself is main-loop
 * work we keep out of a take). Cumulative peaks stay as they are. Format: one "name value" pair list per line.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>

#define Y_OPEN        ((int (*)(const char *, int, int))0x01A997F4u)
#define Y_CLOSE       ((int (*)(int))0x01A99908u)
#define Y_WRITE       ((int (*)(int, const void *, int))0x01A99F84u)
#define STATS_PATH    "/yaffs2/user/coop_stats.txt"
#define WINDOW_CYCLES 2000000000u                                     /* 5 s at 400 MHz */
#define WINDOWS       12

extern volatile uint32_t g_last, g_max, g_blocks, g_max_win, g_blocks_win;                 /* coop_cb.cc */
extern volatile uint32_t g_stereo_hook_cycles_max, g_stereo_mix_cycles_max, g_stereo_mix_cycles_win, g_stereo_side_blocks;
extern volatile uint32_t g_lfo2_cycles_max;
extern volatile uint32_t g_tape_rec_calls, g_tape_rec_cycles_max, g_tape_rec_cycles_win, g_tape_rec_side_blocks;
extern volatile uint32_t g_tape_state_mismatch, g_tape_state_mismatch2, g_tape_mismatch_word, g_tape_seam_fills;
extern volatile uint32_t g_tape_io_loads, g_tape_io_flushes, g_tape_io_held, g_tape_io_errors;
extern volatile uint32_t g_tape_io_step_cycles_max, g_tape_io_load_cycles_max, g_tape_io_flush_cycles_max;
extern volatile uint32_t g_tape_io_step_cycles_win, g_tape_io_load_cycles_win, g_tape_io_flush_cycles_win, g_tape_io_steps_win;
extern volatile uint32_t g_edit_lifts, g_edit_drops, g_edit_erases, g_edit_errors, g_edit_mono_fallbacks;
extern volatile uint32_t g_master_cycles, g_fx_cycles;

volatile uint32_t g_stats_writes, g_stats_errors, g_stats_windows;

struct window {
    uint32_t blocks, cb_max, rec_max, mix_max, io_steps, io_step_max, io_load_max, io_flush_max;
    uint8_t recording;
};
static struct window g_ring[WINDOWS];
static uint32_t g_ring_next;
static uint32_t g_window_start;
static int g_started;
static uint8_t g_win_recording;
static char g_text[2048];
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

static void write_file(void)
{
    g_len = 0;
    put("coop_stats 1\n");
    put("cb"); kv("blocks", g_blocks); kv("max", g_max); kv("last", g_last); put("\n");
    put("stages"); kv("rec_max", g_tape_rec_cycles_max); kv("mix_max", g_stereo_mix_cycles_max);
    kv("hook_max", g_stereo_hook_cycles_max); kv("lfo2_max", g_lfo2_cycles_max); kv("master_max", g_master_cycles);
    kv("fx_max", g_fx_cycles); put("\n");
    put("tape"); kv("rec_calls", g_tape_rec_calls); kv("rec_side_blocks", g_tape_rec_side_blocks); kv("side_blocks", g_stereo_side_blocks);
    kv("mismatch", g_tape_state_mismatch); kv("mismatch2", g_tape_state_mismatch2); kv("mismatch_word", g_tape_mismatch_word);
    kv("seam_fills", g_tape_seam_fills); put("\n");
    put("io"); kv("loads", g_tape_io_loads); kv("flushes", g_tape_io_flushes); kv("held", g_tape_io_held); kv("errors", g_tape_io_errors);
    kv("step_max", g_tape_io_step_cycles_max); kv("load_max", g_tape_io_load_cycles_max); kv("flush_max", g_tape_io_flush_cycles_max); put("\n");
    put("edits"); kv("lifts", g_edit_lifts); kv("drops", g_edit_drops); kv("erases", g_edit_erases); kv("errors", g_edit_errors);
    kv("mono_fallbacks", g_edit_mono_fallbacks); put("\n");
    put("stats"); kv("writes", g_stats_writes); kv("errors", g_stats_errors); kv("windows", g_stats_windows); put("\n");
    for (uint32_t i = 0; i < WINDOWS; i++) {                           /* oldest first */
        const struct window *w = &g_ring[(g_ring_next + i) % WINDOWS];
        if (!w->blocks && !w->io_steps) continue;
        put("win"); kv("blocks", w->blocks); kv("cb_max", w->cb_max); kv("rec_max", w->rec_max); kv("mix_max", w->mix_max);
        kv("io_steps", w->io_steps); kv("io_step_max", w->io_step_max); kv("io_load_max", w->io_load_max);
        kv("io_flush_max", w->io_flush_max); kv("recording", w->recording); put("\n");
    }
    int fd = Y_OPEN(STATS_PATH, 0x242, 0x180);                        /* O_RDWR | O_CREAT | O_TRUNC, as the tape marker */
    if (fd < 0) { g_stats_errors++; return; }
    int ok = Y_WRITE(fd, g_text, g_len) == g_len;
    if (Y_CLOSE(fd) != 0) ok = 0;
    if (ok) g_stats_writes++; else g_stats_errors++;
}

/* From coop_tape_io_step (main loop, mode 0) on every pass. */
extern "C" void coop_stats_tick(int recording)
{
    uint32_t now = now_cycles();
    if (!g_started) { g_started = 1; g_window_start = now; }
    if (recording) g_win_recording = 1;
    if (now - g_window_start < WINDOW_CYCLES) return;
    g_window_start = now;
    struct window *w = &g_ring[g_ring_next];
    g_ring_next = (g_ring_next + 1) % WINDOWS;
    w->blocks = g_blocks_win;            g_blocks_win = 0;
    w->cb_max = g_max_win;               g_max_win = 0;
    w->rec_max = g_tape_rec_cycles_win;  g_tape_rec_cycles_win = 0;
    w->mix_max = g_stereo_mix_cycles_win; g_stereo_mix_cycles_win = 0;
    w->io_steps = g_tape_io_steps_win;   g_tape_io_steps_win = 0;
    w->io_step_max = g_tape_io_step_cycles_win;   g_tape_io_step_cycles_win = 0;
    w->io_load_max = g_tape_io_load_cycles_win;   g_tape_io_load_cycles_win = 0;
    w->io_flush_max = g_tape_io_flush_cycles_win; g_tape_io_flush_cycles_win = 0;
    w->recording = g_win_recording;      g_win_recording = 0;
    g_stats_windows++;
    if (!recording) write_file();
}
