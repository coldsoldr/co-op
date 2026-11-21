# tape_echo

A Space Echo style tape delay as a coop effect: one tape loop per channel, a record head and three playback heads at
1/3, 2/3 and 1 of TIME, each repeat going round the record amp, a low cut, the tape loss and the tape saturation
again; wow and flutter after Mutable Instruments Beads; TIME glides like a tape motor (the echoes bend, never click).
The DSP is te-op1-cfw's fixed-point TAPE ECHO (`ports/fx/src/tape_echo`, design and measurements in te-op1-cfw
`docs/tape-echo-port.md`), as the hundekraft kennel plugin ran it (`ports/knl/tape_echo` 0.2, commit 5aa314b): 2:1
down to 22.05 kHz through Clouds' halfband filters, the echo, back up, an equal-power mix with the dry signal and a
soft limit. Stereo at every position.

Licence: coldsoldr's GPL-3.0-or-later (with the OP-1 firmware permission) for the plugin and the echo; the wow and
flutter (Beads) and the half-rate output stage (Clouds) follow Mutable Instruments' MIT designs (`LICENSE-MIT`).

## Controls (guidelines §3.2)

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | time (30 ms .. 1.5 s) | feedback (0 .. 1.1) | age | mix |
| Click | — | hold (latch) | heads (3, 2+3, 1+2+3, 1+3) | — |
| Shift + turn | time fine | feedback fine | age fine | mix fine |
| Shift + click | — | — | — | — |
| fn + turn | gain (-6 .. +6 dB, 0.5 dB a detent) | sync (off, 1/16T .. 1/2) | — | — |
| fn + click | — | — | — | — |
| Shift + fn + turn | gain fine (0.1 dB a detent) | — | — | — |
| Shift + fn + click | — | — | — | — |

- **Turns** (`params[0..3]`, kennel's four knobs unchanged): time is head 3's delay, 30 ms x 50^x (exponential);
  feedback is the loop gain, 1.0 at 91 % (above it the tape self-oscillates, the saturation bounds it); age wears the
  tape (wow and flutter depth, the loss from 9 kHz to 1.8 kHz, the low cut from 40 to 110 Hz, the drive 1..2); mix is
  dry to wet. The core hands them in the filter type's range units (`sdk/coop_plugin.h`); the plugin normalises them
  to 0..32767 once a block as kennel's `op1_fx_param_q15` did. Shift + turn is the stock fine step on each.
- **gain** (fn + blue, word 5, continuous, default 16384 = 0 dB, saved, not randomised): what reaches the record head;
  the dry side of the mix doesn't see it. **Blue's shift + fn layer is gain fine** (the owner's decision,
  `docs/ui-features.md` §14): `"fine": true`. Steps (the owner, 2026-11-11, dev 50e7dbd): `"step": 1365` (0.5 dB a
  detent, 2730.7 Q15 a dB over the +-6 dB range; kennel's page click was 1536, 0.56 dB) and `"fine_step": 273`
  (0.1 dB, kennel's `knl::Fine` tenth of a dB).
- **sync** (fn + green, word 6, steps, saved, no wrap, randomised, default `off`): kennel's tempo lock. `off`: the
  blue turn sets the time. `1/16t`, `1/16`, `1/8t`, `1/16.`, `1/8`, `1/4t`, `1/8.`, `1/4`, `1/2t`, `1/4.`, `1/2`
  (shortest first): the time follows that division at the tempo, and the blue turn does nothing to the time until sync
  is back on `off` (its value is kept). The time glides to a new division like any time change. **The tempo is the
  project tempo** (0.3, the tempo screen's BPM, which the tape and the sequencer follow): the core's export
  `coop_tempo_bpm_q16` (`sdk/coop_plugin.h`, e10; `docs/request-tempo-export-tape-echo.md`), read once a block, so a
  tempo change moves a synced time while the echo plays (it glides there). 120 BPM (a 16th 125 ms, a half note 1 s)
  when the core reports no tempo, kennel's own fallback (guidelines §2.3). A core before e10 refuses the 0.3 file
  ("an import is not exported by the core"): use tape_echo 0.2 there (sync at 120 BPM). At a tempo where a division is
  over 1.5 s (1/2 below 80 BPM, 1/4. below 60, 1/2t below 54) the time falls back to the turn, as kennel skipped such
  divisions. Arithmetic as kennel's (`DivisionWord`, `TapeEchoTimeWord`): the division's delay as a time word.
- **hold** (green click, latch bit 0 of `params[8]`, not saved, never randomised): the input off, the loop gain exactly
  1, a quarter of the tape loss and a 10 Hz low cut: the tape loops what it holds, slowly fading. **Latches clear
  when the stock switches between synth and drum** (the stock starts the instrument effect again; accepted by the
  owner, `docs/ui-features.md` §14), and whenever the effect is selected or the sound changes.
- **heads** (white click, word 7, steps, saved, wraps, randomised, default `3`): the head pattern, one click a
  pattern: head 3 alone; heads 2 and 3; all three; heads 1 and 3. A change ramps the head weights over 8 calls
  (11.6 ms).

`plugin.json` "controls":

```json
{"id": "gain", "name": "gain", "action": "fn_turn", "encoder": "blue", "word": 5, "kind": "continuous",
 "default": 16384, "fine": true, "step": 1365, "fine_step": 273, "random": false},
{"id": "hold", "name": "hold", "action": "click", "encoder": "green", "kind": "latch", "bit": 0},
{"id": "sync", "name": "sync", "action": "fn_turn", "encoder": "green", "word": 6, "kind": "steps",
 "steps": ["off", "1/16t", "1/16", "1/8t", "1/16.", "1/8", "1/4t", "1/8.", "1/4", "1/2t", "1/4.", "1/2"],
 "default": 0},
{"id": "heads", "name": "heads", "action": "click", "encoder": "white", "word": 7, "kind": "steps",
 "steps": ["3", "2+3", "1+2+3", "1+3"], "default": 0, "wrap": true}
```

No `"stereo"` flag: a `coop_fx_t` is stereo by its ABI, and coop's loader reads kennel's flag as the
`op1_fx_stereo_t` layout and would refuse the file.

**Upstream mapping** (kennel `tape_echo_knl.cc`):

| Kennel | coop | Why |
|---|---|---|
| knobs blue time, green feedback, white age, orange mix | the same four turns | the owner kept them |
| blue press: page 2, blue turns the input gain (word 5); shift + blue a tenth of a dB (`knl::Fine`, reading the shift byte at 0x01DD7F78 and rewriting the stored word) | fn + blue (0.5 dB), shift + fn + blue its fine (0.1 dB) | every layer is held (guidelines §4); the core does the fine step |
| green press: hold, `OP1_PRESS_LATCH` (`op1_fx_page[pos][1]`) | green click, latch bit 0 | guidelines §7.1 |
| white press: page 2, white clicks the head pattern (word 7, `ClickMode(word, 4)`: 1536 a click) | white click, steps, word 7 holds the step index | enumerations are steps (§3.1); a sound saved by kennel decodes clamped (word 7 >= 3 plays as `1+3`) |
| shift + blue on page 1: a shifted click walked the time through the tempo divisions (1/16T .. 1/2; the tempo read at 0x01AADAD4); a free turn unlocked it; not saved | fn + green sync, steps (`off` + the 11 divisions), word 6, saved; the tempo from the core's export `coop_tempo_bpm_q16` (0.3; 120 BPM in 0.1-0.2) | the owner, 2026-11-11: shift + turn is the core's fine step and the plugin never sees shift; a sync setting belongs to the sound |
| `op1_fx_param_q15` (the stock range table at 0x01DE5C50) | the SDK's constant filter ranges, once a block | no firmware addresses in a plugin (§2.1) |
| the instance in the position's scratch buffer (`op1_fx_scratch`) | `.bss` (one instance per relocation, i.e. per position) | §2.3 |
| `TapeEcho::Init` clears the 133 KB tape on every (re)start, in the audio interrupt | the clear is spread over the first 17 blocks (2,048 pairs a block, 25 ms), the dry passing through (accepted as it is by the owner, 2026-11-11: listen for it) | a slow init doesn't ship (§2.3) |
| the screen (`tape_echo.svg`, design B "Schematic") | kennel's screen on e9, made lean (below); coop shows the control's name and value top left | e9 effect screens (`docs/plugins.md` "e9") |

Time already has a fine step (shift + blue), so there is no separate delay-time fine control.

## Screen (e9, 0.2)

Kennel's screen (`te-op1-cfw/ports/knl/tape_echo`: `tape_echo.svg` from `gen_tape_echo_knl_svg.py`, the canvas "Tape
echo screen" design B "Schematic", `tape_echo_knl.cc` ScreenUpdate) on the e9 effect-screen path (`docs/plugins.md`
"e9"): `tape_echo.svg` (from `gen_tape_echo_svg.py`) and `tape_echo_screen.cc`. It shows wherever tape_echo is the
effect on the page: FX 1 of the synth, the drum or the master, FX 2 in focus.

The Space Echo's signal path, left to right on the tape row: the input arrow, the record head, the tape, the three
play heads tapping down to a bus, the sum, the output arrow; the return path under it.

| Part | Encoder | What moves it |
|---|---|---|
| the play heads' distance from the record head (head 3 at 32 + 130 t units) | blue (time), or green with fn (sync) | the time word in force (the render's display word) |
| the dot on the tape, record head to head 3 | blue (time) | one trip per TIME, at most 8 a second |
| the beat ticks over the tape (16ths, every fourth longer) | green with fn (sync) | bright green while fn is held; green's dark tone at rest while a division is in force; none otherwise |
| the heads lit (white) or dark | white click (heads) | the pattern: 3, 2+3, 1+2+3, 1+3 |
| the tape wave | white (age) | its height (a tenth at age 0), breathing with the wow (faster with age) |
| the feedback chevrons | green (feedback) | a count set, 0..4 |
| the return path, the hold glyph (top right), the input | green click (hold) | return path and glyph green, the input and record head dim while held (all four chevrons); the glyph in green's dark tone at rest |
| the gain gauge (left) | blue with fn (gain) | the bar about the centre, -6..+6 dB; a bracket while fn is held |
| the mix gauge (right) | orange (mix) | scaled about its foot |

What changed from kennel's screen:
- **Inputs**: the four turns from `view->knobs` (kennel's `op1_fx_knobs_live`, the stock pages' values with the LFO);
  gain, sync and the head pattern from `view->words[0..2]` (words 5..7); hold from `view->latches` bit 0 (kennel's
  `knl::Pressed(pos, 1)`).
- **Display words** (`state.h` `Display`, at the end of the state; written once a block by the render after the DSP,
  the clearing blocks included; read through `Published<>` while `view->live`): `time`, the time word the echo gets
  (the turn's, or the sync division's: `SyncedTime`), `div`, the division in force (0..10, 1/16T .. 1/2; -1 when sync
  is off or the division fell back to the turn), and `beat`, a 96th note at the tempo in use (Q4 samples at
  22.05 kHz; 7350 at 120 BPM; 0.3: at the project tempo, so the ticks follow a tempo change; before the first render
  the screen reads the export itself). Kennel's screen
  read its `TimeLock::div[]` and recomputed the time word itself. Before the first render (`time` -1) the screen
  shows the blue turn. `TapeEchoTimeSamplesQ4` (the word's delay, for the dot and the ticks) is back in
  `src/tape_echo_core.cc` for the screen; the render doesn't call it.
- **Kennel's page 2 and its time lock are fn held** (`view->mods` bit 1): fn + blue turns the gain and fn + green the
  sync, so while fn is down the gain's bracket shows and the beat ticks are bright green (kennel showed them blue while
  shift was held on the time page, its lock gesture). With a division in force the ticks stay at rest in green's dark
  tone: the sync is a saved mode that takes the time away from the blue turn, and the screen says so without words
  (guidelines §5.4).
- **The readouts are gone**: the division names (dv0..dv10), the head patterns (pt0..pt3) and the page-2 gain in dB
  (gs/gd/gdot/gt), about 16 KB of kennel's file. The core's popup names the control and its value on a turn or a
  click, and the lit heads show the pattern. Kennel's white page-2 bracket round the heads (wbox) went with the page:
  the pattern is a white click now.
- **The beat ticks are drawn by the code** with raster lines (`coop_scr_line`, kennel's geometry: y 43..46, a beat
  43..49) instead of 24 SVG lines.
- **Shapes**: kennel's head polygons are closed polylines with a rect inside for the fill (lit white, or the near-black
  occluder over the tape); the hold glyph has 24 segments (kennel 32).
- **Colours** are coop's brights (blue 698EFF, green 00ED95, white FFFFFF, orange FF3A5D); the fixed path in the
  pencil DFD9FF, the bus, rails and foot in the body tone 38354D, unlit heads in white's dark tone 5C5870, the hold
  glyph at rest in green's 00573A.
- SVG 8,118 bytes (kennel's 26,607), checked by `tools/plugin/pack.py` (R246's parser rules) and by the generator.

The render's only additions are the three display stores a block and the time computed before the clearing blocks
(it was after); the audio is bit-identical (`Tests` below). Not yet seen in op1emu or on the device: the tick
positions and the head fills against the SVG are the gate's to confirm.

## Stereo, memory, cost

- **Stereo:** the upstream's own stereo path (`render_stereo`, kennel ran it only at the master): one tape of L | R
  pairs, every head read serving both channels, so stereo costs about 1.56x the mono path rather than 2x (te-op1-cfw
  simulator: 9.2 MHz stereo, 5.9 MHz mono, whole effect). coop runs it at every position (synth, drum, master and both
  FX 2 slots): the instrument positions now hear the echo in stereo, and each position is a full stereo instance.
- **0.4 (2026-11-20, no divide in a steady block; `docs/efficiency-audit.md` finding 8):** the four turns, the gain
  and the sync division's time word are kept per slot and redone only for a word (or a tempo) that moved. Before, a
  block made six `___divsi3` calls for them (seven and a table walk with sync on, ~190 cycles each); a steady block
  now makes none. Simulator **15,039 avg / 16,018 max** over the same 3,000 blocks at 133.5 BPM (0.3 on that
  scenario: 16,079 / 16,833): 1,040 less a block. The same values reach the DSP, so the output is bit for bit 0.3's:
  `make -C tests/host/tape_echo` (te-op1-cfw's DSP, five tempo runs of 20,000 blocks, now with stretches where every
  turn and the gain move every block) and `sim`. With this change alone: file 24,508 bytes, `.text` 9,348, `.rodata`
  3,576, `.bss` 404 (+220: 44 bytes a slot, cleared by `init`). 0.4 also has the idle path ("Idle" below, with the
  file as it ships and its figures).
- **0.3 (2026-11-11, the tempo export):** file 23,892 bytes, `.text` 9,060, `.rodata` 3,560, `.bss` 184 (8 more: the
  tempo the render last saw and its 96th, so the 16-step division runs only when the tempo moves); one import more,
  `_coop_tempo_bpm_q16`. Simulator 16,090 avg / 16,844 max at 120 BPM (+41 over e9's 16,049: the export call and the
  compare a block; without the cache it was 16,440, the division every block).
- **e9 (2026-11-11, 0.2, the screen):** file 23,616 bytes (SVG 8,118), `.text` 8,864, `.rodata` 3,564, `.bss` 176 (the
  screen's node tables and the five buffer pointers); load 12,604 bytes once (+3,468 over e8's 9,136 with the e9
  `coop_fx_t.screen` field); the state 134,776 bytes (12 more: the three display words), `"scratch": 134784`
  unchanged. No 64-bit divide in the screen (`__divdi3` would add 1.4 KB). The SVG is parsed into the stock TE heap,
  not heap 0.
- **Memory (e8, 2026-11-11):** the tape, the core's state, the half-rate path and the block buffers (at most the declared 134,784 bytes, static_assert) are in the slot's position buffer (plugin.json `"scratch": 134784`, `coop_fx_scratch`), as kennel's `op1_fx_scratch` had them; the file is relocated once and its `.bss` is the five buffer pointers (20 bytes); init resets the state there at every selection. The figures below are from before e8 (state in `.bss`, five relocations). File 10,160 bytes, load 9,132 once; the core's five buffers are sized to this declaration when tape_echo is the largest installed (5 x 134,784 = 673,920 bytes for all scratch effects together).
- **Memory:** file 10,664 bytes; `.text` 5,836, `.rodata` 3,052, `.data` 0, `.bss` 134,764 (the tape 132,868, the
  core's state, the half-rate filters, the dry buffers). Load size 143,652 bytes; five relocations: **718,260 bytes of
  heap 0** (heap_largest was ~9 MB after the plugin scan in e4). No imports.
- **L1:** none (effects have no L1 lease yet, [proposed]). The hot data per sample is the kernel block (88 bytes), the
  six head reads and one write across the 133 KB tape, the soft-limit table (1 KB) and the saturation table.
- **Cost:**

  | | per block (64 samples) |
  |---|---|
  | kennel stereo, te-op1-cfw simulator (9.2 MHz / 689 blocks a second) | 13,350 |
  | coop, GNU simulator, `make -C tests/host/tape_echo sim` (compute only, the scenario's 3,000 blocks) | 16,049 avg / 16,804 max (e9; e8 15,987 / 16,746 on the same build, so the display words cost ~60 a block) |
  | coop, op1emu (`fx1_plugin=`, `s2_instr=`, `s2_master=`) | 0.3 at synth FX 1, sync 1/4 at 95 and 145 BPM: `fx1_plugin=17197/19074` (`docs/device-tests.md` "tape_echo 0.3"; 0.1: 16,778-16,821 avg) |
  | **coop, device** | **pending** |

  The ~2.4 k over kennel is the Q31 <-> int16 conversion of the bus, the dry copy and the clearing blocks averaged in.
  The device was 1.5-4x the simulator for kennel's effects (cache misses: the head reads span 133 KB of SDRAM), so
  expect 20-35 k a position on the device [I]: near the 30 k line, under 60 k. Five positions all running tape_echo
  would be ~80-160 k of the 580,499 budget [I].
- **Optimisation plan** (after the device figure, `docs/plugin-performance.md`): prefetch the three heads' tape lines
  a few lines ahead in the kernel (they advance about one pair a sample, so one line serves 8 samples); keep the
  kernel block in registers across the loop instead of loading and storing it every sample (the kernel touches
  `[P5 + ...]` some 20 times a sample); fuse the int16 -> Q31 output into the mix kernel; an L1 home for the kernel
  block and the soft-limit table once effects get a lease. The mono kernel for L == R is not an option: it records
  only the left half of the tape, and the right half would be stale when width returns.

## The tail settles (0.5)

**What changed in the sound.** Until 0.4 an echo with the feedback up never quite ended: its last, very quiet
repeats went round for ever (the owner's decision, 2026-11-21: cut them). In 0.5 every echo below the self-oscillation
region fades fully out, to digital silence. The repeats one hears are the same; only the last of the tail, from about
50 dB under full scale down, is shorter.

- **Why it was there.** The loop rounds to the nearest LSB at each of its stages (the heads' interpolation, their
  sum, the loss filter's step). With feedback g, a repeat under about 1 / (2 (1 - g)) LSB a stage comes back
  unchanged: 0.4 held 4 LSB of the 16-bit output at feedback 0.6 on a new tape, 11 at 0.87, 27 at 0.95, 41-47 at 0.97,
  for good (less or nothing on a worn tape, whose loss filter eats it). It also kept such an echo from ever idling.
- **The fix** (`src/tape_echo_core.cc` `SettleTail`, called from the render after the echo on every block whose
  input is exact silence): a block of tape written wholly within 127 (-48 dBFS on the tape) is taken one LSB towards
  zero, sample by sample. Louder blocks are written as they were, and nothing at all changes while anything is
  coming in. Tried and not kept: a step on every silent block whatever its level (it shortened high-feedback tails
  from -30 dBFS down) and a level of 31 (feedback 0.97 on a new tape still held).
- **Against 0.4** (`make -C tests/host/tape_echo tail`: the plugin at two positions in lockstep, one with 0.4's tail
  kept, 600 s of silence after two notes, full wet; 53 settings: feedback 0.6 / 0.87 / 0.95 / 0.97 x age 0 / 8,000 /
  32,767 x four times, the defaults, sync 1/16T, 1/4 and 1/2). No difference while the input plays. Then:

| Setting | largest difference in the first 3 s | largest in the whole tail (0.4's level there) | RMS of the difference over the tail | 0.5 exact zero / idle after the input | what 0.4 still gave after 10 minutes |
|---|---|---|---|---|---|
| defaults | 4 LSB | 4 LSB (32) | 0.04 LSB | 2.6 s / 3.9 s | 0 |
| longest time, feedback 0.6, age 0 | 2 | 8 (281) | 1.0 | 25.2 s / 25.2 s | 4 LSB |
| longest time, feedback 0.6, age 8,000 | 4 | 6 (63) | 0.3 | 19.4 s / 20.3 s | 0 |
| longest time, feedback 0.87, age 0 | 4 | 19 (29) | 4.4 | 59.3 s / 59.3 s | 11 LSB |
| longest time, feedback 0.87, age 8,000 | 0 | 14 (23) | 0.9 | 45.3 s / 45.8 s | 0 |
| longest time, feedback 0.95, age 0 | 0 | 52 (65) | 10.7 | 106.7 s / 107.3 s | 27 LSB |
| longest time, feedback 0.95, age 8,000 | 0 | 21 (21) | 1.7 | 81.8 s / 83.3 s | 0 |
| longest time, feedback 0.97, age 0 | 0 | 75 (75) | 11.9 | 165.8 s / 165.8 s | 41 LSB |
| longest time, feedback 0.97, age 8,000 | 4 | 35 (39) | 3.3 | 238.2 s / 238.2 s | 0 (19 LSB at 5 minutes) |
| shortest time, feedback 0.97, age 0 | 81 | 81 (83) | 27.6 | 2.9 s / 4.4 s | 47 LSB |
| sync 1/4, feedback 0.95, age 8,000 | 0 | 23 (23) | 1.4 | 37.7 s / 39.0 s | 0 |
| sync 1/16T, feedback 0.87, age 0 | 22 | 22 (29) | 5.6 | 2.4 s / 3.9 s | 9 LSB |
| sync 1/2, feedback 0.87, age 0 | 0 | 23 (23) | 4.5 | 33.3 s / 33.8 s | 11 LSB |

  (16-bit output LSB.) The largest differences sit where 0.4's echo is itself under about 100 LSB (-50 dBFS): that is
  the ghost going, and the difference can be as large as the ghost was. At the short times a tail has many repeats
  in three seconds, so its end comes inside them. Every one of the 53 settings reaches exact zero and the idle state;
  feedback 1.0 does not (three minutes: still 5,233 LSB, no idle block).
- **Cost.** A silent block whose echo has not died yet costs 15,328 cycles on average, 16,140 at most (simulator;
  0.4: 14,774): the tape's new pairs are read, and written back when the block is a quiet one. A playing block is
  unchanged (14,371), idle 1,382.
- `tests/host/tape_echo` (the comparison with te-op1-cfw's echo) builds the plugin with `-DCOOP_TAPE_ECHO_GHOST`,
  which is 0.4's echo sample for sample: bit-identical as before, five tempo runs on the host and the simulator.
  `tests/host/idle` runs 0.5 as it ships: its former ghost settings now idle (26.3 s and 77.6 s after the input in
  that test), and the idle state is still exact (0 LSB before, while idle, on the wake and after).
- File 25,444 bytes, `.text` 10,164, `.bss` 444; imports unchanged (it loads on core e10 as 0.3 does).

## Idle (0.4)

An echo left on a position used to compute its whole block on silence. 0.4 stops when there is nothing left to
compute (`docs/efficiency-audit.md` finding 6; the code is in `tape_echo.cc`, "The idle path"). **0.5:** the ghost
this section speaks of is gone ("The tail settles" above), so every echo under the feedback bound now gets here; the
figures below are 0.4's.

- **When.** The input block is exact silence (every sample's top 16 bits 0), hold is off, the feedback turn is under
  0.97 (29,000 of 32,767; 1.0 is 29,789), and for 1,047 blocks in a row (1.52 s: the tape once round, 1,038 blocks,
  and a margin) every pair the echo wrote to the tape was zero. The whole tape is then zeros: whatever the time, the
  heads or the age are set to next, the echo plays zeros.
- **Idle.** The render leaves the silent input as it came and runs only the control: the time's glide, the
  smoothers, the wow and flutter (in phase), the heads' ramps, the mix's smoothing, the screen's words.
- **Waking.** The check is at the top of the render, before any DSP: the first block with a non-zero sample, hold,
  or the feedback at 0.97 or over is rendered in full. Nothing is faded and no sample is skipped.
- **Never idle:** hold; feedback from 0.97 up (self-oscillation); any input; and a tail the loop's own rounding keeps
  alive. At high feedback on a new tape the echo holds a faint ghost of its last repeats for ever (measured: longest
  time, feedback 0.87, age 0: 9 LSB of the 16-bit output after three minutes; feedback 0.6, age 8,000: 2 LSB). That
  is this echo's sound, with or without the idle path: the check never passes on it and such an echo stays awake,
  at its full cost. Cutting a ghost under some level would be a change to the sound: not done (the owner's call).
- **What it gives while idle is what the echo would have given: zeros from a tape of zeros.** In the lockstep test
  the two never differ, awake or idle or after waking (0 LSB in every case below).

| Simulator, cycles a block (`make -C tests/host/idle sim FX=tape_echo`) | 0.4 without the idle path (its divides already out) | 0.4 |
|---|---|---|
| playing | 14,305 | 14,371 (the six-sample silence test and the idle state's flags) |
| silent, not yet idle | 14,295 | 14,774 (the input read through, the tape's new pairs read back) |
| idle | | **1,382** |
| the block that wakes | | 14,381 |

`make -C tests/host/idle FX=tape_echo` (the effect at two positions, one with the idle path switched off, every
sample compared; `tests/host/idle/fx_idle_test.cc`):
- 20,000 blocks that never go silent (controls walking and jumping, the latches, re-inits) and 20,000 of notes with
  150 ms gaps: identical, 0 idle blocks.
- A note, silence, a soft note (40 LSB), a loud one, at four settings: idle 3.9 s after the input at the defaults
  (the last echo falls under 1 LSB at 2.6 s), 2.0 s (short, worn tape, three heads), 39.4 s (sync 1/4, feedback 0.95),
  68.3 s (0.6 s, feedback 0.6). Difference before, while idle, on the wake's first block and through both notes: 0 LSB.
- Hold, feedback at 1.0 and at 1.1: a minute of silence, 0 idle blocks, 0 LSB; engaged while idle: awake on that
  block, 0 idle blocks after.
- The ghosts above: three minutes, 0 idle blocks, 0 LSB.
- Every turn walking and jumping through 1,500 idle blocks, then a note: 0 LSB.

`tests/host/tape_echo` builds the plugin with `-DCOOP_FX_NO_IDLE` (its scenario has silences longer than the hold, and
it compares with te-op1-cfw's echo sample for sample): bit-identical as before, host and simulator.
File 25,188 bytes, `.text` 9,908, `.bss` 444; imports unchanged (it loads on core e10 as 0.3 does).

## Tests

- `make -C tests/host/tape_echo`: the plugin against te-op1-cfw's own DSP and the kennel glue, 20,000 random
  blocks (noise, saws, impulses, silence, L != R; knobs walking and jumping; feedback over 1; gain, sync, heads and
  hold changing; restarts by pause and by re-init), on the host: **bit-identical** (11,521 blocks changed by the
  effect, 9,194 with sync on). The reference takes sync from kennel's own lock (`TimeLock::Word`, `DivisionWord`,
  `kDivision96ths`), and both builds print the 11 division time words, which must agree; at 120 BPM
  8543 11940 14362 15350 17746 20156 21142 23565 25961 26948 29358.
- **0.3, the tempo:** the same run at 120, 90, 133.5 and 174 BPM and a walk through seven tempos (90, 133.5, 174, 120,
  60, 200, 45; a change every 1,300 blocks, sync on and off). The coop build gets the core's export through
  `tests/host/tempo_host.cc` (the IEEE single the stock would hold, through the core's own conversion,
  `core/include/coop_tempo.h`); the reference gets kennel's `TempoIncrement` for the same tempo (the export's BPM x
  1662149.125, truncated: 199457895 at 120, kennel's constant). **Bit-identical at every tempo** (20,000 blocks each).
  96ths: 9800 at 90, 6606 at 133.5, 5068 at 174, 7350 at 120. The coop build also checks `NinetySixthQ4` against
  kennel's quotient at every tenth of a BPM from 20.0 to 400.0 (3,801 of 3,801 equal) and the fallback (0, over 400:
  120 << 16). Against the increment the stock itself computes (bpm x 1662149.125 in single precision, truncated), 27
  of the 3,801 tempos differ by one Q4 unit (a sixteenth of a sample at 22.05 kHz a 96th; e.g. 90 BPM: kennel on the
  device 9799, coop 9800): kennel's rounding, not a coop change. The 120 run is the scenario as before the export.
- `make -C tests/host/tape_echo sim`: the same in the Blackfin simulator (at 133.5 BPM, `SIM_TEMPO=1335`; also run at
  1200), the plugin's assembly kernels against the original's C: **bit-identical** over 3,000 blocks, plus the cycle
  figures above.
- `make -C tests/host/tape_echo tables`: regenerates `src/tape_echo_tables.h` from the original's constexpr tables.
- `tools/asm_lint.py plugins/tape_echo/src/*.S`: 0 packets flagged (neither kernel has a parallel packet).

**op1emu gate (not run yet: gate runs go one at a time):**

```sh
make -C plugins                                                     # build/plugins/tape_echo.coop
mkdir -p build/nand_te/plugins && cp build/plugins/tape_echo.coop build/nand_te/plugins/
python3 -I tools/make_nand.py --provision ~/op1emu-work/provision_nand.py --firmware ~/Downloads/op1_246.op1 \
    --mkyaffs2 ~/op1emu-work/yaffs2utils/mkyaffs2 --overlay build/nand_te --output ~/op1emu-work/plug_te.bin
```

1. `tests/sim/det_fx_select.txt` on that NAND (the e6 image): tape_echo at the synth position, a chord through it;
   `plugins seen 1 loaded 1 failed 0`, no fault, the `cost` line's `fx1_plugin=` figure; the WAV has echoes.
2. `tests/sim/det_fx_tape_echo.txt` (new: `det_fx_controls.txt`'s walk with tape_echo's controls): the plugin at p0
   synth FX 1, p3 instrument FX 2, p2 master FX 1, p4 master FX 2, p1 drum FX 1; expected `fxctl` per position in the
   file's header (p0 11197/0/1/0 at the end, p1 17749/0/1/0, p2 15019/0/0/1, p3 19114/0/1/0, p4 16384/3/2/0 with sync
   on `1/8t`; turns 11, fine 1, clicks 5, latches 4, ignored 2, cancelled 1), no fault, `s2_instr=` / `s2_master=`.
3. `tests/sim/det_fx_reload.txt` on the same NAND: gain, sync and heads kept at every position, hold off everywhere.
4. A pristine-NAND `det_chord.txt` unchanged against e6.

Read the stats cost line as an emulator figure (the device is 10-35 % higher, more with SDRAM traffic).
