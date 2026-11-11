// The modal resonator's per-sample loop: a bank of Chamberlin band-pass
// filters in fixed point, summed into two outputs (even-indexed modes -> out,
// odd-indexed -> aux; upstream's "odd" and "even" harmonics). This is the
// hot loop of the modal model, so it's kept apart from the per-block
// coefficient code, benchmarked on its own, and implemented in Blackfin
// assembly (mode_bank_bfin.S) with exactly the same arithmetic.
//
// Per mode, with LP = lp / 2 and HP = hp / 2 stored to avoid shifts:
//   LP += (f/2) BP
//   HP  = x'/2 - LP - (q/2) BP
//   BP += 4 (f/2) HP              (= f hp)
//   w  += dw;   y += w BP
// where x' = in[n] + in[n-1] is shared by all modes.

#ifndef OP1RINGS_MODE_BANK_H_
#define OP1RINGS_MODE_BANK_H_

#include <cstddef>
#include <cstdint>

#include "src/fixed.h"

namespace op1rings {

// Per-mode data, interleaved so the kernel walks one pointer. Field order is
// part of the assembly kernel's interface.
struct Mode {
  int32_t lp;               // Q27, lp / 2
  int32_t bp;               // Q27
  int32_t half_f;           // f / 2, Q31
  int32_t half_q;           // q / 2, Q31
  int32_t weight;           // amplitude * gain, Q31 (top 16 bits used)
  int32_t weight_increment; // per sample, Q31
};

// in: Q27 resonator input (already scaled), size samples. previous_input is
// the last input of the previous block (updated). out/aux: Q27; out == aux
// sums all the modes into one buffer (the same values as out + aux: integer
// sums don't depend on order). num_modes is even (modes are processed in
// pairs). half_x: size words of scratch for the assembly (the halved
// inputs it reads once per mode and sample: in L1 on the OP-1, src/scratch.h).
extern "C" void ModeBankProcessAsm(Mode* modes, int32_t num_modes,
                                   const int32_t* in, int32_t* previous_input,
                                   int32_t* out, int32_t* aux, int32_t size,
                                   int32_t* half_x);

inline void ModeBankProcessC(Mode* modes, int32_t num_modes, const int32_t* in,
                             int32_t* previous_input, int32_t* out,
                             int32_t* aux, size_t size) {
  int32_t x_prev = *previous_input;
  for (size_t s = 0; s < size; ++s) {
    const int32_t input = in[s];
    const int32_t half_x = (input + x_prev) >> 1;
    x_prev = input;
    int32_t sums[2] = { 0, 0 };
    for (int32_t m = 0; m < num_modes; ++m) {
      Mode& mode = modes[m];
      int32_t lp = AddSat(mode.lp, MulQ31Fast(mode.bp, mode.half_f));
      int32_t hp = half_x - lp - MulQ31Fast(mode.bp, mode.half_q);
      int32_t bp = AddSat(mode.bp, ShlSat(MulQ31Fast(hp, mode.half_f), 2));
      mode.lp = lp;
      mode.bp = bp;
      mode.weight += mode.weight_increment;
      sums[m & 1] += MulQ31ByQ15(bp, static_cast<int16_t>(mode.weight >> 16));
    }
    if (out == aux) {
      out[s] = sums[0] + sums[1];
    } else {
      out[s] = sums[0];
      aux[s] = sums[1];
    }
  }
  *previous_input = x_prev;
}

inline void ModeBankProcess(Mode* modes, int32_t num_modes, const int32_t* in,
                            int32_t* previous_input, int32_t* out,
                            int32_t* aux, size_t size, int32_t* half_x = nullptr) {
#if defined(__bfin__) && !defined(OP1RINGS_NO_ASM)
  int32_t local[32];   // MAX_BLOCK in mode_bank_bfin.S
  ModeBankProcessAsm(modes, num_modes, in, previous_input, out, aux,
                     static_cast<int32_t>(size), half_x ? half_x : local);
#else
  (void)half_x;
  ModeBankProcessC(modes, num_modes, in, previous_input, out, aux, size);
#endif
}

}  // namespace op1rings

#endif  // OP1RINGS_MODE_BANK_H_
