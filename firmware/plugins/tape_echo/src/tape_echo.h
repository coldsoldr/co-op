// TAPE ECHO: a Space Echo style multi-head tape delay in fixed point, at
// 22050 Hz (the glue decimates 2:1 and interpolates the wet signal back).
// Wet only. From te-op1-cfw ports/fx/src/tape_echo (5aa314b), its design and
// measurements in te-op1-cfw docs/tape-echo-port.md; the coop copy keeps the
// stereo path only (coop runs every effect in stereo, guidelines §2.2) and
// leaves the tape's clear to its caller (ClearTape, a slice a block: a
// 133 KB clear does not belong in the audio interrupt, guidelines §2.3).
//
// One tape loop per channel (L and R side by side in 32-bit pairs, so every
// read serves both channels), a record head and three playback heads at
// 1/3, 2/3 and 1 of TIME:
//
//   in x drive + feedback x (the active heads' mix)        (record amp)
//     -> low cut (one-pole high-pass, 40..110 Hz with age)
//     -> tape loss (one-pole low-pass, 9 kHz..1.8 kHz with age)
//     -> tape saturation (sin() up to pi/2, then flat; a table)
//     -> the tape
//   wet = the active heads' mix / drive
//
// Wow and flutter: Mutable Instruments Beads' WowAndFlutter (Emilie Gillet,
// MIT; see ../LICENSE-MIT), in integers: a 0.23 Hz wow whose rate and depth
// drift with a slow random sine, and a 1.99 Hz flutter, up to 96 + 16
// samples of read offset at AGE 1 (on head 3; the heads keep their
// proportions, as with a tape speed change).
//
// TIME glides like a tape motor (93 ms, at most 0.75 sample per sample).
//
// Arithmetic: the Blackfin's 16-bit MACs (unbiased rounding, saturated).
// The run kernel has a C reference (TapeEchoRunC, host builds only) and the
// Blackfin version (tape_echo_bfin.S, bit-exact).

#ifndef COOP_TAPE_ECHO_TAPE_ECHO_H_
#define COOP_TAPE_ECHO_TAPE_ECHO_H_

#include <stdint.h>

#include "src/tape_echo_layout.h"

namespace op1fx {

struct TapeEchoParams {
  int32_t time;        // 0..32767 -> 30 ms .. 1.5 s (exponential): head 3's delay
  int32_t feedback;    // 0..32767 -> 0..1.1 (linear; 1.0 at 29789)
  int32_t age;         // 0..32767: new tape -> worn (wow/flutter, loss, drive)
  int32_t heads;       // the head pattern, 0..kTapeEchoNumPatterns - 1
  bool hold;           // input off, feedback 1: the tape loops what it holds
};

// Head patterns, in order: head 3; heads 2+3; heads 1+2+3; heads 1+3.
const int32_t kTapeEchoNumPatterns = 4;
extern const uint8_t kTapeEchoPatternHeads[kTapeEchoNumPatterns];
const int32_t kTapeEchoMaxBlock = 64;

// The run kernel's block (tape_echo_layout.h's TEK_ offsets).
struct TapeEchoKernel {
  int32_t pos[3];      // head delays, Q15 samples
  int32_t step[3];
  int32_t w[3];        // out weight (low, Q15) | feedback weight (high, Q15)
  int32_t ws[3];       // per-sample steps, per half
  int32_t gain;        // input gain, Q31 (high half used)
  int32_t gstep;
  int32_t hp_l, hp_r;  // low cut states
  int32_t lp;          // tape loss states, L | R
  int32_t k;           // low cut k (low) | tape loss k (high), Q15
  int32_t adj[3];      // per run: the wrap of each head's reads, bytes (0 or -4 N)
  int32_t sat;         // the assembly's scratch
};

// x: n stereo pairs (L low, R high), the input; replaced by the wet pairs.
// tape: the ring's first pair; p: the write position (pair index in
// 1..TE_TAPE_SIZE; each sample first moves it down one, the run doesn't
// wrap). The C reference wraps each read itself; the assembly takes the
// wrap from k->adj (TapeEchoPrepareRun).
#if !defined(__bfin__) || defined(COOP_TAPE_ECHO_C)
void TapeEchoRunC(TapeEchoKernel* k, int32_t* x, int32_t* tape, int32_t p, int32_t n);
#endif
extern "C" void TapeEchoRunAsm(TapeEchoKernel* k, int32_t* x, int32_t* tape, int32_t* p, int32_t n);
int32_t TapeEchoPrepareRun(TapeEchoKernel* k, int32_t p, int32_t m);

int32_t TapeEchoSaturate(int32_t x);
extern const int16_t kTapeEchoSatTable[257];

class TapeEcho {
 public:
  // Everything but the tape (cheap). The tape must be all zero before the
  // next Process: ClearTape in slices, or a tape the loader zeroed.
  void Init();
  // Zeroes pairs [from, from + count) of the tape (and its mirror pair);
  // returns the next `from` (TE_TAPE_PAIRS when done).
  int32_t ClearTape(int32_t from, int32_t count);
  // n even, 2..64; stereo in and out. Wet at the input's scale, saturated.
  // In-place calls are fine.
  void Process(const int16_t* in_l, const int16_t* in_r, int16_t* wet_l, int16_t* wet_r,
               int32_t n, const TapeEchoParams& p);

 private:
  void Control(const TapeEchoParams& p, int32_t n);

  TapeEchoKernel k_;
  int32_t t_, t_target_, v_;
  uint32_t wow_phase_, flutter_phase_, rand_phase_, rand_inc_, rng_;
  int32_t mod_;
  int32_t fb_, age_;
  int32_t cur_out_[3], cur_fb_[3];
  int32_t w_out_[3], w_fb_[3];
  int32_t gain_;
  int32_t ptr_;
  int32_t last_time_, last_age_;
  bool last_hold_;
  int32_t inv_drive_, gain_target_, coefs_;
  bool started_;
  bool settled_;
  TapeEchoParams last_;
  int32_t x_[kTapeEchoMaxBlock];     // input pairs, then wet pairs
  int32_t tape_[TE_TAPE_PAIRS];
};

}  // namespace op1fx

#endif  // COOP_TAPE_ECHO_TAPE_ECHO_H_
