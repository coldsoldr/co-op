/* The Rings port's sample rate and what follows from it. Preprocessor only:
 * this header is included by C++ and by the assembly kernels (reverb,
 * limiter), so every constant that is a count of samples or a per-sample
 * coefficient in both places comes from here.
 *
 *   OP1RINGS_SAMPLE_RATE   44100 (default): 64 samples per OP-1 render call,
 *                          upsampled 2x to the 128 at 88.2 kHz the voice
 *                          interface wants (op1/engine.cc).
 *                          22050: 32 samples per call, upsampled 4x (a
 *                          half-band FIR to 44.1 kHz, then the 2x stage).
 *                          Build with -DOP1RINGS_SAMPLE_RATE=22050
 *                          -DOP1RINGS_CONTROL_BLOCK_SIZE=32 -DOP1RINGS_UPSAMPLE=4
 *                          (ports/rings/Makefile op1-seq-r22, RINGS_R22_FLAGS).
 *
 * At 22.05 kHz the things measured in samples keep their duration:
 *   - the reverb's delay lines and modulated taps are half as long
 *     (OP1RINGS_REVERB_DIV; src/reverb_fixed.cc and src/reverb_bfin.S);
 *   - the limiters' attack and release coefficients are twice as large
 *     (src/op1_voice.cc and src/limiter_bfin.S);
 *   - the string exciter's noise colour coefficient doubles
 *     (src/string_fixed.cc ConfigureTone).
 * Everything relative to the note's frequency (the modes, the exciter
 * cutoff, the plucker, the delay lengths of the strings) follows the pitch
 * word, which op1/engine.cc scales for the rate. The 44.1 kHz build is
 * unchanged by this header (every value reduces to what it was). */
#ifndef OP1RINGS_RATE_CONFIG_H_
#define OP1RINGS_RATE_CONFIG_H_

#ifndef OP1RINGS_SAMPLE_RATE
#define OP1RINGS_SAMPLE_RATE 44100
#endif

#if OP1RINGS_SAMPLE_RATE == 22050
#define OP1RINGS_REVERB_DIV 2
#define OP1RINGS_LIMITER_ATTACK_Q31 214748365    /* 0.1 */
#define OP1RINGS_LIMITER_RELEASE_Q31 85899       /* 0.00004 */
#else
#define OP1RINGS_REVERB_DIV 1
#define OP1RINGS_LIMITER_ATTACK_Q31 107374182    /* 0.05 (upstream, per sample at 48 kHz) */
#define OP1RINGS_LIMITER_RELEASE_Q31 42950       /* 0.00002 */
#endif

#endif  /* OP1RINGS_RATE_CONFIG_H_ */
