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
 *   params[7]     the head pattern (white click): step 0..3.
 *   params[8]     bit 0: hold (green click, a latch).
 *
 * Memory: one instance per relocation (the core relocates the file once per position, each with its own .bss):
 * the tape is 132,868 bytes. Its clear is spread over the first blocks after a (re)start (kClearPairs a block, the
 * dry passing through meanwhile) instead of 133 KB of stores in init, which runs in the audio interrupt at the stock
 * positions (guidelines §2.3).
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission. The DSP in src/ is te-op1-cfw's
 * (GPL-3.0-or-later), with Mutable Instruments Beads' wow and flutter and Clouds' output stage (Emilie Gillet, MIT,
 * LICENSE-MIT). */
#include <stdint.h>

#include "coop_plugin.h"
#include "src/half_rate.h"
#include "src/tape_echo.h"

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

const int32_t kBlock = op1clouds::kFxBlockSize;        /* 64 */
const int32_t kHalfN = op1clouds::kHalfN;              /* 32 */
const int32_t kClearPairs = 2048;                      /* tape pairs zeroed a block: 17 blocks (25 ms) for all */

/* The filter type's knob ranges (sdk/coop_plugin.h): what kennel's op1_fx_param_q15 read from the stock table. */
const int16_t kRangeLo[4] = { 64, 0, 0, 8192 };
const int16_t kRangeHi[4] = { 14528, 24576, 22528, 32767 };

struct Instance {
    op1fx::TapeEcho echo;
    op1clouds::HalfRate half;
    int32_t clear;                                     /* the next tape pair to zero; TE_TAPE_PAIRS = clean */
    int16_t dry[2][kBlock];                            /* the input as int16: the mix writes the output here */
    int16_t gained[2][kHalfN];                         /* the decimated input after the gain */
};

Instance g;                                            /* .bss: zeroed by the loader (the tape too) */

inline int32_t Clamp15(int32_t v) { return v < 0 ? 0 : (v > 32767 ? 32767 : v); }

/* A turn's word in the filter range -> 0..32767 (kennel's op1_fx_param_q15; per block, not per sample). */
inline int32_t Knob(const int16_t *p, int k)
{
    const int32_t lo = kRangeLo[k], hi = kRangeHi[k];
    const int32_t v = (p[k] - lo) * 32767 / (hi - lo);
    return Clamp15(v);
}

/* The input gain word as a Q14 factor: -6 dB at 0, 0 dB at the centre, +6 dB at the top (10^(dB / 20) x 16384, one
 * table entry per dB, interpolated): kennel's knl::GainQ14 (ports/knl/common/knl_fx.h). */
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

int init(int slot)
{
    (void)slot;
    g.half.Stop();                                     /* the next render restarts: core reset, tape cleared */
    return 0;
}

void render(int slot, int32_t *l, int32_t *r, const int16_t *params, int n)
{
    (void)slot;
    if (n != kBlock) return;                           /* the host's block is 64; anything else passes through */
    if (!g.half.Begin(Cycles())) {
        g.echo.Init();
        g.clear = 0;
    }
    if (g.clear < TE_TAPE_PAIRS) {                     /* starting: clear a slice, the dry passes through */
        g.clear = g.echo.ClearTape(g.clear, kClearPairs);
        g.half.End(Cycles());
        return;
    }

    op1fx::TapeEchoParams q;
    q.time = Knob(params, 0);
    q.feedback = Knob(params, 1);
    q.age = Knob(params, 2);
    const int32_t mix = Knob(params, 3);
    int32_t pattern = params[7];
    q.heads = pattern < 0 ? 0 : (pattern >= op1fx::kTapeEchoNumPatterns ? op1fx::kTapeEchoNumPatterns - 1 : pattern);
    q.hold = (params[COOP_FX_LATCH] & 1) != 0;
    const int32_t gain = GainQ14(params[5]);

    /* The bus to int16, into the decimators' buffers and the dry copy the mix works on. */
    int16_t *dl = g.dry[0], *dr = g.dry[1];
    int16_t *il = g.half.in_full(0), *ir = g.half.in_full(1);
    for (int32_t i = 0; i < kBlock; ++i) {
        const int16_t a = static_cast<int16_t>(l[i] >> 16), b = static_cast<int16_t>(r[i] >> 16);
        dl[i] = il[i] = a;
        dr[i] = ir[i] = b;
    }
    g.half.Decimate();

    /* The input gain: what reaches the record head, saturating (the dry side of the mix is the input as it came). */
    for (int32_t c = 0; c < 2; ++c) {
        const int16_t *src = g.half.in(c);
        int16_t *dst = g.gained[c];
        for (int32_t i = 0; i < kHalfN; ++i) {
            int32_t y = (src[i] * gain) >> 14;
            if (y > 32767) y = 32767;
            if (y < -32768) y = -32768;
            dst[i] = static_cast<int16_t>(y);
        }
    }
    g.echo.Process(g.gained[0], g.gained[1], g.half.wet(0), g.half.wet(1), kHalfN, q);
    g.half.Mix(dl, dr, mix);
    for (int32_t i = 0; i < kBlock; ++i) {
        l[i] = static_cast<int32_t>(dl[i]) << 16;
        r[i] = static_cast<int32_t>(dr[i]) << 16;
    }
    g.half.End(Cycles());
}

}  // namespace

extern "C" const coop_fx_t FX_tape_echo = { COOP_FX_MAGIC, COOP_PLUGIN_API, 0, init, render };
