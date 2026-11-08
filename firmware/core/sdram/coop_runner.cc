/*
 * coop-fw test runner (p5b): plays a scripted test on the device, key presses and encoder turns at block-exact times,
 * so the device and op1emu run the same inputs (docs/test-runner.md).
 *
 * The host (tools/coop_sysex.html, or tools/run_script.py for op1emu) compiles an op1emu headless script
 * (the tests/sim scenarios: `at MS`, press / release / tap KEY, turn ENC cw|ccw N) into a CRUN file, puts it under
 * /yaffs2/user/ and sends RUN start (coop_sysex.cc, 0x27). The main loop loads and checks the file; the audio
 * callback (CoopAudioCb -> coop_stereo_block_start -> coop_runner_block) then plays it, one block at a time.
 *
 * Injection: the stock key and encoder producers run in the audio interrupt (the key scan after the master push,
 * docs/audio-graph.md, te-op1-cfw docs/screen-interface.md "Input"): a key is KeyHandler::PushKey(code | pressed << 8)
 * (0x019C2F24) then the wake 0x019C2F04; an encoder step is KeyHandler::AddKnob(k, +-1) (0x019C3044), one per
 * quadrature transition, two per detent. op1emu write traces of s_key_queue (0x01DD5044) and s_knob_acc (0x01DD5250)
 * for every key and encoder confirmed the values (tests/sim/probe_knobs.txt): enc1..enc4 are knobs 0..3, cw is +1,
 * the encoder pushes are keys 0x3B..0x3E. The runner calls the same functions from the same interrupt, before the
 * stock callback, so the KeyHandler thread sees nothing it could tell from a hand on the keys. [C op1emu, I device]
 *
 * Marks: a MARK event closes the section before it (the cost table, the callback cycles) and starts the next; the
 * table is copied and zeroed in the interrupt (where the costs are added, so nothing races) and the main loop formats
 * the copy into /yaffs2/user/coop_run.txt. A CLICK event writes one block of a +-12000 doublet onto the master bus
 * (coop_master_apply, before the level stage): the start and end clicks line a recording up with the script.
 *
 * Safety: only the codes of real keys are accepted (0x00..0x37 without 0x2C, the encoder pushes 0x3B..0x3E; not the
 * jack-sense codes 0x38/0x39 or the virtual 0x3F); every key the runner pressed is released at the end and on an
 * abort. Nothing here touches the audio path but the click block.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_cost.h"

#define PUSH_KEY  ((void (*)(int))0x019C2F24u)                    /* KeyHandler::PushKey(key | state << 8) */
#define WAKE_KEYS ((void (*)(void))0x019C2F04u)                   /* post the KeyHandler semaphore */
#define ADD_KNOB  ((void (*)(int, int))0x019C3044u)               /* KeyHandler::AddKnob(knob, delta) */
#define Y_OPEN    ((int (*)(const char *, int, int))0x01A997F4u)
#define Y_CLOSE   ((int (*)(int))0x01A99908u)
#define Y_READ    ((int (*)(int, void *, int))0x01A99C90u)
#define Y_WRITE   ((int (*)(int, const void *, int))0x01A99F84u)
#define RUN_PATH  "/yaffs2/user/coop_run.txt"

#define MAX_EV    2048
#define MAX_MARKS 32
#define NAME_LEN  32
#define MARK_LEN  16
#define HDR       (12 + NAME_LEN)
#define FILE_MAX  (HDR + MAX_MARKS * MARK_LEN + MAX_EV * 8)
#define SNAPS     4

enum { EV_KEY = 1, EV_KNOB = 2, EV_CLICK = 3, EV_MARK = 4, EV_END = 5 };
enum { RS_IDLE = 0, RS_ARMED = 1, RS_RUNNING = 2, RS_DONE = 3, RS_ABORTED = 4, RS_ERROR = 5 };

struct ev { uint32_t block; uint8_t type, a; int8_t b; uint8_t pad; };
struct snap { struct coop_cost cost[COOP_COST_N]; uint32_t blocks, cb_max; uint64_t cb_sum; int mark;
              struct coop_gap gaps[COOP_GAP_N]; uint32_t gap_blocks; };

static struct ev g_ev[MAX_EV];
static uint8_t g_file[FILE_MAX];
static char g_name[NAME_LEN + 1];
static char g_marks[MAX_MARKS][MARK_LEN + 1];
static uint16_t g_nev, g_nmarks;
static struct snap g_snap[SNAPS];
static volatile uint32_t g_snap_w, g_snap_r;
static char g_run_text[24576];                                   /* p5g: a gaps line per section */
static int g_run_len;

volatile uint32_t g_run_state, g_run_block, g_run_idx, g_run_sections, g_run_abort, g_run_click, g_run_error;
volatile uint32_t g_run_lost;                                     /* sections the main loop did not format in time */
static uint32_t g_held[2];                                        /* keys 0..63 the runner holds down */
static uint32_t g_sec_blocks, g_sec_cb_max;
static uint64_t g_sec_cb_sum;
static int g_sec_mark = -1;                                       /* -1: the lead-in before the first mark */
static int g_written;

extern volatile uint32_t g_last;                                  /* coop_cb.cc: the previous block's callback cycles */

static int key_ok(int c) { return (c >= 0 && c <= 0x37 && c != 0x2C) || (c >= 0x3B && c <= 0x3E); }

static void key(int code, int pressed)
{
    PUSH_KEY(code | (pressed ? 0x100 : 0));
    WAKE_KEYS();
    uint32_t bit = 1u << (code & 31);
    if (pressed) g_held[code >> 5] |= bit; else g_held[code >> 5] &= ~bit;
}

static void release_all(void)
{
    for (int c = 0; c < 64; c++) if (g_held[c >> 5] & (1u << (c & 31))) key(c, 0);
}

static void zero_costs(void)
{
    for (int i = 0; i < COOP_COST_N; i++) { g_coop_cost[i].sum = 0; g_coop_cost[i].n = 0; g_coop_cost[i].max = 0; g_coop_cost[i].p0 = 0; g_coop_cost[i].p1 = 0; }
    g_sec_blocks = 0; g_sec_cb_max = 0; g_sec_cb_sum = 0;
    for (int i = 0; i < COOP_GAP_N; i++) { g_gap[i].key = 0; g_gap[i].n = 0; g_gap[i].max = 0; g_gap[i].sum = 0; }
    g_gap_blocks = 0;
}

static void close_section(void)                                    /* interrupt: snapshot, zero, count */
{
    if (g_snap_w - g_snap_r < SNAPS) {
        struct snap *s = &g_snap[g_snap_w % SNAPS];
        for (int i = 0; i < COOP_COST_N; i++) s->cost[i] = g_coop_cost[i];
        s->blocks = g_sec_blocks; s->cb_max = g_sec_cb_max; s->cb_sum = g_sec_cb_sum; s->mark = g_sec_mark;
        for (int i = 0; i < COOP_GAP_N; i++) s->gaps[i] = g_gap[i];
        s->gap_blocks = g_gap_blocks;
        g_snap_w++;
    } else g_run_lost++;
    zero_costs();
    g_run_sections++;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

/* Check a CRUN image and arm. Layout (little-endian): "CRUN", u16 version 1, u16 events, u16 marks, u16 0,
 * char name[32]; marks x char[16]; events x { u32 block, u8 type, u8 a, i8 b, u8 0 }. Returns 0 or an error code
 * (2 format, 3 too big, 4 bad event). */
static int parse(const uint8_t *f, int n)
{
    if (n < HDR || f[0] != 'C' || f[1] != 'R' || f[2] != 'U' || f[3] != 'N' || rd16(f + 4) != 1) return 2;
    uint32_t nev = rd16(f + 6), nmarks = rd16(f + 8);
    if (nev == 0 || nev > MAX_EV || nmarks > MAX_MARKS) return 3;
    if (n != (int)(HDR + nmarks * MARK_LEN + nev * 8)) return 2;
    for (int i = 0; i < NAME_LEN; i++) g_name[i] = (char)f[12 + i];
    g_name[NAME_LEN] = 0;
    const uint8_t *p = f + HDR;
    for (uint32_t m = 0; m < nmarks; m++, p += MARK_LEN) {
        for (int i = 0; i < MARK_LEN; i++) g_marks[m][i] = (char)p[i];
        g_marks[m][MARK_LEN] = 0;
    }
    uint32_t prev = 0;
    for (uint32_t i = 0; i < nev; i++, p += 8) {
        struct ev e; e.block = rd32(p); e.type = p[4]; e.a = p[5]; e.b = (int8_t)p[6]; e.pad = 0;
        int ok = e.block >= prev;
        if (e.type == EV_KEY) ok = ok && key_ok(e.a) && (e.b == 0 || e.b == 1);
        else if (e.type == EV_KNOB) ok = ok && e.a < 4 && e.b != 0 && e.b >= -4 && e.b <= 4;
        else if (e.type == EV_MARK) ok = ok && e.a < nmarks;
        else ok = ok && (e.type == EV_CLICK || e.type == EV_END);
        if (!ok) return 4;
        g_ev[i] = e; prev = e.block;
    }
    if (g_ev[nev - 1].type != EV_END) return 4;
    g_nev = (uint16_t)nev; g_nmarks = (uint16_t)nmarks;
    g_snap_w = g_snap_r = 0; g_run_sections = 0; g_run_lost = 0; g_run_error = 0; g_run_abort = 0; g_written = 0; g_run_len = 0;
    g_run_state = RS_ARMED;                                       /* the interrupt starts it on the next block */
    return 0;
}

/* From coop_stereo_block_start (the audio interrupt, before the stock callback): this block's events. */
extern "C" void coop_runner_block(void)
{
    uint32_t st = g_run_state;
    if (st == RS_ARMED) {                                         /* the lead-in costs start clean */
        zero_costs();
        g_sec_mark = -1; g_run_block = 0; g_run_idx = 0; g_held[0] = g_held[1] = 0;
        g_run_state = st = RS_RUNNING;
    } else if (st == RS_RUNNING) {
        uint32_t c = g_last;                                      /* the previous block, in this section */
        g_sec_cb_sum += c; if (c > g_sec_cb_max) g_sec_cb_max = c; g_sec_blocks++;
        g_run_block++;
    }
    if (st != RS_RUNNING) return;
    if (g_run_abort) { release_all(); g_run_abort = 0; g_run_state = RS_ABORTED; return; }
    while (g_run_idx < g_nev && g_ev[g_run_idx].block <= g_run_block) {
        const struct ev *e = &g_ev[g_run_idx++];
        switch (e->type) {
        case EV_KEY:   key(e->a, e->b); break;
        case EV_KNOB:  ADD_KNOB(e->a, e->b); WAKE_KEYS(); break;
        case EV_CLICK: g_run_click = 1; break;
        case EV_MARK:  close_section(); g_sec_mark = e->a; break;
        case EV_END:   release_all(); close_section(); g_run_state = RS_DONE; return;
        }
    }
}

/* From coop_master_apply (the master bus d34/db4, int16, 64 samples): the alignment click. */
extern "C" void coop_runner_click(int16_t *l, int16_t *r)
{
    if (!g_run_click) return;
    g_run_click = 0;
    for (int i = 0; i < 64; i++) { int16_t v = i < 16 ? 12000 : i < 32 ? -12000 : 0; l[i] = v; r[i] = v; }
}

/* Main loop (RUN start): load a CRUN file and arm. Returns 0 or an error code (1 open, 2 format, 3 too big,
 * 4 bad event, 5 busy). */
extern "C" int coop_runner_start(const char *path)
{
    uint32_t st = g_run_state;
    if (st == RS_ARMED || st == RS_RUNNING) return 5;
    int fd = Y_OPEN(path, 0, 0);
    if (fd < 0) { g_run_state = RS_ERROR; g_run_error = 1; return 1; }
    int n = Y_READ(fd, g_file, FILE_MAX);
    Y_CLOSE(fd);
    int err = n < 0 ? 1 : parse(g_file, n);
    if (err) { g_run_state = RS_ERROR; g_run_error = (uint32_t)err; }
    return err;
}

extern "C" void coop_runner_abort(void)
{
    if (g_run_state == RS_ARMED) g_run_state = RS_ABORTED;        /* not started: nothing is held */
    else if (g_run_state == RS_RUNNING) g_run_abort = 1;          /* the interrupt releases the keys and stops */
}

/* The run file text, formatted by coop_stats.cc (one line per section). */
extern "C" int coop_stats_run_section(char *out, int cap, const char *name, const struct coop_cost *cost, uint32_t blocks, uint64_t cb_sum, uint32_t cb_max, const struct coop_gap *gaps, uint32_t gap_blocks);
extern "C" int coop_stats_run_header(char *out, int cap, const char *script, uint32_t events, uint32_t blocks, uint32_t state, uint32_t lost);
extern "C" void coop_stats_write_now(void);

/* op1emu (no USB-MIDI): with g_coop_run_autostart = N (build_image.sh --set-word; 0 = never, the packages leave it 0)
 * the main loop starts AUTORUN_PATH (a NAND overlay file, tools/make_nand.py) once the audio has run N blocks:
 * the device's loader and runner, only the SysEx start is missing. */
#define AUTORUN_PATH "/yaffs2/user/coop_autorun.crun"
extern "C" { __attribute__((section(".data"))) volatile uint32_t g_coop_run_autostart = 0; }
extern volatile uint32_t g_blocks;                                /* coop_cb.cc */

/* Main loop, every pass (coop_sysex_service): format closed sections; write the files once the run has ended. */
extern "C" void coop_runner_service(void)
{
    static int autostarted;
    if (g_coop_run_autostart && !autostarted && g_blocks >= g_coop_run_autostart) { autostarted = 1; coop_runner_start(AUTORUN_PATH); }
    while (g_snap_r != g_snap_w) {
        const struct snap *s = &g_snap[g_snap_r % SNAPS];
        const char *nm = s->mark < 0 ? "lead-in" : g_marks[s->mark];
        int room = (int)sizeof g_run_text - 1 - g_run_len;
        if (room > 0) g_run_len += coop_stats_run_section(g_run_text + g_run_len, room, nm, s->cost, s->blocks, s->cb_sum, s->cb_max, s->gaps, s->gap_blocks);
        g_snap_r++;
    }
    uint32_t st = g_run_state;
    if ((st == RS_DONE || st == RS_ABORTED) && !g_written && g_snap_r == g_snap_w) {
        g_written = 1;
        static char head[640];
        int hl = coop_stats_run_header(head, (int)sizeof head, g_name, g_nev, g_run_block, st, g_run_lost);
        int fd = Y_OPEN(RUN_PATH, 0x242, 0x180);                  /* O_RDWR | O_CREAT | O_TRUNC */
        if (fd >= 0) { Y_WRITE(fd, head, hl); Y_WRITE(fd, g_run_text, g_run_len); Y_CLOSE(fd); }
        coop_stats_write_now();
    }
}

/* RUN status for the host: state, u32 block, u16 event index, u16 events, sections, error, written. */
extern "C" void coop_runner_status(uint8_t *out)
{
    uint32_t b = g_run_block, i = g_run_idx;
    out[0] = (uint8_t)g_run_state;
    out[1] = (uint8_t)b; out[2] = (uint8_t)(b >> 8); out[3] = (uint8_t)(b >> 16); out[4] = (uint8_t)(b >> 24);
    out[5] = (uint8_t)i; out[6] = (uint8_t)(i >> 8);
    out[7] = (uint8_t)g_nev; out[8] = (uint8_t)(g_nev >> 8);
    out[9] = (uint8_t)g_run_sections; out[10] = (uint8_t)g_run_error; out[11] = (uint8_t)g_written;
}
