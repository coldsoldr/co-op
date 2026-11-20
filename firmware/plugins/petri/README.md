# petri

A granular micro-loop effect, inspired by the signal flow of a granular micro-loop pedal. The input is recorded all the
time into a 1.36 s stereo buffer at 22.05 kHz (0.2; 0.1 had 1.49 s); one of eleven effects plays that buffer back through up to eight voices
(loops, grains, slices, taps), each with its own start, rate and direction, length, envelope, gain and pan; the wet
goes through a shared tone section (the effect's envelope on every repeat or grain, a gentle pitch drift fixed per
effect, a resonant low-pass, a small space) and is mixed, equal power, with the untouched dry signal. **Hold** stops
the recording, so the effect keeps working on the slice it holds while you play over it; **reverse** plays everything
backwards. There is no phrase looper: the OP-1's tape is the looper.

Signal path, per 64-sample block: the Q31 bus to int16, 2:1 down to 22.05 kHz (Clouds' halfband filters, copied from
`plugins/tape_echo`), the effect's scheduler (once a block: which voice starts where, how fast, how long), the voices,
the record (the input, plus the wet fed back for the effects that have it), the makeup, the filter, the space, back up
to 44.1 kHz, the equal-power mix and the soft limit, back to Q31. Stereo at every position.

Licence: coldsoldr's GPL-3.0-or-later (with the OP-1 firmware permission); the half-rate output stage follows Mutable
Instruments Clouds (Emilie Gillet, MIT, `LICENSE-MIT`).

## Controls (guidelines §3.2)

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | time | activity | repeats | mix |
| Click | reverse (latch) | — | — | hold (latch) |
| Shift + turn | time fine | activity fine | repeats fine | mix fine |
| Shift + click | — | — | — | — |
| fn + turn | effect (11 steps, wraps) | — | filter | space |
| fn + click | — | — | — | — |
| Shift + fn + turn | — | — | filter fine | space fine |
| Shift + fn + click | — | — | — | — |

- **Turns** (`params[0..3]`, normalised to 0..32767 once a block from the filter type's ranges, as tape_echo does):
  **time** is the length the effect works with (a loop, a grain, a slice, a step, a tap), exponential, its range per
  effect (table below); **activity** is how busy the effect is (layers, density, pattern variation, notes, taps);
  **repeats** is how long things come back (per effect: decay a repeat, passes, strums, feedback, or how often);
  **mix** is dry to wet, equal power (Clouds' crossfade tables), `defaults` 16384 (50 %). `defaults`: time 16384,
  activity 13107, repeats 13107, mix 16384.
- **effect** (fn + blue, word 5, steps, wraps, saved, randomised, default `mosaic`): `mosaic`, `seq`, `glide`, `haze`,
  `tunnel`, `strum`, `blocks`, `cut`, `arp`, `pattern`, `warp`. A change fades the voices out (8 calls, 11.6 ms) and
  starts the new effect clean on the same buffer.
- **filter** (fn + white, word 6, continuous, `"fine": true`, default 30000): a resonant low-pass (a TPT state
  variable filter, 12 dB/oct) on the wet only, 150 Hz at 0 to 10 kHz at the top, exponential (7 kHz at the default);
  Q 0.85 at the top rising to 1.7 at the bottom (it rises only in the lower part). Never closed: the floor is 150 Hz.
- **space** (fn + orange, word 7, continuous, `"fine": true`, default 8000): a small stereo space on the wet (four
  allpasses, then a figure-of-eight tank of two allpass + delay halves, damped, at 11.025 kHz): 0 is none, the top a
  wash (the tank's gain 0.5 to 0.92 with the knob; the wet's share falls to a half while the tank's rises to 1.5).
  The tank takes the wet's mid (L + R) and returns two decorrelated halves: the wet itself stays stereo beside it.
- **reverse** (blue click, latch bit 1 of `params[8]`, not saved, never randomised): every voice started from then on
  plays its material backwards (a grain from its end; pattern's taps become reversed chunks). Voices already playing
  finish as they were.
- **hold** (orange click, latch bit 0, not saved, never randomised): the record stops (the write head with it), the
  feedback with it; every effect keeps working on what the buffer holds, its "now" the moment hold went on. Onsets
  are not detected while held; strum, blocks and arp then play the held onsets on a clock. **Latches clear** whenever
  the effect is (re)started at a position, including the stock's synth / drum switch (`docs/ui-features.md` §14).

`plugin.json` "controls":

```json
{"id": "effect", "name": "effect", "action": "fn_turn", "encoder": "blue", "word": 5, "kind": "steps",
 "steps": ["mosaic", "seq", "glide", "haze", "tunnel", "strum", "blocks", "cut", "arp", "pattern", "warp"],
 "default": 0, "wrap": true},
{"id": "reverse", "name": "reverse", "action": "click", "encoder": "blue", "kind": "latch", "bit": 1},
{"id": "filter", "name": "filter", "action": "fn_turn", "encoder": "white", "word": 6, "kind": "continuous",
 "default": 30000, "fine": true},
{"id": "space", "name": "space", "action": "fn_turn", "encoder": "orange", "word": 7, "kind": "continuous",
 "default": 8000, "fine": true},
{"id": "hold", "name": "hold", "action": "click", "encoder": "orange", "kind": "latch", "bit": 0}
```

Not upstream of anything: a new coop effect. It follows the common meanings (§4.3): time on the blue turn, mix on the
orange turn, the hold latch on the orange click.

## The effects

Every effect is a scheduler over the same voices (`src/petri_engine.cc`). A voice reads the buffer with linear
interpolation at its rate (Q17), its envelope (the effect's shape) and gains ramped over each call; nothing reads past
the write head (a guard in `Spawn`: a forward read stays behind it from start to end, a held or reversed one ends before
it), and nothing older than the buffer holds is started.

| Effect | Time (turn) | Activity | Repeats | Shape | Pitch drift | Voices |
|---|---|---|---|---|---|---|
| mosaic | loop 30 ms .. 0.96 s | layers 1..4: 1x, 2x, 0.5x, 1.5x of one slice, spread in pan | decay a repeat 0.35..0.98; a layer grabs fresh material when it has faded (or the slice would be overwritten) | trap (1/8 fades) | 6 ct, 0.3 Hz | 4 |
| seq | step 40 .. 172 ms | the order of 4 slices of the last 4 steps (fixed at low, shuffled per pass above), steps reversed, ratcheted, an octave up, rests | passes of one window 1..8 before it takes the newest | gate (1/32 fades) | none | 4 |
| glide | loop 30 ms .. 0.96 s | 2..4 staggered layers; each repeat glides 0.5..11.5 semitones (alternately up and down, the outer pair half) | decay a repeat, as mosaic | hann | 3 ct, 0.2 Hz | 4 |
| haze | grain 20 .. 453 ms | density 1..6 grains overlapping; spread back over 0..0.9 s; detune 4..24 ct; octaves up (and some down) high up | feedback into the record 0..0.75 | hann | 8 ct, 0.17 Hz | 6 |
| tunnel | the drone's period 8 .. 314 ms | layers 1..3: the period at 1x, 0.5x, 2x (two overlapping hann grains each: a granular comb) | feedback 0.4..0.9 (the drone's decay) | hann | 5 ct, 0.13 Hz | 6 |
| strum | between notes 15 .. 240 ms | notes 2..6: the last onsets, oldest first | strums again 0..6 (the other way, 0.7 softer each) | perc | 4 ct, 0.5 Hz | 8 |
| blocks | block 10 .. 243 ms | glitch: 2x, 0.5x, 1.5x or reversed stutters, jumps inside the block, accelerating stutters | stutters 2..16 a burst | gate | none | 4 |
| cut | burst 20 .. 393 ms | the intervals drawn from: +12, then -12, +7, +19, -5, +24; up to 4 stutters | how often: a burst every ~4 s .. 0.2 s (+-30 %) | gate | none | 4 |
| arp | step 60 .. 591 ms | steps 1..8 of 0, 7, 12, 19, 24, 19, 12, 7 semitones over the last 3 onsets (or the newest audio) | note length 0.4..2.4 steps | perc | 4 ct, 5 Hz (vibrato) | 4 |
| pattern | bar 0.1 .. 1.27 s (0.1: 1.39 s) | taps 1..6 at 16, 8, 12, 6, 14, 3 sixteenths of the bar, panned | feedback 0..0.85 (loop gain, whatever the taps) | flat (taps; gate chunks reversed or held) | 3 ct, 0.4 Hz | 6 |
| warp | longest tap 60 ms .. 1.1 s | three taps at 1/3, 2/3, 1 of it pitched by 0..+12, 0..-12, 0..+7 semitones, each low-passed (bright, dark, mid) | feedback 0..0.8 (pitched taps feed back: cascades) | hann (two grains a tap) | 10 ct, 0.25 Hz | 6 |

- **Onsets** (strum, blocks, arp): a peak follower on the decimated input, once a block: an onset when a block's peak
  passes twice the slow level plus 1000 and 2500 (-22 dBFS), at most one each 70 ms; a held sound (no onset for 1 s
  but loud) counts as one each second. The onset's frame (5 ms before, for the attack) goes into an 8-onset ring.
- **Pattern's taps** are continuous voices: a time change glides them to the new delay like a tape (up to +-33 % rate)
  or, for a jump over 6000 frames, fades them out and starts them there. Reversed or held, each tap plays chunks.
- **The feedback** (haze, tunnel, pattern, warp) goes through a DC blocker (~17 Hz) and its gain truncates toward zero,
  so the loop can never hold an offset or a last LSB; the record saturates.
- **The makeup** (Q12, per effect) brings the wets near the dry's level at the defaults (`tests/host/petri` takes):
  mosaic 1.59, seq 1.27, glide 1.83, haze 1.76, tunnel 1.39, strum 1.15, blocks 0.66, cut 1.37, arp 1.71, pattern 1.22,
  warp 1.56.
- **Starting**: after a (re)start the record (118.5 KiB) and the space (9.8 KiB) are cleared over the next 17 calls
  (25 ms, the dry passing through), then the record fades in over 32 calls (46 ms) so no read across its start meets a step.

## Display (e9)

The render publishes, once a block after the DSP (the clearing blocks too), whole words at the end of its state
(`state.h` `Display`, read through `coop_scr::Published<Instance>` while `view->live`):

| Word | Meaning |
|---|---|
| `effect` | the effect in force, 0..10 (-1 before the first render) |
| `head` | the write head, 0..32767 frames (it stops while held) |
| `level` | the input level, 0..32767 (a fast peak follower) |
| `onsets` | onsets detected (a count: the screen reacts to a change) |
| `flags` | bit 0 hold, bit 1 reverse, bit 2 clearing |
| `active` | bit i: voice i sounding |
| `v[8].pos` | each voice's read position, 0..32767 frames (-1 idle) |
| `v[8].rate` | its rate, Q8 signed (256 = 1x forward, -512 = 2x backwards) |
| `v[8].level` | its envelope x gain, 0..32767 |

## Screen (e9)

The lead's mock-up "petri screen" (`Petri.dc.html`, 2026-11-14): a stompbox seen from above in 3/4, drawn as wireframe
faces. `petri.svg` (from `gen_petri_svg.py`, 5,067 bytes, 46 ids) and `petri_screen.cc`.

| Part | Encoder | What moves it |
|---|---|---|
| the box: front, right side, top (pencil on near-black) | — | a slow sway of the yaw (-0.10 + 0.06 sin 0.45 t rad); a hop on each input onset (higher with activity) |
| the elevation | fn | 1.01 rad at rest, easing to 0.70 while fn is held: the box tips forward and its front panel shows |
| the petri dish (a ring, a lid 4 units up with two wall lines at the silhouette, guide rings r 31 and r 15) | orange click (hold) | the lid's rim and walls orange while held, else pencil |
| the voices: dots in the dish | — | angle = the read position (clockwise from 12 o'clock), distance = pitch (16 + 8 log2 \|rate\| units, 6..29), tint body tone -> pencil by envelope; hidden when quiet |
| the write head: a tick on the dish's rim (r 31 .. 36) | — | the record's head; gone while held |
| four knobs (cylinders, pointers -135..+135 degrees) | blue time, green activity, white repeats, orange mix | the turns (`view->knobs`, live with the LFO) |
| the reverse and hold footswitches, their LEDs | blue click, orange click | pressed down (7 -> 2.5 units) and lit while the latch is on |
| 11 program LEDs on the front | fn + blue (effect) | the effect's LED lit blue |
| two gauges on the front | fn + white (filter), fn + orange (space) | the fill (at least 3 %) |

- **One matrix a face.** Every face is flat art in its own local coordinates, moved by one screen-space matrix a frame
  (`SetMatrix6`: a face's origin and two unit axes through the orthographic projection, Q10). No group transform is
  nested in another, so nothing depends on R246 composing them. The knobs' sides are rects under their own matrix (the
  cylinder's silhouette between its two tangent points); their tops circles under the top's axes (ellipses).
- **Raster lines** (`coop_scr_line`, drawn over the SVG): the knob pointers (2 units), the write tick, the dish's two
  wall lines, the gauges' fills. The voices are eight top-level dots translated in screen space (no squash).
- **Strokes under a matrix**: whether R246 scales a stroke with its node's matrix is not known [I]; if it does, the
  faces' strokes are squashed to ~0.85 of 1.5 vertically (the elevation), which reads fine in the preview.
- **What differs from the mock**: the projection's centre is 6 units lower (y 84, not 78) to keep the core's popup
  corner clearer; at the sway's extremes the box's back-left corner still comes within the top-left 100 x 20 (about
  x 74, y 13): a long popup name may touch it. The hop follows the input's real onsets (the mock's were random). The
  animation runs on `dt` (the core's frame time), not the CYCLES clock.
- No text (the core's popup names the control and value top left); colours are coop's brights, their dark tones
  (blue 2B3A70, green 00573A, white 5C5870, orange 6E1A2B, as the mock and tape_echo), the body tone 38354D, the pencil
  DFD9FF, the occluder 010101.
- Previews: `make -C tests/host/petri screens` renders six scenes (mosaic, mosaic held, haze with fn, warp reversed,
  arp, tunnel held and reversed with fn) to `build/petri_screens/*.png`, from the screen code over the running effect,
  through a recorder of the screen calls and a small rasteriser (a preview, not R246).

## Memory, numbers, cost

- **State** (`state.h`, in the position buffer, e8): `"scratch": 134752` (`sizeof(Instance)` 134,732, 32-rounded;
  `petri.cc` asserts both). **0.2**: under tape_echo's 134,784, so petri no longer grows the five position buffers
  (0.1 declared 150,624). The record: 30,336 words (a ring of 30,048 stereo frames, L low / R high, plus a mirror of
  its first 288 frames after its end) = 121,344 bytes; the space's window 5,008 int16 = 10,016 bytes; voices,
  schedulers, filter, half-rate path, onsets and display ~3.4 KB.
  - **The ring** (`src/petri_engine.h` `kRecLen`): positions still count modulo 32,768 frames (a voice's Q17 position,
    the onsets, the display words: nothing in the schedulers or the screen changed), but a frame is stored by its age
    behind the write head, at `(hp - age) mod 30,048`. `RenderVoices` works that out once a voice a call and hands the
    kernel a base pointer and a position that cannot wrap in 32 samples; the mirror covers a call's reach (a rate is
    held to 8x: 258 frames; the effects reach 4x). The kernels (`voice_bfin.S`, `space_bfin.S`) are unchanged.
  - Checked: built with `-DCOOP_PETRI_REC_LEN=32768` (the old length through the new mapping, and the space's new
    slide), the 31 listening takes are byte-identical to 0.1's and the stress run changes the same 3,505,395 samples. `.bss`: 324 bytes (the five slot pointers, the screen's node pointers and
  its per-position animation).
- **File**: 36,104 bytes packed; `.text` 22,476, `.rodata` 4,108, `.bss` 324 (load ~26.9 KB, relocated once). The SVG
  is in the TE heap at e9, not heap 0. Core imports: `coop_fx_scratch` and the E2 screen calls only.
- **Numbers**: fixed point throughout. Two kernels in Blackfin assembly with bit-exact C references (single-issue
  instructions only, no parallel packets; `tools/asm_lint.py`: 0 flagged): `src/voice_bfin.S` (one voice, 32 samples,
  both channels in the two halves of a register; ~19 cycles a sample) and `src/space_bfin.S` (16 samples of the space,
  MAC allpasses with rounded, saturated extraction, the two tank halves packed). The space's extraction rounds as the
  Blackfin does by default (ASTAT RND_MOD 0, ties to even): a core that set RND_MOD would differ from the C reference
  by an LSB on exact ties, inaudibly. The voice kernel's interpolation uses (T) truncation and does not depend on it.
  The halfband kernels are tape_echo's (`src/halfband_bfin.S`, unchanged). The space's buffer is read through a
  pointer stepping down with constant offsets (no masking); every 64 calls (93 ms; 0.1: every 256) its live 7.9 KB
  window is copied up once (1,984 word copies in that call, about 2.4 k cycles: inside the max below).
- **Cost** (GNU simulator, compute only: no cache misses or SDRAM waits; `make -C tests/host/petri sim`, 1,500 blocks
  per effect, the riff, knobs, filter and space walking, activity at the top for the second half, hold and reverse
  toggling):

| Effect | avg | max |
|---|---|---|
| mosaic | 19,939 | 25,514 |
| seq | 18,224 | 22,171 |
| glide | 20,499 | 27,959 |
| haze | 22,628 | 29,457 |
| tunnel | 22,418 | 27,666 |
| strum | 21,316 | 28,562 |
| blocks | 18,269 | 23,301 |
| cut | 17,162 | 22,166 |
| arp | 18,250 | 25,828 |
| pattern | 23,814 | 30,144 |
| warp | 29,697 | 36,152 |
| all | 21,111 | 36,152 |

  **0.3 (2026-11-20, `docs/efficiency-audit.md` finding 8):** the four turns' values are kept per slot (`.bss`, 28
  bytes a slot, cleared by `init`) and divided only for a word that moved (four `___divsi3` calls a block before):
  every row about 620 lower, all 20,488 / 35,523, the clearing blocks 22,082 at most. The output and the display
  words are 0.2's bit for bit (the test's digest, "Tests"). The engine's own per-block divides (pattern's and warp's
  loop gain, warp's tap times, a grain's envelope step at its start) are as they were. File 36,348 bytes, `.text`
  22,584, `.bss` 464; imports unchanged.
  The table is 0.2's (0.1: all 20,936 / 33,416). The averages moved with the material (the ring's mapping is about 40
  cycles a voice); the maxima are up to 2.8 k higher where the space's copy now falls in an effect's busiest block
  (it comes four times as often). **Over the 30 k SHOULD** (guidelines section 2.4): warp, as in 0.1 (three filtered
  buses beside six voices), and pattern by 144 cycles in the copy's block; every effect is far under the 60 k line.
  The clearing blocks after a start: 21,895 at most. Fixed per block: the half-rate path ~8 k (decimate 2.1 k, the
  interpolate-mix-soft-limit 5.9 k), the filter ~3 k, the space ~2.2 k; each voice ~0.6 k. The device figure (stats
  `cost` line) is pending: expect 10-35 % more, more where the voices read far apart in SDRAM (each voice touches about
  4 cache lines a block). **Device: not run.**

## Tests

- `make -C tests/host/petri` (host): **stress** 60,000 random blocks (noise, saws, impulses, the riff, silence, DC,
  full-scale squares, L != R; every effect, out-of-range effect words; the turns walking and jumping in the filter
  type's range units; filter and space walking and jumping; hold and reverse toggling; 7 re-inits and 10 pauses):
  low 16 bits always clear, no run of 48 full-scale samples while the input is at most -6 dB, the display words in
  range. **Decay**: for each effect, 3 s of the riff then silence, at the defaults and with repeats and space at the
  top: the output falls to 0 (within 2 LSB, then exactly 0 in the last second) in 2.0-3.1 s at the defaults and
  12.8-19.3 s at the top (the space's longest tail and pattern's 0.85 feedback); held, it keeps playing (as it
  should). **Clicks**: a plucked 220 Hz sine, wet only, three variants per effect (defaults; reverse with a time jump;
  more activity then hold): no second difference over 3000 LSB anywhere. **e8**: `coop_fx_scratch_check` for every
  effect (slot 0 beside slot 3 on dirty buffers, a restart): 0 blocks differ. **Listening**: 8 s of a plucked-saw
  riff through each effect into `build/petri_wav/` (mix 60 %; `wet/` the same at 100 %), plus mosaic and haze held,
  glide and pattern reversed, and `00_dry.wav`.
- **The digest (0.3):** every rendered block's output and display words are folded into one number, printed last
  (`output digest ...`). Two builds of the plugin that print the same digest gave the same samples in every check:
  `make -C tests/host/petri COOP=<the older sources' folder> BUILD=<another>` builds an older version under the same
  test. 0.2 and 0.3: `0178aade26c9622a` (host, 60,000 blocks) and `474736ab08745fff` (`sim`). The stress run has
  stretches where every turn moves every block.
- `make -C tests/host/petri sim` (Blackfin simulator): the two kernels against their C references (3,000 random calls
  each: 0 differ) and the cycle table above.
- `make -C tests/host/petri screens`: the screen previews (above).
- `make -C tests/host/petri tables`: regenerates `src/petri_tables.h` (grain envelopes, 2^x, the filter's coefficients)
  with `gen_petri_tables.py`; `python3 gen_petri_svg.py petri.svg` the screen.
- `tools/asm_lint.py plugins/petri/src/*.S`: 0 packets flagged.

**op1emu gate** (0.2 on e10 and e11, 2026-11-20): `docs/device-tests.md` "fickle 0.2 / petri 0.2". **Not done yet**
(guidelines §7.4 step 9): the device run with the `cost` figures.

## Open points for the owner

- **The heap budget (0.2, 2026-11-20):** with all 13 catalogue plugins on core e10 the plugin memory budget ran out
  (fickle was refused at boot); petri 0.1's 150,624-byte declaration grew the five position buffers by 79,200 bytes
  over tape_echo's. 0.2 declares 134,752, under tape_echo's 134,784, so petri costs the budget only its image and
  screen object (~27 KB).
  - **What you hear:** the record is 1.36 s (30,048 frames), was 1.49 s. Pattern's longest bar is 1.27 s (was
    1.39 s). At long times an effect takes fresh material a little sooner: a mosaic or glide layer whose slice would
    be overwritten, haze's oldest grains (its spread reaches 0.9 s back plus the grain), the oldest onsets of strum,
    arp and blocks (kept while they are less than 1.36 s old). Everything else, every other range in the table above
    included, is as 0.1.
  - How the 15,872 bytes were found: 6,144 from the space's buffer (its slide 1,024 entries instead of 4,096: no
    change in sound, a copy every 93 ms instead of every 372 ms) and 9,728 from the record (2,720 frames less, after
    288 for the mirror). Putting the slide back to 4,096 would lower the worst block by ~2.4 k cycles and cost another
    70 ms of record (1.29 s): `kSpaceSlide` and `COOP_PETRI_REC_LEN` in `src/petri_engine.h`.
  - The kennel build (`petri.knl`) keeps the 1.49 s record: kennel's arena is 6 MiB.
- The time ranges are per effect (table above) rather than one 30 ms .. 1.5 s for all: a seq step of 1.5 s or a
  tunnel period of 1 s would not do what the names say, and a loop that must repeat inside a 1.36 s buffer has to be
  shorter than it.
- Repeats means a different thing per effect (decay, passes, strums, stutters, how often, note length, feedback). It
  is always "how long things come back", but a player will hear the difference.
- Glide and arp come out quieter than the others at the defaults (their envelopes are sparse); their makeup is held
  back so a loud input at four times the pitch does not clip the wet.
- The plugins Makefile rebuilds a `.coop` when `plugin.json` or a top-level `.cc` changes, not `src/` (tape_echo has
  the same): `rm build/plugins/petri.coop` after editing `src/`.
