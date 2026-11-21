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
  0.4: the scratch holds the modal end weights too (896 bytes), and the lease also takes the engine's per-render
  state and the modal modes (3,296 bytes in all; "0.4" below).
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

## 0.4: fewer SDRAM stores (2026-11-10)

After m1b the rest of rings' excess over `probe_alu` on the device was mostly stores (the write probe's pattern:
`dsp_rest` +10-11 k, `cb_rest` +4-7 k a block). 0.4 moves what rings writes every block out of SDRAM. **The output is
bit-identical with 0.3**; only where the state lives changes. Gates [C host / simulator]: `make -C
tests/host/rings` (te-op1-cfw's engine over the 90 s scenario: with the lease, with every lease refused
(`RINGS_NO_LEASE`), and with 14 modes a voice in L1 so the te-op1-cfw build's "all" moves its modes across the L1 home
at every polyphony change); the shipped build 0.4 against 0.3 with and without the lease (300,505 voice blocks, the
output and the mix); `halfband-check` 0 mismatches.

**Measured first: `make -C tests/host/rings dirty [DIRTY_MODEL=m]`** (`dirty_lines.py`, `rings_compare.cc
-DRINGS_DIRTY`): the shipped build in the GNU simulator compares its `.bss` block to block in 32-byte lines and names
each written line (the symbol, and inside the voice states the member, from the debug info). A lower bound: a store
of the value already there is not seen. 0.3, polyphony 4, the scenario's notes and knob walk: SDRAM lines written a
block (the core's L1 scratch loan and the lease excluded) modal 86, symp 136, string 95, fm 34, quant 134, verb 136.
Modal's were the mode bank's state (`Mode`: lp, bp and the weights, every sample: ~33 lines), the engine's own
per-render state (the upsamplers' history 8.5, the smoothers, the batch's records, the allocator, the block clock,
the screen levels: ~19) and the modal control arrays (~25, every other block). The string models' are the delay
lines (45 lines a block in symp, 1.3-2.6 K distinct lines), the plucker's comb (25), the strings' own scalars: too big
for the lease, they stay.

What moved:
- **The engine's per-render state** (`rings.cc` `Hot`: the upsamplers, smoothers, allocator, the batch's records, the
  block clock, the screen's levels and the voice pointers; 800 bytes) into the L1 lease, `hot_fallback` in `.bss`
  when refused.
- **The modal modes** of each voice state, the first 10 (polyphony 4's count; `OP1RINGS_L1_MODES`), into the lease
  (6 x 10 x 24 = 1,440 bytes): `ModalResonator::set_l1_bank`. While the resolution fits, modes 0-9 live there and the
  voice state keeps only those above; `set_resolution` moves them across when polyphony 1 (60 modes) or 2 (28) is
  struck, and `RingsVoice::CopyFrom` (a steal) keeps each voice state's own L1 home (`RebindBank`).
- **The modal end weights** (`scratch_end_`, 256 bytes a voice state, written every render and read only inside it)
  into the core's per-call L1 scratch (`src/scratch.h` `modal.weights_end`; the Scratch is now 896 of the loan's
  1,312 bytes, a `static_assert` guards it).
- `op1/reverb_bus.h`: the send buffer is zeroed only when a carrier wrote into it (it is otherwise zero already):
  four lines a block in every model but verb that the scan can't see (the same values) [I: from the code].
- The soft-limit table stays leased (now third in line). Init leases most-written first; each part falls back to
  SDRAM on its own. **Rings 0.4 leases 3,296 bytes** (0.3: 1,056); with saw's 192 the engines hold 3,488 of the
  4,096 (`docs/engines.md` "L1").

Result, SDRAM lines written a block (simulator, the same scenario; on the device each is a cache miss now and a
write-back later):

| model | 0.3 | 0.4 |
|---|---|---|
| modal | 86 | 31 |
| symp | 136 | 117 |
| string | 95 | 77 |
| fm | 34 | 17 |
| quant | 134 | 115 |
| verb | 136 | 117 |

What modal still writes: the control arrays (`scratch_start_`, `gain_`, `log2_pf_`, `geometry_`, ~20 lines a block,
6 x ~400 bytes: more than the lease has left). Compute does not move (simulator, `make profile` with the shipped
flags: per render -0.1 to -0.3 %, note on +300 cycles for the moves across the L1 home); `.text` 43,264 -> 44,048
bytes.

**Code per render: measured, nothing cut.** `code_footprint.py` now also prints each function's lines per block and
the blocks without a note on. Shipped build, polyphony 4, knobs still (`FOOTPRINT_FLAGS="-DRINGS_STILL
-DRINGS_NO_JUMP -DRINGS_NO_BEND"`): modal 4.9 KB a block without a note on (6.0 KB with notes), symp 7.4 / 8.3 KB;
0.3 the same within 0.2 KB (the lease's pointer). Of modal's, `Render` is 1.5 KB and runs every line in every block
(each voice's path differs: silent, fading, batch, upsampler, screen), so moving code out of line saves no fetches;
the rest are the kernels and their glue (the mode bank's process 0.64 KB, the half-band and limiter kernels, the
excitation). Symp adds ~2 KB of control code a block even with the knobs still (`StringVoice::SetParameters` 1.2 KB,
`StringFixed::Configure`, `Prepare`): the sympathetic strings' positions follow their LFOs, so the parameters move
every block and can't be skipped bit-identically. The upsampler's remaining C (the 2x stage and copies) is ~0.6 k
cycles of the stage's 1.8 k, not 1.6 k as estimated above: not worth assembly for the code it saves (~100 bytes). A
smaller per-block footprint still needs the control code rewritten (0.3's note), or a lower control rate (the owner's
call).

op1emu (`build/e4e.ldr`, a NAND with only `rings.coop`, `tests/sim/det_eng_rings.txt`) [C op1emu]: `plugins seen 1
loaded 1 failed 0`, `l1 ... leased 3296 leases 3 lease_refused 0 free 1920`, `eng_rings_block=55378/122341` (0.3 the same
way: 55,457 / 122,785, `leased 1056 ... free 4160`; op1emu has no cache, so it can't show the gain), no fault. The device run decides: m1's `run_m1_mode0.txt`
rings sections, against m1b.

### 0.4: the 44.1 kHz voice

Rings 0.4 also renders its voice at 44.1 kHz (`OP1RINGS_VOICE_44K=1` and `"voice_rate": 44100` in `plugin.json`, the
core's `COOP_ENGINE_F_44K`, `firmware/docs/engines.md` "The 44.1 kHz voice"): `Upsampler::Process` writes the
half-band's 64 outputs and drops the Lagrange 2x stage, the fade runs over 64 samples at twice the step a sample (the
same length), and the screen's peak is taken over the 64. The core repeats each sample before the stock's half-band
decimator. `make -C tests/host/rings voice44` [C host]: over the 90 s scenario the 44.1 kHz samples are the 88.2 kHz
build's even samples bit for bit (19,232,320 samples, 0 differ); the odd ones were the Lagrange stage's, so what the
OP-1 hears differs by the two interpolators' responses above the decimator, about 0.2 dB at 10 kHz [I, derived]. A
core older than the 44.1 kHz voice refuses this file ("engine flags not supported yet").
Cost [C simulator, `make profile`, the shipped flags, 300,505 renders]: the upsampler 1,570 -> 1,056 cycles a render
(-514; the README's earlier ~1.6 k estimate was high), less the core's repeat (~140 [I]): about -0.4 k a render.

## 0.5: polyphony "all" again, controls that wrap (2026-11-10)

The owner's choice after the m1c run: rings is left at 0.4's performance, with:
- **Polyphony 1, 2, 4, all** (fn + green, `OP1RINGS_COOP_POLY_ALL=1`, `op1/settings.h` `kCoopPolyOrder`): "all" is
  te-op1-cfw's six voices of 14 modes again (0.3 had folded it into 4); 4 keeps 0.3's 10 modes. Default 4. The
  word holds the step index, so a sound saved with 0.3 / 0.4 (0 = 4, 1 = 1, 2 = 2) now opens as 1, 2, 4.
- **fn + blue and fn + green wrap** (`"wrap": true`, coop e5): modal ... verb, modal; 1, 2, 4, all, 1. An older coop
  core loads 0.5 too, with turns that stop at the ends.
- [C host] With the new order the settings play bit for bit what 0.4 played (models 0-2, 6,000 blocks each: new 1, 2,
  4 against 0.4's 1, 2, 4, and new "all" against the build without the 4-voice cap); "all" differs from 4.
- "all" costs the most: six voices of 14 modes, beside effects or tape recording expect crackles (0.2's device runs).

## 0.6: a new model or polyphony cuts the old voices (2026-11-13)

On an Opus 2 OP-1, a model change while notes were still ringing, then new notes, overran the audio block and the
core switched rings off: each voice keeps the model and mode count it was struck with, so the old setting's rings
(60 modes at polyphony 1) played on beside the new setting's voices. Now each voice remembers the setting it was
struck with (`struck_setting`), and when the sound's model or polyphony changes, every voice struck with another
one is cut off at its next render (`FollowSetting`, `allocator.Forget`: silent, out of the voice list, reset when
next struck). The shared reverb clears itself after 50 ms without a carrier, as before.

## 0.7: the heavy settings lighter (2026-11-13)

On an Opus 2 OP-1, chords at polyphony 1 and 2 after a model change still overran the audio block (a voice's block
up to 270,000 cycles, a note on 170,000: the steal's copy of an eight-string or 60-mode voice). Opus's host starts a
chord's notes one a block, so each note had rendered once before the next stole it, and every steal faded and copied
a full voice. `op1/voice_allocator.h` now: a voice struck within 8 renders (11.6 ms) counts as fresh, so a chord's
steals go silent at once with no copy (the ring goes on in the new voice, as Rings' own polyphony does); one fade at
a time (was two), 2.9 ms (was 5.8). A stolen voice's tail is shorter at polyphony 1, 2 and 4; "all" is unchanged.

## 0.8: a string's line cleared as it is reached (2026-11-14)

On an Opus 2 OP-1, one note of sympathetic strings at polyphony 1 took 598,000 cycles in its first block (the block
is 580,525) and rings was switched off: the strike's `StringVoice::Configure` cleared all 2,052 words of eight
strings' delay lines (64 KB of SDRAM). `StringFixed::Init` now clears nothing; `clean_` counts the line above the
write position that is the string's own, and before each block `Unstale` clears what the block can reach that isn't
(its delays, half again and 16, the block; and the line's oldest slots, which a delay under two samples reads).
A typical note clears a few hundred words a string, spread over its first blocks. [C host] The output is the same as
clearing the whole line at Init (a line filled with 2^27 at Init plays the same, bit for bit), except after a steal:
the stealing string's line beyond the copied samples is now cleared instead of left with its own older note.

## 0.9: the voice states in Opus's audio pool (2026-11-14)

Built for Opus 2 (op1-plugins' adapter defines `COPA_OPUS`), the voice states and the shared reverb (~572 KB) are
no longer .bss: `Init` takes them from `copa_alloc_audio` (the adapter's: `opus_alloc_audio`, else `opus_alloc`,
zeroed as .bss is) and refuses the file without them. Opus loads a plugin's code and zero-filled data into whichever
pool has room; on the OP-1 rings landed in the far pool (Opus's figures: bank 2), which the display shares and which
costs about twice as much while it sends a frame, and a note of sympathetic strings at polyphony 1 took a whole
audio block. The audio pool (1.3 MB) is not shared with the display. coop's build is unchanged.

## 0.10: FM's five divides a render folded (2026-11-20)

`FmVoiceFixed::Process` divided five times by its block size for the per-sample steps (carrier, modulator,
brightness, feedback twice): five `___divsi3` calls a render, ~190 cycles each on this CPU
(`docs/efficiency-audit.md` finding 8). The size is `kSubblockSize` = 32 in every build of this port, so the
quotients are now the constant's (an add and a shift each), with the general divide kept for any other size, as
`modal_resonator.cc` already does. The same truncating quotient, so the same samples. [C simulator] `make -C
tests/host/rings profile PROFILE_BLOCKS=6000 PROFILE_MODEL='3 0'` (FM, polyphony all): the fm stage 7,637 cycles a
render (0.9: 8,548), the heaviest render 14,810 (15,726): 911 less a voice, 3.6-5.5 k a block at four to six voices;
the two builds' output files are identical. [C host] the 90 s scenario's output (`rings_coop`, every model) is 0.9's
byte for byte. File 113,816 bytes, `.text` 45,072; imports unchanged.

**The reverb model's idle path (`docs/efficiency-audit.md` finding 6): not done in 0.10, and why (done in 0.11,
below, by the first of the two ways).** The core renders a voice
for as long as its key is down, so in string + reverb the shared reverb, and each held voice's limiter and upsampler,
run on after the string has gone to sleep. The plan was to stop them once the reverb's tail is under the voices' own
sleep level (half a 16-bit LSB). [C host, measured with the build as it ships: a chord struck and held 24 s] the
tail never gets there: from about 8 s on, the one voice that carries the reverb gives a steady **-52 LSB with 2 LSB
of ripple** (the reverb's wet peak stays near 197,000 in Q27, 48 LSB), for as long as the key is held. It is the
reverb's rounding: its 16-bit buffer floors every write (te-op1-cfw's fixed version does the same; upstream
truncates towards zero), and the bias adds up round the loop. Cutting an offset of 52 LSB to nothing is neither under
the idle threshold nor silent if it is done at once (a step the core's DC blocker turns into a thump), so the reverb
is left as it is. Two ways on, both the owner's decision: (1) round the reverb's writes without the bias (not
bit-identical to te-op1-cfw's reverb any more; the tail would then die and the idle state is one condition in
`rings.cc`: an asleep carrier of an idle reverb renders as silence, as a sleeping voice of any other model does, the
reverb then restarts empty with the next note exactly as after all keys were up); (2) keep the rounding and fade
the offset out over some tens of ms when the ripple has gone, which changes the output by those 52 LSB of DC.

Found while checking: `make -C tests/host/rings` (this engine against te-op1-cfw's) no longer reports bit-identical
at HEAD before this change (the two outputs first differ at byte 3,786,753 of the 90 s scenario); 0.10 gives the
same bytes as 0.9 there, so the difference is older than this version and is not looked into here.

## 0.11: the reverb's tail dies, and a held chord idles (2026-11-21)

**What changed in the sound.** In string + reverb, a held key used to leave the output on a faint constant offset
once the note had died (-52 LSB with the default patch, -127 with more damping), for as long as the key was down:
0.10's note above. That offset is gone: the reverb's tail now fades to digital silence. The reverb itself sounds as
it did (the same decay, the same level). The other five models are untouched. The owner's decision (2026-11-21):
option 1 of 0.10's note.

- **The rounding** (`src/reverb_fixed.cc` `Compress`, `src/reverb_bfin.S` `COMPRESS`): what the reverb writes to
  its 16-bit buffer is truncated towards zero, as upstream Rings does, where 0.10 (and te-op1-cfw's fixed-point
  version) floored. Floored, every write leaned half an LSB down and the lean went round the loop. Rounding to
  nearest would have removed the lean but left a dead band (a tail that stops falling a few LSB up and stays);
  towards zero a value can only come back smaller, so the tail ends in exact zeros. Three instructions more a
  write, ten writes a sample.
- **The idle state** (`op1/reverb_bus.h`, `rings.cc`): the core renders a voice for as long as its key is down, so
  the reverb, and each held voice's limiter and upsampler, ran on after the sound had gone. Now, 207 runs in a row
  (0.3 s) with nothing sent (no carrier's string awake) and an all-zero wet block, and the reverb is no longer run;
  a held voice whose string is asleep and whose limiter has come to rest then renders as silence, as a sleeping
  voice of any other model does. The voice's smoother and the bus's block clock still run. The block in which a
  string wakes (a new note) ends it: the reverb takes up empty, which is what it held, with its write pointer and
  its two LFOs where the skipped blocks would have left them (`ReverbFixed::Resume`), so the modulation keeps its
  phase. It never idles while anything is sent to it.
- **Proof** (`make -C tests/host/rings idle OLD=<0.10's folder>`, `rings_idle.cc`: four voices, every key held
  throughout; a note every 0.2 s for 10 s, a chord held, then a soft and a loud note into it):
  - As it ships against its twin that never idles (`-DOP1RINGS_NO_REVERB_IDLE`): **the same sample for sample** in
    every part, 0 samples differ, with the chord held 24 s (damping 24,000) and 100 s (damping 29,000): before the
    idle state, in it, on the note that wakes it and after. No idle render while notes play or in the half second
    after the soft note.
  - The held chord's last non-zero sample comes 14.5 s after it is struck (damping 24,000), the idle state 0.3 s
    later (14.8 s); with damping 29,000 at 42.0 s and 42.3 s. With damping at the top the string itself rings for
    more than four minutes (never idle, as it should be).
  - Against 0.10, the four voices summed, 16-bit LSB:

| Part (damping 24,000) | 0.10's offset (the mean difference) | the rest of the difference, RMS | the signal's RMS |
|---|---|---|---|
| a note every 0.2 s | 39.7 | 15.0 | 6,837 |
| the chord held, first 3 s | 49.9 | 6.1 | 2,865 |
| the chord held, its tail | 51.0 | 4.7 | 194 |
| the chord held, after the tail | 52.7 | 0.8 | 0 (0.10: 52.7) |
| the soft note | 51.1 | 10.4 | 2,512 |
| the loud note | 48.9 | 11.4 | 1,069 |

    The largest single difference is 82 LSB (damping 29,000: 191, with 0.10 resting on -127). Decay: the held
    chord's swing falls under 2,048 LSB at 3.74 s in both, under 256 at 6.99 s in both, under 32 at 10.48 s (0.10:
    10.73 s); with damping 29,000 at 9.49 / 17.72 / 28.21 s (0.10: 9.49 / 17.97 / 28.96 s): the same to within 0.25 s
    at the first two levels (the test's window) and 0.75 s at the last, where 0.10 is on its offset.
  - The reverb alone (`rings_idle reverb`: a quarter second of noise, then ten minutes of silence): exact zero for
    good 5.5 s after the noise at the test patch's reverb time (0.81) and 21.5 s at the longest (0.98); nothing
    after, no limit cycle. 0.10 on the same: never zero, resting on a mean of -92 LSB (0.81) and -887 LSB (0.98) of
    its wet output.
  - The other models (`make models OLD=...`): modal, sympathetic, string, FM and quantised, 8,000 blocks each at
    polyphony "all" and 4: the same bytes as 0.10.
  - The assembly against the C: the string + reverb model's 6,000-block scenario in the simulator (the reverb's
    kernel) and on the host (its C loop) give the same file. `tools/asm_lint.py`: 0 packets flagged.
- **Cost** (simulator, compute only). Playing string + reverb: 86,086 cycles a block on `make profile
  PROFILE_MODEL='5 0'` (0.10: 84,649): +1.4 k a block, the rounding's instructions and the idle state's two scans of
  32 words; the render that runs the reverb 38,673 at most (37,106). A block of four held voices
  (`make idle-sim`): playing 24,747; the chord asleep, the reverb still running 15,510 (what 0.10 paid for as long
  as the keys were down); **idle 2,518** on average, 11,311 at most (the blocks the batched control work falls in).
  The block of the note that wakes costs 33,548 (29,892 without an idle time before it): the reverb's first 0.14 s
  after it run in its C loop, as after any restart.
- **What the engine cannot save.** The core still calls `render` for every held voice (the 2.5 k above is four
  calls: the settings, the block clock and the batch, the smoother, the bus, 64 zeros), and then runs the stock
  envelope, coop's stereo placement and the mix on those zeros. For that to go, the core would need to know a
  voice gave silence: a return value or a flag from `render`, or the core testing the 64 samples itself
  (`docs/efficiency-audit.md` finding 5 is the same question for the whole instrument).
- te-op1-cfw's reverb floors, so string + reverb is no longer bit-identical to it by design (the other models'
  comparison with it: see 0.10's note on `make -C tests/host/rings`).
- File 114,948 bytes, `.text` 46,148, `.bss` 572,544; imports unchanged.

## Building and checking

```sh
make -C plugins                      # build/plugins/rings.coop
make -C tests/host/rings             # bit-identical to te-op1-cfw's engine over a 90 s scenario (halving off),
                                     # with the L1 lease, with every lease refused, and with 14 modes in L1
make -C tests/host/rings halved      # the halved schedule against te-op1-cfw: levels and spectra
make -C tests/host/rings batched     # the batch against the halved schedule (steady state), and the shipped build
make -C tests/host/rings halfband-check   # the upsampler kernel against its C loop, in the simulator
make -C tests/host/rings profile     # cycles per render by model and stage, in the simulator
make -C tests/host/rings footprint   # rings' code lines a block (FOOTPRINT_MODEL, FOOTPRINT_FLAGS)
make -C tests/host/rings dirty       # rings' SDRAM lines written a block, by owner (DIRTY_MODEL)
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

### m1c on the device (2026-11-10): 0.4 with the three levers

`firmware/docs/m1-results.md` "m1c, second run", nitro on FX 1: the callback beside rings modal 326 k a block (0.3 on
m1: 364 k), sympathetic 391 k (445 k), knobs turning 370 k (m1b 395 k). Peaks: modal 554 k (under the 580 k budget),
sympathetic 684 k and knobs turning 602 k (over: crackles possible). Over probe_alu, sympathetic still leaves +34 k:
its strings' delay lines (too big for the lease) and its per-block control code (`SetParameters`, `Configure`).

