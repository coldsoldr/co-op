# Handover 2026-11-07: tracker mode (feasibility first)

The owner wants to explore a tracker workflow on the OP-1. This is a new feature line with no code yet. The first run
is a **feasibility study plus a mock-up**: measure whether the OP-1 can play several engines at once, then mock the
screens in the playground. Do not start the tracker itself until the owner has read the findings and approved the
mock.

Read first: `CLAUDE.md`, `docs/brick-safety.md`, `docs/performance.md` (especially "Per-engine cost and the engine
scan" and "p5d on the device"), `docs/ui-features.md`, `guide/plugin-guidelines.md`,
`../te-op1-cfw/docs/engine-interface.md`, `../te-op1-cfw/docs/sequencer-interface.md`.

Background for the concepts (patterns, cells, ticks, effects, order list, LSDJ-style hierarchy):
the owner's primer page, https://claude.ai/artifact/Ndm9kag3iNTxyyVgN4nawR. You don't need it to follow this
handover. The glossary below covers the terms.

## 1. What the owner decided (2026-11-07)

These are the owner's own decisions. Don't reopen them; raise a problem with one only if a measurement forces it.

1. **The tracker plays live.** Every channel renders its engine in real time. It is not a note editor that bounces
   to audio as you go.
2. **Each channel is monophonic.** One sounding note per channel; a new note replaces the previous one. Chords come
   from several channels or from an arpeggio effect. This is the main lever for keeping the CPU cost down.
3. **An instrument is an engine:** a stock synth engine, a coop engine plugin, or a drum engine (with its preset).
   Different channels may use different engines at the same time.
4. **Tracker mode replaces tape mode.** The user switches between tape and tracker. They are two separate
   workspaces, not one view inside the other.
5. **Render to tape is a one-way export at the end.** The finished song is recorded onto tape. There is no workflow
   that goes back and forth between tape and tracker.
6. **Two views:** an **overview** with all channels side by side, and a **zoomed view** of one channel per screen
   (LSDJ / M8 style, with more room per field).
7. **Channel count is open:** 4, 5 or 6, and the owner doesn't know whether the OP-1 can do it. Answering that is
   the main job of this run.

## 2. Glossary

| Term | Meaning here |
|---|---|
| Row | One time step. The playhead advances one row at a time. |
| Channel | One column of rows. Monophonic. Each channel has one instrument at a time (changeable per row). |
| Cell | One channel's fields on one row: note, instrument, volume, effect(s). |
| Pattern | A block of rows across all channels (16 to 64 rows, length TBD). |
| Order list | The song: the list of pattern numbers in play order. Patterns are reused by number. |
| Tick | Subdivision of a row. Notes start on tick 0; effects act on later ticks. Speed = ticks per row. |
| Effect | A three-character command in a cell, e.g. arpeggio `037`, retrigger, cut, slide. |

## 3. The central question: can the OP-1 play several engines at once?

### What is known [C unless marked]

- **Stock runs one synth engine at a time.** The factory builds 6 voices of the selected engine
  (`producers[v] = Engine_GetVoice(v)`, `kSynthVoiceCount = 6`) and places them in the **4 KB L1 scratchpad at
  0xffb00000** (`../te-op1-cfw/docs/engine-interface.md`, "Factory and polyphony"). A second engine's voices would
  have to live somewhere else. The stock instrument FX object also sits in that scratchpad (0xffb00600, the S1
  note in `docs/plugins.md`).
- **Synth and drum voices already render in the same block** into one voice bus, with one instrument effect on
  both (S3, `docs/stereo-bus-v2.md`). So two engine *families* can already coexist. Core keeps
  `s_producers[instance]` (`Core.cpp:1216`), which suggests a per-instance array. Map this first; it is the
  existing model for "more than one engine".
- **Shared per-render inputs:** the encoder values passed to render (knobs at 0xff801724) and the per-sample pitch
  increment array at **0xff801950** (128 entries, L1 data A). Both are set by Core before each voice render. Per
  channel, those values would have to be set before each render.
- **Notes reach voices through `VoiceCtl::NoteOn` 0x1a85750 / `NoteOff` 0x1a85840** (`sequencer-interface.md`).
  These play the *current* instrument. The stock sequencer thread ticks only the live instrument's player.
- **Cost, device [C device]:** a six-voice DSynth chord costs about 245 k cycles per block (42 % of 580,499) on p4b
  (`docs/handover-2026-11-04.md`), so roughly 40 k per DSynth voice. Six FM voices with the tape rolling cost about
  41-45 % (`docs/performance.md`). Device engine costs come out 1.2-1.7 times the op1emu figure.
- **Cost, op1emu, per voice render:** dsynth 18.6 k, pulse 16.5 k, phase 10.3 k (5.8 k with the rewrite), drwave
  9.6 k, voltage 8.4 k (rewritten), sampler 7.2 k, digital 7.1 k, fm 5.9 k (`docs/performance.md`, engine
  ranking). The device's `engines` stats line gives the real ranking. If the owner has a p5a+ stats file with that
  line, use it.
- **Budget:** coop audio stays under 0.75 of the block (about 435 k cycles), measured per-block **peak**. The UI
  fps floor is 40 idle and 27 under load (`coop-fw-performance-constraint`, `docs/performance.md`).
- **Instruction cache:** 16 KB; 12 KB of hot code fits (`docs/performance.md`). The FM lag was a data-miss cost
  (the sine table in SDRAM, fixed by moving it to L1).

### What is not known [I]

- **Instruction-cache thrash across engines.** Every measurement so far runs *one* engine's code per block. Six
  channels on six different engines will cycle through several render loops per block. The cost of that is new and
  could dominate. Measure it with the perf counters (PFCTL 0xFFE08000, PFCNTR0-1 0xFFE08100-8104 count I- and
  D-cache fills, see kennel `kn_diag.h`).
- **Whether engines tolerate being instanced outside 0xffb00000.** Some may assume their L1 address or keep
  single-instance state (`ls` is documented as single instance; check the others' setup functions in the "then"
  column of the engine table).
- **Per-channel envelopes, LFO and FX.** Stock gives each voice the OP-1 ADSR, and gives the instrument one effect
  and one LFO. Does each channel get its own effect? (Probably not in v1; see the open questions.)
- **Mono channels and the 6-voice pool.** Six mono channels on six engines is six voice renders per block, the same
  count as one stock six-voice chord. Release tails make it a little more if a channel's old note is allowed to ring
  out. Recommendation: cut on retrigger (choke) in v1.

### Rough arithmetic [I]

Six channels with one voice each, at device costs of about 1.5 times op1emu, is roughly 10-30 k per channel: 60-180 k
for six channels, before FX, the stage, the master and any cache-thrash cost. That leaves room under 435 k on paper.
Cache thrash and per-channel setup are the unknowns that could break it. In tracker mode the tape tick and tape I/O
may not need to run (tape mode is not active), which would give back some budget. Verify this before counting on it.

## 4. Feasibility work for this run (in order)

Each step ends with a written result in `docs/tracker.md` (create it) and, where it needs the device, a measurement
package plus a `docs/device-tests.md` entry the owner can run. No tracker UI and no sequencer code in this run.

- **F1. Map instances.** How Core holds synth and drum producers (`s_producers[instance]`), where each family's
  voices, ADSR state, knob slot and pitch array come from, and what the factory's "synth reset" tears down. Output:
  what it takes to have N engine instances alive at once, and which addresses each one needs of its own. Cite
  addresses from `symbols/*.sym` or the L1 map.
- **F2. Engine survey for relocation.** For each stock engine (and the coop P3 engine ABI, `coop_engine_t`, planned
  in `docs/plugins.md`), list the state that is tied to 0xffb00000 or a single instance. Classify each engine as
  "relocatable", "needs a patch" or "single instance only".
- **F3. Mixed-engine cost measurement.** A diagnostic image, no UI, that renders K voices of K *different* engines
  in one block (K = 4, 5, 6), using the existing per-render CYCLES thunks and the `engines` / `cost` stats lines,
  plus I-cache and D-cache fill counts. Run it in op1emu (gate it, `tests/sim/`), then build a device package for
  the owner. Only the device figures decide anything (`docs/performance.md` "p5d on the device"). Runs in op1emu
  must be sequential (wall-clock waits).
- **F4. Recommendation.** Channel count, which engines are allowed per channel (if some are too expensive to mix),
  and what FX a channel gets. Include headroom for the instrument FX, the master and the fps floor. State
  confidence [C]/[I] for each line.
- **F5. Mock-up.** Add the overview and zoomed views to `tools/playground/coop-playground.html` (section 5 below),
  each in landscape (320 × 160) and portrait (160 × 320). Also run one op1emu check that a Text object drawn with a
  rotated matrix comes out right (no product code, a scratch image). The owner reviews the mock before anything is
  built.

## 5. Proposed design to mock (not decided, for the owner to react to)

### Views
- **Overview:** all channels side by side, one row per line, playhead row highlighted. At 320 × 160 each channel
  only has room for its note and a marker (instrument and effect shown as short glyphs or colour). Cursor moves
  across channels and rows.
- **Zoomed channel:** one channel, the full cell (note, instrument, volume, effect, maybe a second effect), about
  8 rows visible, and a thin strip at the edge showing every channel's activity on the current row. This is the
  editing view.
- Switching between overview and zoom: one key, to be chosen with the owner. Check it against the existing
  shortcuts in `docs/ui-features.md`.

### Portrait orientation (owner's idea, 2026-11-07: mock it beside landscape)
The owner asked whether the tracker could run with the screen turned 90 degrees, so it is taller than wide
(160 × 320). Trackers read top to bottom, so a tall screen suits them.
- **What it gives:** about twice the rows on screen (roughly 16-20 at a legible size instead of about 8). Each line
  is 160 units wide. That holds one full cell (`C-4 SY 30 ARP7`, about 13-14 characters) comfortably, so the zoomed
  channel view fits well. The overview gets tight: 4 channels of note-only columns fit, but 6 channels need a smaller
  font or glyphs instead of text. Mock both counts.
- **How the device is held:** the whole OP-1 is turned, so the keyboard runs vertically beside the screen, and the
  four encoders become a column, blue at one end and orange at the other. Pick the rotation direction (clockwise or
  anticlockwise) that puts the screen nearest the user and keeps the encoders on the side of the hand that turns
  them. The owner decides this with the mock.
- **Drawing [I]:** coop's text already goes through the stock Text object, which draws with a 3 × 3 Q10 matrix
  (`core/sdram/coop_text.h`, `op1_text_draw`, 0x01A6259C). A 90-degree rotation may only need a different matrix,
  e.g. `{0,-1024,tx, 1024,0,ty, 0,0,1024}`, for text and for any shapes drawn through the same raster. Confirm in
  op1emu with one rotated string before planning the views around it. Check that clipping and the font's hinting
  survive a rotated matrix.
- **Controls follow the orientation:** in portrait, "down a row" is along the screen's long axis. The encoder and
  arrow-key meanings must be remapped so "up / down" still means up and down on the turned screen.
- **Mode-wide:** if the tracker is portrait, the whole tracker mode should be portrait (settings and song pages
  included). Mixing orientations inside one mode makes the user keep turning the device.

### Controls (draft)
- Keyboard: writes a note at the cursor with the channel's current instrument, then moves down by the edit step.
- Encoders on the zoomed view map to the four fields, using the OP-1 colours: **blue = note** (semitone; shift =
  octave), **green = instrument**, **white = volume**, **orange = effect** (type; shift = amount).
- Rows per beat, speed and BPM on a shifted page. The transport plays from the cursor or from the song start.
- Effects shown as short words (`ARP`, `SLD`, `RTG`, `CUT`, `DLY`, `VOL`, `PAN`, `PRB`) with a value, not raw hex
  (the OP-1 leans to words and pictures; ask the owner).

### Song structure
- Order list → patterns → rows × channels (PC style), or song → chain → phrase per channel (LSDJ style).
  The two-view decision fits PC-style patterns, with the zoomed view as a per-channel lens on the same pattern.
  Raise the choice with the owner in the mock.

### Effect set v1 (proposal)
Arpeggio, pitch slide, glide to note, retrigger, note cut, note delay, volume, pan, probability. Everything acts at
tick resolution through a voice hook the tracker owns, not through the stock sequencer.

## 6. Engineering notes for later (after the owner approves)

- **Clock:** run tracker ticks from the audio callback, not the UI thread, so rows stay steady when the screen is busy.
  At 125 BPM a tick is 20 ms, about 13.8 blocks, so block-quantised ticks jitter by up to 1.45 ms. Note-on at a
  sample offset inside the block is a later refinement.
- **Note dispatch:** the tracker owns its channel voices and calls render / note-on / note-off on each channel's
  engine instance directly. `VoiceCtl` only reaches the current instrument.
- **Mode switch:** tracker replaces tape. Decide what happens to the tape engine while the tracker runs (stopped,
  memory kept, so returning to tape is instant), and whether the stock tape screen draw must be kept away from any
  coop state (see the T2 lesson in `op1-firmware-facts`: the UI thread sees transient audio-path values).
- **Render to tape:** the song plays once from the start while its output records onto tape, then the user is in
  tape mode with the result. Options: a stereo mix onto one track, or stems (up to 4 groups onto the 4 stereo tape
  tracks in one pass, through the existing stereo record tap). One way only.
- **Storage:** songs as files under `/yaffs2/user/` (format TBD, versioned header). A 64-row pattern with 6
  channels × about 6 bytes is about 2.3 KB. Transfer over SysEx through co-op, the same way plugins install.
- **Hard rules:** fixed point only, no float, no libc, no static constructors, small hot loops. Nothing the UI
  thread reads gets a transient value. No `MALLOC` in the audio interrupt. Every package passes
  `tools/verify_bootloader.py`, and the owner flashes.

## 7. Open questions for the owner (put them in the findings, don't guess)

1. Per-channel effects: none in v1 (one shared instrument effect, as stock), one per channel, or one per engine
   family?
2. Can one channel change engine per row (instrument column), or is the engine fixed per channel and only the
   preset changes? The CPU and memory answer from F1-F3 may decide this.
3. Drum channels: mono like the others (a new hit cuts the last), or allowed to overlap hits?
4. Pattern length and the song structure (PC-style patterns, or LSDJ-style chains).
5. Effect display: words or hex.
6. The keys for: entering tracker mode, switching overview ↔ zoom, render to tape.
7. Stems or a stereo mix for render to tape.
8. Orientation: portrait (screen turned 90 degrees, which way round) or landscape. Decide from the two mocks.

## 8. Deliverables of this run

- `docs/tracker.md`: F1-F4 findings with confidence tags, and the answers the owner needs to make the
  channel-count decision.
- A measurement package (F3) and its `docs/device-tests.md` entry, gated in op1emu first.
- The overview and zoom mock in the playground (F5).
- No commits unless the owner asks. Commits are authored as coldsoldr <349275560+coldsoldr@users.noreply.github.com>, with no AI trailers.
