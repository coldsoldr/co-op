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
    if (!g->half.Begin(Cycles())) {
        petri::Reset(&g->eng);
        g->clear = 0;
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

    int16_t *dl = g->dry[0], *dr = g->dry[1];
    int16_t *il = g->half.in_full(0), *ir = g->half.in_full(1);
    for (int32_t i = 0; i < kBlock; ++i) {
        const int16_t a = static_cast<int16_t>(l[i] >> 16), b = static_cast<int16_t>(r[i] >> 16);
        dl[i] = il[i] = a;
        dr[i] = ir[i] = b;
    }
    g->half.Decimate();
    petri::Process(&g->eng, g->half.in(0), g->half.in(1), g->half.wet(0), g->half.wet(1), c);
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
