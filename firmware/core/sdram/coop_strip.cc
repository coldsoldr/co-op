/*
 * coop-fw per-tape channel strip, milestone CS3: the audio path (docs/channel-strip.md "CS3").
 *
 * Four strips (core/include/coop_strip_bf.h, the CS2 kernels), one per tape track, on the stereo tape's lanes:
 *  - split: coop_tape_gains (tape_stereo.cc, once per tick after the stock pan law) calls coop_strip_tape_gains with
 *    the stock (gL, gR) pairs. A track whose strip is not neutral while the strip mode is on is "split": its two
 *    lanes (t, 4 + t) get a destination in g_strip_lanes. The lane loops' meter MAX (main 0xffa0712c, seam
 *    0xffa0709c) call coop_strip_lane_main / _seam (strip_thunks.S): the MAX as stock, then, for a split lane, the
 *    lane's samples added at unity into the track's own tape-rate pair at the stock destination offset, and CC = 1
 *    so the stock MAC skips the lane. A neutral track stays in the stock sum, bit for bit.
 *  - resample: the tick's `CALL 0xffa06194` (0xffa07250) calls coop_strip_rs_thunk: the stock pass (the summed
 *    tracks into d34 / db4), then coop_strip_tape: per split track a copy of the stock resampler (coop_rs_pair,
 *    pointers passed in) with the same position, increment and gain ramp, into a 64-sample int16 track block; the
 *    strip on it with the track's fader / pan gains (Q15); dry added into d34 / db4, sends into the two send buses.
 *    The pair is zeroed after use over what the lane hooks wrote (g_strip_lanes.end).
 *  - master FX 1 (0xffa017b8 rewrite -> coop_master_fx1): strip off = the stock call; on = the effect renders send
 *    bus 1 and d34 / db4 get FX(send) - send (wet only). FX 2 (coop_strip_master, from coop_master_apply): the
 *    same with coop_slot2_render(1, ...) on send bus 2. The live instrument is in d34 / db4 and reaches neither.
 *  - master EQ (0xffa0110e rewrite -> coop_master_eq, strip_thunks.S): skipped while the strip mode is on.
 *
 * Parameter changes: coop_strip_post (any thread; a seqlock mailbox) -> the audio thread stages the new form and
 * computes ONE changed EQ band per block (the int64 divides, ~6 k cycles a band in the simulator), then commits
 * (coop_strip_bf_commit: the cheap rest) in a later block. The strip runs on its old form until the commit. Bands
 * whose fields did not change keep their coefficients and cost nothing; a change that touches no band (comp, sat,
 * levels) commits in the block it is taken. At most one band is computed per block over all four strips.
 *
 * Send buses: int16 L / R, 64 each, filled by the tick, read and zeroed by the master stage of the same block.
 * Tails: an effect renders while any split track sends to it with a nonzero gain this block (`live`); after that it
 * keeps rendering the (silent) bus until its return has stayed within +-2 LSB for g_coop_strip_tail_blocks blocks
 * in a row (default 1380, 2 s), so delay and reverb tails ring out; then it is skipped.
 *
 * Image words (inject_app --set-word; the UI of CS5 replaces the presets):
 *  g_coop_strip_on      bit 0: strip mode; bit 1 (test): keep the master EQ while the strip is on
 *  g_coop_strip_t1..t4  test preset per track: byte 0 dry %, byte 1 FX 1 %, byte 2 FX 2 %, byte 3 sections
 *                       (bit 0 EQ, 1 comp, 2 sat, 3 pre tap, 4 fold, 5 tube). 0x64 = neutral (dry 100 %).
 *  g_coop_strip_wobble  test: every N blocks track 1's band 2 gain flips (+-6 dB): exercises the spread update
 *  g_coop_strip_tail_blocks
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "coop_strip_bf.h"

#define D34 ((int16_t *)0xff800d34u)                  /* the master pair (docs/stereo-bus.md) */
#define DB4 ((int16_t *)0xff800db4u)
#define TRACKS 4
#define PAIR_N 600                                    /* the stock play pair: 600 samples a channel (hook map) */
#define PAIR_GUARD 8                                  /* the stock resampler's base is 8 samples below the pair */
#define BLK 64
#define FX1_INST (*(uint8_t *volatile *)0x01aaad68u)  /* master FX 1 instance (gate 0xffa01202) */
#define FX1_PARAMS ((const int16_t *)0xff8016c4u)     /* its params (0xffa017ce) */

extern "C" {
__attribute__((section(".data"))) volatile uint32_t g_coop_strip_on = 0;
__attribute__((section(".data"))) volatile uint32_t g_coop_strip_t1 = 0x64;
__attribute__((section(".data"))) volatile uint32_t g_coop_strip_t2 = 0x64;
__attribute__((section(".data"))) volatile uint32_t g_coop_strip_t3 = 0x64;
__attribute__((section(".data"))) volatile uint32_t g_coop_strip_t4 = 0x64;
__attribute__((section(".data"))) volatile uint32_t g_coop_strip_wobble = 0;
__attribute__((section(".data"))) volatile uint32_t g_coop_strip_tail_blocks = 1380;

/* read by strip_thunks.S: per lane the split destination (the pair's index 0, 0 = stock) and the end (samples) the
 * lane hooks wrote up to; the split mask */
struct strip_lanes { int16_t *dst[8]; int32_t end[8]; };
__attribute__((aligned(4))) struct strip_lanes g_strip_lanes;
volatile uint32_t g_strip_split;

/* counters (op1emu --dump-at-exit of g_strip_stat; docs/channel-strip.md CS3) */
struct strip_stat {
    uint32_t tape_calls, tape_max, tape_sum, tape_n;      /* coop_strip_tape: cycles a block */
    uint32_t stage_max, commit_max, bands, commits;       /* the spread update: worst band block, worst commit */
    uint32_t fx1_renders, fx2_renders, fx1_tail, fx2_tail;/* blocks rendered; of them in the tail */
    uint32_t fx_max, master_max, posts, zero_max;         /* coop_master_fx1 / coop_strip_master cycles; pair zeroing */
    uint32_t splits, gains_calls, split_samples, last_mask;
};
volatile struct strip_stat g_strip_stat;

int32_t coop_rs_pair(int32_t pos, int32_t inc, const void *io);   /* strip_thunks.S */
void coop_slot2_render(int pos, int16_t *l, int16_t *r);          /* plugin_host.cc */
}
extern volatile uint32_t g_coop_s2_live[2];                        /* plugin_host.cc */

static inline uint32_t cyc(void) { uint32_t v; __asm__ __volatile__("%0 = CYCLES;" : "=d"(v)); return v; }

/* ---- state (region A; ~1.6 KB a strip) ---- */
static coop_strip_bf g_strip[TRACKS];
static __attribute__((aligned(4))) int16_t g_pair[TRACKS][2][PAIR_GUARD + PAIR_N + PAIR_GUARD];
static __attribute__((aligned(4))) int16_t g_trk[2][BLK];        /* one track's resampled block, L / R */
static __attribute__((aligned(4))) int16_t g_send[2][2][BLK];    /* send bus 1, 2: L / R */
static __attribute__((aligned(4))) int16_t g_dry[2][BLK];        /* a send's copy before its effect renders it */
static int32_t g_gl[TRACKS], g_gr[TRACKS];
static uint32_t g_was_split;
static int g_inited;
static volatile uint32_t g_send_used[2];                          /* the tick wrote into the bus this block */
static uint32_t g_fx_run[2], g_fx_quiet[2];
static uint32_t g_wobble_n;

/* the staged update of one strip */
struct strip_pend {
    coop_strip_params p;
    uint32_t eqc[4][18];
    uint8_t on[4], fresh[4];
    int stage;                                         /* -1 idle, 0..3 the next band, 4 commit */
};
static struct strip_pend g_pend[TRACKS];
/* the mailbox: a form posted by any thread; seq odd while it is written */
struct strip_mail { volatile uint32_t seq, took; coop_strip_params p; };
static struct strip_mail g_mail[TRACKS];

struct rs_io { const int16_t *step, *inL, *inR; int16_t *outL, *outR; };

static void copy_params(coop_strip_params *d, const volatile coop_strip_params *s)
{
    const volatile uint8_t *a = (const volatile uint8_t *)s;
    uint8_t *b = (uint8_t *)d;
    for (unsigned i = 0; i < sizeof *d; i++) b[i] = a[i];
}

/* any thread (the UI of CS5; the test presets): the strip takes it within a few blocks */
extern "C" void coop_strip_post(int t, const coop_strip_params *p)
{
    if (t < 0 || t >= TRACKS) return;
    struct strip_mail *m = &g_mail[t];
    uint32_t s = m->seq;
    m->seq = s | 1u;
    __asm__ __volatile__("ssync;" ::: "memory");
    copy_params((coop_strip_params *)&m->p, p);
    __asm__ __volatile__("ssync;" ::: "memory");
    m->seq = (s | 1u) + 1u;
    g_strip_stat.posts++;
}

extern "C" const coop_strip_params *coop_strip_params_of(int t) { return &g_strip[t & 3].p; }
extern "C" int32_t coop_strip_meter(int t, int out) { return out ? g_strip[t & 3].peak_out : g_strip[t & 3].peak_in; }

static void preset_params(uint32_t w, coop_strip_params *p)
{
    coop_strip_params_default(p);
    p->dry = (uint8_t)((w & 0xff) > 100 ? 100 : (w & 0xff));
    p->fx1 = (uint8_t)(((w >> 8) & 0xff) > 100 ? 100 : ((w >> 8) & 0xff));
    p->fx2 = (uint8_t)(((w >> 16) & 0xff) > 100 ? 100 : ((w >> 16) & 0xff));
    uint32_t s = w >> 24;
    if (s & 1) {                                       /* EQ: +6 dB low shelf 80 Hz, -4 dB 640 Hz, +5 dB 2.5 kHz, +3 dB shelf 10 kHz */
        p->eq_on = 1;
        p->eq_gain[0] = STRIP_EQ_GAIN0 + 12; p->eq_gain[1] = STRIP_EQ_GAIN0 - 8;
        p->eq_gain[2] = STRIP_EQ_GAIN0 + 10; p->eq_gain[3] = STRIP_EQ_GAIN0 + 6;
    }
    if (s & 2) { p->comp_on = 1; p->c_thresh = 40; p->c_makeup = 8; }   /* -20 dB, the default ratio, +4 dB */
    if (s & 4) { p->sat_on = 1; p->s_drive = 50; p->s_tone = 10; }
    if (s & 8) p->tap_post = 0;
    if (s & 16) p->s_type = STRIP_SAT_FOLD;
    if (s & 32) p->s_type = STRIP_SAT_TUBE;
}

static void strip_init_once(void)
{
    if (g_inited) return;
    g_inited = 1;
    for (int t = 0; t < TRACKS; t++) {
        coop_strip_bf_init(&g_strip[t]);
        coop_strip_bf_update(&g_strip[t]);             /* the defaults: every band at 0 dB, no divide */
        g_pend[t].stage = -1;
    }
    const uint32_t w[TRACKS] = { g_coop_strip_t1, g_coop_strip_t2, g_coop_strip_t3, g_coop_strip_t4 };
    for (int t = 0; t < TRACKS; t++) {
        coop_strip_params p;
        preset_params(w[t], &p);
        coop_strip_post(t, &p);
    }
}

static void zero16(int16_t *p, int n)                 /* n even, p 4-byte aligned */
{
    uint32_t *w = (uint32_t *)p;
    for (int i = 0; i < n / 2; i++) w[i] = 0;
}

/* ---- the spread update (audio thread, once a block) ---- */
static void strip_stage_service(void)
{
    for (int t = 0; t < TRACKS; t++) {                 /* take posted forms */
        struct strip_mail *m = &g_mail[t];
        uint32_t s = m->seq;
        if (s == m->took || (s & 1u)) continue;
        struct strip_pend *q = &g_pend[t];
        copy_params(&q->p, &m->p);
        if (m->seq != s) continue;                     /* written meanwhile: next block */
        m->took = s;
        q->stage = 0;
    }
    int spent = 0;                                     /* one band computation a block, over all strips */
    for (int t = 0; t < TRACKS && !spent; t++) {
        struct strip_pend *q = &g_pend[t];
        coop_strip_bf *s = &g_strip[t];
        while (q->stage >= 0 && q->stage < 4) {
            int b = q->stage;
            if (coop_strip_bf_band_same(&q->p, &s->p, b)) { q->on[b] = s->band_on[b]; q->fresh[b] = 0; q->stage++; continue; }
            if (spent) break;
            uint32_t c0 = cyc();
            q->on[b] = (uint8_t)coop_strip_bf_band(&q->p, b, q->eqc[b]);
            q->fresh[b] = 1;
            q->stage++;
            spent = 1;
            uint32_t dt = cyc() - c0;
            if (dt > g_strip_stat.stage_max) g_strip_stat.stage_max = dt;
            g_strip_stat.bands++;
        }
        if (q->stage == 4 && !spent) {                 /* commit (cheap), in a block of its own after a band */
            uint32_t c0 = cyc();
            s->p = q->p;
            for (int b = 0; b < 4; b++)
                if (q->fresh[b]) for (int i = 0; i < 18; i++) s->eqc[b][i] = q->eqc[b][i];
            coop_strip_bf_commit(s, q->on);
            q->stage = -1;
            uint32_t dt = cyc() - c0;
            if (dt > g_strip_stat.commit_max) g_strip_stat.commit_max = dt;
            g_strip_stat.commits++;
        }
    }
    if (g_coop_strip_wobble && ++g_wobble_n >= g_coop_strip_wobble) {   /* test: flip track 1's band 2 */
        g_wobble_n = 0;
        coop_strip_params p = g_strip[0].p;
        p.eq_gain[1] = p.eq_gain[1] == STRIP_EQ_GAIN0 + 12 ? STRIP_EQ_GAIN0 - 12 : STRIP_EQ_GAIN0 + 12;
        coop_strip_post(0, &p);
    }
}

/* ---- the tick: split decision (from coop_tape_gains, once a tick, before the lane loops) ---- */
extern "C" void coop_strip_tape_gains(const int16_t *g, int stereo)
{
    strip_init_once();
    g_strip_stat.gains_calls++;
    uint32_t mask = 0;
    if ((g_coop_strip_on & 1u) && stereo)
        for (int t = 0; t < TRACKS; t++) if (!coop_strip_neutral(&g_strip[t].p)) mask |= 1u << t;
    for (int t = 0; t < TRACKS; t++) {
        int on = (mask >> t) & 1;
        if ((g_strip_lanes.end[t] | g_strip_lanes.end[TRACKS + t]) && on) {   /* a tick that wrote lanes but never
                                                           reached the resampler: clear what it left */
            int32_t e = g_strip_lanes.end[t] > g_strip_lanes.end[TRACKS + t] ? g_strip_lanes.end[t] : g_strip_lanes.end[TRACKS + t];
            if (e > PAIR_N) e = PAIR_N;
            e = (e + 1) & ~1;
            zero16(g_pair[t][0] + PAIR_GUARD, e);
            zero16(g_pair[t][1] + PAIR_GUARD, e);
        }
        g_strip_lanes.end[t] = g_strip_lanes.end[TRACKS + t] = 0;
        if (on && !((g_was_split >> t) & 1)) {         /* newly split: a clean pair */
            zero16(g_pair[t][0], PAIR_GUARD + PAIR_N + PAIR_GUARD);
            zero16(g_pair[t][1], PAIR_GUARD + PAIR_N + PAIR_GUARD);
            g_strip_stat.splits++;
        }
        g_strip_lanes.dst[t] = on ? g_pair[t][0] + PAIR_GUARD : 0;
        g_strip_lanes.dst[TRACKS + t] = on ? g_pair[t][1] + PAIR_GUARD : 0;
        g_gl[t] = g[t];
        g_gr[t] = g[TRACKS + t];
    }
    g_was_split = mask;
    g_strip_split = mask;
    g_strip_stat.last_mask = mask;
}

/* ---- the tick: after the stock resampler pass (coop_strip_rs_thunk, only with a split track) ---- */
extern "C" void coop_strip_tape(int32_t pos, int32_t inc, const int16_t *step)
{
    uint32_t c0 = cyc();
    uint32_t mask = g_strip_split;
    coop_strip_bf_bus bus = { D34, DB4, g_send[0][0], g_send[0][1], g_send[1][0], g_send[1][1] };
    struct rs_io io;
    io.step = step; io.outL = g_trk[0]; io.outR = g_trk[1];
    for (int t = 0; t < TRACKS; t++) {
        if (!((mask >> t) & 1)) continue;
        io.inL = g_pair[t][0];                         /* = the pair - 8 samples, as the stock's 0x01b5ef84 */
        io.inR = g_pair[t][1];
        coop_rs_pair(pos, inc, &io);
        uint32_t z0 = cyc();
        int32_t e = g_strip_lanes.end[t] > g_strip_lanes.end[TRACKS + t] ? g_strip_lanes.end[t] : g_strip_lanes.end[TRACKS + t];
        if (e > PAIR_N) e = PAIR_N;
        g_strip_stat.split_samples += (uint32_t)e;
        e = (e + 1) & ~1;
        zero16(g_pair[t][0] + PAIR_GUARD, e);
        zero16(g_pair[t][1] + PAIR_GUARD, e);
        g_strip_lanes.end[t] = g_strip_lanes.end[TRACKS + t] = 0;
        uint32_t dz = cyc() - z0;
        if (dz > g_strip_stat.zero_max) g_strip_stat.zero_max = dz;
        coop_strip_bf *s = &g_strip[t];
        coop_strip_bf_process(s, g_trk[0], g_trk[1], g_gl[t], g_gr[t], &bus);
        if (s->jobs[0].g0 | s->jobs[0].g1 | s->jobs[1].g0 | s->jobs[1].g1) g_send_used[0] = 1;
        if (s->jobs[2].g0 | s->jobs[2].g1 | s->jobs[3].g0 | s->jobs[3].g1) g_send_used[1] = 1;
    }
    uint32_t dt = cyc() - c0;
    g_strip_stat.tape_calls++;
    if (dt > g_strip_stat.tape_max) g_strip_stat.tape_max = dt;
    g_strip_stat.tape_sum += dt;
    g_strip_stat.tape_n++;
}

/* ---- the master: a send through an effect, wet only ---- */
/* Whether bus k's effect renders this block: while sent to; then until its return stayed quiet for the tail. */
static int fx_due(int k)
{
    if (g_send_used[k]) { g_fx_run[k] = 1; g_fx_quiet[k] = 0; return 1; }
    return g_fx_run[k] != 0;
}

static void fx_return(int k)                           /* d34 / db4 += FX(send) - send; the bus zeroed */
{
    int16_t *l = g_send[k][0], *r = g_send[k][1];
    int32_t pk = 0;
    for (int i = 0; i < BLK; i++) {
        int32_t a = (int32_t)l[i] - g_dry[0][i], b = (int32_t)r[i] - g_dry[1][i];
        D34[i] = coop_sat16(D34[i] + a);
        DB4[i] = coop_sat16(DB4[i] + b);
        a = a < 0 ? -a : a; b = b < 0 ? -b : b;
        if (a > pk) pk = a;
        if (b > pk) pk = b;
    }
    if (!g_send_used[k]) {                             /* the tail */
        if (pk <= 2) { if (++g_fx_quiet[k] >= g_coop_strip_tail_blocks) g_fx_run[k] = 0; }
        else g_fx_quiet[k] = 0;
        if (k) g_strip_stat.fx2_tail++; else g_strip_stat.fx1_tail++;
    }
    zero16(l, BLK); zero16(r, BLK);
    g_send_used[k] = 0;
}

static void fx_keep_dry(int k)
{
    const uint32_t *l = (const uint32_t *)g_send[k][0], *r = (const uint32_t *)g_send[k][1];
    uint32_t *dl = (uint32_t *)g_dry[0], *dr = (uint32_t *)g_dry[1];
    for (int i = 0; i < BLK / 2; i++) { dl[i] = l[i]; dr[i] = r[i]; }
}

static void send_drop(int k)                           /* nothing renders it: just clear it */
{
    if (!g_send_used[k]) return;
    zero16(g_send[k][0], BLK); zero16(g_send[k][1], BLK);
    g_send_used[k] = 0;
}

typedef void (*fx_render_fn)(void *self, int16_t *l, int16_t *r, const int16_t *params);

/* 0xffa017b8 (the master FX 1 block, reached when FX 1 is on): replaces `render(this + adjust, d34, db4, params)`
 * (L1 0xffa017b8..e6). Called from L1 with the master stage's SP (its outgoing area holds the stock's 4th argument
 * slot); a normal C function: the stage reloads what it uses after 0xffa01220 (P5-based). */
extern "C" void coop_master_fx1(void)
{
    uint8_t *inst = FX1_INST;
    uint8_t *vt = *(uint8_t **)inst;
    fx_render_fn render = *(fx_render_fn *)(vt + 0x14);
    void *self = inst + *(int16_t *)(vt + 0x10);
    if (!(g_coop_strip_on & 1u)) { render(self, D34, DB4, FX1_PARAMS); return; }
    if (!fx_due(0)) { send_drop(0); return; }
    uint32_t c0 = cyc();
    fx_keep_dry(0);
    render(self, g_send[0][0], g_send[0][1], FX1_PARAMS);
    fx_return(0);
    g_strip_stat.fx1_renders++;
    uint32_t dt = cyc() - c0;
    if (dt > g_strip_stat.fx_max) g_strip_stat.fx_max = dt;
}

/* ---- CS4 research probe (test images only, g_coop_strip_probe = 1): the master comp words' neutral values ----
 * From 10 s of audio on, segments of 345 blocks (45 silent, then 300 of a 1,034 Hz sine written over d34 / db4 right
 * before the stock level stage 0xffa04280): two baseline segments (the words as found; amplitude high, low), then per
 * comp word W[0xff80189c + 2 w] (w 0..3) and value in PROBE_V, the word forced every block, the others as found,
 * high then low amplitude. The audio dump's sine runs, in order, give the stage's gain per setting and level. */
__attribute__((section(".data"))) volatile uint32_t g_coop_strip_probe = 0;
#define PROBE_WORDS ((volatile int16_t *)0xff80189cu)
#define SINE_TAB ((const int16_t *)0x0101e5e8u)          /* OP1_SINE_TABLE, 8192 x int16 (l1_stereo_insert.py) */
static const int16_t PROBE_V[7] = { 0x0000, 0x1000, 0x2000, 0x2800, 0x4000, 0x6000, 0x7fff };
static const int16_t PROBE_A[2] = { 0x6000, 0x0400 };
static uint32_t g_probe_blk, g_probe_ph;
static int16_t g_probe_base[4];
volatile uint32_t g_probe_seg;
static void comp_probe(void)
{
    uint32_t n = g_probe_blk++;
    const uint32_t start = 6890, seg = 345, nseg = 2 + 4 * 7 * 2;
    if (n < start) return;
    if (n == start) for (int w = 0; w < 4; w++) g_probe_base[w] = PROBE_WORDS[w];
    uint32_t s = (n - start) / seg, k = (n - start) % seg;
    if (s >= nseg) { if (s == nseg && k == 0) for (int w = 0; w < 4; w++) PROBE_WORDS[w] = g_probe_base[w]; return; }
    g_probe_seg = s;
    int amp = PROBE_A[s & 1];
    for (int w = 0; w < 4; w++) PROBE_WORDS[w] = g_probe_base[w];
    if (s >= 2) { uint32_t i = (s - 2) / 2; PROBE_WORDS[i / 7] = PROBE_V[i % 7]; }
    for (int i = 0; i < BLK; i++) {
        int16_t v = 0;
        if (k >= 45) { v = (int16_t)((SINE_TAB[(g_probe_ph >> 4) & 8191] * amp) >> 15); g_probe_ph += 192 << 4; }
        D34[i] = v; DB4[i] = v;
    }
}

/* From coop_master_apply (master_suite.cc), first thing, every block: the staged updates, FX 2 on send bus 2 and
 * the buses cleared. Returns 1 when the strip mode is on (the master's slot 2 then does not run on d34 / db4). */
extern "C" int coop_strip_master(void)
{
    if (!g_inited) strip_init_once();
    if (g_coop_strip_probe) comp_probe();
    strip_stage_service();
    if (!(g_coop_strip_on & 1u)) { send_drop(0); send_drop(1); return 0; }
    send_drop(0);                                      /* FX 1 off (or nothing due): its bus is still cleared */
    if (g_coop_s2_live[1] && fx_due(1)) {
        uint32_t c0 = cyc();
        fx_keep_dry(1);
        coop_slot2_render(1, g_send[1][0], g_send[1][1]);
        fx_return(1);
        g_strip_stat.fx2_renders++;
        uint32_t dt = cyc() - c0;
        if (dt > g_strip_stat.master_max) g_strip_stat.master_max = dt;
    } else {
        send_drop(1);
    }
    return 1;
}

/* ---- CS5: reads for the strip pages (coop_strip_ui.cc, Gfx thread) ---- */
/* the compressor's gain reduction now (log2 Q16, >= 0), 0 while the comp is off */
extern "C" int32_t coop_strip_gr(int t) { const coop_strip_bf *s = &g_strip[t & 3]; return s->p.comp_on ? s->comp.gr : 0; }
/* how many blocks coop_strip_tape has run: the meters are fresh when it moved */
extern "C" uint32_t coop_strip_blocks(void) { return g_strip_stat.tape_n; }
