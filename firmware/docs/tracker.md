| phase | 28 B | adds to the level word `0x01DE4B8C` (the live engine's display) | nothing: harmless (F2c, `pha`) |
| T1..T4 | the view (Shift + T2: the channel's patch for its coop voice, F2b) | | |# The tracker (t1): first pass, 2026-11-07

**Parked 2026-11-08** (the owner: "remove the tracker entirely for now… it's got a lot of gaps"): the code is in
`attic/tracker/` (sources, scenarios) and in the branch history up to `c2810de`; nothing of it is built or hooked.
To bring it back: the four sources into core/sdram and the Makefile, the hooks in coop_ui.cc (key / knob / frame /
service), stereo_bus.cc (block, the stage's extra voices), tape_io.cc (boot), coop_settings.cc (the UI row), the
stats counters. The gaps that parked it: drums beside the synth, dna / drwave / pulse, per-engine cache cost on the
device, the whole control scheme untested by the owner.

**Status before parking:** t1 (the tracker, both orientations) → F1/F2 (coop voices from seven stock engine kinds
beside the live family) → F2b (the channel patch) → t5 (render to tape). One package carries it all:
`build/pkg/op1_246_coop_t7_tracker_engines.op1` (device tests "Tracker 1..7" in docs/device-tests.md, all pending
the owner). Everything below marked **guess** waits for the owner's reaction.

The owner's brief is `docs/tracker-handover.md`; its decisions 1-7 stand. This is the first build, made with the
owner away ("make your best guesses… I'll have a look at the first pass… try make it both ways [portrait and
landscape]"). Everything marked **guess** is mine and is easy to change. [I] until the device; the op1emu gate is
in `docs/device-tests.md` "Tracker 1 (t1)".

Code: `core/sdram/coop_tracker.cc` (data, clock, sequencer, file), `core/sdram/coop_tracker_ui.cc` (views, keys,
encoders, the orientation transform), `core/sdram/coop_tracker.h`, `coop_text.h` (`coop_text_draw_m`: stock text
through a matrix), hooks in `coop_ui.cc` (keys, knobs, frame, service), `stereo_bus.cc` (the clock), `tape_io.cc`
(boot), `coop_settings.cc` (the UI page's "tape view" row).

## What it is

- **Tracker mode replaces the tape view** (decision 4). While it is on, tape mode paints black over the stock tape
  screen and shows the tracker; the tape's keys and encoders are taken. The tape itself is untouched (its engine
  keeps running; its audio still records what plays, since the voices are the stock's). Switch: **Shift + tape** on
  the tape page (guess), or settings → UI → **tape view**: tape / tracker (green). The choice is saved.
- **The tracker plays live** (decision 1) through the stock voices: each note is a `VoiceCtl::NoteOn` /
  `NoteOff` (`0x01A85750` / `0x01A85840`) with the bank word `[0x01DE4C40]` set to the cell's family for the call
  (0 synth, 1 drum), the same path every stock sequencer uses. Velocity carries the volume.
- **Channels are monophonic** (decision 2): six of them, a new note on a channel chokes its last one.
- **One family at a time, for now** [C in op1emu]. The core has ONE live play mode, `[0xFF801038]` (0 synth,
  1 drum): its voice loop renders that family's voices, and the note paths assert on the other one
  (`VoiceCtl.cpp:387 core_playMode == shared::PmDrum`; the UI's bank word `[0x01DE4C40]` is only the switch
  request, which the core applies at its own moment, cutting the old family's voices). The first t1 build flipped the
  bank word per note and hit that assert at the first drum cell. So t1 plays the cells whose family is the live one
  (the header says `syn` or `drm`: the mode you were in before the tape) and skips the rest; every synth channel
  plays the current synth sound, every drum channel the current kit. Synth + drums together, and engines per
  channel, need the handover's engine-instance work (F1-F3). The first pass therefore answers the workflow
  questions (views, controls, orientation), not the engine-count question.
- **Render to tape** (decision 5, one way; t5): **Shift + play** in the tracker. coop pushes the keys the user would
  press for the stock recording (rec held + play, KeyHandler::PushKey 0x019C2F24 as the test runner does; those keys
  are marked so the tracker lets them through), starts the song from its beginning when the play release has gone
  through, and pushes stop when the order list wraps: the tape holds one pass of the song on the current track at
  the current position, in the stock's own format, FX included. "rec" shows in orange in the header meanwhile; stop
  cancels (and stops the tape). Found in op1emu: the pushed keys must be spaced (~100 ms, a main-loop queue) and
  must wait for **Shift to be released** (Shift + play asked for the render, and shifted rec / play mean other
  things in stock); back-to-back pushes under Shift recorded nothing. Verified: the song plays, the tape holds the
  clip, and plays it back (`tests/sim/det_ui_tracker_render.txt`). Recording from the tape page with the tracker
  on works with real keys too (`det_ui_tracker_recreal.txt` on a `g_coop_trk_passkeys=1` image).
- **Tempo**: the tracker's own bpm (setup view), not the stock's; 4 rows a beat, `speed` ticks a row (default 6).
  The clock is counted in the audio block (`coop_tracker_block` from `coop_stereo_block_start`: samples per tick
  from bpm × 4 × speed); the notes are dispatched by the main loop (`coop_tracker_service` from `coop_fx_service`),
  so no stock call happens in the interrupt. Timing jitter = the main loop's pass interval: measured in the gate
  (the WAV's onset grid), to be measured on the device.

## Song structure (guess: PC-style)

16 patterns × up to 64 rows × 6 channels; an order list of up to 32 pattern numbers (the song); the pattern length
is per pattern (default 16). A cell: note (none / c-0..g-9 / `off`), instrument (`syn` / `drm`), volume 0..99
(`--` = the channel's default 80), one effect word with a value.

Effects (tick resolution, guess at the set; words, not hex):

| word | value | does |
|---|---|---|
| `arp` | two digits `xy` (default 37) | the note, +x, +y semitones, a tick each, round and round |
| `rtg` | n ticks | retrigger the note every n ticks |
| `cut` | n | note off at tick n of the row |
| `dly` | n | note on at tick n instead of 0 |
| `vol` | 0..99 | the channel's default volume from this row on |
| `prb` | 0..100 % | the note plays with this probability |

Not in t1: slides (`sld`) and pan (`pan`) need per-voice pitch and pan control the stock does not expose to a
sequencer (pan would come with the per-voice pan of S3 / u6 later).

## Views

T1 **overview**: all six channels side by side, one row a line, the row number left; a note as `c-4`, a `.` for
an empty cell, drum cells in green, the cursor cell in blue, an orange letter after the note for an effect
(`a r c d v p`). Small marks above the columns, the cursor's channel blue. The cursor row has a dark bar, the
playhead row a dark green one; the view follows the playhead while playing.

T2 **zoom**: one channel: row, note, `syn`/`drm`, volume, effect word and value; at the right a strip of six dots,
one per channel, big where that channel has a note on that row (the shown channel's dot blue). Landscape only has
the strip (portrait has no room).

T3 **song**: the order list: position, `p03`, its length in rows. T4 **setup**: bpm, speed, octave, view
(landscape / portrait a / portrait b), each in its encoder's colour.

Header: `tracker  p03  01/04  120` (pattern, position / song length, bpm) and `play` / `stop`, `edit`.

## Controls (guess, from the handover's draft)

| control | overview / zoom | song (T3) | setup (T4) |
|---|---|---|---|
| blue | note ±1 semitone (Shift: ±12); an empty cell gets c-4 (drum: f-3) first | position | bpm (Shift: ±10) |
| green | instrument `syn` / `drm` (also the channel's default) | the pattern at that position | speed 1..16 |
| white | volume ±1 (Shift: ±10) | song length | keyboard octave 0..6 |
| orange | effect word (Shift: its value) | that pattern's length (Shift: ±16) | orientation |
| blue push | note → `off` marker → empty | | |
| white / orange push | volume back to default / clear the effect | | |
| left / right | a row up / down (wraps); Shift: a channel left / right | a position | |
| keyboard | writes the note (with `edit` on), moves down a row; previews with the cell's family | | |
| play | play from the cursor row; again: stop. **Shift + play: render to tape** (below) | | |
| stop | stop; stopped: the cursor to row 0, then (again) to the song's start | | |
| rec | `edit` on / off (off: the keyboard only previews) | | |
| lift / drop / scissors | copy / paste / clear the cell | | |
| T1..T4 | the view | | |

The mode keys, Shift, Help, metronome, mic, COM pass to the stock. The tracker keeps playing when you leave tape
mode (play the song while on the synth page), and stops when the tracker is switched off.

## Orientation

Landscape (320 × 160, 9 rows), or portrait (160 × 320, 21 rows): **a** puts the view's top at the screen's right edge (the
device turned anticlockwise: the keyboard runs down the left), **b** at the left edge (clockwise). One transform maps view coordinates
to the screen: lines and fills by their endpoints. Text: landscape uses the stock font (Text objects bake place,
size and colour, so a pool holds one per (view, row, field, colour)). Portrait tried the stock Text object's 3 × 3
matrix (kennel's row-vector convention `[a b 0; c d 0; tx ty 1]`, Q10; `coop_text_draw_m`): **the stock takes only
the translation; the glyphs stay upright** [C op1emu]. So the portrait views draw kennel's stroke font (capitals,
4 × 6 grid) with every segment transformed: 9 px a character in the zoom / song / setup views, 7.5 px in the
overview and header. Not the stock font, but turnable; the owner judges whether that is acceptable (a rotated
stock font would mean rendering its glyph outlines ourselves). In portrait the arrows still mean "a row up / down";
the encoders keep their colours.

## Storage

`/yaffs2/user/coop_song.bin`: the `trk_song` struct as is ("CSNG", version 1, ~37 KB), written by the main loop
two seconds after the last edit and when the tracker is switched off; read after the tape mounts (header first into
a small buffer, the patterns straight into place). One song for now; files / SysEx transfer later.

## Cost

The tracker adds nothing to the audio block but a counter; the drawing is ~60 stock text draws and ~50 strokes a
frame in tape mode only. The gate's `trk_frames` and the device's fps are the measure. `g_song` and the text pool
add ~190 KB of bss (region A has room).

## Open questions for the owner (the handover's §7, with what t1 did)

1. Per-channel effects: t1 shares the stock's one instrument effect (and coop's FX 2). Per channel needs per-channel
   voices.
2. Engine per row: t1 has the family per cell, and only the live family sounds (above). The F1-F3 work (a second
   voice set rendered in the same block, outside the core's single play mode) is the next step if the workflow is
   what the owner wants; it decides engines per channel too.
3. Drum channels: mono like the others (a new hit cuts the last) in t1.
4. Pattern length and structure: PC-style patterns + order list, 16 patterns, 1..64 rows each.
5. Effect display: words.
6. Keys: Shift + tape enters; T1 / T2 switch overview / zoom; Shift + T2 the channel patch; Shift + play renders.
7. Stems or a mix: t5 records the instrument mix onto the current tape track (the stock recording); stems would
   mean four passes with channels soloed, or a coop record tap per group.
8. Orientation: both built; the setup view's orange picks. Which one feels right, and which way round?
Plus: is the main-loop dispatch tight enough on the device (listen for swing in a straight pattern)? Should the
keyboard write without `edit` by default (it does)? Should the tracker's tempo follow the stock's bpm instead?

## F1: a second engine at the same time (2026-11-07)

The owner asked whether notes from different engines can sound together. Through the stock's note API: no (the core
has one play mode, above). Rendered by coop: yes, and F1 is the probe (`core/sdram/coop_xvoice.cc`):

- One stock **FM voice** (Fm.cpp, engine 0) is built in coop memory with the stock's own constructor (`0x019A21D8`,
  60 bytes, in the coop L1 pool when it is taken), with its own parameter block filled by the engine's setup
  (`0x019A3D84`, from the FM patch words at `0x01D05880`).
- Every block, from coop's instrument stage (before the pair is decimated), the voice is rendered with the stock
  render (`0x019A23D4`) and placed into the stereo pair exactly like a stock voice. The render reads the per-sample
  pitch words at `0xFF801950` and the shared parameter pointer at `0xFF906F6C`: both are swapped in for the call
  and put back, so the live engine never sees a change. The sine table is already the L1 copy in every block.
- The ADSR the stock applies after render is coop's own here: a linear 2 ms attack and 120 ms release in Q15. Pitch
  is `f / 88200 * 2^32` from the MIDI note (a 12-entry 2^(k/12) table and a shift).
- The tracker's instrument field has a third value, **`fm`** (green: syn, drm, fm). Cells on `fm` go to this voice
  whatever the core's play mode; it is monophonic (one voice for the whole tracker in F1).
- Cost: `xv_max` / `xv_sum` / `xv_blocks` on the stats "stages" line, cycles per block while the voice sounds.
  Silent, it costs a few loads.

What F1 does not answer yet: a second *instance* of the live engine type (two patches of one engine), engines whose
voices depend on the scratchpad address (`this - 0xFFB00000`: Pd does), and the I-cache cost of several different
engines' render loops in one block. Those are F2 / F3.

**F1 result (op1emu, 2026-11-07, `tests/sim/det_ui_tracker_fm.txt`)** [C op1emu]: with the stock synth live, a
channel on `fm` sounds together with a synth channel: at every pattern start the WAV carries the FM voice's
spectrum (its `e4`, then `c5` at row 2) over the synth's `c4` / `g4`; no faults. Cost: **`xv_max` 9,795 cycles a
block** while the voice sounds (average 9,766 over 952 blocks: the FM render ~6 k as the stock loop pays it, plus
the pitch-word save / restore, the envelope and the pair accumulate), 1.7 % of the 580 k block; silent, nothing.
Six such voices would be ~10 % in op1emu, times the device's 1.2–1.7 cache factor: inside the budget on paper.
The FM patch words at `0x01D05880` hold a plucky sound in the pristine NAND (the voice decays in ~0.4 s): the
channel's own FM patch is the next step. Observation: in op1emu both the stock synth and the FM voice sounded an octave
below nominal (`c4` at 130 Hz, the FM `c5` at 260.6 Hz); F2 found why: the pristine NAND has the synth octave at −1
(the core folds it into its pitch words) and the FM patch words carry a 0.5 carrier ratio. The pitch formula is
right (the string and dsynth land on the nominal note). The device test listens for the FM voice in tune.

## F2: a voice per channel, four engine kinds (2026-11-07)

`coop_xvoice.cc` generalised: every tracker channel owns a slot with one voice object per extra engine kind, all
built at the tape mount with the stock constructors (in the coop L1 pool while it has room, else SDRAM), and the
cell's instrument picks the engine at note-on: `syn`, `drm` (the live families, as before) then **`fm`, `str`
(string), `dig` (digital), `dsy` (dsynth)**. What each engine reads outside its object, and what coop does about
it, is in the file's header; the short version:

| engine | object | outside the object | coop |
|---|---|---|---|
| fm | 60 B | the shared parameter block via `0xFF906F6C` | one coop block, the pointer swapped in around the block |
| string | 32 B | a 16 KB delay line at `[+0x14]` (`0x010225EC + i * 0x4000` in stock) | a coop buffer each (96 KB of bss) |
| digital | 32 B | nothing but the pitch words and L1 scratch | nothing |
| dsynth | 72 B | per-voice tables by the index at `[+0x14]`; the voice envelope at `0xFF801D50` | the slot number is the index (**clashes with the live DSynth's voice i**: probe limit); its envelope written before the render |

Common to all: the pitch words at `0xFF801950` (and the envelope words) are saved once a block, written per voice,
put back; the sine table is the L1 copy in every block (the package's retarget); the L1 scratch buffers the renders
use (`0xFF801B50`, `0xFF801E50`, `0xFF801F50`) are free at the stage. Note-off goes to the engine where it has one
(string sets its release, dsynth its own); the coop envelope (2 ms / 120 ms) shapes everything on top.

**F2d: voltage and cluster** join as `vol` and `clu`: their getters build the voice inline at the scratchpad
address (vtable word, the stock's small sub-object constructor `0x019F15F8`, its array constructor
`0x01AA3862(dst, n, size, ctor)`), so coop replays the same words and constructors on its own object
(`build_voltage`, `build_cluster` in coop_xvoice.cc). Voltage writes the live engine's 32-sample display copy
(`0x01DF6154`), cluster reads the engine's shared parameter words (`0x01BAA91C..`) and the shared LCG: harmless.

Not offered: **dna** (its render takes the voice index from `this - 0xFFB00000` for per-voice tables), **drwave**
(a voice index at `+0x1C` for per-voice tables, like dsynth, plus a web of globals) and **pulse** (its render is
L1 code); the drum family (above). The channel's patch and pan came in F2b, phase in F2c.

**F2 findings (op1emu, 2026-11-07)** [C op1emu]:
- **The parameter pointer is the slot's 8 knob words**, not 4: dsynth's render reads `+0x8..+0xc`, so a four-word
  array fed it whatever lay after it (a loud fixed tone in the first three-engine run). And the ranges are per
  engine: the string's knob 0 `>> 7` indexes a 128-entry table from 128 *down*, so mid-scale 0x4000 is index 0 (an
  extreme: the string went silent); 0x2000 is the middle. Each engine kind now has its own 8-word set until the
  channel carries a patch. A peak meter per engine (`xv_peak_*` in the stats) says whether a render makes sound.
- **Pitch is right.** From the same words (f / 88200 · 2^32) the string and dsynth land on the nominal note; the FM
  voice an octave down (its stock patch words carry a 0.5 carrier ratio); the stock synth an octave down in op1emu
  because the pristine NAND's synth octave is −1 (the core folds it into its own pitch words). So no octave bug in
  coop's words, and the earlier "dump sits an octave low" reading was wrong: it was the FM patch and the octave.
- **Cost a block, op1emu, one voice sounding (`xv_max`, coop's ~2 k of save / restore and envelope included):**
  fm ~10–11 k, phase ~12 k, digital ~13 k, voltage ~14 k, cluster ~18 k, dsynth ~25 k, string ~30 k (its 16 KB
  delay line). The three-engine run (fm +
  string + dsynth beside the live synth, `tests/sim/det_ui_tracker_x.txt`): **63.0 k peak, 45.6 k average** a
  block, 11 % of the 580 k block at peak, additive: op1emu models no cache, so the I-cache cost of mixed engines
  stays a device question (the stats` xv_max` on the device answers it).

## F2b: the channel's patch (2026-11-07)

**Shift + T2** opens the channel's patch view for its coop voice: four bars in the encoder colours, the engine's
four knobs (0..99 %, mapped onto each engine's own range: the string's knob 0 tops out at 0x3FFF, the others at
0x7FFF), and **pan** (Shift + blue, l50..r50) top right. The values are the song's (`ch_knob`, `ch_pan`; song
file version 2) and apply live while the voice sounds (coop_xvoice_set rewrites the slot's knob words and pan
delta); the keyboard previews with them. A channel whose default instrument is a live family shows "a live
family: no patch here" (the stock's own page is its patch). Guess: the four knobs mean what the stock engine's
four encoders mean; the owner's engine screens would be the next step for reading them.

**Drums beside the synth (not done, looked at):** the stock drum voices are not self-contained objects like the
synth engines: the drum note-on (`0x01A84BA0`) allocates one of 12 voices in an L1 flag array (`0xFF801070`) and
writes per-voice pitch / level / position words into L1 arrays (`0xFF8010F4`, `0xFF801124`, `0xFF8012EC`,
`0xFF801304`, `0xFF801334`, through the float helpers) that the drum render reads, and the voice objects live in
the scratchpad with the kit's sample memory behind them. A coop drum player therefore means either reproducing
that L1 state for a second voice set, or a coop one-shot sampler over the kit's sample buffers (their base and the
per-key start / end from the drum patch). Both need another RE pass; until then drum channels play when the kit
is the live family.
