/* coop-fw e15: the plugin effect wrap in assembly (efficiency audit finding 9, docs/performance.md "e15").
 *
 * A coop effect takes its block as Q31 L / R (int32, the sample in the high half), the core holds it as int16: at an
 * instrument position as mid + two sides (the carrier: l = sat16(m + sl), r = sat16(m + sr)), at the master positions
 * as l / r. These four kernels do the conversions the C loops in plugin_host.cc did, bit for bit
 * (tests/sim/wrap_check.cc compares them with the C over random and edge inputs on the bfin simulator):
 *   coop_wrap_ms_in   L[i] = sat16(m[i] + sl[i]) << 16, R[i] = sat16(m[i] + sr[i]) << 16
 *   coop_wrap_ms_out  l = L[i] >> 16, r = R[i] >> 16 (an int32 >> 16 is in int16 range: the C's sat16 is a no-op);
 *                     m[i] = (l + r) >> 1; sl[i] = (int16_t)(l - m[i]); sr[i] = (int16_t)(r - m[i])   (FX 1)
 *   coop_wrap_ms_out_sat  the same with sl[i] = sat16(l - m[i]), sr[i] = sat16(r - m[i])               (slot 2)
 *   coop_wrap_lr_in   L[i] = l[i] << 16, R[i] = r[i] << 16
 *   coop_wrap_lr_out  l[i] = L[i] >> 16, r[i] = R[i] >> 16
 * The Q31 block is one array of 128 words, L at [0..63], R at [64..127] (the core's work block in the L1 pool).
 * 64 samples, no overreads, no stores outside the named arrays. Packets: only forms the stock or coop's own kernels
 * already run on the device (one 32-bit ALU op beside a D-register store and / or I-register loads); no two slots
 * write one register. tools/asm_lint.py: clean.
 * C ABI: arguments in R0 / R1 / R2; clobbers R0-R3, P0-P2, I0-I3, LC0 / LT0 / LB0 only; L0-L3 stay zero.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
    .text

/* void coop_wrap_ms_in(const int16_t *const src[3] (m, sl, sr), int32_t *lr)
 * R2 and R3 keep a zero low half for the whole loop: the saturating 16-bit adds write only their high halves. */
    .global _coop_wrap_ms_in
    .type   _coop_wrap_ms_in, STT_FUNC
    .align 4
_coop_wrap_ms_in:
    P0 = R0;
    P1 = R1;                                       /* L */
    R0 = [P0 + 0];
    I0 = R0;                                       /* m */
    R0 = [P0 + 4];
    I1 = R0;                                       /* sl */
    R0 = [P0 + 8];
    I2 = R0;                                       /* sr */
    R0 = 256 (X);
    R0 = R0 + R1;
    P2 = R0;                                       /* R = L + 64 words */
    R2 = 0;
    R3 = 0;
    P0 = 64;
    LSETUP (.Lmsi_a, .Lmsi_b) LC0 = P0;
.Lmsi_a: MNOP || R0.L = W[I0++] || R1.L = W[I1++];
    R3.H = R0.L + R1.L (S) || R1.L = W[I2++] || NOP;       /* sat16(m + sl); then sr into R1 */
    R2.H = R0.L + R1.L (S) || [P1++] = R3 || NOP;          /* sat16(m + sr); L stored */
.Lmsi_b: [P2++] = R2;
    RTS;
    .size _coop_wrap_ms_in, .-_coop_wrap_ms_in

/* void coop_wrap_ms_out(const int32_t *lr, int16_t *const dst[3] (m, sl, sr))       sl, sr = (int16_t)(l - m): FX 1
 * void coop_wrap_ms_out_sat(const int32_t *lr, int16_t *const dst[3])                 sl, sr = sat16(l - m): slot 2
 * l - m leaves int16 at one corner only (l = 32767, r = -32768: m = -1, sl = 32768; and its mirror): FX 1's C wraps
 * it to -32768, slot 2's C saturates it to 32767, so each keeps its own (the (NS) / (S) of the two subtracts). */
.macro MS_OUT name, mode
    .global \name
    .type   \name, STT_FUNC
    .align 4
\name:
    I0 = R0;                                       /* L */
    R2 = 256 (X);
    R2 = R2 + R0;
    I1 = R2;                                       /* R */
    P0 = R1;
    R0 = [P0 + 0];
    I2 = R0;                                       /* m */
    R0 = [P0 + 4];
    I3 = R0;                                       /* sl */
    P1 = [P0 + 8];                                 /* sr */
    P0 = 64;
    LSETUP (1f, 2f) LC0 = P0;
1:  MNOP || R0 = [I0++] || R1 = [I1++];
    R0 = R0 >>> 16;                                /* l */
    R1 = R1 >>> 16;                                /* r */
    R2 = R0 + R1;
    R2 = R2 >>> 1;                                 /* m = (l + r) >> 1 */
    R0.L = R0.L - R2.L (\mode) || W[I2++] = R2.L || NOP;   /* sl = l - m; m stored */
    R1.L = R1.L - R2.L (\mode) || W[I3++] = R0.L || NOP;   /* sr = r - m; sl stored */
2:  W[P1++] = R1;
    RTS;
    .size \name, .-\name
.endm
    MS_OUT _coop_wrap_ms_out, NS
    MS_OUT _coop_wrap_ms_out_sat, S

/* void coop_wrap_lr_in(const int16_t *l, const int16_t *r, int32_t *lr)
 * The half-register loads leave R0.L / R1.L at zero. */
    .global _coop_wrap_lr_in
    .type   _coop_wrap_lr_in, STT_FUNC
    .align 4
_coop_wrap_lr_in:
    I0 = R0;
    I1 = R1;
    P1 = R2;                                       /* L */
    R0 = 256 (X);
    R0 = R0 + R2;
    P2 = R0;                                       /* R */
    R0 = 0;
    R1 = 0;
    P0 = 64;
    LSETUP (.Llri_a, .Llri_b) LC0 = P0;
.Llri_a: MNOP || R0.H = W[I0++] || R1.H = W[I1++];
    [P1++] = R0;
.Llri_b: [P2++] = R1;
    RTS;
    .size _coop_wrap_lr_in, .-_coop_wrap_lr_in

/* void coop_wrap_lr_out(const int32_t *lr, int16_t *l, int16_t *r) */
    .global _coop_wrap_lr_out
    .type   _coop_wrap_lr_out, STT_FUNC
    .align 4
_coop_wrap_lr_out:
    I0 = R0;
    R3 = 256 (X);
    R0 = R0 + R3;
    I1 = R0;
    I2 = R1;
    I3 = R2;
    P0 = 64;
    LSETUP (.Llro_a, .Llro_b) LC0 = P0;
.Llro_a: MNOP || R0 = [I0++] || R1 = [I1++];
    W[I2++] = R0.H;
.Llro_b: W[I3++] = R1.H;
    RTS;
    .size _coop_wrap_lr_out, .-_coop_wrap_lr_out
