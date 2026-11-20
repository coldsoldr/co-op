/* tape_echo: te-op1-cfw's TAPE ECHO (a Space Echo style three-head tape delay, wow and flutter after Mutable
 * Instruments Beads) as a coop effect. The kennel plugin was ports/knl/tape_echo (tape_echo_knl.cc, 5aa314b); its
 * DSP is copied into src/ (src/tape_echo_core.cc, src/half_rate.h, src/halfband.cc and the two .S kernels), stripped of
 * the kennel-only parts. Port notes, the controls and the cost: README.md.
 *
 * Per block (64 samples at 44.1 kHz, stereo at every position, guidelines §2.2): the Q31 bus to int16 (the bus is
 * the int16 bus widened by 16 bits), 2:1 down to 22.05 kHz (Clouds' halfband), the input gain on what goes to the
 * record head, the echo core in stereo (L and R share each tape read), back up, the equal-power mix with the dry
 * input and the soft limit, back to Q31.
 *
 * Controls (plugin.json "controls", sdk/coop_plugin.h e6), read once at the block start:
 *   params[0..3]  time, feedback, age, mix: the four turns, in the filter type's range units; normalised to 0..32767
 *                 as kennel's op1_fx_param_q15 did (the same ranges, the same rounding).
 *   params[5]     gain (fn + blue, shift + fn + blue its fine): Q15, -6 dB at 0, 0 dB at 16384, +6 dB at 32767.
 *   params[6]     sync (fn + green): step 0 off (the blue turn sets the time), 1..11 a tempo division (1/16T .. 1/2,
 *                 kennel's tempo lock): the time follows the division at the project tempo (the core's export
 *                 coop_tempo_bpm_q16, read once a block; docs/request-tempo-export-tape-echo.md). A division over 1.5 s
 *                 at the tempo falls back to the turn.
 *   params[7]     the head pattern (white click): step 0..3.
 *   params[8]     bit 0: hold (green click, a latch).
 *
 * Memory (e8): the instance is in the slot's position buffer (sdk/coop_plugin.h coop_fx_scratch, plugin.json
 * "scratch"), as kennel's position scratch buffer had it; the file is relocated once and its .bss holds only the five
 * buffer pointers. The tape is 132,868 bytes. Its clear is spread over the first blocks after a (re)start
 * (kClearPairs a block, the dry passing through meanwhile) instead of 133 KB of stores in init, which runs in the
 * audio interrupt at the stock positions (guidelines §2.3). Every selection restarts it so: the buffer holds whatever
 * the effect before left.
 * Screen (e9): tape_echo_screen.cc, tape_echo.svg; the render publishes the time word in force, the sync division in
 * force and the 96th note's length in the state (state.h Display), once a block.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. The DSP in src/ is te-op1-cfw's
 * (GPL-3.0-or-later), with Mutable Instruments Beads' wow and flutter and Clouds' output stage (Emilie Gillet, MIT,
 * LICENSE-MIT). */
#include <stdint.h>

#include "coop_plugin.h"
#include "state.h"

extern "C" const coop_fx_screen_t SCREEN_tape_echo;    /* tape_echo_screen.cc (e9) */

#if defined(__bfin__) && !defined(COOP_TAPE_ECHO_HOST_CLOCK)
static inline uint32_t Cycles()
{
    uint32_t c;
    __asm__ __volatile__("%0 = CYCLES;" : "=d"(c));
    return c;
}
#else
extern uint32_t g_tape_echo_host_cycles;               /* the host test (tests/host/tape_echo) steps it */
static inline uint32_t Cycles() { return g_tape_echo_host_cycles; }
#endif

namespace {

using coop_tape_echo::kBlock;
using coop_tape_echo::kHalfN;
const int32_t kClearPairs = 2048;                      /* tape pairs zeroed a block: 17 blocks (25 ms) for all */

/* The filter type's knob ranges (sdk/coop_plugin.h): what kennel's op1_fx_param_q15 read from the stock table. */
const int16_t kRangeLo[4] = { 64, 0, 0, 8192 };
const int16_t kRangeHi[4] = { 14528, 24576, 22528, 32767 };

/* Sync: kennel's tempo divisions (tape_echo_knl.cc), shortest first, in 96th notes. */
const int32_t kNumDivisions = 11;                      /* 1/16T 1/16 1/8T 1/16. 1/8 1/4T 1/8. 1/4 1/2T 1/4. 1/2 */
const int32_t kDivision96ths[kNumDivisions] = { 4, 6, 8, 9, 12, 16, 18, 24, 32, 36, 48 };

/* A 96th note in Q4 samples at 22050 Hz at the project tempo: the core's export (e10, sdk/coop_plugin.h; 120 << 16 when
 * the tempo is unknown, kennel's fallback, guidelines §2.3), through state.h NinetySixthQ4. Kennel read the stock LFO's
 * tempo increment at 0x01AADAD4 instead (a firmware address, §2.1). Called once a block. */
inline int32_t NinetySixthQ4() { return coop_tape_echo::NinetySixthQ4(coop_tempo_bpm_q16()); }

/* The render's: the same, the division redone only when the tempo moved (one word compare a block otherwise). The two
 * words are .bss shared by the slots, which is right (one tempo), and only the audio interrupt touches them: init (which
 * may run on the main loop) and the screen call NinetySixthQ4 above. */
uint32_t g_tempo_q16;                                  /* 0: nothing yet */
int32_t g_tempo_s96;
inline int32_t RenderNinetySixthQ4()
{
    const uint32_t q16 = coop_tempo_bpm_q16();
    if (q16 != g_tempo_q16) {
        g_tempo_s96 = coop_tape_echo::NinetySixthQ4(q16);
        g_tempo_q16 = q16;
    }
    return g_tempo_s96;
}

/* The time word sync gives (step 1..11), or the turn's word when off or when the division is out of range; *div the
 * division in force (0..10), -1 when the turn's word is used (the screen's display word). */
inline int32_t SyncedTime(int32_t sync, int32_t knob_word, int32_t s96, int32_t *div, int32_t *kept_sync,
                          int32_t *kept_s96, int32_t *kept_word)
{
    *div = -1;
    if (sync < 1 || sync > kNumDivisions) return knob_word;
    if (sync != *kept_sync || s96 != *kept_s96) {      /* a new division or tempo: the word again (0.4) */
        *kept_word = op1fx::TapeEchoTimeWord(s96 * kDivision96ths[sync - 1]);
        *kept_sync = sync;
        *kept_s96 = s96;
    }
    const int32_t word = *kept_word;
    if (word < 0) return knob_word;
    *div = sync - 1;
    return word;
}

typedef coop_tape_echo::Instance Instance;

/* The idle path (0.4; docs/efficiency-audit.md finding 6, README "Idle"). An echo left on a position cost its whole
 * block on silence. Now: while the input block is exact silence (every sample's top 16 bits 0), hold is off and the
 * feedback turn is under kIdleFeedback, each block's check looks at what the echo just wrote to the tape; kIdleHold
 * blocks of zeros in a row (the tape once round and a margin: nothing is left anywhere on it, whatever the time or
 * the heads are set to next) and the render stops computing. It passes the silent input and runs only the control
 * (TapeEcho::Idle: the time's glide, the smoothers, the wow and flutter in phase, the mix's smoothing), so a note
 * comes back into an echo that is where it would have been. The first block with any input, hold, or the feedback
 * turn at kIdleFeedback or over, is rendered in full (the check is before the DSP: no sample of a new note is lost).
 * A tape of zeros gives zeros, so the idle blocks are what the echo would have given; what is not carried on is the
 * two filters' last sub-LSB state. A tail the loop's rounding holds for ever (high feedback on a new tape: a ghost
 * of a few LSB) never passes the check: such an echo stays awake, as before.
 * Tiny per-slot values in .bss (guidelines 2.3), cleared by init and by a restart. */
const int32_t kIdleHold = TE_TAPE_SIZE / kHalfN + 9;   /* blocks: 1,047 (1.52 s), the tape once round is 1,038 */
const int32_t kIdleFeedback = 29000;                   /* the turn, Q15: 0.97 (1.0 is 29789); from here it never idles */
struct IdleState {
    int32_t quiet;                                     /* silent, quiet blocks in a row */
    int32_t on;
};
IdleState g_idle[COOP_FX_SLOTS];

#if defined(COOP_FX_NO_IDLE)                           /* tests/host/tape_echo: the echo against te-op1-cfw's, sample */
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

#ifdef COOP_FX_SCRATCH_BYTES
static_assert(sizeof(Instance) <= COOP_FX_SCRATCH_BYTES, "plugin.json scratch: the state must fit");
#endif
Instance *g_inst[COOP_FX_SLOTS];                       /* .bss: each slot's state, in its position buffer (e8) */

inline int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }

/* A turn's word in the filter range -> 0..32767 (kennel's op1_fx_param_q15). One software divide (libgcc's
 * ___divsi3, ~190 cycles on this CPU): only when the word moved (Knobs below). */
inline int32_t Knob(int32_t word, int k)
{
    const int32_t lo = kRangeLo[k], hi = kRangeHi[k];
    const int32_t v = (word - lo) * 32767 / (hi - lo);
    return Clamp15(v);
}

/* The input gain word as a Q14 factor: -6 dB at 0, 0 dB at the centre, +6 dB at the top (10^(dB / 20) x 16384, one
 * table entry per dB, interpolated): kennel's knl::GainQ14 (ports/knl/common/knl_fx.h). Two divides: likewise. */
inline int32_t GainQ14(int32_t word)
{
    static const int32_t kTable[13] = { 8211, 9215, 10338, 11600, 13016, 14605, 16384, 18382, 20626, 23143, 25966,
                                        29135, 32690 };
    const int32_t pos = Clamp15(word) * 12;
    const int32_t i = pos / 32767;
    const int32_t frac = pos - i * 32767;
    if (i >= 12) return kTable[12];
    return kTable[i] + (((kTable[i + 1] - kTable[i]) * frac) / 32767);
}

/* 0.4: what the block's control words give the DSP, kept per slot and redone only for a word that moved: the four
 * turns and the gain (six divides a block before), and the sync division's time word (TapeEchoTimeWord: a table walk
 * and a divide, every block while sync was on). The same values, so the same output bit for bit
 * (docs/efficiency-audit.md finding 8). Tiny per-slot values in .bss (guidelines 2.3); init clears `valid`, so the
 * first render after a selection computes everything. */
struct Knobs {
    int16_t raw[5];                                    /* params[0..3], params[5] as last seen */
    int16_t valid;
    int32_t q[4];                                      /* the turns, 0..32767 */
    int32_t gain;                                      /* GainQ14(params[5]) */
    int32_t sync, s96, word;                           /* the sync step and the 96th note `word` was made from */
};
Knobs g_knobs[COOP_FX_SLOTS];

inline Knobs &ReadKnobs(int slot, const int16_t *p)
{
    Knobs &c = g_knobs[slot];
    const bool all = !c.valid;
    for (int k = 0; k < 4; ++k) {
        if (all || p[k] != c.raw[k]) {
            c.raw[k] = p[k];
            c.q[k] = Knob(p[k], k);
        }
    }
    if (all || p[5] != c.raw[4]) {
        c.raw[4] = p[5];
        c.gain = GainQ14(p[5]);
    }
    if (all) c.sync = 0;                               /* no division's word kept */
    c.valid = 1;
    return c;
}

/* e9: the display words (state.h Display), one store each, once a block after the DSP; the DSP never reads them. */
inline void Publish(Instance *g, int32_t time, int32_t div, int32_t s96)
{
    g->disp.time = time;
    g->disp.div = div;
    g->disp.beat = s96;
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
    g->half.Stop();                                     /* the next render restarts: core reset, tape cleared */
    g->disp.time = -1;                                  /* e9: nothing published yet (the screen shows the turn) */
    g->disp.div = -1;
    g->disp.beat = NinetySixthQ4();
    return 0;
}

void render(int slot, int32_t *l, int32_t *r, const int16_t *params, int n)
{
    if (slot < 0 || slot >= COOP_FX_SLOTS || !g_inst[slot]) return;   /* not bound: dry */
    Instance *const g = g_inst[slot];
    /* The time in force (the sync division's or the turn's): the echo's and the screen's (published at every exit). */
    Knobs &kn = ReadKnobs(slot, params);
    const int32_t s96 = RenderNinetySixthQ4();
    int32_t div;
    const int32_t time = SyncedTime(params[6], kn.q[0], s96, &div, &kn.sync, &kn.s96, &kn.word);
    if (n != kBlock) {                                  /* the host's block is 64; anything else passes through */
        Publish(g, time, div, s96);
        return;
    }
    IdleState &idle = g_idle[slot];
    if (!g->half.Begin(Cycles())) {
        g->echo.Init();
        g->clear = 0;
        idle.quiet = idle.on = 0;
    }
    if (g->clear < TE_TAPE_PAIRS) {                     /* starting: clear a slice, the dry passes through */
        g->clear = g->echo.ClearTape(g->clear, kClearPairs);
        g->half.End(Cycles());
        Publish(g, time, div, s96);
        return;
    }

    op1fx::TapeEchoParams q;
    q.time = time;
    q.feedback = kn.q[1];
    q.age = kn.q[2];
    const int32_t mix = kn.q[3];
    int32_t pattern = params[7];
    q.heads = pattern < 0 ? 0 : (pattern >= op1fx::kTapeEchoNumPatterns ? op1fx::kTapeEchoNumPatterns - 1 : pattern);
    q.hold = (params[COOP_FX_LATCH] & 1) != 0;
    const int32_t gain = kn.gain;

    /* Idle: silence in and nothing that holds a sound: the control alone; l and r stay as they came (silence). */
    const bool may_idle = !q.hold && q.feedback < kIdleFeedback && Silent(l, r);
    if (__builtin_expect(idle.on, 0)) {
        if (may_idle && !IDLE_OFF) {
            g->echo.Idle(q, kHalfN);
            g->half.IdleMix(mix);
            g->half.End(Cycles());
            Publish(g, time, div, s96);
            IDLE_COUNT();
            return;
        }
        idle.on = idle.quiet = 0;                       /* awake: this block in full */
    }

    /* The bus to int16, into the decimators' buffers and the dry copy the mix works on. */
    int16_t *dl = g->dry[0], *dr = g->dry[1];
    int16_t *il = g->half.in_full(0), *ir = g->half.in_full(1);
    for (int32_t i = 0; i < kBlock; ++i) {
        const int16_t a = static_cast<int16_t>(l[i] >> 16), b = static_cast<int16_t>(r[i] >> 16);
        dl[i] = il[i] = a;
        dr[i] = ir[i] = b;
    }
    g->half.Decimate();

    /* The input gain: what reaches the record head, saturating (the dry side of the mix is the input as it came). */
    for (int32_t c = 0; c < 2; ++c) {
        const int16_t *src = g->half.in(c);
        int16_t *dst = g->gained[c];
        for (int32_t i = 0; i < kHalfN; ++i) {
            int32_t y = (src[i] * gain) >> 14;
            if (y > 32767) y = 32767;
            if (y < -32768) y = -32768;
            dst[i] = static_cast<int16_t>(y);
        }
    }
    g->echo.Process(g->gained[0], g->gained[1], g->half.wet(0), g->half.wet(1), kHalfN, q);
    if (__builtin_expect(may_idle, 0)) {                /* a silent block: did the echo write silence too? */
        const bool zeros = g->echo.WroteSilence(kHalfN);
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
    Publish(g, time, div, s96);
}

}  // namespace

extern "C" const coop_fx_t FX_tape_echo = { COOP_FX_MAGIC, COOP_PLUGIN_API, COOP_FX_F_SCREEN, init, render,
                                             &SCREEN_tape_echo };
