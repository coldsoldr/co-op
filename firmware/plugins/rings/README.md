# rings

Mutable Instruments Rings (Emilie Gillet, MIT) as a coop synth engine: a physical-modelling resonator with six models
and up to six voices. The DSP is te-op1-cfw's fixed-point port (`ports/rings`, commit 5aa314b), the build hundekraft's
kennel plugin used (`ports/knl/rings`): 22.05 kHz with 32-sample control blocks, upsampled 4x to the stock voice rate.
E3 in `docs/engines.md`.

## Controls (guidelines §3.2)

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | structure | brightness | damping | position |
| Click | — | — | — | — |
| Shift + turn | structure fine | brightness fine | damping fine | position fine |
| Shift + click | — | — | — | — |
| fn + turn | model (6 steps, 2 detents a step) | polyphony (4 steps) | — | — |
| fn + click | — | — | — | — |
| Shift + fn + turn | — | — | — | — |
| Shift + fn + click | — | — | — | — |

- Model: `modal` (modal resonator), `symp` (sympathetic strings), `string` (inharmonic string), `fm` (FM voice),
  `quant` (quantized sympathetic strings), `verb` (string and reverb). Polyphony: `all` (6 voices, 14 modes each),
  `1` (60 modes), `2` (28), `4` (12), Rings' own trade of voices for modes; string and reverb plays `all` as 4.
- Both are latched at note on: a change applies from the next note and never clicks a ringing voice.
- Stored in knob words 5 and 6 as the step index (plugin.json `controls`), saved with the sound; the core shows
  `MODEL VERB` etc. when one moves, and the screen draws both rows, bracketed while fn is held.
- Velocity is not used (as in the kennel build). The stock envelope, gain and LFO follow the render.

**Upstream mapping.** Kennel rings: the four turns as here; a blue press flipped blue to page 2, where it turned the
model (word 5, 1536 a click), a green press flipped green to page 2 for the polyphony (word 6). coop retires the
page-2 toggle (guidelines §4: every layer is held), and the owner put both on fn + turn (2026-11-10). The words now
hold step indexes; a sound saved by kennel decodes clamped (an out-of-range word plays as the last step).

## Screen

Kennel rings' own screen (`rings.svg`, from `gen_rings_svg.py`, unchanged): the rings light from the core with
structure, the rays with brightness (flashing with the loudest voice), the pad lifts with damping, the strike dot
moves with position, one ripple per sounding voice; MODEL and POLY on the right. While fn is held the rows are
bracketed with white labels and the rings and rays dim (`coop_scr_mods`). Inputs: `coop_scr_words`, and the voices'
levels from the engine (`RingsScreenVoices`).

## Memory, L1, cost

- File 112,004 bytes (0.2); `.text` 43,040, `.rodata` 49,048, `.bss` 573,572 (six voice states and the shared
  reverb, raw storage set up by `init`). One relocation (engines are relocated once): ~665 KB of heap 0.
- L1: the per-call scratch (render buffers, the mode bank's and string kernels' per-sample buffers, the upsampler's
  work area: 748 bytes) in the core's scratchpad loan (`coop_l1_scratch`, 1312 bytes), and the limiter's soft-limit
  table (1,028 bytes, read every sample of every voice) in the lease (`coop_l1_lease`, 1,056 bytes taken). SDRAM
  fallbacks for both. The other tables are read per block, or are too big (the FM model's 16 KB sine table).
- Cost (avg / max / n; the budget is 580,499 cycles a block):

  | | per voice render | per block |
  |---|---|---|
  | 0.1, op1emu (`tests/sim/det_eng_rings.txt`) | 15,124 / 32,128 | 85,742 / 153,017 |
  | **0.1, device** (`coop_stats (21).txt`, 2026-11-10) | **32,741 / 187,416** | **181,951 / 415,022** |
  | 0.2, op1emu (same scenario) | 13,412 / 30,417 | 76,056 / 143,676 |
  | 0.2, device | pending | pending |

  The device ran 0.1 at about twice op1emu's figures, not 10-35 % over: op1emu models neither the instruction cache
  nor SDRAM. The device counters split a render (`synth_rest` p0 / p1, most renders rings): ~29 % code-fetch stalls
  (46 % on the first voice of a block), ~12 % data stalls, the rest compute. With one effect in each slot the audio
  callback averaged ~490 k of 580 k cycles in busy windows (crackles at the peaks, the UI starved).

## Optimisation (0.2, 2026-11-10)

Measured first: `make -C tests/host/rings profile` runs the host test's scenario in the GNU simulator with stage
timers (`src/profile.h`, compiled out elsewhere) and prints cycles per render by model and stage, and the heaviest
calls. Simulator compute (no cache, no SDRAM), polyphony "all", per render, 0.1:

| stage | modal | string models | fm | verb |
|---|---|---|---|---|
| upsampler (C) | 5,125 | ~5,300 | 5,293 | 3,853 |
| mode bank / strings / fm | 10,226 | 13,650-14,575 | 8,687 | 10,760 |
| modal coefficients | 2,821 | - | - | - |
| string reconfiguration | - | 1,291-2,608 | - | - |
| limiter | 1,503 | ~1,560 | 1,683 | 1,141 |
| total | 22,949 | 22,177-23,106 | 17,335 | 18,861 |

The heaviest single renders (~110 k) are polyphony 1 in the modal model: one 60-mode voice recomputing all its
coefficients, Rings' own trade (64 / polyphony - 4 modes). Note-ons are cheap (under 12 k).

What 0.2 changes:
- **The upsampler's half-band stage in assembly** (`src/halfband_bfin.S`, generated by `gen_halfband_asm.py`): the
  symmetric 24-tap FIR on the MAC units, one product a cycle, packet shapes the stock firmware uses (`asm_lint` clean).
  Bit-identical with the C loop (`make -C tests/host/rings halfband-check`: 20,000 blocks with edge values, 0
  mismatching words). The upsampler: 5.1 k -> 2.8 k a render.
- **The control rate halved** (`-DOP1RINGS_CONTROL_HALVED=1`, the owner's choice): each voice does its control work
  (the modal coefficients and excitation cutoff, the strings' tuning and parameters) every other block, 2.9 ms apart
  instead of 1.45 ms (kennel's rate; upstream's is 0.5 ms), the voices offset so half of them do it in any block, a
  strike always; the sympathetic strings' glide takes a two-block step, so a glide lasts as long. The FM model is
  unchanged. Not bit-identical by design: against te-op1-cfw the level matches (+0.03 dB over the 90 s scenario), and
  with no knob jumps or bends the voices' block levels match closely (modal, string: 99th percentile under 0.6 dB;
  sympathetic models: medians 0.04-0.28 dB); knob jumps land one block later, which moves the strings' phase
  (`make -C tests/host/rings halved`).
- Simulator compute per render, 0.2: modal 19,482, symp 18,452, string 20,497, fm 15,017, quant 18,277, verb 16,615
  (11-18 % below 0.1). On the device the halved rate should also cut code fetches (half the renders skip the control
  code): the device figure decides.
- Tried and dropped: `-Os` on the control-heavy files shrinks `.text` 43.0 -> 31.4 KB but costs 27-75 % more compute
  (the fixed-point helpers stop inlining in the per-sample C), more than the fetches it could save.

Still open, by size: the string kernels and the mode bank (already assembly, ~23 cycles a mode-sample), the
upsampler's 2x stage and copies (~2.8 k), the limiter (~47 cycles a sample), batching the voices' control work in
one pass a block (the largest lever on code fetches), and fewer modes in "all" (14 per voice).

## Building and checking

```sh
make -C plugins                      # build/plugins/rings.coop
make -C tests/host/rings             # bit-identical to te-op1-cfw's engine over a 90 s scenario (halving off)
make -C tests/host/rings halved      # the shipped build against te-op1-cfw: levels and spectra
make -C tests/host/rings halfband-check   # the upsampler kernel against its C loop, in the simulator
make -C tests/host/rings profile     # cycles per render by model and stage, in the simulator
```

## Differences from te-op1-cfw

The DSP files (`src/`, `op1/`) are te-op1-cfw's, with only what coop's rules require changed:
- **No float or double in any file** (guidelines §2.1): upstream's compile-time constants (`Q(x, n)`, `St(x)`,
  `LfoIncrement(hz)`, `FFloat(double)`) are integer literals, made by `defloat.py` with the same IEEE double arithmetic
  as GCC's constexpr evaluation at 22.05 kHz; the original expression stays in a comment beside each. `FFloat` gains
  `Raw(m, e)`; the host-only float helpers are gone.
- **No firmware addresses** (§2.1): `op1/op1.h` keeps only block size, voice count, the cycle counter (a register) and
  saturation; the engine (`rings.cc`, from `op1/engine.cc`) takes voice, pitch and knobs from its arguments, and the
  scratch from the core (`src/scratch.h`).
- **No libc, no static constructors**: the debug `printf` is gone, `StringFixed::CopyRecentFrom` copies by bytes, the
  voice states and reverb are raw `.bss` (their `FFloat` members have constructors), `op1/support.cc` gives the
  compiler's own `memcpy` / `memset` calls (struct copies) their plugin-local bodies, as kennel did.
- `op1/settings.h` decodes words 5 and 6 as step indexes (`OP1_COOP`).
- 0.2: the half-band kernel and the halved control rate (above); `src/profile.h` stage timers.

The host test (`tests/host/rings/rings_compare.cc`) builds te-op1-cfw's own engine and this one from the same seeded
call sequence (4,367 notes, every model and polyphony pair, knob jumps, bends, steals, idle gaps): the outputs are
bit-identical, and a 1-LSB change to a live constant makes them differ.
