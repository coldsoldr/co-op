/* petri: a granular micro-loop effect (inspired by the signal flow of a granular micro-loop pedal) as a coop effect.
 * The input is recorded continuously into a 1.36 s stereo buffer at 22.05 kHz; one of eleven effects plays it back
 * through up to eight voices (loops, grains, slices, taps); the wet goes through a shared tone section (the effect's
 * envelope on every repeat, a gentle pitch modulation, a resonant low-pass, a small space) and is mixed with the
 * untouched dry signal. Hold stops the recording (the effect keeps working on the held slice); reverse plays it
 * backwards. Contract, display, memory and cost: README.md.
 *
 * Per block (64 samples at 44.1 kHz, stereo at every position, guidelines §2.2): the Q31 bus to int16, 2:1 down to
 * 22.05 kHz (Clouds' halfband, src/half_rate.h, copied from tape_echo), the engine (src/petri_engine.cc), back up, the
 * equal-power mix with the dry input and the soft limit, back to Q31.
 *
 * Controls (plugin.json "controls", sdk/coop_plugin.h e6), read once at the block start:
 *   params[0..3]  time, activity, repeats, mix: the four turns, in the filter type's range units; normalised to 0..32767.
 *   params[5]     effect (fn + blue): step 0..10, mosaic seq glide haze tunnel strum blocks cut arp pattern warp.
 *   params[6]     filter (fn + white, shift + fn + white its fine): Q15, 150 Hz .. 10 kHz.
 *   params[7]     space (fn + orange, shift + fn + orange its fine): Q15, dry .. a wash.
 *   params[8]     bit 0 hold (orange click), bit 1 reverse (blue click): latches.
 *
 * Memory (e8): the instance is in the slot's position buffer (coop_fx_scratch, plugin.json "scratch"); the .bss holds
 * only the five pointers. The record (118.5 KiB) and the space (9.8 KiB) are cleared over the first blocks after a
 * (re)start (kClearWords a block, the dry passing through), not in init (guidelines §2.3).
 * Screen (e9): petri_screen.cc, petri.svg; the render publishes the voices, the write head, the input level and
 * onsets and the latches in the state (state.h Display), once a block.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. The half-rate path follows
 * Mutable Instruments Clouds (Emilie Gillet, MIT, LICENSE-MIT). */
#include <stdint.h>

#include "coop_plugin.h"
#include "state.h"

extern "C" const coop_fx_screen_t SCREEN_petri;        /* petri_screen.cc (e9) */

#if defined(__bfin__) && !defined(COOP_PETRI_HOST_CLOCK)
static inline uint32_t Cycles()
{
    uint32_t c;
    __asm__ __volatile__("%0 = CYCLES;" : "=d"(c));
    return c;
}
#else
extern uint32_t g_petri_host_cycles;                   /* the host test (tests/host/petri) steps it */
static inline uint32_t Cycles() { return g_petri_host_cycles; }
#endif

namespace {

using coop_petri::kBlock;
using coop_petri::kHalfN;
const int32_t kClearWords = 2048;                      /* words zeroed a block: 17 blocks (25 ms) for all */

/* The filter type's knob ranges (sdk/coop_plugin.h). */
const int16_t kRangeLo[4] = { 64, 0, 0, 8192 };
const int16_t kRangeHi[4] = { 14528, 24576, 22528, 32767 };

typedef coop_petri::Instance Instance;

#ifdef COOP_FX_SCRATCH_BYTES
static_assert(sizeof(Instance) <= COOP_FX_SCRATCH_BYTES, "plugin.json scratch: the state must fit");
static_assert(COOP_FX_SCRATCH_BYTES - sizeof(Instance) < 32, "plugin.json scratch: declare the state's size, 32-rounded");
#endif
Instance *g_inst[COOP_FX_SLOTS];                       /* .bss: each slot's state, in its position buffer (e8) */

inline int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }

/* A turn's word in the filter range -> 0..32767. One software divide (libgcc's ___divsi3, ~190 cycles on this CPU),
 * only for a word that moved (Knobs below). */
inline int32_t Knob(int32_t word, int k)
{
    const int32_t lo = kRangeLo[k], hi = kRangeHi[k];
    return Clamp15((word - lo) * 32767 / (hi - lo));
}

/* 0.3: the four turns as the DSP takes them, kept per slot and redone only for a word that moved (four divides a block
 * before: docs/efficiency-audit.md finding 8). The same values, so the same output bit for bit. Tiny per-slot values
 * in .bss (guidelines 2.3: the state has no room, it is sized to the position buffers' 134,784 bytes); init clears
 * `valid`, so the first render after a selection computes all four. */
struct Knobs {
    int16_t raw[4];                                    /* params[0..3] as last seen */
    int32_t valid;
    int32_t q[4];                                      /* 0..32767 */
};
Knobs g_knobs[COOP_FX_SLOTS];

inline const Knobs &ReadKnobs(int slot, const int16_t *p)
{
    Knobs &c = g_knobs[slot];
    const bool all = !c.valid;
    for (int k = 0; k < 4; ++k) {
        if (all || p[k] != c.raw[k]) {
            c.raw[k] = p[k];
            c.q[k] = Knob(p[k], k);
        }
    }
    c.valid = 1;
    return c;
}

/* The idle path (0.3; docs/efficiency-audit.md finding 6, README "Idle"). petri left on a position cost its whole
 * block on silence. Now: while the input block is exact silence (every sample's top 16 bits 0) and hold is off, each
 * block's check asks whether the engine made and left only zeros: the frames it just wrote to the record (the input,
 * and for the effects that feed back, the wet: petri::WroteSilence) and its wet output. kIdleHold such blocks in a row
 * (the record once round and a margin: every frame of it is then zero, so no voice, whatever it is told to play,
 * plays anything) and the render stops computing samples. Idle, a block is the
 * detector, the effect's scheduler and the voices' positions and envelopes as ever (petri::ProcessIdle: the same
 * grains start and end at the same blocks, the same random numbers are drawn, the write head moves on), the two tone
 * controls' and the mix's smoothing, and the display; the voices' kernel, the record's write, the filter, the space
 * and the half-rate path are not run. The first block with any input, or hold, is rendered in full (the check is
 * before the DSP: no sample of a new note is lost). warp's buses must be at rest too, or the effect must not be warp
 * (a bus left with a level when warp was switched away would speak when it comes back).
 * What an idle petri leaves as it was, where the never-idle one works on: the filter's and the space's last states
 * under the output (the space gates its last +-2 LSB itself). A note after an idle time therefore meets the same
 * grains through a filter and a space an LSB or so apart (measured: README "Idle").
 * Tiny per-slot values in .bss (guidelines 2.3: the state has no room), cleared by init and by a restart. */
const int32_t kIdleHold = petri::kRecLen / petri::kN + 8;   /* blocks: 947 (1.37 s) */
struct IdleState {
    int32_t quiet;                                     /* silent blocks of zeros in a row */
    int32_t on;
};
IdleState g_idle[COOP_FX_SLOTS];

#if defined(COOP_FX_NO_IDLE)                           /* a build that never idles (a test's reference) */
#define IDLE_OFF true
#define IDLE_COUNT() ((void)0)
#define IDLE_LEVEL(x) ((void)0)
#elif defined(COOP_IDLE_PROBE)                         /* tests/host/idle: the lockstep's switch and its counters */
}  // namespace
extern "C" {
extern int g_coop_idle_off;
extern uint32_t g_coop_idle_blocks;
extern int32_t g_coop_idle_level;
}
namespace {
#define IDLE_OFF (g_coop_idle_off != 0)
#define IDLE_COUNT() ((void)++g_coop_idle_blocks)
#define IDLE_LEVEL(x) ((void)(g_coop_idle_level = (x)))
#else
#define IDLE_OFF false
#define IDLE_COUNT() ((void)0)
#define IDLE_LEVEL(x) ((void)0)
#endif

/* True when the block's input is exact silence on the int16 bus (the top halves all 0). Six samples first: while
 * anything plays, one of them is not zero and the block costs a few cycles here; only a block that may be silent is
 * read through (SilentScan, out of line: a playing block's code stays as it was). */
__attribute__((noinline)) bool SilentScan(const int32_t *l, const int32_t *r)
{
    uint32_t any = 0;
    for (int32_t i = 0; i < kBlock; ++i) any |= static_cast<uint32_t>(l[i]) | static_cast<uint32_t>(r[i]);
    return (any >> 16) == 0;
}
inline bool Silent(const int32_t *l, const int32_t *r)
{
    const uint32_t some = static_cast<uint32_t>(l[0] | r[0] | l[21] | r[42] | l[kBlock - 1] | r[kBlock - 1]);
    if (__builtin_expect((some >> 16) != 0, 1)) return false;
    return SilentScan(l, r);
}

/* A silent block's check (out of line: a playing block never comes here): the record's new frames and the wet output
 * all zero. */
__attribute__((noinline)) bool WroteZeros(const Instance *g, const int16_t *wl, const int16_t *wr)
{
    int32_t any = 0;
    for (int32_t i = 0; i < kHalfN; ++i) any |= wl[i] | wr[i];
    return any == 0 && petri::WroteSilence(&g->eng);
}

/* e9: the display words (state.h Display), one store each, once a block after the DSP; the DSP never reads them. */
void Publish(Instance *g)
{
    const petri::Engine &e = g->eng;
    coop_petri::Display &d = g->disp;
    int32_t active = 0;
    for (int32_t i = 0; i < petri::kVoices; ++i) {
        const petri::Voice &v = e.v[i];
        const bool on = v.blocks != 0 || v.fade != 0;
        if (on) active |= 1 << i;
        d.v[i].pos = on ? static_cast<int32_t>(v.pos >> petri::kPosShift) : -1;
        d.v[i].rate = v.rate >> 9;
        d.v[i].level = on ? v.level : 0;
    }
    d.effect = e.effect;
    d.head = static_cast<int32_t>(e.head & petri::kRecMask);
    d.level = e.level;
    d.onsets = e.onset_count;
    d.flags = (e.hold ? coop_petri::kDispHold : 0) | (e.reverse ? coop_petri::kDispReverse : 0) |
              (g->clear < petri::kClearEnd ? coop_petri::kDispClearing : 0);
    d.active = active;
}

int init(int slot)
{
    if (slot < 0 || slot >= COOP_FX_SLOTS) return -1;
    g_knobs[slot].valid = 0;                            /* nothing kept across a selection */
    g_idle[slot].quiet = g_idle[slot].on = 0;
    uint32_t bytes = 0;
    Instance *g = static_cast<Instance *>(coop_fx_scratch(slot, &bytes));
    g_inst[slot] = (g && bytes >= sizeof(Instance)) ? g : nullptr;
    if (!g_inst[slot]) return -1;
    g->half.Stop();                                     /* the next render restarts: engine reset, buffers cleared */
    g->clear = 0;
    g->disp.effect = -1;                                /* e9: nothing published yet */
    g->disp.head = 0;
    g->disp.level = 0;
    g->disp.onsets = 0;
    g->disp.flags = coop_petri::kDispClearing;
    g->disp.active = 0;
    for (int32_t i = 0; i < petri::kVoices; ++i) {
        g->disp.v[i].pos = -1;
        g->disp.v[i].rate = 256;
        g->disp.v[i].level = 0;
    }
    return 0;
}

void render(int slot, int32_t *l, int32_t *r, const int16_t *params, int n)
{
    if (slot < 0 || slot >= COOP_FX_SLOTS || !g_inst[slot]) return;   /* not bound: dry */
    Instance *const g = g_inst[slot];
    if (n != kBlock) return;                            /* the host's block is 64; anything else passes through */
    IdleState &idle = g_idle[slot];
    if (!g->half.Begin(Cycles())) {
        petri::Reset(&g->eng);
        g->clear = 0;
        idle.quiet = idle.on = 0;
    }
    if (g->clear < petri::kClearEnd) {                  /* starting: clear a slice, the dry passes through */
        g->clear = petri::ClearStep(&g->eng, g->clear, kClearWords);
        g->half.End(Cycles());
        Publish(g);
        return;
    }

    petri::Controls c;
    const Knobs &kn = ReadKnobs(slot, params);
    c.time = kn.q[0];
    c.activity = kn.q[1];
    c.repeats = kn.q[2];
    const int32_t mix = kn.q[3];
    c.effect = params[5] < 0 ? 0 : (params[5] >= petri::kNumEffects ? petri::kNumEffects - 1 : params[5]);
    c.filter = Clamp15(params[6]);
    c.space = Clamp15(params[7]);
    c.hold = (params[COOP_FX_LATCH] & 1) != 0;
    c.reverse = (params[COOP_FX_LATCH] & 2) != 0;

    /* Idle (above): silence in, hold off, warp's buses at rest. half.in() is the decimated input: zeros by then. */
    const bool may_idle = !c.hold && (c.effect != petri::FX_WARP || petri::BusAtRest(&g->eng)) && Silent(l, r);
    if (__builtin_expect(idle.on, 0)) {
        if (may_idle && !IDLE_OFF) {
            petri::ProcessIdle(&g->eng, g->half.in(0), g->half.in(1), c);
            g->half.IdleMix(mix);
            g->half.End(Cycles());
            Publish(g);
            IDLE_COUNT();
            return;
        }
        idle.on = idle.quiet = 0;                       /* awake: this block in full */
    }

    int16_t *dl = g->dry[0], *dr = g->dry[1];
    int16_t *il = g->half.in_full(0), *ir = g->half.in_full(1);
    for (int32_t i = 0; i < kBlock; ++i) {
        const int16_t a = static_cast<int16_t>(l[i] >> 16), b = static_cast<int16_t>(r[i] >> 16);
        dl[i] = il[i] = a;
        dr[i] = ir[i] = b;
    }
    g->half.Decimate();
    petri::Process(&g->eng, g->half.in(0), g->half.in(1), g->half.wet(0), g->half.wet(1), c);
    if (__builtin_expect(may_idle, 0)) {                /* a silent block: did the engine write and give only zeros? */
        const bool zeros = WroteZeros(g, g->half.wet(0), g->half.wet(1));
        IDLE_LEVEL(zeros ? 0 : 1);
        idle.quiet = zeros ? idle.quiet + 1 : 0;
        if (idle.quiet >= kIdleHold && !IDLE_OFF) idle.on = 1;
    } else {
        idle.quiet = 0;
    }
    g->half.Mix(dl, dr, mix);
    for (int32_t i = 0; i < kBlock; ++i) {
        l[i] = static_cast<int32_t>(dl[i]) << 16;
        r[i] = static_cast<int32_t>(dr[i]) << 16;
    }
    g->half.End(Cycles());
    Publish(g);
}

}  // namespace

extern "C" const coop_fx_t FX_petri = { COOP_FX_MAGIC, COOP_PLUGIN_API, COOP_FX_F_SCREEN, init, render, &SCREEN_petri };
