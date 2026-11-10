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
| fn + turn | model (6 steps, 2 detents a step) | polyphony (3 steps) | — | — |
| fn + click | — | — | — | — |
| Shift + fn + turn | — | — | — | — |
| Shift + fn + click | — | — | — | — |

- Model: `modal` (modal resonator), `symp` (sympathetic strings), `string` (inharmonic string), `fm` (FM voice),
  `quant` (quantized sympathetic strings), `verb` (string and reverb). Polyphony (0.3): `4` (four voices, the
  oldest stolen and faded; 10 modes a modal voice), `1` (60 modes), `2` (28), Rings' own trade of voices for modes.
  0.1 / 0.2 had `all` (six voices of 14 modes) first and Rings' `4` last; a sound saved with either plays as `4`.
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

**Device, 0.2 (owner, 2026-11-10, `coop_stats (22).txt`, the tape recording, no effect):** it felt worse.
`eng_rings=31037/113871`, `eng_rings_block=173719/278457`: 5 % below 0.1 on average, the peak a third lower; the
code-fetch stalls a render fell 8,551 -> 5,003 (`synth_rest` p0), the compute did not move. The callback averaged
460-470 k in the busy windows and peaked at 882 k with the recording on (~25 k a block in the interrupt, the flushes
on the main loop); idle it is ~100 k, and rings, the voice hook, the stage and the recorder add ~330 k, so ~130 k is
the rest of the block running slower beside rings (its code and state pushing the stock's out of the caches) [I].
A rings voice costs about three stock voices (digital ~11 k a render). Shaving percentages could not make it fit.

## Optimisation (0.3, 2026-11-10)

The owner chose four cuts; three went in, each behind its own switch (plugin.json), so with them off the host test
still proves the rest bit-identical to te-op1-cfw:
- **`OP1RINGS_COOP_POLY4`: four voices at most.** The polyphony setting 0 plays as Rings' polyphony 4 (the oldest
  voice stolen and faded) instead of six voices of their own: Rings' own maximum. The steps are `4`, `1`, `2`.
- **Fewer modal modes (same switch):** 10 a voice at polyphony 4 instead of Rings' 12 (0.1 / 0.2: 14 at "all").
- **`OP1RINGS_CONTROL_BATCHED`: the control work batched.** At a block's first render the control work of every
  voice whose turn it is (the halved schedule) runs back to back (`RingsVoice::Control`: the modal cutoff and
  coefficients, the strings' parameters and configuration, `StringVoice::Control` / `StringFixed::Prepare`), with
  each voice's last pitch and smoothed knobs; the renders then run the audio path only. A strike, a fading or
  sleeping voice, or a voice with no last block does its own. The coefficient budget is no longer used (at most two
  voices of 10 modes, or one of 60, update in a block). Against the halved schedule alone, steady state at
  polyphony 4, the voices' block levels match (medians 0.00-0.39 dB; `make -C tests/host/rings batched`); the modal
  coefficients now follow the knobs without waiting for the budget.
- **Not done: the shorter half-band filter.** With the kernel in assembly (0.2) the stage costs ~1.2 k; half its taps
  would save ~0.5 k a render, and the 6-tap design (Kaiser, `te-op1-cfw/ports/rings/resources/gen_halfband.py`,
  N_SIDE 11) lets the images of 9-10 kHz through at 12-13 kHz only 12-24 dB down (now 21-59 dB). Not worth it; the
  2x stage and the copies (~1.6 k) can go to assembly bit-identically instead.

Simulator, per block at the default polyphony (0.2 six voices -> 0.3 four): modal 110,371 -> 75,616, symp
104,537 -> 75,575, string 116,119 -> 83,862, fm 85,076 -> 61,290, quant 103,547 -> 74,948, verb 94,128 -> 94,843
(it already played four); the batch itself 0.7-2.3 k a block. op1emu (e4e, `det_eng_rings.txt`):
`eng_rings_block=55373/122785` (0.2: 76,056; 0.1: 85,742), every model sounding within ~1 dB of 0.2.

Still open: the upsampler's 2x stage and copies in assembly (~1.6 k a render, bit-identical), the limiter (~47
cycles a sample), the string kernels and the mode bank (assembly already, ~23 cycles a mode-sample).

**Device, 0.3 (owner, 2026-11-10, `coop_stats (23).txt` rings alone, `(24)` with the tape recording):** rings
25,701 a render, 148,180 / 270,880 a block (0.2: 173,719 / 278,457); the callback 330-360 k in the busy windows, peak
603 k alone, 380-395 k and 705 k with the recording. Busy minus idle is ~270 k, of which rings, the voice hook and the
recorder are ~175 k: the rest of the block still runs ~95 k slower beside rings (six voltage voices: ~6 k) [I: code,
data or SDRAM writes; the counters don't tell]. The first render of a block 54 k (27 k code-fetch stalls).

**Tried after 0.3, not kept: the code footprint.** `make -C tests/host/rings footprint` (`code_footprint.py`, an
instruction trace in the GNU simulator, `RINGS_TRACE`) counts rings' 32-byte code lines a block. 0.3, polyphony 4:
5.0 KB (modal) / 7.0 KB (symp) a block while playing with the knobs still, 8.0 / 10.0 KB while knobs move and notes
come fast (the control code), of the 16 KB 4-way cache. Hot code first in its own section (`.text.hot` first in
merge.ld) with the control code apart: no fewer lines, no better spread (laying ~10 KB a block into 16 KB fills the
sets whatever the order). The control code compiled for size (GCC `cold`): 8.0 -> 7.2 / 10.0 -> 9.1 KB busy, nothing
when still, for 2-9 % more compute a block (`RecomputeFilters` slower). Neither is worth a device run; a smaller
footprint needs the control code rewritten (the modal coefficients in compact assembly), and the ~95 k may not be
code at all.

## Building and checking

```sh
make -C plugins                      # build/plugins/rings.coop
make -C tests/host/rings             # bit-identical to te-op1-cfw's engine over a 90 s scenario (halving off)
make -C tests/host/rings halved      # the halved schedule against te-op1-cfw: levels and spectra
make -C tests/host/rings batched     # the batch against the halved schedule (steady state), and the shipped build
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
- 0.3: four voices, 10 modal modes, the batched control work (above).

The host test (`tests/host/rings/rings_compare.cc`) builds te-op1-cfw's own engine and this one from the same seeded
call sequence (4,367 notes, every model and polyphony pair, knob jumps, bends, steals, idle gaps): the outputs are
bit-identical, and a 1-LSB change to a live constant makes them differ.

## m1: where the collateral goes (device, 2026-11-10)

From `firmware/docs/request-rings-collateral.md`; the full tables are in `firmware/docs/m1-results.md`. Cycles per
block, six-note chords, rings 0.3; "collateral" is the callback minus the engine's renders and coop's hook, over the
idle section. `probe_alu` renders as much as rings (149 k a block) with no memory and almost no code, so its collateral
is what **any** engine costs beside its renders here; the rest is what rings' resources add:

| section | renders | hook | collateral | over alu | where the excess is (vs alu) |
|---|---|---|---|---|---|
| voltage (stock) | 59,111 | 19,916 | 50,075 | | |
| probe_alu | 149,308 | 19,401 | 52,012 | 0 | |
| rings modal | 100,172 | 23,648 | 75,152 | +23,140 | coop's thunk +6.1 k, hook +4.2 k, the rest of the DSP +10.3 k, coop's block start / input +3.1 k |
| rings sympathetic | 154,420 | 26,153 | 98,545 | +46,533 | thunk +18.1 k, hook +6.8 k, the stock's per-voice work +6.6 k, DSP rest +11.6 k, block start +6.9 k, dispatcher +3.2 k |
| probe_code 4 KB | 149,812 | 36,238 | 85,044 | +33,032 | thunk +23.9 k, hook +16.8 k (not in the collateral) |
| probe_code 12 KB | 177,071 | 39,543 | 99,003 | +46,991 | thunk +33.2 k, hook +20.1 k, the stock's per-voice work +5.9 k |
| probe_sdram reads, 128 KB | 158,759 | 21,347 | 65,219 | +13,207 | DSP rest +4.5 k, block start +5.0 k |
| probe_sdram writes, 128 KB | 159,061 | 21,647 | 81,934 | +29,922 | DSP rest +12.3 k, block start +10.2 k, master +1.7 k |

What the probes say [C device, this run]:
1. **Code is the larger part.** A render that runs several KB of code evicts coop's own per-voice code between the
   renders (the render thunk, LFO 2's stage 0, the counters and bookkeeping, the voice hook: SDRAM code that runs six
   times a block, once after every render), and it is fetched again after every voice: the hook's code fetch stalls
   go from 3.2 k a block (alu) to 17.9 k (code 4 KB), the thunk grows by 24 k (4 KB) to 33 k (12 KB). Rings' thunk
   and hook grow less (modal +6 k / +4 k, sympathetic +18 k / +7 k): its code eviction is smaller than the 4 KB
   probe's, and its stores (2.) make up the rest. Inside its own render rings is code-bound too: of the sympathetic
   render's 154 k a block, 51 k are code fetch stalls (modal 21 k of 100 k).
2. **Stores are the other part.** The write probe adds 12 k to the stock DSP outside the voices and 10 k to the
   block start (dirty lines written back, the stock's data evicted); rings adds 10-12 k to the DSP rest, as the write
   probe does, where the read probe adds 4.5 k. Region size made little difference (16 KB within ~15 % of 128 KB for
   reads, equal for writes): the probe's 12 KB a block did not stay in the data cache from one block to the next.
3. **Not the dispatcher, not the stock's per-voice work, not the master stage**: each moves 1-7 k.

So rings' ~95 k (request) is in this run: ~52 k that any engine of rings' cost leaves (half of it the stock's own
per-voice work, 25 k, which the request's "unaccounted" included), plus 23 k (modal) / 47 k (sympathetic) from code
and stores. The m1 marks themselves add to the thunk path (op1emu: 2.1 k of thunk a block; device with alu: 15.4 k),
so the thunk figures are high in absolute terms; the differences between sections are what the probes measure.

Next levers, by size:
- **Core: keep coop's per-voice code resident.** The thunk + hook path between renders is SDRAM code refetched six
  times a block once an engine's code is large (+24-40 k for 4-12 KB of engine code). Shrinking it and keeping it
  contiguous, or running it from L1 instruction memory, helps every plugin engine, not only rings.
- **Rings: less code per render** (the control code compact, the sympathetic path's footprint) and fewer SDRAM
  stores in the hot state (the L1 lease for the per-voice state that is written every block).
- The 44.1 kHz voice interface (~5 % of rings' render) comes after both.

### m1b on the device (2026-11-10)

coop's per-voice path in one 2.5 KB section (`docs/m1-results.md` "m1b"): rings sympathetic's callback falls by 33 k a
block (445 k -> 403 k with nitro on FX 1; thunk -12.5 k, hook -5.4 k, rings' own render -12.7 k), modal by 2 k.
What is left over `probe_alu` is mostly stores (the stock DSP outside the voices +10-11 k, the block start +4-7 k),
then coop's thunk +5-6 k: the next rings lever is the L1 lease for its per-voice state written every block.

