/* cs_plate: coldsoldr's plate reverb (Dattorro's figure-of-eight plate, te-op1-cfw ports/fx/src/plate) as a coop
 * effect. The kennel build (te-op1-cfw ports/knl/cs_plate/plate_knl.cc, 9ced415, never released) ran the same core
 * behind fx/op1/half_rate.h; this is that glue on coop's ABI (sdk/coop_plugin.h), true stereo at every position.
 *
 *   turns (params[0..3], the filter type's range units, mapped to Q15 as kennel's op1_fx_param_q15 did):
 *     blue decay, green damp, white pre-delay, orange mix
 *   fn + blue   size (params[5]: step 0..7, the plate's 8 tank lengths)
 *   fn + green  diffusion (params[6], Q15; shift + fn + green fine)
 *   fn + white  motion (params[7], Q15; shift + fn + white fine)
 *   orange click freeze (params[8] bit 0, a latch: not saved)
 *   fixed: low cut 3277 (kennel's choice: its orange press couldn't be a page knob)
 *
 * Memory (e8): the plate and the half-rate path (~76 KB) in the slot's position buffer (coop_fx_scratch, plugin.json
 * "scratch"), as kennel's position scratch buffer had them; the file is relocated once and its .bss holds only the five
 * buffer pointers. init only arms a clear: the next renders zero the state 8 KB a block (10 blocks, ~15 ms, dry passed
 * through), then reset it and run (guide/plugin-guidelines.md §2.3: no 76 KB clear on the audio thread in one go).
 * The buffer holds whatever the effect before left, so every selection restarts it this way.
 * Screen (e9): cs_plate_screen.cc, cs_plate.svg; kennel's screen read no DSP state, so the render publishes nothing.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. The half-rate kernels and tables
 * come from te-op1-cfw's Clouds port (MIT, LICENSE-MIT). */
#include <stdint.h>
#include "coop_plugin.h"
#include "half_rate.h"
#include "plate.h"

extern "C" const coop_fx_screen_t SCREEN_cs_plate;   /* cs_plate_screen.cc (e9) */

namespace {

using namespace csplate;

const int32_t kLowCut = 3277;                          /* kennel's fixed low cut */
const int32_t kClearChunk = 2048;                      /* words a block: 8 KB (~2.2 k cycles in the GNU sim) */

struct Instance {
    PlateReverb plate;
    HalfRate half;
};
static_assert(sizeof(Instance) % 4 == 0, "cleared in words");
const int32_t kWords = sizeof(Instance) / 4;

/* e8: a slot's state in its position buffer (sdk/coop_plugin.h coop_fx_scratch, plugin.json "scratch"): the plate
 * (cleared by the renders after init), the block's int16 copies, the clear's progress. Trivial types: no constructor. */
struct Slot {
    Instance in;
    int16_t l[kFxBlockSize], r[kFxBlockSize];
    int32_t cleared;                                   /* words of `in` zeroed since init; kWords + 1 = running */
};
#ifdef COOP_FX_SCRATCH_BYTES
static_assert(sizeof(Slot) <= COOP_FX_SCRATCH_BYTES, "plugin.json scratch: the state must fit");
#endif
Slot* g_slot[COOP_FX_SLOTS];                           /* .bss: each slot's state (bound by init) */

/* The four turns' ranges (sdk/coop_plugin.h): what kennel's op1_fx_param_q15 read from the firmware's table. */
const int16_t kLo[4] = { 64, 0, 0, 8192 };
const int16_t kHi[4] = { 14528, 24576, 22528, 32767 };

inline int32_t Clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* op1_fx_param_q15 with the constant ranges: (v - lo) x 32767 / (hi - lo), clamped. One software divide (libgcc's
 * ___divsi3, ~190 cycles on this CPU), only for a word that moved (Turns below). */
inline int32_t Turn(int32_t word, int k) {
    return Clamp((word - kLo[k]) * 32767 / (kHi[k] - kLo[k]), 0, 32767);
}

/* 0.3: the four turns as the DSP takes them, kept per slot and redone only for a word that moved (four divides a block
 * before: docs/efficiency-audit.md finding 8). The same values, so the same output bit for bit. Tiny per-slot values
 * in .bss (guidelines 2.3); init clears `valid`, so the first render after a selection computes all four. */
struct Turns {
    int16_t raw[4];                                    /* params[0..3] as last seen */
    int32_t valid;
    int32_t q[4];                                      /* 0..32767 */
};
Turns g_turns[COOP_FX_SLOTS];

inline const Turns& ReadTurns(int slot, const int16_t* p) {
    Turns& c = g_turns[slot];
    const bool all = !c.valid;
    for (int k = 0; k < 4; ++k) {
        if (all || p[k] != c.raw[k]) {
            c.raw[k] = p[k];
            c.q[k] = Turn(p[k], k);
        }
    }
    c.valid = 1;
    return c;
}

/* The idle path (0.3; docs/efficiency-audit.md finding 6, README "Idle"). A plate left on a position cost its whole
 * block on silence. Now: while the input block is exact silence (every sample's top 16 bits 0) and freeze is off,
 * each block's check looks at the wet signal the plate just made (64 samples at 22.05 kHz, before the mix).
 * kIdleHold blocks of zeros in a row and the render stops computing: it passes the silent input and runs only the
 * control (PlateReverb::Idle: the smoothers and the modulation LFO, in phase). kIdleHold is the tank's ring once
 * round and the pre-delay's (0.93 s): by then every word the tank's taps, or a later size's, can read was written
 * while the output was zero. The first block with any input, or freeze, is rendered in full (the check is before the
 * DSP: no sample of a new note is lost) on the tank as it was left. What the buffers still hold then is under what
 * the output shows (the allpasses' rounding keeps a +-1 LSB pattern going round for ever; the taps' gain of 0.6
 * rounds it to nothing). An idle plate's silence is therefore the plate's own silence, sample for sample; a note
 * after it meets that pattern where it stopped instead of where it would have turned to, an LSB or so of difference
 * in the tail (measured: README "Idle"). Tiny per-slot values in .bss (guidelines 2.3), cleared by init. */
const int32_t kIdleHold = (PL_TANK_SIZE + PL_PRE_WORDS) / kHalfN + 1;   /* blocks: 640 (0.93 s) */
struct IdleState {
    int32_t quiet;                                     /* silent blocks with a silent plate in a row */
    int32_t on;
};
IdleState g_idle[COOP_FX_SLOTS];

#if defined(COOP_FX_NO_IDLE)                           /* tests/host/cs_plate: the plate against te-op1-cfw's, sample */
#define IDLE_OFF true                                  /* for sample (the idle path has its own test) */
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
__attribute__((noinline)) bool SilentScan(const int32_t* l, const int32_t* r) {
    uint32_t any = 0;
    for (int32_t i = 0; i < kFxBlockSize; ++i) any |= static_cast<uint32_t>(l[i]) | static_cast<uint32_t>(r[i]);
    return (any >> 16) == 0;
}
inline bool Silent(const int32_t* l, const int32_t* r) {
    const uint32_t some = static_cast<uint32_t>(l[0] | r[0] | l[21] | r[42] | l[kFxBlockSize - 1] | r[kFxBlockSize - 1]);
    if (__builtin_expect((some >> 16) != 0, 1)) return false;
    return SilentScan(l, r);
}

/* True when the two half-rate blocks are all zero (a silent block's check: out of line). */
__attribute__((noinline)) bool Zeros(const int16_t* a, const int16_t* b) {
    int32_t any = 0;
    for (int32_t i = 0; i < kHalfN; ++i) any |= a[i] | b[i];
    return any == 0;
}

int init(int slot) {
    if (slot < 0 || slot >= COOP_FX_SLOTS) return -1;
    g_turns[slot].valid = 0;                           /* nothing kept across a selection */
    g_idle[slot].quiet = g_idle[slot].on = 0;
    uint32_t bytes = 0;
    Slot* s = static_cast<Slot*>(coop_fx_scratch(slot, &bytes));
    g_slot[slot] = (s && bytes >= sizeof(Slot)) ? s : nullptr;
    if (!g_slot[slot]) return -1;
    s->cleared = 0;
    return 0;
}

void render(int slot, int32_t* l, int32_t* r, const int16_t* params, int n) {
    if (slot < 0 || slot >= COOP_FX_SLOTS || !g_slot[slot]) return;   /* not bound: dry */
    Slot& s = *g_slot[slot];
    Instance& g_in = s.in;
    int16_t* const g_l = s.l;
    int16_t* const g_r = s.r;
    int32_t& g_cleared = s.cleared;
    if (n != kFxBlockSize) return;                    /* coop's block is 64 (§2.2); anything else passes dry */
    if (g_cleared < kWords) {                          /* restarting: clear a chunk, the input passes */
        uint32_t* w = reinterpret_cast<uint32_t*>(&g_in) + g_cleared;
        const int32_t m = kWords - g_cleared < kClearChunk ? kWords - g_cleared : kClearChunk;
        for (int32_t i = 0; i < m; ++i) w[i] = 0;
        g_cleared += m;
        return;
    }
    if (g_cleared == kWords) {                         /* cleared: the start state (HalfRate's is all zero) */
        g_in.plate.Reset();
        g_cleared = kWords + 1;
    }

    PlateParams q;                                     /* read once a block, clamped (docs/plugins.md e6) */
    const Turns& tn = ReadTurns(slot, params);
    q.decay = tn.q[0];
    q.damp = tn.q[1];
    q.predelay = tn.q[2];
    const int32_t mix = tn.q[3];
    q.size = Clamp(params[5], 0, kPlateNumSizes - 1);
    q.diffusion = Clamp(params[6], 0, 32767);
    q.motion = Clamp(params[7], 0, 32767);
    q.lowcut = kLowCut;
    q.freeze = (params[COOP_FX_LATCH] & 1) != 0;

    /* Idle: silence in and no freeze: the control alone; l and r stay as they came (silence). */
    HalfRate& h = g_in.half;
    IdleState& idle = g_idle[slot];
    const bool may_idle = !q.freeze && Silent(l, r);
    if (__builtin_expect(idle.on, 0)) {
        if (may_idle && !IDLE_OFF) {
            g_in.plate.Idle(q, kHalfN);
            h.IdleMix(mix);
            IDLE_COUNT();
            return;
        }
        idle.on = idle.quiet = 0;                      /* awake: this block in full */
    }

    for (int32_t i = 0; i < kFxBlockSize; ++i) {       /* the bus is int16 << 16 */
        g_l[i] = static_cast<int16_t>(l[i] >> 16);
        g_r[i] = static_cast<int16_t>(r[i] >> 16);
    }
    h.Decimate(g_l, g_r);
    g_in.plate.Process(h.in(0), h.in(1), h.wet(0), h.wet(1), kHalfN, q);
    if (__builtin_expect(may_idle, 0)) {               /* a silent block: is the plate silent too? */
        const bool zeros = Zeros(h.wet(0), h.wet(1));
        IDLE_LEVEL(zeros ? 0 : 1);
        idle.quiet = zeros ? idle.quiet + 1 : 0;
        if (idle.quiet >= kIdleHold && !IDLE_OFF) idle.on = 1;
    } else {
        idle.quiet = 0;
    }
    h.Mix(g_l, g_r, mix);
    for (int32_t i = 0; i < kFxBlockSize; ++i) {
        l[i] = static_cast<int32_t>(g_l[i]) << 16;
        r[i] = static_cast<int32_t>(g_r[i]) << 16;
    }
}

}  // namespace

extern "C" const coop_fx_t FX_cs_plate = { COOP_FX_MAGIC, COOP_PLUGIN_API, COOP_FX_F_SCREEN, init, render, &SCREEN_cs_plate };
