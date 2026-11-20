/* cs_hall: coldsoldr's FDN hall reverb (te-op1-cfw ports/fx/src/hall, the kennel build ports/knl/cs_hall) as a coop
 * effect. The core of hall.h at half the effect rate behind the half-rate path (decimate 2:1, the hall makes 32 wet
 * samples at 22.05 kHz, interpolate, equal-power mix with the dry signal and a soft limit), true stereo at every
 * position (guide/plugin-guidelines.md section 2.2: a reverb is cross-channel; the hall sums its input to mono itself
 * and its tank makes the stereo).
 *
 * Controls (plugin.json "controls", the owner's layout of 2026-11-11):
 *   turns   blue size, green decay, white damp, orange mix (params[0..3], the filter type's range units: mapped to
 *           Q15 here, as kennel's op1_fx_param_q15 did, with the SDK's constant ranges instead of the table address)
 *   fn + blue predelay (word 5), fn + green early (word 6), fn + white width (word 7); shift + fn + the same encoder is
 *           each one's fine step (the core does it: "fine": true)
 *   orange click: freeze (a latch, bit 0 of params[8])
 *   fixed: motion at 9830 (the firmware build's fourth page-2 knob, as kennel)
 *
 * What changed from the kennel glue (hall_knl.cc):
 *   - the instance is in the slot's position buffer, as kennel's position scratch buffer had it (e8: coop_fx_scratch,
 *     plugin.json "scratch"); the file is relocated once and its .bss holds only the five buffer pointers;
 *   - the tank's clear (84 KB) is not done in init (guidelines section 2.3: init runs in the audio interrupt at the
 *     stock positions): init resets the reverb's control state and marks the buffers for clearing; render then clears
 *     kClearWords a block and passes the input through dry until it is done (~21 blocks, 30 ms), on every selection
 *     (the buffer holds whatever the effect before left). The restart after a pause (kennel's half_rate.h: over 5 ms
 *     without a call, e.g. bypassed) clears the same way;
 *   - knl::Fine (the shift byte at a firmware address) is gone: the core gives the fine step;
 *   - the stereo render runs at every position (kennel: mono in / mono out at synth and drum, width at the master
 *     only); there is no mono render;
 *   - the screen (e9) is cs_hall_screen.cc and cs_hall.svg; kennel's screen read nothing from the DSP, so the render
 *     publishes nothing (the state is unchanged).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>

#include "coop_plugin.h"
#include "halfband.h"
#include "hall.h"

extern "C" const coop_fx_screen_t SCREEN_cs_hall;   /* cs_hall_screen.cc (e9) */

#if !defined(__bfin__)
extern "C" uint32_t cs_hall_host_cycles;               /* the host test steps it (tests/host/cs_hall) */
#endif

namespace {

const int32_t kBlock = 64;
const int32_t kHalfN = kBlock / 2;
const int32_t kMotion = 9830;
enum { P_SIZE, P_DECAY, P_DAMP, P_MIX, P_PREDELAY, P_EARLY, P_MOTION, P_WIDTH };

/* The filter type's knob ranges, as the core hands params[0..3] (sdk/coop_plugin.h). */
const int32_t kLo[4] = { 64, 0, 0, 8192 };
const int32_t kHi[4] = { 14528, 24576, 22528, 32767 };

inline int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }

/* kennel's op1_fx_param_q15 with the constant ranges: one software divide (libgcc's ___divsi3, ~190 cycles on this
 * CPU), only for a word that moved (Knobs below). */
inline int32_t KnobQ15(int32_t word, int i)
{
    const int32_t v = (word - kLo[i]) * 32767 / (kHi[i] - kLo[i]);
    return Clamp15(v);
}

/* 0.3: the four turns as the DSP takes them, kept per slot and redone only for a word that moved (four divides a block
 * before: docs/efficiency-audit.md finding 8). The same values, so the same output bit for bit. Tiny per-slot values
 * in .bss (guidelines 2.3); init clears `valid`, so the first render after a selection computes all four. */
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
            c.q[k] = KnobQ15(p[k], k);
        }
    }
    c.valid = 1;
    return c;
}

inline uint32_t Cycles()
{
#if defined(__bfin__)
    uint32_t c;
    __asm__ __volatile__("%0 = CYCLES;" : "=d"(c));   /* the core clock's counter: a register, not memory */
    return c;
#else
    return cs_hall_host_cycles;
#endif
}

/* te-op1-cfw ports/fx/op1/half_rate.h for one instance: the soft-limit table is constant data (halfband.cc). */
class HalfRate {
 public:
    bool Begin(uint32_t now) {
        if (running_ && now - last_end_ > kGapCycles) running_ = false;
        last_end_ = now;
        if (running_) return true;
        for (int32_t c = 0; c < 2; ++c) {
            decimator_[c].Init();
            interpolator_[c].Init();
        }
        mix_ = 0;
        running_ = true;
        return false;
    }
    void Stop() { running_ = false; }
    void End() { last_end_ = Cycles(); }
    /* 0.3, the idle path: Mix's smoothing of the mix alone (no wet, a silent dry: nothing to compute) */
    void IdleMix(int32_t mix) {
        const int32_t target = (mix << 1) + (mix >> 14);
        mix_ += (target - mix_) >> 2;
    }
    void Decimate(const int16_t *l, const int16_t *r) {
        decimator_[0].Process(l, half_[0], kHalfN);
        decimator_[1].Process(r, half_[1], kHalfN);
    }
    const int16_t *in(int32_t c) const { return half_[c]; }
    int16_t *wet(int32_t c) { return wet_half_[c]; }
    /* the wet back to full rate and mixed into l and r, mix 0..32767, smoothed by a quarter a call (~6 ms) */
    void Mix(int16_t *l, int16_t *r, int32_t mix) {
        for (int32_t c = 0; c < 2; ++c) {
            int16_t *w = wet_half_[c];
            for (int32_t i = 0; i < kHalfN; ++i) w[i] = static_cast<int16_t>(w[i] >> 1);   /* half scale */
            interpolator_[c].Process(w, wet_full_[c], kHalfN);
        }
        const int32_t target = (mix << 1) + (mix >> 14);                                   /* Q16 */
        mix_ += (target - mix_) >> 2;
        const int16_t fade_out = FadeOut(mix_), fade_in = FadeIn(mix_);
        op1clouds::MixSoftLimit(l, wet_full_[0], kBlock, fade_out, fade_in, &op1clouds::kSoftLimit);
        op1clouds::MixSoftLimit(r, wet_full_[1], kBlock, fade_out, fade_in, &op1clouds::kSoftLimit);
    }

 private:
    static const uint32_t kGapCycles = 2000000;   /* 5 ms at 400 MHz */
    static int16_t FadeOut(int32_t mix_q16) {
        const int32_t f = op1clouds::InterpolateQ16(op1clouds::kXfadeOutQ31, mix_q16, 4) >> 16;
        const int32_t g = (f * 46341) >> 15;
        return static_cast<int16_t>(g > 32767 ? 32767 : g);
    }
    static int16_t FadeIn(int32_t mix_q16) {
        const int32_t f = op1clouds::InterpolateQ16(op1clouds::kXfadeInQ31, mix_q16, 4) >> 16;
        return static_cast<int16_t>((f * 23170) >> 15);
    }

    op1clouds::HalfbandDecimator decimator_[2];
    op1clouds::HalfbandInterpolator interpolator_[2];
    int16_t half_[2][kHalfN];
    int16_t wet_half_[2][kHalfN];
    int16_t wet_full_[2][kBlock];
    bool running_;
    uint32_t last_end_;
    int32_t mix_;   /* Q16 */
};

/* 4 KB a block: 1024 word stores (~1-2 k cycles [I]); the whole 84,774 bytes in 21 blocks. */
const int32_t kClearWords = 2048;

struct Instance {
    op1fx::HallReverb hall;
    HalfRate half;
    int32_t clear_at;      /* next int16 word to clear; == ClearWords(): nothing to clear */
};
#ifdef COOP_FX_SCRATCH_BYTES
static_assert(sizeof(Instance) <= COOP_FX_SCRATCH_BYTES, "plugin.json scratch: the state must fit");
#endif

Instance *g_inst[COOP_FX_SLOTS];   /* .bss: each slot's state, in its position buffer (e8, bound by init) */

/* The idle path (0.3; docs/efficiency-audit.md finding 6, README "Idle"). A hall left on a position cost its whole
 * block on silence. Now: while the input block is exact silence (every sample's top 16 bits 0), freeze is off and the
 * decay turn is under infinite, each block's check looks at the wet signal the hall just made (64 samples at 22.05
 * kHz, before the mix). kIdleHold blocks of zeros in a row and the render stops computing: it passes the silent input
 * and runs only the control (HallReverb::Idle: the smoothers and the two modulation LFOs, in phase). kIdleHold is the
 * tank's ring once round and the pre-delay's (1.06 s): by then every word a line, the early taps or a later size
 * can read was written while the output was zero. The first block with any input, freeze, or infinite decay is
 * rendered in full (the check is before the DSP: no sample of a new note is lost) on the tank as it was left.
 * What the tank still holds then is under what the output shows: the three input allpasses keep a +-1 LSB pattern
 * going round for ever (their rounding; found with the test), which reaches the lines as nothing. An idle hall's
 * silence is therefore the hall's own silence, sample for sample; a note after it meets that last pattern where it
 * stopped instead of where it would have turned to, and the hall's dithered rounding then takes other turns: the
 * tail differs from the never-idle hall's by its own rounding noise (a few LSB under a loud tail: README "Idle").
 * Tiny per-slot values in .bss (guidelines 2.3), cleared by init and by a restart. */
const int32_t kIdleHold = (HL_SIZE + HL_PRE_WORDS) / kHalfN + 1;   /* blocks: 732 (1.06 s) */
struct IdleState {
    int32_t quiet;                 /* silent, quiet blocks in a row */
    int32_t on;
};
IdleState g_idle[COOP_FX_SLOTS];

#if defined(COOP_FX_NO_IDLE)       /* tests/host/cs_hall: the hall against te-op1-cfw's, sample for sample (the idle */
#define IDLE_OFF true              /* path has its own test) */
#define IDLE_COUNT() ((void)0)
#define IDLE_LEVEL(x) ((void)0)
#elif defined(COOP_IDLE_PROBE)     /* tests/host/idle: the lockstep's switch and its counters */
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

/* True when the two half-rate blocks are all zero (a silent block's check: out of line). */
__attribute__((noinline)) bool Zeros(const int16_t *a, const int16_t *b)
{
    int32_t any = 0;
    for (int32_t i = 0; i < kHalfN; ++i) any |= a[i] | b[i];
    return any == 0;
}

/* e8: always the whole tank: the position buffer holds whatever the effect before left (before e8 a tank the reverb
 * never wrote, zeroed by the loader, was skipped) */
void StartClear(Instance &g)
{
    g.hall.InitKeepBuffers();
    g.clear_at = 0;
}

/* One block's share of the clear. The buffers start 4-byte aligned (the class holds int32 members before them). */
void ClearStep(Instance &g)
{
    int16_t *const base = g.hall.ClearBase();
    const int32_t total = op1fx::HallReverb::ClearWords();
    int32_t end = g.clear_at + kClearWords;
    if (end > total) end = total;
    int32_t i = g.clear_at;
    if ((reinterpret_cast<uintptr_t>(base + i) & 3) == 0) {
        uint32_t *w = reinterpret_cast<uint32_t *>(base + i);
        const int32_t pairs = (end - i) >> 1;
        for (int32_t k = 0; k < pairs; ++k) w[k] = 0;
        i += 2 * pairs;
    }
    for (; i < end; ++i) base[i] = 0;
    g.clear_at = end;
}

int init(int slot)
{
    if (slot < 0 || slot >= COOP_FX_SLOTS) return -1;
    g_knobs[slot].valid = 0;                                   /* nothing kept across a selection */
    g_idle[slot].quiet = g_idle[slot].on = 0;
    uint32_t bytes = 0;
    Instance *p = static_cast<Instance *>(coop_fx_scratch(slot, &bytes));
    g_inst[slot] = (p && bytes >= sizeof(Instance)) ? p : nullptr;
    if (!g_inst[slot]) return -1;
    StartClear(*p);
    p->half.Stop();
    return 0;
}

void render(int slot, int32_t *l, int32_t *r, const int16_t *params, int n)
{
    if (slot < 0 || slot >= COOP_FX_SLOTS || !g_inst[slot]) return;   /* not bound: dry */
    Instance &g = *g_inst[slot];
    if (n != kBlock) return;                                   /* the core always gives 64; anything else passes dry */
    IdleState &idle = g_idle[slot];
    if (!g.half.Begin(Cycles())) {                             /* a restart: the hall starts again, cleared */
        idle.quiet = idle.on = 0;
        StartClear(g);
        g.half.End();
        return;
    }
    if (g.clear_at < op1fx::HallReverb::ClearWords()) {        /* still clearing: dry */
        ClearStep(g);
        g.half.End();
        return;
    }
    op1fx::HallParams q;                                       /* the block's controls, read once */
    const Knobs &kn = ReadKnobs(slot, params);
    q.size = kn.q[P_SIZE];
    q.decay = kn.q[P_DECAY];
    q.damp = kn.q[P_DAMP];
    const int32_t mix = kn.q[P_MIX];
    q.predelay = Clamp15(params[5]);
    q.early = Clamp15(params[6]);
    q.width = Clamp15(params[7]);
    q.motion = kMotion;
    q.freeze = (params[COOP_FX_LATCH] & 1) != 0;

    /* Idle: silence in and nothing that holds a sound: the control alone; l and r stay as they came (silence). */
    const bool may_idle = !q.freeze && q.decay < op1fx::kHallInfinite && Silent(l, r);
    if (__builtin_expect(idle.on, 0)) {
        if (may_idle && !IDLE_OFF) {
            g.hall.Idle(q, kHalfN);
            g.half.IdleMix(mix);
            g.half.End();
            IDLE_COUNT();
            return;
        }
        idle.on = idle.quiet = 0;                              /* awake: this block in full */
    }

    int16_t l16[kBlock], r16[kBlock];
    for (int32_t i = 0; i < kBlock; ++i) {                     /* Q31 from the int16 bus: the high half is exact */
        l16[i] = static_cast<int16_t>(l[i] >> 16);
        r16[i] = static_cast<int16_t>(r[i] >> 16);
    }
    g.half.Decimate(l16, r16);
    g.hall.Process(g.half.in(0), g.half.in(1), g.half.wet(0), g.half.wet(1), kHalfN, q);
    if (__builtin_expect(may_idle, 0)) {                       /* a silent block: is the hall silent too? */
        const bool zeros = Zeros(g.half.wet(0), g.half.wet(1));
        IDLE_LEVEL(zeros ? 0 : 1);
        idle.quiet = zeros ? idle.quiet + 1 : 0;
        if (idle.quiet >= kIdleHold && !IDLE_OFF) idle.on = 1;
    } else {
        idle.quiet = 0;
    }
    g.half.Mix(l16, r16, mix);
    for (int32_t i = 0; i < kBlock; ++i) {
        l[i] = static_cast<int32_t>(l16[i]) * 65536;
        r[i] = static_cast<int32_t>(r16[i]) * 65536;
    }
    g.half.End();
}

}  // namespace

extern "C" const coop_fx_t FX_cs_hall = { COOP_FX_MAGIC, COOP_PLUGIN_API, COOP_FX_F_SCREEN, init, render, &SCREEN_cs_hall };
