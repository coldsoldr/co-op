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

## 0.12: a tail that stalls under 4 LSB sleeps (2026-11-21)

**Why.** On the owner's OP-1 (rings 0.11, core e13) a chord held 75 s in string + reverb cost the same in every
5-second window (`peng` 158-160 k cycles a block), although it "sounded like it had died down". What the source and
the host say about that [C host, simulator; the device's settings are not known]:

- **When a voice sleeps (0.11, every model).** `RingsVoice::asleep()`: the voice's own output (Q27, before the
  reverb's share and the limiter) has peaked under 2,048 (half an LSB of the 16-bit output, -96 dBFS) for 8 renders
  in a row (12 ms), and no strike is pending. Asleep, a voice of the five models without reverb renders zeros at
  once (`rings.cc`). In string + reverb the string stops, the voice goes on playing its share of the reverb, and
  0.11's idle state ends that: 207 runs (0.3 s) with nothing sent and an all-zero wet block, and the voice's limiter
  at rest. A new note wakes the voice in the block it arrives.
- **What a held key feeds.** Nothing. The core calls `note_on` once, when the voice starts
  (`core/sdram/engine_host.cc` `voice_start`), and then `render` every block with the pitch words and the four
  controls; rings has no audio input and does not use the velocity. Nothing on the device path strikes a held
  string again.
- **So the sleep already follows the 16-bit output.** Six keys held, the default patch: the last non-zero output
  sample comes 17.1 s after the strike and the last voice renders silence from 17.4 s (the reverb's 0.3 s). The
  simulator, with the assembly kernels, gives the same samples as the host over those 25 s and the same times; its
  cost falls from 67.7 k cycles a block (first 5 s) through 50 k (10-15 s, the strings going to sleep one by one) to
  5.6 k asleep. A rule "sleep once the 16-bit output has been exactly zero" would sleep later than this, or never:
  the upsampler floors (`in >> 12`), so a tail far under one LSB still gives 0 and -1, and a twin that never sleeps
  (`-DOP1RINGS_NO_SLEEP`) plays 0/-1 from every voice for good.
- **A flat cost for 75 s needs a tail that is really that long.** String + reverb, six keys ("all" plays four),
  the other three controls at their defaults: time until the last voice is asleep

| damping | 16,384 | 22,937 (default) | 26,000 | 28,000 | 29,500 | 30,000 | 31,000 | 32,000 and up |
|---|---|---|---|---|---|---|---|---|
| asleep after | 11.6 s | 17.4 s | 20.7 s | 39.3 s | 67.1 s | 90.9 s | 176 s | not in 300 s |

  From about 29,800 (the top tenth of the control) all four strings are awake for 75 s. At 30,000 the four voices'
  sum peaks at 84 LSB (-52 dBFS) around 60 s and 18 LSB (-65 dBFS) at 75 s: died down to the ear, not to the
  arithmetic. An exponential tail spends the last third of its life under -60 dBFS. Structure and register move the
  table (damping 28,000: 16.9 s with the structure at the top, 28.7 s with position 0, 52.6 s two octaves down; with
  the structure at 0 it never ends, below). That is the likely cause on the device, and 0.12 does not change it:
  those strings still sound in 16 bits, and cutting them is a change of the sound (rings' Test 29 found -72 dBFS
  cut tails short).
- **What the search did find: tails that never end.** A tail does not always fall to zero. The strings and the
  modes can settle in a small cycle that the rounding keeps alive, and the FM voice's envelope stops falling at a
  floor set by the damping. Held, such a voice computed at full cost for as long as the key was down. Seen from
  damping 26,000 up, mostly with the structure away from its default: FM, structure 0 or 16,384, damping 26,000 and
  28,000: all six voices on 1 and 2 LSB for good; string + reverb, structure 0, damping 28,000: the top voice on
  -3..2 LSB, and the reverb fed by it never idle; modal, brightness at the top, damping 28,000: one voice on 2 to 3 LSB.
  (Higher still, the same floors are loud and meant: FM from damping 29,500 holds its note, strings at structure 0
  hold theirs.)

**What changed.** `src/op1_voice.h`, `op1_voice.cc`: a second way to sleep. A voice whose own output has peaked
under `kQuietLevel` (16,384 in Q27: 4 LSB of the 16-bit output, -78 dBFS) for `kQuietRendersToSleep` renders in a row
(6,890: 10 s) is asleep, like one under half an LSB for 12 ms. Everything after that is 0.11's: silence at once in
five models, the reverb's share and then the idle state in string + reverb. A strike wakes it as before and the
count starts again. One compare and one add a render.

- A tail that is still falling crosses from 4 LSB to half an LSB (18 dB) inside 10 s unless the whole decay takes
  about a minute or more, so it ends where it ended in 0.11. A slower one is cut 10 s after it went under 4 LSB,
  somewhere between 4 and half an LSB (modal, damping 28,000: asleep at 62.7 s where 0.11 slept at 69.3 s; at the
  top 217 s for 251 s).
- A stalled voice above 4 LSB is left alone (string + reverb, structure 0, brightness at the top, damping 28,000:
  -15..13 LSB for good). The level is one constant.
- What is frozen while a voice sleeps is what 0.11 froze: the string or the modes as they stood. Struck again, the
  voice starts from that, at most 4 LSB of it.

**Proof** (`make -C tests/host/rings sleep OLD=<0.11's folder>`, `rings_sleep.cc`: six keys struck three blocks
apart and held, every voice rendered every block, the 16-bit output compared render by render):

- As it ships against its twin with 0.11's rule alone (`-DOP1RINGS_NO_QUIET_SLEEP`), the default patch, 60 s, voice
  0 struck again every 7 s and voice 1 at 40 s: **the same sample for sample**, all six models at the four
  polyphony settings (24 runs); the same at damping 28,000 in 23 of 24 (modal at polyphony 4: 2 LSB from 55.9 s, a
  voice the twin leaves on its cycle). The twin against 0.11's sources: the same samples in all 48.
- Where tails stall (models x polyphony 4 and "all" x structure 0 and 8,192 x brightness 19,660 and 32,767 x
  damping 26,000 to 31,000, 150 s held, no second strike: 192 runs): 149 the same sample for sample; 43 differ, by
  **at most 5 LSB** in one voice (12 by 1, 10 by 2, 11 by 3, 6 by 4, 4 by 5), from the moment the voice sleeps: the
  stalled tail the twin goes on playing.
- A strike into a voice that slept this way (the same 192 with voice 0 struck again every 40 s and voice 1 at
  100 s): the samples after it are not the twin's and cannot be: the strings' noise comes from one generator that
  every awake string draws from, as upstream's, and the FM voice's phases stood still. That was already so for
  0.11's own sleep. The level agrees: the summed output's RMS over 0.1 s windows above -54 dBFS is the same in 156
  of 192; in the rest the mean difference is 0.1 to 1.8 dB, the largest single window 9.1 dB (string + reverb). The
  same measure between 0.11 and a twin that never sleeps: mean up to 6.3 dB, largest window 25.5 dB.
- `make idle` (0.11's test, the reverb's idle state against its twin): 0 samples differ, as before. `make models
  OLD=<0.11>`: the five models without reverb give 0.11's bytes on that scenario.
- The assembly is untouched. Simulator against host, string + reverb, 25 s held: the same file, and the same file
  as 0.11's.

**Cost** (simulator, compute only, cycles a block for six held keys, `make sleep-sim`).

| six keys held | 0.11 | 0.12 |
|---|---|---|
| string + reverb, default patch: playing (first 5 s) | 67,717 | 67,889 |
| the same, asleep (from 17.4 s) | 5,627 | 5,619 |
| string or FM, asleep (five models: 426 a voice) | 2,545 | 2,557 |
| FM, structure 0, damping 26,000: playing | 74,786 | 74,915 |
| the same from 21.5 s (all six on 1 LSB) | **74,864, for as long as the keys are down** | **2,701** |
| string + reverb, structure 0, damping 28,000: playing | 88,642 | 88,804 |
| the same from 69 s (the top voice on -3..2 LSB) | 51,037 at 75-80 s (one string and the reverb never stop) | **5,671** |

Awake, the count costs 20 to 40 cycles a render. Asleep, a voice still costs its render call: the engine's block
clock, the control smoother, and 64 zeros written to `out`. **Every sample of a sleeping voice's block is zero**,
written by the engine (the block is not left untouched), which is what the core's e14 test of the render's samples
needs to see.

**The same imports as 0.11** (it loads on e10 and later). Not on the device yet; `docs/device-tests.md` "rings
0.12".

## 0.13: the early cut, a quiet tail is faded out and sleeps (2026-11-21)

**What a listener may notice.** The very end of a long tail is now a one-second fade to silence instead of the last
stretch of its own decay. A voice (one key's string, modes or FM pair) that has stayed quieter than **-66 dBFS** for
**5 seconds** is faded out over **1 second** and then stops, whether its key is still down or not. -66 dBFS is a peak
of 16 steps of the 16-bit output, 66 dB under full scale; a strike peaks within a few dB of it. Nothing changes while a
voice is louder than that, and a note struck during those seconds plays exactly as before. The owner's decision
(2026-11-21): a chord at a long damping cost its full voices for a minute and more after it could no longer be
heard, and 0.12 only cut tails under 4 LSB (-78 dBFS). The level is one constant, to be moved up or down after a
hearing test; a second file with **-60 dBFS** is built beside this one for that test.

- **The level is per voice, not of the chord.** Six voices near the level together sum higher: in the modal model
  the six keys of a chord reach the level within half a second of each other, and their fades begin with the summed
  output at about -57 dBFS (-50 dBFS in the -60 build). That is the case to listen to.
- **The reverb is not cut.** In string + reverb a faded string stops feeding the reverb over the same second; the
  reverb then runs its own tail out, as it did since 0.11 (it idles 0.3 s after its last non-zero sample). With the
  damping at the top that tail is 10 s long (26 s after a chord with the structure at the top). No level rule is
  applied to it: measured, it never outlasts the strings by more than that, with nothing sent it costs a quarter of
  what the playing chord costs (0.11's figures: 15.5 k cycles a block for 68 k), it ends by itself, and its decay is
  the smooth end the cut needs.
- **What it does not do: shorten a loud tail.** With the damping at the very top the strings ring for minutes at a
  level anyone hears: six keys, the default patch otherwise, the summed output peaks at -12 dBFS after 60 s and -13
  dBFS after 75 s in string + reverb (-8.5 and -10 dBFS in the string model), and between -3 and -27 dBFS after 75 s
  with the brightness at 0, a quarter or the top, or the structure at 0 or the middle; only the structure at the top
  ends inside a minute. Those voices stay awake in 0.13 as in 0.12, and so does their cost. So if a chord at the
  longest damping was "no longer heard" after a minute on the device, it was not rings' output that had gone quiet
  [C host; I for the device]: what comes after the engine (the OP-1's envelope and levels) the engine does not
  see, and a voice silenced there still costs its full render.

**The rule** (`src/op1_voice.h`, every constant in one place; `op1_voice.cc` `RenderMono`):

| constant | value | meaning |
|---|---|---|
| `kEarlyLevel` (`OP1RINGS_EARLY_LEVEL`) | 67,268 (Q27) | -66 dBFS: 16.4 LSB peak of the 16-bit output. The -60 build: 134,218, 32.8 LSB |
| `kEarlyHoldRenders` | 3,445 renders | 5 s under the level, without one render at or above it |
| `kEarlyFadeRenders` | 689 renders | the fade: 1 s, linear, 48,701 of 2^30 a sample at 22.05 kHz |
| `kEarlyFadeInSpeed` | 32 | back up 32 times faster (31 ms) when the level returns with no strike |
| `-DOP1RINGS_NO_EARLY_SLEEP` | | the rule's code is not compiled: the build is 0.12 |
| `-DOP1RINGS_EARLY_FREEZE=1` | | a faded voice keeps its state (the test's other twin) |

- The level is measured where 0.11 and 0.12 measure theirs: the peak of each render (1.45 ms) of the voice's own
  output, before the reverb's share and the limiter. A peak, and five seconds of it, so partials that beat through
  zero do not start a fade. It is compared at the scale of the voice's 16-bit output: the own peak times the model's
  gain (1.12 modal and string, 0.56 FM, 0.8 sympathetic), so -66 dBFS is 60,060 of own peak for the modes and the
  string, 120,121 for FM, 84,085 for the sympathetic strings. In string + reverb the own peak is the voice's dry
  part (the string times 1 - the reverb's amount), what the voice itself plays.
- One render at or above the level starts the five seconds again. If that happens during the fade (a control moved:
  the FM voice's envelope rises again when the damping is turned up), the gain goes back to full in 31 ms.
- The fade is a gain on the voice's own output, applied after the level is taken and before the send to the reverb
  and the limiter. The model under it runs on untouched, so nothing in the fade depends on the model.
- At the end of the fade the voice is asleep, as 0.12's sleeping voices are: zeros at once in the five models
  without reverb (all 64 samples of the render written), and in string + reverb the reverb's share until the bus
  idles. 0.11's rule (under half an LSB for 12 ms) and 0.12's (under 4 LSB for 10 s) are still there underneath;
  0.12's can no longer come first.
- **A strike during the hold or the fade** sets the gain to full at once: the voice plays what it would have played
  without the rule (the fade never touched the model). The gain's jump is at most the level, under the strike.
- **A strike into a voice that was faded out: the string starts from rest.** Its modes, its exciter or its strings
  are cleared (`RingsVoice::Rest`); in a steal the new key takes the cleared string. A string that has died away is
  at rest, and that is what the fade said it was; kept, what the string held at the end of the fade (up to the
  level) would come back at full gain with the strike, from wherever it stood. Measured both ways (below): the two
  differ by 2 to 11 LSB in the first tenth of a second after the strike and by 0.00 dB in level, with the same first
  sample; cleared is the one with nothing left over. The FM voice is not cleared: an oscillator has no rest, its
  envelope fell and the strike raises it, phase unbroken. Voices put to sleep by 0.11's or 0.12's rule keep their
  state as before.

**When each build sleeps** (`make -C tests/host/rings early-table`: six keys C3 G3 C4 E4 G4 C5 struck together and
held; the default patch but for the damping; seconds after the strike until every voice is asleep; `>` = still
awake when the run ends):

| six keys, damping | 0.12 | 0.13 (-66) | the -60 build |
|---|---|---|---|
| string + reverb, 22,937 (default) | 17.1 | 16.9 | 15.6 |
| string + reverb, 28,000 | 39.0 | 30.7 | 27.5 |
| string + reverb, 30,000 | 90.2 | 72.3 | 63.7 |
| string + reverb, 31,000 | 171.7 | 144.3 | 126.7 |
| string + reverb, 32,767 (the top) | 322.0 | 322.0 | 322.0 |
| string, 22,937 | 10.0 | 10.0 | 10.0 |
| string, 28,000 | 16.9 | 16.7 | 15.6 |
| string, 30,000 | 20.4 | 19.0 | 17.5 |
| string, 31,000 | 21.2 | 19.5 | 17.9 |
| string, 32,767 | > 480 | > 480 | > 480 |
| modal, 22,937 | 16.9 | 15.8 | 14.3 |
| modal, 28,000 | 62.8 | 46.7 | 40.6 |
| modal, 30,000 | 104.6 | 77.5 | 66.9 |
| modal, 32,767 | > 150 | > 150 | 136.7 |
| sympathetic, 28,000 / 30,000 | 21.2 / 23.3 | 21.2 / 23.3 | 21.2 / 23.0 |
| quantised, 28,000 / 30,000 | 18.2 / 21.5 | 18.0 / 19.3 | 16.8 / 18.0 |
| FM, 22,937 / 28,000 | 6.8 / 16.1 | 6.8 / 16.1 | 6.8 / 16.1 |

String + reverb plays four strings for six keys (the keys C4 E4 G4 C5 keep theirs), and the higher a string the
sooner it is done. Each string, seconds after the strike, 0.12 / -66 / -60:

| damping | C5 | G4 | E4 | C4 |
|---|---|---|---|---|
| 28,000 | 17.4 / 16.2 / 14.7 | 29.2 / 24.1 / 21.8 | 30.8 / 24.8 / 22.2 | 39.0 / 30.7 / 27.5 |
| 30,000 | 34.5 / 29.5 / 25.9 | 61.0 / 51.1 / 45.4 | 69.5 / 54.4 / 47.8 | 90.2 / 72.3 / 63.7 |
| 31,000 | 56.3 / 44.9 / 39.2 | 108.6 / 88.3 / 78.0 | 122.8 / 99.6 / 87.3 | 171.7 / 144.3 / 126.7 |
| 32,767 | 97.8 / 77.0 / 67.0 | 214.9 / 199.3 / 176.3 | 255.4 / 255.4 / 254.5 | 322.0 / 322.0 / 322.0 |

At the top two things show that have nothing to do with 0.13. The strings are loud for minutes (above). And a
string there does not always die away smoothly: after minutes of a slow, nearly linear decay it can collapse to
zero within 50 to 400 ms from well above the level (the string model's E4 from 43 LSB, -57 dBFS, in 50 ms at
264.6 s; G3 and C4 at 453.1 and 335.7 s; string + reverb's E4 and C4 at 255.4 and 322.0 s), in 0.12 as in 0.13.
That is the string's own arithmetic, not a rule of the engine: the level never spends five seconds under -66 dBFS
there, so no fade comes first. [C host; why it collapses just there was not looked into.]

The other models at the top: FM from damping 29,500 holds its note for as long as the key is down, by design; the
sympathetic and quantised strings ring past 150 s at -3 to -14 dBFS; none of these is under the level, so none is
cut.

**The summed 16-bit output when a fade starts and when it ends** (the peak of the six voices' sum in the quarter
second before; the first strings to fade do so under the others, which are still loud):

| six keys | -66: start / end | -60: start / end |
|---|---|---|
| string + reverb, 28,000: the first string (C5) | -41.4 / -44.7 dBFS | -39.0 / -39.8 |
| string + reverb, 28,000: the last (C4) | -71.2 / -76.3 | -66.8 / -74.7 |
| string + reverb, 30,000: the first | -29.4 / -30.7 | -32.3 / -28.5 |
| string + reverb, 30,000: the last | -66.8 / -74.7 | -57.2 / -68.7 |
| string + reverb, 31,000: the last | -63.5 / -71.2 | -56.5 / -64.3 |
| string, 30,000: the last (C3) | -80.8 / -90.3 | -76.3 / -90.3 |
| modal, 28,000: the six within 0.3 s | -57 / -65 to -72 | -50 / -55 to -76 |
| modal, 30,000: the six within 1.5 s | -54 to -58 / -57 to -78 | -50 to -57 / -52 to -75 |

A fade starts five seconds after the voice went under the level, so the faster a tail falls the lower it is by
then. In the string model a string under the level is nearly done anyway: the fade often ends on 0.11's rule before
its second is over, and the cut saves a second or two. In string + reverb and the modal model the tail under the
level is long, and that is where the time goes.

**A single held note** (C3), seconds until it sleeps, and the output's peak when its fade starts / ends:

| one key, damping | 0.12 | 0.13 (-66) | the -60 build |
|---|---|---|---|
| string + reverb, 22,937 | 18.8 | 17.6 (-84 / -90 dBFS) | 16.1 (-78 / -90) |
| string + reverb, 28,000 | 44.6 | 33.8 (-72 / -84) | 30.2 (-67 / -81) |
| string + reverb, 30,000 | 107.6 | 84.7 (-67 / -81) | 74.4 (-61 / -69) |
| string + reverb, 31,000 | 226.8 | 183.1 (-66 / -78) | 159.5 (-61 / -71) |
| string + reverb, 32,767 | > 480 | > 480 | > 480 |
| string, 28,000 | 16.9 | 16.7 | 15.5 |
| string, 30,000 | 20.4 | 19.0 (-84 / -90) | 17.5 (-78 / -90) |
| string, 32,767 | > 480 | > 480 | > 480 |

**Cost** (`make -C tests/host/rings early-sim`: the simulator, compute only, cycles a block averaged over each 5 s,
six keys held; on the device a block costs more, about 160 k in the 0.11 run where the simulator says 68 k, and the
times are the same):

| string + reverb, damping 30,000: seconds after the strike | 0.12 | 0.13 (-66) | the -60 build |
|---|---|---|---|
| 0-25 (four strings and the reverb) | 67,751 | 68,275 | 68,279 |
| 25-30 | 67,754 | 67,634 | 61,069 |
| 30-35 | 66,947 | 59,054 | 59,057 |
| 35-45 (three strings) | 58,640 | 59,050 | 59,160 |
| 45-50 | 58,640 | 59,107 | 46,793 |
| 50-55 | 58,643 | 51,342 | 40,603 |
| 55-60 (-66, -60: one string) | 58,685 | 40,602 | 40,617 |
| 60-65 | 51,446 | 40,593 | 36,410 |
| 65-70 | 48,640 | 40,622 | 5,749 |
| 70-75 | 40,418 | 23,491 | **5,613** |
| 75-90 | 40,440 | **5,613** | 5,613 |
| 90-95 | 8,553 | 5,613 | 5,613 |
| from 95 (every render silent) | **5,613** | 5,613 | 5,613 |

| the same in fewer words: cycles a block, and until when | 0.12 | 0.13 (-66) | the -60 build |
|---|---|---|---|
| string + reverb, 28,000 | 67.8 k to 15 s, 57-63 k to 30 s, 5.6 k from 40 s | 68.3 k to 15 s, 5.6 k from 35 s | 68.3 k to 10 s, 5.6 k from 30 s |
| string + reverb, 31,000 | 67.8 k to 55 s, 58.6 k to 105 s, 40.4 k to 170 s, 5.6 k from 175 s | 68.3 k to 45 s, 59.0 k to 85 s, 40.6 k to 140 s, 5.6 k from 145 s | 68.3 k to 35 s, 59.0 k to 75 s, 40.6 k to 125 s, 5.6 k from 130 s |
| string + reverb, 32,767 (the top) | 67.8 k to 95 s, then 58.7 k (to the run's end, 130 s) | 68.3 k to 75 s, then 59.1 k | 68.3 k to 65 s, then 59.1 k |
| string, 30,000 | 82.1 k, 2.6 k from 25 s | 82.9 k, 2.6 k from 20 s | 82.9 k, 2.7 k from 20 s |
| string, 32,767 | 82.1 k to 110 s, then 68.9 k | 82.9 k to 100 s, then 69.6 k | 82.9 k to 90 s, then 69.6 k |
| modal, 28,000 | 101.0 k to 60 s, 2.7 k from 65 s | 101.7 k to 45 s, 2.8 k from 50 s | 101.7 k to 40 s, 2.8 k from 45 s |
| one key, string + reverb, 30,000 | 23.6 k to 105 s, 1.5 k from 110 s | 23.8 k to 80 s, 1.5 k from 90 s | 23.8 k to 70 s, 1.5 k from 80 s |

So at damping 30,000 the held chord is at its floor 18 s sooner than in 0.12 (25 s with -60), at 31,000 27 s (44 s),
in the modal model at 28,000 16 s (22 s); **at the very top the cost is what it was**, less one string 20 to 30 s
sooner. The rule is not free while a voice plays: 0.13 costs 0.7 to 1.0 % more than 0.12 with every voice awake
(68,435 for 67,911 cycles a block in string + reverb, 82,860 for 82,043 in the string model, 101,723 for 101,018 in
the modal model: about 130 cycles a voice a render, the test for the fade and the level's compare). Asleep, the same
as 0.12 to within 16 cycles a voice.

**Proof** (`make -C tests/host/rings early`, `rings_early.cc`, `early.sh`: the build as it ships, the -60 build,
the twin without the rule and the build that keeps a faded voice's state; six keys, every voice rendered every
block, the 16-bit output of every render compared; a render's `out` is filled with a pattern first):

- **Above the level, 0.12's samples.** Every key struck again every 4 s for 40 s (no voice is ever 5 s under the
  level): the six models at the four polyphony settings, the -66 and the -60 build against the twin: **0 samples
  differ** in all 48. Keys held and left to die, 60 s (seven model and structure settings x four dampings x
  polyphony 4 and "all", both builds: 112 runs): the twin's samples up to the first render in which any voice
  fades, in all 112. The twin itself against 0.12's sources (`make sleep OLD12=<0.12's folder>`, the six models x
  four polyphony settings x three dampings with restrikes, and five stalled tails held 150 s): the same samples in
  all 77. `rings_compare.cc`'s 90 s of playing (4,367 notes, every model): the same file as 0.12's.
- **The fade has no click.** In those 112 runs 78 have fades, 266 of them. The largest step between two
  neighbouring samples of a fading voice's output, from the fade's first render to the render after its last: 2 LSB
  in the models without reverb, 14 LSB in string + reverb (the reverb's share, which the voice goes on playing); in
  every run it is the twin's own largest step in the same renders or 1 LSB more. In the models without reverb the
  faded output is the twin's sample times the gain to within 1.95 LSB, and the fade's last render peaks at 0 or 1
  LSB, after which the render is zeros.
- **A strike 2 s before a voice would fade** (in the hold): the voice's output is the twin's, 0 LSB apart for the
  second after the strike, in the string, modal, FM, quantised and reverb models; the files are the same up to the
  first fade of another voice.
- **A strike half a second into the fade:** the first sample's step is the twin's to 1 LSB, the voice is within
  3 LSB of the twin's for the next second (0 to 2 without reverb), the level the same to 0.00 dB.
- **A strike 3 s after the fade (asleep), cleared against kept:** the same first sample; apart by 2 LSB (string), 3
  to 6 (string + reverb), 11 (modal, where the kept state still rang at 7 LSB), 0 (FM, not cleared); level 0.00 dB.
  Quantised strings differ more (0.2 dB): the cleared strings' slow detune starts again. Against the twin the
  strings' samples cannot agree, as 0.12's note says (one noise generator feeds every awake string's pluck), and
  the level moves with the pluck: in the twin alone the same key struck at eight moments a tenth of a second apart
  gives 1,141 to 3,624 LSB RMS over its first second in string + reverb and 2,380 to 4,043 in the string model.
  The modal model has no noise: cleared against the twin 9 LSB and 0.00 dB.
- **A steal while a voice fades** (polyphony 4, four keys, a fifth struck half a second into the oldest voice's
  fade): the new voice is the twin's (0 LSB string, 2 LSB modal), the stolen one fades out in its 3 ms as before.
  In string + reverb the new voice's level is the pluck's again: over eight strike times its first second is 1,153
  to 4,818 LSB RMS against the twin's 1,355 to 3,281 (means 2,234 and 2,338), and cleared and kept are the same
  sample for sample (the stolen string was not yet faded out).
- **The level returns with no strike** (FM, structure 0, damping 26,000, the damping turned to the top half a second
  into the six fades): all six gains are back at full 60 ms later; no step larger than the twin's own.
- **A sleeping voice's render is 64 zeros**: every silent render of every run above was checked (none with a
  non-zero sample), and a voice asleep in a model without reverb is rendered silent from its next render.
- `make idle` (0.11's test): as it ships against its twin that never idles, 0 samples differ (damping 24,000 held
  24 s, and 29,000 held 100 s, where the chord's last non-zero sample now comes at 32.6 s; 0.12: 42.0 s). `make
  models OLD=<0.12>`: the same bytes. `make sleep`: as before, and it now builds both sides with the early cut off,
  since it is about 0.12's rule (said in the Makefile).
- The assembly is untouched (no `.S` file changed). **Simulator against host**, the build as it ships with the
  assembly kernels, every render's 16-bit output and every voice's gain: the same file in string + reverb (40 s:
  four fades, a strike into a fade, a strike into a faded voice), the string model (22 s), FM with the structure
  at 0 (22 s, five fades, both strikes) and the modal model (24 s, five fades, both strikes).

**What the tail at the top of the damping is** (the owner's report, 2026-11-21: on the device, string + reverb,
the damping at its longest end, the envelope's sustain at the top, six keys held, the chord "fades in about 4
seconds" while `peng` stays at 158-163 k for 75 s). On the host it is none of the things that could be loud and
unheard: it is the chord. `rings_early bands | audible | dump` (a run's file per voice or summed, at the 44.1 kHz
output or, with `EARLY_OWN=1`, the voice's own output at 22.05 kHz before the limiter):

| six keys summed, damping 32,767, RMS in dBFS | DC (LSB) | under 20 Hz | 20-200 Hz | 200 Hz-2 kHz | 2-8 kHz | 8-16 kHz | over 16 kHz | peak (LSB) |
|---|---|---|---|---|---|---|---|---|
| string + reverb, 5 s | -10 | -58 | -50 | **-8** | -58 | -89 | -89 | 33,250 |
| string + reverb, 30 s | -39 | -77 | -70 | **-18** | -85 | -95 | -102 | 10,701 |
| string + reverb, 60 s | -47 | -90 | -84 | **-22** | -87 | -95 | -103 | 8,034 |
| string + reverb, 75 s | -49 | -92 | -84 | **-26** | -88 | -95 | -104 | 5,512 |
| string, 5 s | -46 | -88 | -28 | **-12** | -57 | -97 | -95 | 25,167 |
| string, 60 s | -8 | -96 | -29 | **-19** | -94 | -97 | -101 | 10,893 |
| string, 75 s | -4 | -90 | -29 | **-19** | -94 | -97 | -101 | 9,907 |

- The energy is the strings' fundamentals (C3 to C5: 131 to 523 Hz), each voice a clean sine by then: one voice's
  own output at 75 s reads 2417, 2551, 2669 ... 2969, 2967, 2942 ... LSB, a smooth wave of its note's period. No
  DC to speak of (the sum's mean is 4 to 49 LSB, -57 dBFS at most, under a tone of 5,000 to 10,000 LSB peak),
  nothing under 20 Hz, nothing near Nyquist, no stuck state. Per voice at 60 s: -30 to -33 dBFS each in string +
  reverb, -22 to -50 in the string model (the low strings longest). The voice's own output before the limiter and
  the 16-bit output after the half-band agree (-32.6 dBFS both for the reverb model's C4 at 75 s).
- The audible band (30 Hz to 16 kHz) of the sum is above -60 dBFS for the whole 120 s (-21 dBFS at 60 s, -32 at
  120 s in string + reverb; -20 at 60 s and at 120 s in the string model). **A listener hears this chord at 60 s.**
  For comparison the band's last second above -60 / -70 / -80 dBFS: at 32,000 past 200 s (string + reverb) and
  22 / 27 / 32 s (string); at 31,000 129 / 143 / 165 s and 9 / 13 / 15 s.
- The knob's top is 32,767: the core hands the engine the slot's four knob words in the stock iter range, 0 to
  32,767 (`docs/engines.md`), and rings takes the word as it is (`ParamToQ31`: `p << 16`). The damping is smooth
  to the end of the control (32,500: 4,911 LSB at 80 s; 32,700: 5,407; 32,767: 5,512), and from 0 to the top no
  value gives "gone in 4 s and never asleep", nor does any structure x brightness at the top (36 settings, 0.11's
  rule alone; the structure at the top comes nearest: 874 LSB at 40 s, then a 6 LSB stall that 0.11 never put to
  sleep, 0.12 does).
- The simulator with the assembly kernels gives the host's file at 32,767 (12 s, both string models); the kernels
  use no instruction whose result depends on the processor's rounding mode.
- **So the device and the host disagree, and the host cannot say why** [C host, simulator; I for the cause]. What
  the host run does not have: the stock's envelope, voice gain and decimator ramp after the render; the stock's
  real pitch words (LFO, bend, tuning; the host's are constant); a hand-played chord's timing (keys tens of ms
  apart: in string + reverb keys 5 and 6 then take over the strings of keys 1 and 2 with a 3 ms fade; the
  test strikes three blocks apart); the patch's other three controls and its polyphony as they stood; the
  real processor under the stock's state. Nothing was changed in rings for this: there is no 0.14.
- **The measurement that tells the two apart** is in `docs/device-tests.md` ("rings at the top of the damping"):
  rings' own screen draws each voice's level from the engine's 16-bit output before the envelope, and core e14's
  stats give `env_peak`, `vgain` and `ramp` beside `peng`. Bars up and a closed gain: the quiet comes after rings,
  and only the core can save the cost. Bars down within seconds and `peng` flat: rings on the device is not
  rings on the host, and its output has to be recorded.

**The second file, and why not a second plugin.** `rings60` beside `rings` would fit the memory (rings takes 668 KB
of the 1,769,472-byte plugin budget, the 13-file set uses 834,520, so two copies come to about 1,503,000, 85 %),
but not the L1 lease: rings leases 3,328 of the 4,096 bytes all engines share, so the copy that loads second would
run with its hot state, its modes and its limiter table in SDRAM, and the two would not cost the same: the
comparison the test is for would be off. A second name would also be a second engine type in saved sounds. So the
-60 build is a second `rings.coop` (version `0.13-60`) that replaces the first: copy it over, restart; `coop_usb.py
inventory` shows which version is loaded.

File 115,788 bytes (`.text` 46,940, +704 on 0.12; `.bss` 572,640, +72); the -60 file 115,796. **The same imports as
0.12**, the same ten names in the same order (it loads on e10 and later). Not on the device yet, and not through the
op1emu gate: `docs/device-tests.md` "rings 0.13".

## 0.14: half the memory, the FM and modal coefficient kernels, less code a render (2026-11-22)

**Nothing changes in the sound**: every model, polyphony, steal, restrike, idle, sleep and early cut plays 0.13's
samples bit for bit (proof below). What changes is what rings costs: the memory it takes (audit finding 11) and the
work and code a sounding render runs (finding 15). Why (the owner's OP-1, core e14, `docs/device-tests.md`
2026-11-22): a six-key chord in string + reverb costs 168,800 (0.11) / 174,400 (0.13) cycles a block on the device,
28-29 k a render, where op1emu says ~9.6 k; rings' cost on the device is mostly code and data fetched from SDRAM, and
0.13's small additions cost 3.5 % there for 0.5 % in op1emu. So 0.14 cuts the code a render runs as well as its
instructions.

**Memory** [C packed file]:
- **The reverb's ring halved** (`src/reverb_fixed.h`): upstream's buffer is 32,768 samples and its lines take 21,632;
  at 22.05 kHz each line is half as long and they end at 10,816, so the ring is 16,384 (`kSize = 32768 /
  OP1RINGS_REVERB_DIV`, the buffer `kSize + 10,816`). A line's samples are overwritten only by the line itself a ring
  later, or by a line above it more than its length later, so every read is the sample it was; the LFOs step on the
  same samples (the ring is a multiple of 32). 54,400 bytes.
- **The strings in a pool** (`src/string_voice.h` `StringPool`, `rings.cc` `ReclaimStrings`): each of the six voice
  states held eight strings of 8.3 KB (48, ~400 KB). Now there are 16, which the voice states take as they are
  configured (`HoldStrings`) and give back when reset. The bound: a polyphony of p keeps p voices in the allocator's
  list, and a fade starts only by stealing a voice of the list that has rendered 8 times, and lasts 2 renders, so at
  most p voices fade at once; with 8 / p strings a voice (sympathetic; one for the string models) that is 2p x 8 / p =
  16, and "all" (no steals) six of two. A string's samples from before its Init are never read (0.8's `clean_`), so
  which string a voice gets changes nothing. When the pool is empty, what is taken back: the strings of silent voices
  (they are reset before they sound again), then those of a fade that has had two blocks and has not ended (the core
  stopped rendering that voice; it renders it again only after a note on, which overwrites or resets its state). As a
  last resort the oldest fade would be cut short (never seen with 16; a pool of 15 cuts fades at polyphony 2, below).
- **.bss 572,640 -> 248,756 bytes**; the e13 heap need (`docs/plugins.md` "The rule for the site", synth image in heap
  0, no voice bytes) **668,856 -> 344,656 bytes** (`blk(cap + 32)`, cap = 95,736 + 248,756 + 3 + 12 x 10).

**Kernels** (Blackfin assembly, each bit-exact with the C the host runs):
- **The FM voice's sample loop** (`src/fm_voice_bfin.S` `FmLoopAsm`, from `FmVoiceFixed::Process`): the state, the
  call's values and the constants in one 108-byte block the loop reaches from two pointers, the C's operations in its
  order (the one exact `MulQ31` as GCC emits it). The modulator's increment (FFloat work) is kept while the pitch and
  the ratio hold (`ModulatorIncrement`, out of line). FM's stage 7,780 -> 4,548 cycles a render (`make profile`,
  model 3 polyphony 4, all renders).
- **The modal coefficient update** (`src/modal_bfin.S`, `ModalResonator::RecomputeFilters`): the loop is now three
  passes, each mode's arithmetic as it was (a mode depends on its own state and on two running products only): A the
  state translation from the old coefficients (`ModalTranslateAsm`), B the partials' geometry when it changed (C:
  FFloat), C the new coefficients and the state (`ModalCoeffsAsm`). RecomputeFilters' C was 3.4 KB, run whole at every
  update. The four pf tables (sin, cos, sec, tan of phi) are one interleaved table (`kPhiQ`, the same 16 KB), so a
  mode's four reads at one index touch one or two cache lines instead of four to eight. `make modal-check`: the two
  kernels against their C on 4,000 random updates, every output word the same (1,751,716 words).
- **The upsampler and the screen's peak in one call** (`src/upsampler_bfin.S` `UpsampleAsm`): 0.2's half-band
  kernel's two passes as one loop body, and the C around it (the 16-bit input, the shifted copy, the 44.1 kHz output,
  the histories) and `ScreenRendered`'s 64-sample peak. `src/halfband_bfin.S` is no longer in the plugin (the host's
  `halfband-check` still checks it).
- **The sub-block tail in one call** (`src/limiter_bfin.S` `MonoLimiterPeakAsm`): the voice's own peak, the reverb's
  share and the limiter loop; the early cut's fading renders take the old steps.
- Out of line, so a sounding render runs less code: the silent, idle and fade-out paths of `Render`, the setting
  decode (`FollowNewSetting`: a render compares the two words with the last ones), the limiter's loud gain (FFloat
  reciprocal), the reverb bus's block end; the strings' 32 unused noise draws a render in one step (the LCG to its
  32nd power).

**Code a render and compute** [C simulator, `make v014-footprint`: `code_footprint.py`, 32-byte lines of rings code
executed, per render that runs the voice and per block; compute: CYCLES around each render, renders over 2,000 cycles;
polyphony 4]. "chord" is the device's chord (six keys C3 .. C5 three blocks apart and held, damping at the top, knobs
still: `rings_compare.cc -DRINGS_CHORD`, blocks 260-299, 0.4 s after the strike); "play" the random scenario, knobs
still; "walk" with the knob walk and jumps.

| model | chord: lines a render | lines a block | compute a render | play: lines a render | compute | walk: lines a render | lines a block | compute |
|---|---|---|---|---|---|---|---|---|
| string + reverb | 144 -> **136** | 249 -> 244 | 16,560 -> **15,913** | 179 -> 173 | 16,962 -> 16,343 | 165 -> 158 | 291 -> 289 | 21,176 -> 20,690 |
| string | 130 -> 121 | 193 -> 188 | 13,490 -> 12,988 | 159 -> 152 | 13,781 -> 13,316 | 151 -> 141 | 231 -> 226 | 18,284 -> 17,924 |
| sympathetic | 149 -> 141 | 230 -> 226 | 13,110 -> 12,806 | 183 -> 177 | 15,605 -> 15,266 | 196 -> 190 | 321 -> 320 | 16,349 -> 15,990 |
| quantised | 146 -> 139 | 231 -> 228 | 13,064 -> 12,748 | 185 -> 178 | 15,539 -> 15,183 | 189 -> 181 | 291 -> 288 | 16,193 -> 15,827 |
| modal | 134 -> 129 | 153 -> 154 | 13,731 -> 13,340 | 148 -> 145 | 14,306 -> 13,939 | 163 -> 153 | 257 -> 250 | 16,479 -> 15,773 |
| FM | 128 -> **117** | 141 -> 134 | 12,523 -> **10,654** | 135 -> 126 | 12,797 -> 10,982 | 138 -> 128 | 160 -> 157 | 12,982 -> 11,406 |

A line is 32 bytes (144 lines: 4.5 KB). What a string + reverb render still runs (chord, 0.14, bytes): the engine's
`Render` ~1,080 (the smoother, the reverb bus, the control decision, the records, the screen), `RenderMono` ~490, the
string kernel 460, the plucker 330 (0.4 s after the strike it still rings; held longer it goes quiet), the upsampler
330, the strings' C ~720 (`StringVoice::Process`, `StringFixed::Process`, `RenderStringBlock`), the limiter call 260;
once a block the reverb (kernel 930, its C 610) and the batch's string parameters (~700). The gains are modest: most
of what a render runs is the engine's own C glue, which is spread over many small decisions; what 0.14 could put in
kernels or out of line, it did. Whether the device gains more than the simulator's 2-15 % (compute) and 3-9 % (lines
a render) is what the device measurement is for.

**Proof** [C host, simulator]:
- `make v014 OLD=<0.13's folder>`: `rings_compare.cc`'s scenario (notes on any voice, steals, fades, restrikes, voices
  the "stock" stops rendering, knob jumps and walks, bends, quiet gaps), the build as it ships against 0.13 as it
  ships: the whole 90 s with eight seeds, with a poisoned pool (every string taken from the pool full of junk,
  `OP1RINGS_POOL_POISON`), with every lease refused, in te-op1-cfw's configuration; every model x polyphony setting
  alone (8,000 blocks, two seeds); every struck voice rendered every block (`RINGS_LONG`, the sympathetic models): **the
  same bytes in all 74**. The pool's reclaim ran (fades taken back 0-4 a run in the random scenario, where voices stop
  being rendered), never cut a fade short; a pool of 15 cuts 11 fades at polyphony 2 (`rings_v14s`): 16 is the bound.
- `make v014-long OLD=<0.13's folder>`: rings_early's, rings_sleep's and rings_idle's drivers as they ship, every
  voice rendered every block: six models x four polyphonies x three dampings x three scripts (restrikes every 4 s, left
  to die 60 s, strikes into fades), the sleep scenarios with restrikes and four stalled tails held 150 s, the reverb's
  idle state at 24 s and 100 s: **270 runs, the same bytes**.
- `make v014-sim`: the simulator (every kernel) against the host (the C), each model at polyphony 4 and "all" in
  `rings_compare.cc`'s scenario (knob walk and jumps: the modal update runs), and six held-chord runs with fades and
  strikes: **24, the same bytes**. `make modal-check`: 0 words differ. `tools/asm_lint.py`: 0 packets flagged.
- `make -C tests/host/rings` (te-op1-cfw's engine): first difference at byte 3,786,753, as since 0.10.

**File** `build/plugins-next6/rings.coop` (0.14): 115,712 bytes, `.text` 46,624, `.rodata` 49,088, `.bss` 248,756;
sha256 `ad7969ac021f1a699300b8a7e9f8fd430340d6ea758484a87120b6b2033c1bfc`. Packed with `tools/plugin/pack.py` at
8ea3078 (it drops what nothing reaches: 0.13 packed with it has `.text` 45,468; without it 0.14's is 48,304).
**The same imports as 0.13**, the same ten names in the same order, and the same engine flags (the 44.1 kHz voice): it
loads on e10 and later. Not through the op1emu gate, not on the device yet: the chord above (string + reverb, six
keys, damping longest) against 0.13's 174,400 cycles a block, `plugmem used`, and the heap at boot.

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
make -C tests/host/rings idle        # 0.11: the reverb's idle state against its twin (OLD=<an older folder>)
make -C tests/host/rings sleep       # 0.12: the 4 LSB rule against its twin (OLD12=<0.12's folder>: the same samples)
make -C tests/host/rings early       # 0.13: the early cut: above the level, the fades, the strikes, the steal
make -C tests/host/rings early-table # 0.13: when each voice fades and sleeps, 0.12 / -66 / -60 (host, ~10 min)
make -C tests/host/rings early-sim   # 0.13: simulator against host, and the cost curves (~30 min, 30 runs at once)
make -C tests/host/rings v014 OLD=<0.13's folder>       # 0.14: against 0.13, rings_compare's scenarios (seeds, a poisoned pool)
make -C tests/host/rings v014-long OLD=<0.13's folder>  # 0.14: the held-key drivers (early, sleep, idle) against 0.13
make -C tests/host/rings v014-sim     # 0.14: the simulator (the kernels) against the host
make -C tests/host/rings modal-check  # 0.14: the modal coefficient kernels against their C
make -C tests/host/rings v014-footprint [FP_SRC=<folder>]   # 0.14: code a render and a block, compute a render
```

The two files of 0.13 (the second from a copy of the folder with `-DOP1RINGS_EARLY_LEVEL=134218` added to
`plugin.json`'s `cflags` and the version `0.13-60`; `pack.py` wants the folder named `rings`):

```sh
python3 -I tools/plugin/pack.py plugins/rings -o build/plugins-next4/rings.coop --toolchain ~/op1-toolchain
python3 -I tools/plugin/pack.py /tmp/p60/plugins/rings -o build/plugins-next4/alt60/rings.coop --toolchain ~/op1-toolchain
```

0.14, one file: `python3 -I tools/plugin/pack.py plugins/rings -o build/plugins-next6/rings.coop --toolchain ~/op1-toolchain`.

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

