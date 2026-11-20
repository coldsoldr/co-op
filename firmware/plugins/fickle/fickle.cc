/* fickle: two processors that feed each other, sharing one clock (after the signal flow of a two-channel micro-looper
 * pedal). The input splits three ways: the dry path (untouched, to the mix), the routing switch into the WET CHANNEL
 * (reverb, delay or slip, with a freeze), and the MICRO-LOOPER, always listening: while its latch is off it records
 * the most recent audio (the input plus the wet channel's output) round a circular buffer; the latch on plays back the
 * last loop's worth, the dub latch overdubs the input onto it. Both channels run at the CLOCK, one sample rate from
 * 32 kHz down to 2 kHz: lower is longer, darker and grittier, and recorded material changes pitch as the clock moves,
 * as tape. README.md has the contract, the figures and the tests.
 *
 * Per block (64 samples at 44.1 kHz, stereo at every position, guidelines §2.2), in passes over the block:
 *   A  the input to int16, a gentle one-pole pre-filter (0.42 x the clock: it aliases at low clocks, on purpose), the
 *      clock's phase accumulator (Q16, no divide): where it wraps a clock sample is taken (0 or 1 a 44.1 kHz sample)
 *   B  the looper's read: the loop's output for each clock sample (tape speeds, reverse, ping-pong, the envelope
 *      retrigger, the two-grain stretch)
 *   C  the routing (what the wet channel hears: the input, input + loop or the loop, while the loop plays) and the wet
 *      channel's mode
 *   D  the looper's write: listening records input + wet; dubbing adds the input (never the wet) at the dub head and
 *      fades the old material a little each pass
 *   E  back to 44.1 kHz: the clock samples held and smoothed by two one-poles (0.45 x the clock: the gentle anti-image),
 *      the equal-power mix with the dry input, the soft limit, back to Q31
 * Every product in the per-sample loops is 16 x 16 (M16: one multiply on the Blackfin; the operands are kept in int16
 * range by construction or saturated first), never a 32 x 32 one (docs/plugin-performance.md).
 *
 * Controls (plugin.json "controls", sdk/coop_plugin.h e6), read once at the block start:
 *   params[0..3]  time, modify, clock, mix: the four turns, in the filter type's range units, normalised to 0..32767
 *   params[5]     the wet mode (fn + blue): 0 reverb, 1 delay, 2 slip
 *   params[6]     routing (fn + green): 0 in, 1 both, 2 loop
 *   params[7]     the loop mode (fn + white): LoopMode in state.h
 *   params[8]     bit 0 freeze (orange click), bit 1 loop (white click), bit 2 dub (green click): latches
 *
 * Memory (e8): the state is in the slot's position buffer (plugin.json "scratch"); the file's .bss holds only the five
 * pointers. init resets every small field and binds; the wet line and the diffusers (69 KB) are cleared over the first
 * five blocks with the dry passing; the loop buffer is never cleared: the looper only reads what it recorded.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. */
#include <stdint.h>

#include "coop_plugin.h"
#include "fickle_tables.h"
#include "state.h"

extern "C" const coop_fx_screen_t SCREEN_fickle;      /* fickle_screen.cc (e9) */

#if defined(FICKLE_PROFILE) && defined(__bfin__)
/* The host test's simulator build only (tests/host/fickle "prof"): the cycles of each pass, summed. */
extern "C" uint32_t g_fickle_prof[8];
#define PROF_START                                                                                                    \
    uint32_t prof_t;                                                                                                  \
    __asm__ __volatile__("%0 = CYCLES;" : "=d"(prof_t))
#define PROF(i)                                                                                                       \
    do {                                                                                                              \
        uint32_t c_;                                                                                                  \
        __asm__ __volatile__("%0 = CYCLES;" : "=d"(c_));                                                              \
        g_fickle_prof[i] += c_ - prof_t;                                                                              \
        prof_t = c_;                                                                                                  \
    } while (0)
#else
#define PROF_START do {} while (0)
#define PROF(i) do {} while (0)
#endif

namespace {

using namespace fickle;

#ifdef COOP_FX_SCRATCH_BYTES
static_assert(sizeof(State) <= COOP_FX_SCRATCH_BYTES, "plugin.json scratch: the state must fit");
#endif
State *g_st[COOP_FX_SLOTS];                            /* .bss: each slot's state, in its position buffer (e8) */

const int32_t kClearWords = 4096;                      /* wet + diffuser words zeroed a block: 5 blocks (7 ms) */
const int32_t kGrain = 2048, kGrainHalf = 1024;        /* stretch: the grains, clock samples */
const int32_t kXfJump = 64;                            /* a loop jump's crossfade, clock samples (a power of two) */
const int32_t kXfDelay = 256;                          /* delay: a time change's crossfade (a power of two) */
const int32_t kXfSlip = 256;                           /* slip: the refresh's crossfade (gen_fickle_tables.py SLIP_X) */
const int32_t kEnvThrSoft = 1200, kEnvThrHard = 3000;  /* env: the trigger thresholds (-29 / -21 dBFS) */
const int32_t kDubKeep = 30146;
const int32_t kBakeStep = 128;                         /* seam pairs crossfaded a block (BakeSeam) */                        /* dub: the old material x 0.92 a pass */
const int32_t kUnity = 32767;                          /* Q15 "1" for the 16 x 16 products */
const int32_t kRecGain = 23170;                        /* -3 dB: the input and the wet as the looper records them, and
                                                        * the loop as it joins the wet in the output */

/* The filter type's knob ranges (sdk/coop_plugin.h), as the other ports normalise them. */
const int16_t kRangeLo[4] = { 64, 0, 0, 8192 };
const int16_t kRangeHi[4] = { 14528, 24576, 22528, 32767 };

/* The loop modes' speeds (Q16 a clock sample; the stretch modes' scan speed). */
const int32_t kLoopSpeed[L_MODES] = { 65536, 131072, 32768, 98193, -65536, -131072, -32768, 65536,
                                      65536, 65536, 32768, 8192, 32768 };

/* The reverb's diffusers: delay and ring (each a power of two, at kApOff* in ap[]). */
const int32_t kAp0 = 113, kAp1 = 167, kAp2 = 229, kAp3 = 311;
const int32_t kApOff1 = 128, kApOff2 = 384, kApOff3 = 640;

inline int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }
inline int32_t Sat16(int32_t x) { return x > 32767 ? 32767 : (x < -32768 ? -32768 : x); }
inline int32_t Max(int32_t a, int32_t b) { return a > b ? a : b; }
inline int32_t Min(int32_t a, int32_t b) { return a < b ? a : b; }

/* a x b, both taken as int16: one 16 x 16 multiply. On the Blackfin through the instruction itself: GCC drops the int16
 * casts once it can prove the range (after a saturation) and then emits the multi-cycle 32 x 32 multiply. */
inline int32_t M16(int32_t a, int32_t b)
{
#ifdef __bfin__
    int32_t p;
    __asm__("%0 = %1.L * %2.L (IS);" : "=d"(p) : "d"(a), "d"(b));
    return p;
#else
    return static_cast<int32_t>(static_cast<int16_t>(a)) * static_cast<int32_t>(static_cast<int16_t>(b));
#endif
}

/* (a - b) saturated to int16, times g: a and b int16. Two instructions on the Blackfin (the 16-bit saturating
 * subtract, the multiply): a one-pole's step without a 17-bit difference. */
inline int32_t MulSub(int32_t a, int32_t b, int32_t g)
{
#ifdef __bfin__
    int32_t p;
    __asm__("%0.L = %1.L - %2.L (S); %0 = %0.L * %3.L (IS);" : "=&d"(p) : "d"(a), "d"(b), "d"(g));
    return p;
#else
    return M16(Sat16(a - b), g);
#endif
}

/* A soft knee: unity to 24576, a parabola to full scale at 40960, flat above (no divide). */
inline int32_t Soft(int32_t x)
{
    const int32_t a = x < 0 ? -x : x;
    if (__builtin_expect(a <= 24576, 1)) return x;     /* the common case falls through (no taken branch) */
    const int32_t t = Min(a - 24576, 16384);
    const int32_t y = Min(a - (M16(t, t) >> 15), 32767);
    return x < 0 ? -y : y;
}

inline uint32_t Pack(int32_t l, int32_t r)
{
    return static_cast<uint32_t>(static_cast<uint16_t>(Sat16(l))) | (static_cast<uint32_t>(Sat16(r)) << 16);
}
/* Pack for values already in int16 range (saturated or through Soft). */
inline uint32_t PackRaw(int32_t l, int32_t r)
{
    return (static_cast<uint32_t>(l) & 0xffffu) | (static_cast<uint32_t>(r) << 16);
}
inline int32_t Lo(uint32_t w) { return static_cast<int16_t>(w); }
inline int32_t Hi(uint32_t w) { return static_cast<int32_t>(w) >> 16; }

/* (x * g) >> 15 truncated towards zero (x, g int16): a feedback path's small values decay to 0 instead of sticking at
 * -1 (the floor of an arithmetic shift). */
inline int32_t MulZ(int32_t x, int32_t g)
{
    const int32_t p = M16(x, g);
    return (p + ((p >> 31) & 32767)) >> 15;
}

/* a + (b - a) x f, f Q15 (0..32767), a and b int16. */
inline int32_t Lerp(int32_t a, int32_t b, int32_t f) { return a + (MulSub(b, a, f) >> 15); }

/* Towards target by at most step. */
inline int32_t Ramp(int32_t v, int32_t target, int32_t step)
{
    return v < target ? Min(v + step, target) : Max(v - step, target);
}

/* A turn's word in the filter range -> 0..32767. One software divide (libgcc's ___divsi3, ~190 cycles on this CPU),
 * only for a word that moved (Kept below). */
inline int32_t Knob(int32_t word, int k)
{
    const int32_t lo = kRangeLo[k], hi = kRangeHi[k];
    return Clamp15((word - lo) * 32767 / (hi - lo));
}

/* 0.3: what the block's controls give the DSP, kept per slot and redone only when their inputs moved: the four turns
 * (four divides a block before) and the reverb's decay (ReverbDecay: two more, every block in the reverb mode;
 * docs/efficiency-audit.md finding 8). The same values, so the same output bit for bit. Tiny per-slot values in .bss
 * (guidelines 2.3: the state has no room, it is sized to the position buffers' 134,784 bytes); init clears `valid`, so
 * the first render after a selection computes everything. */
struct Kept {
    int16_t raw[4];                                    /* params[0..3] as last seen */
    int32_t valid;
    int32_t q[4];                                      /* the turns, 0..32767 */
    int32_t rv_size, rv_time, rv_decay;                /* ReverbDecay(rv_size, rv_time); rv_size 0: nothing kept */
};
Kept g_kept[COOP_FX_SLOTS];

inline Kept &ReadKnobs(int slot, const int16_t *p)
{
    Kept &c = g_kept[slot];
    const bool all = !c.valid;
    for (int k = 0; k < 4; ++k) {
        if (all || p[k] != c.raw[k]) {
            c.raw[k] = p[k];
            c.q[k] = Knob(p[k], k);
        }
    }
    if (all) c.rv_size = 0;                            /* a size is at least 512 */
    c.valid = 1;
    return c;
}

inline int32_t ClampStep(int32_t v, int32_t n) { return v < 0 ? 0 : (v >= n ? n - 1 : v); }

/* 2^y x 32768 for y (Q16) in [0, 15) (per block). */
inline int32_t Exp2Q15(int32_t y)
{
    const int32_t i = y >> 16, fr = y & 0xffff, j = fr >> 11, t = fr & 2047;
    const int32_t v = kExp2Frac[j] + (((kExp2Frac[j + 1] - kExp2Frac[j]) * t) >> 11);
    return v << i;
}

/* A 33-entry table over the clock knob, interpolated (per block). */
inline int32_t ClockTable(const int32_t *t, int32_t c)
{
    const int32_t i = c >> 10, f = c & 1023;
    return t[i] + (((t[i + 1] - t[i]) * f) >> 10);
}

inline int32_t QuarterSin(int32_t m)
{
    const int32_t i = m >> 11, f = m & 2047;
    return kQuarterSin[i] + (((kQuarterSin[i + 1] - kQuarterSin[i]) * f) >> 11);
}

inline int32_t WrapL(int32_t i) { return i >= kLoopPairs ? i - kLoopPairs : (i < 0 ? i + kLoopPairs : i); }

inline bool IsRev(int32_t m) { return m == L_REV1 || m == L_REV2 || m == L_REV_HALF; }
inline bool IsStretch(int32_t m) { return m >= L_STRETCH; }

/* The peak of |l|, |r| over n samples. */
inline int32_t PeakOf(const int32_t *l, const int32_t *r, int32_t n)
{
    int32_t hi = 0, lo = 0;
    for (int32_t k = 0; k < n; ++k) {
        hi = Max(hi, Max(l[k], r[k]));
        lo = Min(lo, Min(l[k], r[k]));
    }
    return Max(hi, -lo);
}

/* ---- the micro-looper ---- */

/* The loop mode's entry state at the read position p. */
void LoopModeEnter(State *s, int32_t m)
{
    s->lmode = m;
    s->dir = 1;
    s->xcount = 0;
    s->armed = 1;
    s->holdoff = 0;
    s->slice = 0;
    s->gate = 1 << 30;
    s->envf = 0;
    const int32_t at = s->len > 0 ? (s->p >> 16) : 0;
    s->scan = s->p;
    s->g[0].pos = at;
    s->g[0].age = 0;
    s->g[1].pos = at;
    s->g[1].age = kGrainHalf;
}

/* The latch went on: the last len clock samples become the loop; its end crossfades into the audio before its start
 * (kSeam samples), so the loop wraps without a click in either direction. Once, a few thousand cycles. */
void Capture(State *s)
{
    int32_t len = s->filled - kSeam;
    if (len > kLoopPairs - kLoopMargin) len = kLoopPairs - kLoopMargin;
    s->len = len;
    s->start = WrapL(s->w - len);
    s->bake = kSeam;
    s->p = IsRev(s->lmode) ? (len << 16) - 1 : 0;
    LoopModeEnter(s, s->lmode);
    for (int32_t i = 0; i < kEnvBins; ++i) s->env[i] = 0;
    s->ebin = -1;
    s->gain = 0;
    s->dubg = 0;
    s->lstate = S_PLAY;
}

/* The seam's crossfade, kBakeStep pairs a block from the loop's end backwards (the capture block and the next: a
 * reverse head at x2 reads 94 a block from the end, under the 128 baked ahead of it): pair i of the last kSeam fades
 * towards the pair kSeam before the loop's start, i / kSeam of the way. */
void BakeSeam(State *s)
{
    uint32_t *b = s->loop;
    const int32_t hi = s->bake, lo = Max(hi - kBakeStep, 0);
    for (int32_t i = lo; i < hi; ++i) {
        const int32_t e = WrapL(s->start + s->len - kSeam + i), q = WrapL(s->start - kSeam + i);
        const int32_t f = i << 7;                      /* Q15, 0 .. 1 - 1/256 */
        const uint32_t a = b[e], c = b[q];
        b[e] = Pack(Lerp(Lo(a), Lo(c), f), Lerp(Hi(a), Hi(c), f));
    }
    s->bake = lo;
}

/* One interpolated read at p (Q16 in the loop) of the loop at start, length len. */
inline uint32_t ReadAt(const uint32_t *b, int32_t start, int32_t len, int32_t p, int32_t *yl)
{
    const int32_t idx = p >> 16, fr = (p >> 1) & 0x7fff;
    int32_t i0 = start + idx;
    if (i0 >= kLoopPairs) i0 -= kLoopPairs;
    int32_t i1 = i0 + 1;
    if (i1 >= kLoopPairs) i1 = 0;
    if (idx + 1 == len) i1 = start;
    const uint32_t a = b[i0], c = b[i1];
    *yl = Lerp(Lo(a), Lo(c), fr);
    return static_cast<uint32_t>(Lerp(Hi(a), Hi(c), fr));
}

/* The loop's level (its fade in and out, the mode switch's fade) over the block: nothing to do while both are 1. */
inline void LoopLevel(int32_t *ol, int32_t *orr, int32_t ticks, int32_t *gain, int32_t *duck, int32_t gt, int32_t dt)
{
    int32_t g = *gain, d = *duck;
    if (g == kUnity && d == kUnity && gt == kUnity && dt == kUnity) return;
    for (int32_t k = 0; k < ticks; ++k) {
        const int32_t gg = M16(g, d) >> 15;
        ol[k] = M16(ol[k], gg) >> 15;
        orr[k] = M16(orr[k], gg) >> 15;
        g = Ramp(g, gt, 256);
        d = Ramp(d, dt, 512);
    }
    *gain = g;
    *duck = d;
}

/* Pass B: the loop's output for the block's clock samples (S_PLAY and S_STOP). */
void LoopRead(State *s, int32_t ticks)
{
    const uint32_t *b = s->loop;
    const int32_t start = s->start, len = s->len, L = len << 16;
    const int32_t m = s->lmode;
    const int32_t gtarget = s->lstate == S_PLAY ? kUnity : 0;
    const int32_t dtarget = s->lmode == s->lmode_want ? kUnity : 0;
    int32_t gain = s->gain, duck = s->lduck;
    int32_t *ol = s->ol, *orr = s->or_;
    if (IsStretch(m)) {
        /* two grains (2048 clock samples, triangular, half a grain apart: their windows sum to 1) read at the speed
         * (backwards for str rev) from where the scan was when they started; the scan moves at 1/2 or 1/8 */
        const int32_t ssp = kLoopSpeed[m];
        const int32_t gdir = m == L_STR_REV ? -1 : 1;
        int32_t scan = s->scan;
        int32_t p0 = s->g[0].pos, a0 = s->g[0].age, p1 = s->g[1].pos, a1 = s->g[1].age;
        for (int32_t k = 0; k < ticks; ++k) {
            scan += ssp;
            if (scan >= L) scan -= L;
            if (a0 >= kGrain) {
                a0 = 0;
                p0 = scan >> 16;
                if (gdir < 0) p0 = (p0 + kGrain - 1) % len;
            }
            if (a1 >= kGrain) {
                a1 = 0;
                p1 = scan >> 16;
                if (gdir < 0) p1 = (p1 + kGrain - 1) % len;
            }
            int32_t i0 = start + p0, i1 = start + p1;
            if (i0 >= kLoopPairs) i0 -= kLoopPairs;
            if (i1 >= kLoopPairs) i1 -= kLoopPairs;
            const uint32_t v0 = b[i0], v1 = b[i1];
            const int32_t w0 = a0 < kGrainHalf ? a0 : kGrain - a0;   /* 0..1024 */
            const int32_t w1 = kGrainHalf - w0;                      /* the other half of the pair's sum */
            ol[k] = (M16(Lo(v0), w0) + M16(Lo(v1), w1)) >> 10;
            orr[k] = (M16(Hi(v0), w0) + M16(Hi(v1), w1)) >> 10;
            p0 += gdir;
            p1 += gdir;
            if (p0 >= len) p0 = 0;
            else if (p0 < 0) p0 = len - 1;
            if (p1 >= len) p1 = 0;
            else if (p1 < 0) p1 = len - 1;
            ++a0;
            ++a1;
        }
        LoopLevel(ol, orr, ticks, &gain, &duck, gtarget, dtarget);
        s->scan = scan;
        s->p = scan;
        s->g[0].pos = p0;
        s->g[0].age = a0;
        s->g[1].pos = p1;
        s->g[1].age = a1;
    } else if (m == L_ENV_SOFT || m == L_ENV_HARD) {
        /* the input's peak follower; over the threshold (re-armed under half of it) the head jumps, with a 64-sample
         * crossfade: soft to the loop's start, its level following the playing; hard to the next of 8 slices */
        const bool soft = m == L_ENV_SOFT;
        const int32_t thr = soft ? kEnvThrSoft : kEnvThrHard;
        int32_t p = s->p, xp = s->xp, xc = s->xcount;
        int32_t envf = s->envf, armed = s->armed, holdoff = s->holdoff, gate = s->gate;
        const int32_t *xl = s->xl, *xr = s->xr;
        for (int32_t k = 0; k < ticks; ++k) {
            const int32_t a = Max(Max(xl[k], -xl[k]), Max(xr[k], -xr[k]));
            if (a > envf) envf = a;
            else envf -= envf >> 9;
            if (holdoff) holdoff--;
            if (armed && !holdoff && envf > thr) {
                armed = 0;
                holdoff = 1024;
                xp = p;
                xc = kXfJump;
                if (soft) {
                    p = 0;
                } else {
                    s->slice = (s->slice + 1) & 7;
                    p = (s->slice * (len >> 3)) << 16;
                }
                s->trig++;
            } else if (envf < (thr >> 1)) {
                armed = 1;
            }
            int32_t gg = kUnity;
            if (soft) {
                if (envf > (thr >> 1)) gate = 1 << 30;
                else gate -= gate >> 13;
                gg = Min(gate >> 15, kUnity);
            }
            p += 65536;
            if (p >= L) p -= L;
            int32_t yl;
            int32_t yr = static_cast<int32_t>(ReadAt(b, start, len, p, &yl));
            if (xc) {
                xp += 65536;
                if (xp >= L) xp -= L;
                int32_t zl;
                const int32_t zr = static_cast<int32_t>(ReadAt(b, start, len, xp, &zl));
                yl = (M16(yl, kXfJump - xc) + M16(zl, xc)) >> 6;
                yr = (M16(yr, kXfJump - xc) + M16(zr, xc)) >> 6;
                xc--;
            }
            ol[k] = gg == kUnity ? yl : M16(yl, gg) >> 15;
            orr[k] = gg == kUnity ? yr : M16(yr, gg) >> 15;
        }
        LoopLevel(ol, orr, ticks, &gain, &duck, gtarget, dtarget);
        s->p = p;
        s->xp = xp;
        s->xcount = xc;
        s->envf = envf;
        s->armed = armed;
        s->holdoff = holdoff;
        s->gate = gate;
    } else {
        /* tape: a speed through the loop, either way; ping: forwards and back */
        const bool ping = m == L_PING;
        int32_t sp = kLoopSpeed[m];
        if (ping && s->dir < 0) sp = -sp;
        int32_t p = s->p, dir = s->dir;
        for (int32_t k = 0; k < ticks; ++k) {
            p += sp;
            if (p >= L) {
                if (ping) {
                    p = (L - 1) - (p - (L - 1));
                    sp = -sp;
                    dir = -1;
                } else {
                    p -= L;
                }
            } else if (p < 0) {
                if (ping) {
                    p = -p;
                    sp = -sp;
                    dir = 1;
                } else {
                    p += L;
                }
            }
            int32_t yl;
            orr[k] = static_cast<int32_t>(ReadAt(b, start, len, p, &yl));
            ol[k] = yl;
        }
        LoopLevel(ol, orr, ticks, &gain, &duck, gtarget, dtarget);
        s->p = p;
        s->dir = dir;
    }
    s->gain = gain;
    s->lduck = duck;
}

/* Pass D: listening records the input plus the wet channel; dubbing adds the input at the dub head. */
void LoopWrite(State *s, int32_t ticks, bool dub)
{
    uint32_t *b = s->loop;
    const int32_t *xl = s->xl, *xr = s->xr;
    if (s->lstate != S_PLAY) {
        const int32_t *wl = s->wl, *wr = s->wr;
        int32_t w = s->w;
        for (int32_t k = 0; k < ticks; ++k) {
            b[w] = Pack((M16(xl[k], kRecGain) + M16(wl[k], kRecGain)) >> 15,
                        (M16(xr[k], kRecGain) + M16(wr[k], kRecGain)) >> 15);
            if (++w == kLoopPairs) w = 0;
        }
        s->w = w;
        s->filled = Min(s->filled + ticks, kLoopPairs);
        s->dubg = 0;
        return;
    }
    if (dub && s->dubg == 0) s->dubp = (s->p >> 16) % (s->len > 0 ? s->len : 1);   /* once, when the dub starts */
    if (!dub && s->dubg == 0) return;
    const int32_t start = s->start, len = s->len, target = dub ? kUnity : 0;
    int32_t dp = s->dubp, dg = s->dubg;
    int32_t i = start + dp;
    if (i >= kLoopPairs) i -= kLoopPairs;
    if (dg == kUnity && target == kUnity) {            /* steady: the input at full level, the old x 0.92 */
        for (int32_t k = 0; k < ticks; ++k) {
            const uint32_t o = b[i];
            b[i] = PackRaw(Soft(MulZ(Lo(o), kDubKeep) + xl[k]), Soft(MulZ(Hi(o), kDubKeep) + xr[k]));
            if (++i == kLoopPairs) i = 0;
            if (++dp >= len) {
                dp = 0;
                i = start;
            }
        }
        s->dubp = dp;
        return;
    }
    for (int32_t k = 0; k < ticks; ++k) {
        dg = Ramp(dg, target, 256);
        const uint32_t o = b[i];
        const int32_t keep = kUnity - (M16(kUnity - kDubKeep, dg) >> 15);
        b[i] = Pack(Soft(MulZ(Lo(o), keep) + (M16(xl[k], dg) >> 15)), Soft(MulZ(Hi(o), keep) + (M16(xr[k], dg) >> 15)));
        if (++i == kLoopPairs) i = 0;
        if (++dp >= len) {
            dp = 0;
            i = start;
        }
    }
    s->dubp = dp;
    s->dubg = dg;
}

/* ---- the wet channel ---- */

int32_t ReverbSize(int32_t time) { return Max(512, Min(Exp2Q15(12 * time) >> 7, 16300)); }
/* The decay a round trip of the size S for the RT60 the time asks: 12,000 x 2^(4.4 t) clock samples (0.5 s .. 10.6 s
 * at 24 kHz, longer as the clock drops), i.e. 2^-(9.97 S / RT), Q15, at most 0.95. Once a block (two divides). */
int32_t ReverbDecay(int32_t S, int32_t time)
{
    const int32_t rt = (12000 * (Exp2Q15(M16(time, 18022) >> 11) >> 8)) >> 7;    /* 2^(4.4 t): 4.4 t in Q16 */
    /* 9.97 S / rt in Q12 (S x 2552 x 16 < 2^30), then Q16 */
    int32_t y = static_cast<int32_t>(((static_cast<uint32_t>(S) * 2552u) << 4) / static_cast<uint32_t>(rt)) << 4;
    if (y > 14 << 16) y = 14 << 16;
    return Min((1 << 30) / Exp2Q15(y), 31130);
}
int32_t DelayTime(int32_t time) { return Max(128, Min(Exp2Q15(14 * time) >> 8, kWetPairs - 64)); }

/* The block passes (docs/plugin-performance.md "process a delay line in block passes"): every read the reverb and the
 * delay make reaches further back than a block's clock samples (at most 47: the taps from 57, the diffusers from 113,
 * the delay from 128), so a block's reads come out of the ring in one go, the arithmetic runs on plain arrays and the
 * block's writes go back in one go. A run splits at most once, where the ring wraps. */
inline void RingRead(const uint32_t *ring, uint32_t mask, uint32_t idx, uint32_t *out, int32_t n)
{
    idx &= mask;
    while (n > 0) {
        const uint32_t *p = ring + idx;
        int32_t run = static_cast<int32_t>(mask + 1 - idx);
        if (run > n) run = n;
        for (int32_t j = 0; j < run; ++j) out[j] = p[j];
        out += run;
        n -= run;
        idx = 0;
    }
}

inline void RingWrite(uint32_t *ring, uint32_t mask, uint32_t idx, const uint32_t *in, int32_t n)
{
    idx &= mask;
    while (n > 0) {
        uint32_t *p = ring + idx;
        int32_t run = static_cast<int32_t>(mask + 1 - idx);
        if (run > n) run = n;
        for (int32_t j = 0; j < run; ++j) p[j] = in[j];
        in += run;
        n -= run;
        idx = 0;
    }
}

void WetModeEnter(State *s, int32_t m, int32_t time)
{
    s->wmode = m;
    s->lp_l = s->lp_r = 0;
    s->fb_l = s->fb_r = 0;
    s->size = ReverbSize(time);
    s->d = s->d_old = DelayTime(time);
    s->d_xf = 0;
    s->sa = s->sb = 0;
    s->s_age = 1 << 30;                                /* refresh at the first clock sample */
    s->s_xf = 0;
}

/* One allpass diffuser over the block: v (tl, tr) in and out, its ring at ap + off (read m back, written at n0; the
 * two runs are cut where either wraps). */
inline void ApPass(State *s, int32_t off, uint32_t mask, uint32_t m, uint32_t n0, int32_t g, int32_t ticks)
{
    uint32_t *ring = s->ap + off;
    int32_t *vl = s->tl, *vr = s->tr;
    uint32_t ri = (n0 - m) & mask, wi = n0 & mask;
    int32_t k = 0;
    while (k < ticks) {
        int32_t run = ticks - k;
        run = Min(run, static_cast<int32_t>(mask + 1 - ri));
        run = Min(run, static_cast<int32_t>(mask + 1 - wi));
        const uint32_t *rp = ring + ri;
        uint32_t *wp = ring + wi;
        for (int32_t j = 0; j < run; ++j, ++k) {
            const uint32_t z = rp[j];
            const int32_t zl = Lo(z), zr = Hi(z);
            const int32_t ul = Sat16(vl[k] - (M16(zl, g) >> 15)), ur = Sat16(vr[k] - (M16(zr, g) >> 15));
            wp[j] = PackRaw(ul, ur);
            vl[k] = zl + (M16(ul, g) >> 15);
            vr[k] = zr + (M16(ur, g) >> 15);
        }
        ri = (ri + run) & mask;
        wi = (wi + run) & mask;
    }
}

/* Two taps of the line at once, summed into al, ar (set on the first pair, added after). */
inline void TapPair(const uint32_t *wet, uint32_t ia, uint32_t ib, int32_t gal, int32_t gar, int32_t gbl, int32_t gbr,
                    int32_t *al, int32_t *ar, int32_t ticks, bool first)
{
    ia &= kWetMask;
    ib &= kWetMask;
    int32_t k = 0;
    while (k < ticks) {
        int32_t run = ticks - k;
        run = Min(run, kWetPairs - static_cast<int32_t>(ia));
        run = Min(run, kWetPairs - static_cast<int32_t>(ib));
        const uint32_t *pa = wet + ia, *pb = wet + ib;
        if (first) {
            for (int32_t j = 0; j < run; ++j, ++k) {
                const uint32_t a = pa[j], b = pb[j];
                al[k] = M16(Lo(a), gal) + M16(Lo(b), gbl);
                ar[k] = M16(Hi(a), gar) + M16(Hi(b), gbr);
            }
        } else {
            for (int32_t j = 0; j < run; ++j, ++k) {
                const uint32_t a = pa[j], b = pb[j];
                al[k] += M16(Lo(a), gal) + M16(Lo(b), gbl);
                ar[k] += M16(Hi(a), gar) + M16(Hi(b), gbr);
            }
        }
        ia = (ia + run) & kWetMask;
        ib = (ib + run) & kWetMask;
    }
}

void Reverb(State *s, Kept *kept, const int32_t *inl, const int32_t *inr, int32_t ticks, int32_t time, int32_t modv,
            bool freeze)
{
    /* time: the size (512 .. 16300 clock samples, exponential, eased) and the RT60 (ReverbDecay: 0.5 .. 10.6 s at 24 kHz);
     * modify: the diffusers' gain (0 .. 0.72: from a clean multi-tap to a wash). The input and the crossed-over
     * feedback go through four allpass diffusers into the line, six taps across the size come out (each side its own
     * signs) and return, damped. Freeze: the line circulates exactly (the write copies the sample one size back: the
     * last size's worth of the tank, read by the taps, holds as a pad) and the size holds. */
    if (!freeze) s->size = Ramp(s->size, ReverbSize(time), (ticks >> 2) + 1);
    const int32_t S = s->size;
    const int32_t g = M16(modv, 23593) >> 15;
    if (S != kept->rv_size || time != kept->rv_time) {  /* the size or the turn moved: the decay again (0.3) */
        kept->rv_decay = ReverbDecay(S, time);
        kept->rv_size = S;
        kept->rv_time = time;
    }
    const int32_t decay = kept->rv_decay;
    const int32_t damp = 22000;
    uint32_t *wet = s->wet, *tp = s->tp;
    const uint32_t n0 = s->n;
    int32_t *al = s->tl, *ar = s->tr;
    /* the taps, summed two at a time (Q15 products: six of them stay under 2^30) */
    static_assert(kTaps == 6, "three tap pairs");
    uint32_t T[kTaps];
    for (int32_t j = 0; j < kTaps - 1; ++j) T[j] = static_cast<uint32_t>((S * kTapAt[j]) >> 15);
    T[kTaps - 1] = static_cast<uint32_t>(S);
    for (int32_t j = 0; j < kTaps; j += 2) {
        TapPair(wet, n0 - T[j], n0 - T[j + 1], kTapL[j], kTapR[j], kTapL[j + 1], kTapR[j + 1], al, ar, ticks, j == 0);
    }
    /* the output, the damping and the feedback; the line's input in place of the tap sums (the feedback crosses over:
     * L's tail returns on R; clock sample k takes the feedback of k - 1) */
    int32_t fbl = s->fb_l, fbr = s->fb_r, lpl = s->lp_l, lpr = s->lp_r;
    int32_t *wl = s->wl, *wr = s->wr;
    for (int32_t k = 0; k < ticks; ++k) {
        const int32_t a = al[k] >> 15, b = ar[k] >> 15;
        al[k] = Sat16(inl[k] + fbr);                   /* the input first: it may be wl / wr (routing "both") */
        ar[k] = Sat16(inr[k] + fbl);
        wl[k] = Sat16(a + (a >> 1));
        wr[k] = Sat16(b + (b >> 1));
        lpl += MulSub(a, lpl, damp) >> 15;
        lpr += MulSub(b, lpr, damp) >> 15;
        fbl = MulZ(lpl, decay);
        fbr = MulZ(lpr, decay);
    }
    s->fb_l = fbl;
    s->fb_r = fbr;
    s->lp_l = lpl;
    s->lp_r = lpr;
    if (freeze) {
        RingRead(wet, kWetMask, n0 - static_cast<uint32_t>(S), tp, ticks);
    } else {
        ApPass(s, 0, 127, kAp0, n0, g, ticks);
        ApPass(s, kApOff1, 255, kAp1, n0, g, ticks);
        ApPass(s, kApOff2, 255, kAp2, n0, g, ticks);
        ApPass(s, kApOff3, 511, kAp3, n0, g, ticks);
        for (int32_t k = 0; k < ticks; ++k) tp[k] = PackRaw(Soft(al[k]), Soft(ar[k]));
    }
    RingWrite(wet, kWetMask, n0, tp, ticks);
    s->n = n0 + static_cast<uint32_t>(ticks);
}

void Delay(State *s, const int32_t *inl, const int32_t *inr, int32_t ticks, int32_t time, int32_t modv, bool freeze)
{
    /* time: 128 .. 16320 clock samples, exponential; a change crossfades to the new time (the echoes keep their pitch).
     * modify: the feedback, 0 .. 1 (from 97 % on it holds like a looper, sound on sound under the soft knee) and the
     * damping with it. Freeze: the input off, the feedback 1, no damping. */
    const int32_t dt = DelayTime(time);
    if (s->d_xf == 0 && (dt - s->d > 8 || s->d - dt > 8)) {
        s->d_old = s->d;
        s->d = dt;
        s->d_xf = kXfDelay;
    }
    const int32_t fb = freeze ? kUnity : Min(modv + (modv >> 5), kUnity);
    const int32_t damp = freeze ? kUnity : 16384 + (modv >> 1);
    const int32_t inm = freeze ? 0 : kUnity;
    uint32_t *wet = s->wet, *tp = s->tp, *tq = s->tq;
    const uint32_t n0 = s->n;
    int32_t xf = s->d_xf, lpl = s->lp_l, lpr = s->lp_r;
    int32_t *wl = s->wl, *wr = s->wr;
    RingRead(wet, kWetMask, n0 - static_cast<uint32_t>(s->d), tp, ticks);
    if (xf) {                                          /* the old time, faded out */
        RingRead(wet, kWetMask, n0 - static_cast<uint32_t>(s->d_old), tq, ticks);
        for (int32_t k = 0; k < ticks && xf; ++k, --xf) {
            const uint32_t a = tp[k], c = tq[k];
            tp[k] = Pack((M16(Lo(a), kXfDelay - xf) + M16(Lo(c), xf)) >> 8,
                         (M16(Hi(a), kXfDelay - xf) + M16(Hi(c), xf)) >> 8);
        }
    }
    for (int32_t k = 0; k < ticks; ++k) {
        const uint32_t a = tp[k];
        const int32_t yl = Lo(a), yr = Hi(a);
        lpl += MulSub(yl, lpl, damp) >> 15;
        lpr += MulSub(yr, lpr, damp) >> 15;
        const int32_t il = M16(Sat16(inl[k]), inm) >> 15, ir = M16(Sat16(inr[k]), inm) >> 15;
        tp[k] = PackRaw(Soft(il + MulZ(lpl, fb)), Soft(ir + MulZ(lpr, fb)));
        wl[k] = yl;
        wr[k] = yr;
    }
    RingWrite(wet, kWetMask, n0, tp, ticks);
    s->n = n0 + static_cast<uint32_t>(ticks);
    s->d_xf = xf;
    s->lp_l = lpl;
    s->lp_r = lpr;
}

/* One interpolated read of the wet line at pos (Q16, wrapping at 2^30); L in *yl, R returned. */
inline int32_t WetAt(const uint32_t *wet, uint32_t pos, int32_t *yl)
{
    const uint32_t i0 = (pos >> 16) & kWetMask, i1 = (i0 + 1) & kWetMask;
    const int32_t fr = static_cast<int32_t>((pos >> 1) & 0x7fff);
    const uint32_t a = wet[i0], c = wet[i1];
    *yl = Lerp(Lo(a), Lo(c), fr);
    return Lerp(Hi(a), Hi(c), fr);
}

void Slip(State *s, const int32_t *inl, const int32_t *inr, int32_t ticks, int32_t time, int32_t modv, bool freeze)
{
    /* The input is written round the line all the time; every refresh (time: 512 .. 16384 clock samples, exponential,
     * shortened where the speed needs the room) a head jumps back and plays from there at the speed (modify: 14 steps,
     * reverse x2 .. x0.5, forward x0.5 .. x2 by octaves, fifths, fourths and a tone), the old head fading out. Freeze:
     * nothing is written; every refresh starts from the same place: a repeating tone. */
    const int32_t step = ClampStep((modv * kSlipSteps) >> 15, kSlipSteps);
    const int32_t spd = kSlipSpeed[step];
    const int32_t R = Min(Exp2Q15(10 * time) >> 6, kSlipRmax[step]);
    s->s_r = R;
    int32_t off;
    if (spd > 65536) off = (((spd >> 8) * R) >> 8) + 64;
    else if (spd > 0) off = R + 64;
    else off = 64;
    uint32_t *wet = s->wet;
    uint32_t n = s->n;
    uint32_t sa = static_cast<uint32_t>(s->sa), sb = static_cast<uint32_t>(s->sb);
    int32_t age = s->s_age, xf = s->s_xf;
    const uint32_t nf = s->n_frozen;
    int32_t *wl = s->wl, *wr = s->wr;
    for (int32_t k = 0; k < ticks; ++k) {
        if (!freeze) wet[n & kWetMask] = Pack(inl[k], inr[k]);
        if (age >= R) {
            const uint32_t wp = freeze ? nf : n;
            sb = sa;
            xf = kXfSlip;
            sa = ((wp - static_cast<uint32_t>(off)) & kWetMask) << 16;
            age = 0;
        }
        int32_t yl;
        int32_t yr = WetAt(wet, sa, &yl);
        if (xf) {
            int32_t zl;
            const int32_t zr = WetAt(wet, sb, &zl);
            yl = (M16(yl, kXfSlip - xf) + M16(zl, xf)) >> 8;
            yr = (M16(yr, kXfSlip - xf) + M16(zr, xf)) >> 8;
            sb = (sb + static_cast<uint32_t>(spd)) & 0x3fffffffu;
            xf--;
        }
        sa = (sa + static_cast<uint32_t>(spd)) & 0x3fffffffu;
        age++;
        wl[k] = yl;
        wr[k] = yr;
        ++n;
    }
    s->n = n;
    s->sa = static_cast<int32_t>(sa);
    s->sb = static_cast<int32_t>(sb);
    s->s_age = age;
    s->s_xf = xf;
}

/* ---- the block ---- */

void Publish(State *s, int32_t in_pk, int32_t wet_pk, int32_t loop_pk, bool dub)
{
    Display &d = s->disp;
    d.clock_hz = (s->inc * 44100) >> 16;
    int32_t pos = 0, len = 0;
    if (s->lstate == S_LISTEN) {
        len = Min(s->filled - kSeam, kLoopPairs - kLoopMargin);
        if (len < kMinLoop) len = 0;
        /* (w x 32767) / kLoopPairs without the divide (0.3: it ran every block while listening): the reciprocal
         * rounded up in Q16 gives the quotient or one more (w < 15288: its error stays under 0.17), which the product
         * back tells apart. tests/host/fickle checks every w. */
        static_assert(kLoopPairs == 15288, "kPosRecip is 32767 x 65536 / kLoopPairs, rounded up");
        const uint32_t kPosRecip = 140465u;
        const uint32_t w = static_cast<uint32_t>(s->w);
        uint32_t q = (w * kPosRecip) >> 16;
        if (q * static_cast<uint32_t>(kLoopPairs) > w * 32767u) --q;
        pos = static_cast<int32_t>(q);
    } else if (s->len > 0) {
        len = s->len;
        pos = ((s->p >> 16) * 32767) / s->len;
    }
    d.loop_pos = Clamp15(pos);
    d.loop_len = len;
    d.loop_state = s->lstate == S_LISTEN ? 0 : (s->lstate == S_STOP ? 3 : (dub ? 2 : 1));
    d.wet_level = Clamp15(wet_pk);
    d.loop_level = Clamp15(loop_pk);
    d.in_level = Clamp15(in_pk);
    int32_t head;
    if (s->wmode == W_DELAY) head = (s->d * 32767) / kWetPairs;
    else if (s->wmode == W_SLIP) head = s->s_r > 0 ? (s->s_age >= s->s_r ? 32767 : (s->s_age * 32767) / s->s_r) : 0;
    else head = (s->size * 32767) / kWetPairs;
    d.head = Clamp15(head);
    d.freeze = s->frozen;
    d.trig = s->trig;
    d.wet_mode = s->wmode < 0 ? 0 : s->wmode;
    for (int32_t i = 0; i < kEnvBins / 4; ++i) {
        const uint8_t *e = &s->env[4 * i];
        d.env[i] = static_cast<int32_t>(static_cast<uint32_t>(e[0]) | (static_cast<uint32_t>(e[1]) << 8) |
                                        (static_cast<uint32_t>(e[2]) << 16) | (static_cast<uint32_t>(e[3]) << 24));
    }
}

int init(int slot)
{
    if (slot < 0 || slot >= COOP_FX_SLOTS) return -1;
    g_kept[slot].valid = 0;                            /* nothing kept across a selection */
    uint32_t bytes = 0;
    State *s = static_cast<State *>(coop_fx_scratch(slot, &bytes));
    g_st[slot] = (s && bytes >= sizeof(State)) ? s : nullptr;
    if (!g_st[slot]) return -1;
    s->clear = 0;
    s->phase = 0;
    s->inc = kClockInc[28];
    s->pre_l = s->pre_r = 0;
    s->hold[0] = 0;
    s->s1l = s->s1r = s->s2l = s->s2r = 0;
    s->mix = 0;
    s->n = 0;
    s->wmode = -1;                                     /* the first block enters the word's mode */
    s->wduck = 0;
    s->gx = kUnity;
    s->gl = 0;
    s->lp_l = s->lp_r = s->fb_l = s->fb_r = 0;
    s->size = 512;
    s->d = s->d_old = 128;
    s->d_xf = 0;
    s->sa = s->sb = 0;
    s->s_age = 1 << 30;
    s->s_xf = 0;
    s->s_r = 512;
    s->n_frozen = 0;
    s->frozen = 0;
    s->lstate = S_LISTEN;
    s->lmode = 0;
    s->lmode_want = 0;
    s->lduck = kUnity;
    s->w = 0;
    s->filled = 0;
    s->start = 0;
    s->len = 0;
    s->p = 0;
    s->dir = 1;
    s->xp = 0;
    s->xcount = 0;
    s->gain = 0;
    s->dubp = 0;
    s->dubg = 0;
    s->envf = 0;
    s->armed = 1;
    s->holdoff = 0;
    s->slice = 0;
    s->gate = 1 << 30;
    s->g[0].pos = s->g[1].pos = 0;
    s->g[0].age = 0;
    s->g[1].age = kGrainHalf;
    s->scan = 0;
    s->trig = 0;
    s->ebin = -1;
    s->bake = 0;
    for (int32_t i = 0; i < kEnvBins; ++i) s->env[i] = 0;
    Publish(s, 0, 0, 0, false);
    return 0;
}

void render(int slot, int32_t *l, int32_t *r, const int16_t *params, int nsamp)
{
    if (slot < 0 || slot >= COOP_FX_SLOTS || !g_st[slot]) return;   /* not bound: dry */
    State *const s = g_st[slot];
    if (nsamp != kBlock) {                             /* the host's block is 64; anything else passes through */
        Publish(s, 0, 0, 0, false);
        return;
    }
    if (s->clear < kWetPairs + kApPairs) {             /* starting: clear a slice, the dry passes through */
        int32_t c = s->clear;
        const int32_t end = c + kClearWords;
        for (; c < end && c < kWetPairs; ++c) s->wet[c] = 0;
        for (; c < end && c < kWetPairs + kApPairs; ++c) s->ap[c - kWetPairs] = 0;
        s->clear = c;
        Publish(s, 0, 0, 0, false);
        return;
    }
    PROF_START;

    Kept &kept = ReadKnobs(slot, params);
    const int32_t time = kept.q[0], modv = kept.q[1], clk = kept.q[2], mixk = kept.q[3];
    const int32_t wwant = ClampStep(params[5], W_MODES);
    const int32_t route = ClampStep(params[6], R_ROUTES);
    s->lmode_want = ClampStep(params[7], L_MODES);
    const int32_t latch = params[COOP_FX_LATCH];
    const bool freeze = (latch & 1) != 0, loop_on = (latch & 2) != 0, dub = (latch & 4) != 0;

    /* ---- A: the input, the pre-filter, the clock ---- */
    s->inc += (ClockTable(kClockInc, clk) - s->inc) >> 2;
    const int32_t a_in = ClockTable(kClockAIn, clk), a_out = ClockTable(kClockAOut, clk);
    int32_t ticks, in_pk;
    {                                                  /* L with the clock, then R on the clock's ticks */
        uint32_t ph = s->phase;
        const uint32_t inc = static_cast<uint32_t>(s->inc);
        int32_t pl = s->pre_l, hi = 0, lo = 0;
        int32_t *xl = s->xl;
        uint8_t *tk = s->tick;
        for (int32_t i = 0; i < kBlock; ++i) {
            const int32_t d = l[i] >> 16;
            pl += MulSub(d, pl >> 4, a_in) >> 11;
            hi = Max(hi, d);
            lo = Min(lo, d);
            ph += inc;
            const uint32_t t = ph >> 16;               /* 0 or 1: the clock is under 44.1 kHz */
            ph &= 0xffffu;
            tk[i] = static_cast<uint8_t>(t);
            *xl = pl >> 4;                             /* stored always, kept on a tick */
            xl += t;
        }
        ticks = static_cast<int32_t>(xl - s->xl);
        s->phase = ph;
        s->pre_l = pl;
        int32_t pr = s->pre_r;
        int32_t *xr = s->xr;
        for (int32_t i = 0; i < kBlock; ++i) {
            const int32_t d = r[i] >> 16;
            pr += MulSub(d, pr >> 4, a_in) >> 11;
            hi = Max(hi, d);
            lo = Min(lo, d);
            *xr = pr >> 4;
            xr += tk[i];
        }
        s->pre_r = pr;
        in_pk = Max(hi, -lo);
    }
    PROF(0);

    /* ---- B: the looper's state and its read ---- */
    if (s->lstate == S_PLAY && !loop_on) s->lstate = S_STOP;
    if (s->lstate == S_LISTEN && s->lmode != s->lmode_want) LoopModeEnter(s, s->lmode_want);
    if (s->lstate == S_LISTEN && loop_on && s->filled >= kMinLoop + kSeam) Capture(s);
    if (s->lstate != S_LISTEN && s->lduck == 0 && s->lmode != s->lmode_want) LoopModeEnter(s, s->lmode_want);
    if (s->bake > 0 && s->lstate != S_LISTEN) BakeSeam(s);
    int32_t loop_pk = 0;
    if (s->lstate == S_LISTEN) {
        for (int32_t k = 0; k < ticks; ++k) s->ol[k] = s->or_[k] = 0;
    } else {
        LoopRead(s, ticks);
        if (s->lstate == S_STOP && s->gain == 0) s->lstate = S_LISTEN;
        loop_pk = PeakOf(s->ol, s->or_, ticks);
        if (s->len > 0) {                              /* the envelope bin under the head gets the block's peak */
            const int32_t bin = ClampStep(((s->p >> 16) * kEnvBins) / s->len, kEnvBins);
            const int32_t v = Min(loop_pk >> 7, 255);
            if (bin != s->ebin) s->env[bin] = static_cast<uint8_t>(v);
            else if (v > s->env[bin]) s->env[bin] = static_cast<uint8_t>(v);
            s->ebin = bin;
        }
    }
    PROF(1);

    /* ---- C: the routing and the wet channel ---- */
    const int32_t *inl = s->xl, *inr = s->xr;          /* the wet channel's input: the input alone needs no copy */
    {
        int32_t tx = kUnity, tl = 0;                   /* listening: the wet channel hears the input */
        if (s->lstate == S_PLAY) {
            tx = route == R_LOOP ? 0 : kUnity;
            tl = route == R_IN ? 0 : kUnity;
        }
        int32_t gx = s->gx, gl = s->gl;
        if (!(gx == kUnity && gl == 0 && tx == kUnity && tl == 0)) {
            int32_t *wl = s->wl, *wr = s->wr;
            const int32_t *xl = s->xl, *xr = s->xr, *ol = s->ol, *orr = s->or_;
            if (gx == tx && gl == tl) {                /* steady: input + loop, or the loop alone */
                if (gx == 0) {
                    inl = ol;
                    inr = orr;
                } else {
                    for (int32_t k = 0; k < ticks; ++k) {
                        wl[k] = xl[k] + ol[k];
                        wr[k] = xr[k] + orr[k];
                    }
                    inl = wl;
                    inr = wr;
                }
            } else {
                for (int32_t k = 0; k < ticks; ++k) {
                    gx = Ramp(gx, tx, 1024);
                    gl = Ramp(gl, tl, 1024);
                    wl[k] = (M16(xl[k], gx) + M16(ol[k], gl)) >> 15;
                    wr[k] = (M16(xr[k], gx) + M16(orr[k], gl)) >> 15;
                }
                s->gx = gx;
                s->gl = gl;
                inl = wl;
                inr = wr;
            }
        }
    }
    if (s->wmode < 0 || (s->wduck == 0 && s->wmode != wwant)) WetModeEnter(s, wwant, time);
    if (freeze && !s->frozen) s->n_frozen = s->n;
    s->frozen = freeze ? 1 : 0;
    PROF(2);
    switch (s->wmode) {
    case W_DELAY: Delay(s, inl, inr, ticks, time, modv, freeze); break;
    case W_SLIP: Slip(s, inl, inr, ticks, time, modv, freeze); break;
    default: Reverb(s, &kept, inl, inr, ticks, time, modv, freeze); break;
    }
    PROF(3);
    {                                                  /* the mode switch's fade */
        const int32_t dt = s->wmode == wwant ? kUnity : 0;
        int32_t wd = s->wduck;
        if (!(wd == kUnity && dt == kUnity)) {
            int32_t *wl = s->wl, *wr = s->wr;
            for (int32_t k = 0; k < ticks; ++k) {
                wd = Ramp(wd, dt, 1024);
                wl[k] = M16(wl[k], wd) >> 15;
                wr[k] = M16(wr[k], wd) >> 15;
            }
            s->wduck = wd;
        }
    }
    const int32_t wet_pk = PeakOf(s->wl, s->wr, ticks);
    PROF(4);

    /* ---- D: the looper's write ---- */
    LoopWrite(s, ticks, dub && s->lstate == S_PLAY);
    PROF(5);

    /* ---- E: back to 44.1 kHz, the mix, the limit ---- */
    {
        uint32_t *hold = s->hold;
        const int32_t *wl = s->wl, *wr = s->wr, *ol = s->ol, *orr = s->or_;
        for (int32_t k = 0; k < ticks; ++k) {
            hold[k + 1] = Pack(wl[k] + (M16(ol[k], kRecGain) >> 15), wr[k] + (M16(orr[k], kRecGain) >> 15));
        }
        s->mix += (mixk - s->mix) >> 2;
        const int32_t gw = QuarterSin(s->mix), gd = QuarterSin(32767 - s->mix);
        {                                              /* the hold, one pair a 44.1 kHz sample */
            const uint8_t *tk = s->tick;
            const uint32_t *hp = hold;
            uint32_t *hx = s->holdx;
            for (int32_t i = 0; i < kBlock; ++i) {
                hp += tk[i];
                hx[i] = *hp;
            }
        }
        for (int32_t c = 0; c < 2; ++c) {              /* one channel a pass: its state stays in registers */
            int32_t *io = c ? r : l;
            int32_t s1 = c ? s->s1r : s->s1l, s2 = c ? s->s2r : s->s2l;
            const int16_t *hp = reinterpret_cast<const int16_t *>(s->holdx) + c;   /* L | R pairs, little-endian */
            for (int32_t i = 0; i < kBlock; ++i, hp += 2) {
                s1 += MulSub(*hp, s1 >> 4, a_out) >> 11;
                s2 += MulSub(s1 >> 4, s2 >> 4, a_out) >> 11;
                io[i] = Soft((M16(io[i] >> 16, gd) + M16(s2 >> 4, gw)) >> 15) << 16;
            }
            if (c) {
                s->s1r = s1;
                s->s2r = s2;
            } else {
                s->s1l = s1;
                s->s2l = s2;
            }
        }
        hold[0] = hold[ticks];
    }
    PROF(6);
    Publish(s, in_pk, wet_pk, loop_pk, dub && s->lstate == S_PLAY);
    PROF(7);
}

}  // namespace

extern "C" const coop_fx_t FX_fickle = { COOP_FX_MAGIC, COOP_PLUGIN_API, COOP_FX_F_SCREEN, init, render,
                                         &SCREEN_fickle };
