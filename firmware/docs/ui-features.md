# UI features: the owner's decisions (2026-11-07)

How each coop feature is reached and how it behaves, as the owner decided in conversation on 2026-11-07. Nothing here
is built: every feature is mocked in the playground (`tools/playground/coop-playground.html`) before it's coded, and
nothing is done without a device result in `docs/device-tests.md`. [I] marks a detail still to verify; everything
else is the owner's word.

Process rule (the owner): **mock up everything before building it.**

## 1. Settings menu — built in u1 (2026-11-07), device test pending
- p6c (2026-11-08): a fifth VOICES row "code prio" (the hot code page's I-cache priority) was built and **withdrawn the same day** (p6d): on the device it glitched the sound and lagged the UI (docs/performance.md "p6c"). The menu is back to four rows.
- Open: hold synth + drum + tape + mixer. This replaces the lander game, which coop removed.
- Navigate: blue turn moves between items; right arrow or a blue push enters a submenu; left arrow goes back one
  level. At the top level, left arrow or any mode key leaves. No hints on screen (the owner, after u5).
- Edit: green turn changes the selected item's value; the value is drawn in green.
- Submenus:
  - **Voices**: spread amount 0-100 (synth only; drums have per-voice pan instead); drum ADSR (stock / transient
    designer, TBD); master EQ (stock / advanced, TBD); master compression (stock / advanced, TBD). The TBD
    alternatives are shown greyed until specified.
  - **UI**: tape position (bottom, stock / top, as the op1repacker mod); date and time (for file timestamps on save;
    24 h; `yyyy/mm/dd`).
  - **Plugins**: one row per file with kind, version and measured cost; green turn sets enabled (right) / disabled
    (left), an encoder click toggles (once per 250 ms: e4c), applied at the next boot.

## 2. Pitch bend (per synth preset) — built in u5 (2026-11-07), device test pending (the semitone scale of the stock bend word is inferred: calibrate on the device)
- Bend stays stock: shift + left / right.
- Amount and rate are on the **shifted T2 screen** (the stock page with play mode and portamento). Engines keep all
  four shifted encoders on T1 for their own second rows.
- Amount: 0 to 12 semitones in steps of 1, the same both ways (the owner, after u5: a symmetric range). Time: `OFF`,
  1-127, `MAX`, the stock portamento scale, mapped to 0 ms – 2 s (exponential): how long the pitch takes to reach the
  new bend. u7: the stock's own bend word is a rate toward a fixed octave, so coop drives the live bend word the MIDI
  handler writes instead (docs/screens.md "u7").
- Encoders: white = amount, orange = rate [I: to verify that stock leaves both free on that page; blue = play mode
  and green = portamento in stock].

## 3. Drum per-voice pan (per drum preset) — built in u6 (2026-11-07): the owner moved it to the drum's shifted T2 page (the stock takes all four shifted encoders on T1); green pans the last played key, saved in the kit's "coop" member
- Shifted **T2** on the drum page (decided 2026-11-07): holding Shift there shows coop's own PAN page in the style
  of the synth's play-mode page: "PAN" in blue with the value at the right; **blue** = pan of the **last played key**
  (the 24 drum keys), `L50 … C … R50` (the owner, after u6). Stock's shifted blue there (the dynamic envelope's fifth
  word) is taken over.
- Spread does not apply to drums. White and orange on that layer stay empty.

## 4. Overwrite save — built in u5 (2026-11-07), device test pending (sampler presets: the audio is not re-copied)
- Hold synth (or drum) + hold any slot key S1-S8 through the save clock: the sound replaces its own saved preset.
  The popup says `OVERWRITING`.
- A factory preset, or a sound never saved, falls back to a normal save.

## 5. Tape undo / redo
- Tape + left = undo, tape + right = redo. Every tape edit counts, including recording (the take's overwritten
  audio is kept).
- A few levels; the exact depth follows a memory measurement (recording undo is the expensive one).
- The screen gives feedback: the step undone / redone, and the depth remaining.

## 6. Rename a preset — built in u5 (2026-11-07), synth user presets, device test pending (sampler presets refused: their audio file carries the name)
- As OPUS: in the preset browser (shift + S1-S8) on a user preset, orange push starts naming; blue letters, green
  digits, white symbols (`! # + , - . ; =`); turn to pick, push to keep, push with nothing turned for a space;
  orange anticlockwise deletes; orange push saves; any other key cancels. 13 characters, lowercase. Factory presets
  can't be renamed.

## 7. Two effect slots — built: lift / drop / T3 (u1), the green column on the list (u3, 2026-11-07); device test pending
- u9 (2026-11-08, the owner's u7 notes, op1emu-verified): no "none" on the FX 2 lists; an empty slot 2 takes FX 1's type and knobs, switched off, on the first drop; T3 switches FX 2, **Shift + T3 opens the stock list** (blue FX 1, green FX 2); the stock page's big "FX OFF" is the focused slot's own state (the on-flag getters the overlay predicate calls are wrapped); the dials follow FX 2's encoders (the vector is filled in range units as the stock does: docs/screens.md "u8").
- Shift + T3 on synth, drum or master opens the FX list with **two columns**: blue column = FX 1 (as stock), green
  column = FX 2. An untouched sound has no FX 2 (slot 2 empty, nothing runs), so it stays stock; the first drop
  fills slot 2 with FX 1's type, switched off.
- On the effect screen, **lift** focuses FX 1 and **drop** focuses FX 2. The focused slot's page is shown; T3 shows
  the slot last touched. The choice persists with the sound. FX 2 must look exactly like the stock effect page of its
  type (no coop page, no corner marks: the owner, after u5).
- The stock `FX OFF` overlay shows the focused slot's state (u9): FX 2's while FX 2 is in focus, FX 1's after lift.
  (The earlier idea of `FX1 OFF` / `FX2 OFF` labels is dropped: the page stays exactly stock.)
- This replaces the P3a Shift + Help page (`docs/screens.md`).

## 8. Two LFO slots — the green column on the list built in u3 (2026-11-07); one LFO 2 for both instruments until P3b
- u8 / u9 (2026-11-08): **the LFO 2 page is the stock LFO page** (drop / lift on the LFO page, T4 off / on, the four encoders as the stock's, Shift + T4 the list; the stock's "LFO OFF" overlay shows LFO 2's state in focus, as the FX page does); **LFO 2 is per sound** ("lfo2" in the sound JSON; the synth voices follow the synth slot's sound, the drum voices the kit's). The green column and the page edit the sound shown.
- Shift + T4: blue column = LFO 1, green column = LFO 2; lift / drop focus them on the LFO screen, as for effects.
  Same destinations, additive (`docs/lfo2.md`).

## 9. Mute and solo — solo built in u4 (2026-11-07), device test pending
- Shift + T1-T4: mute the track, as stock.
- Shift + Help + T1-T4: **solo** the track; several T keys held together solo several (a solo set), as Shift +
  T1 + T2 mutes both in stock. Works on the tape and the mixer pages. Leaving the page keeps the state. No text
  on screen (the owner, after u5).
- Help is the fn key for every shift + fn combination (the manifesto's deep layer).

## 10. Randomise — built in u4 (2026-11-07): synth T1-T4, drum T2-T4 (the drum T1 page is per-voice sample data: left alone); device test pending
- Hold synth (or drum) + right: randomise the **current page's** parameters (engine knobs on T1, ADSR on T2, FX on
  T3, LFO on T4). The mode key's own tap acts on release (so a hold can be a modifier) except with Shift held:
  Shift + synth is a stock combo and acts on the press (the owner, after u5).
- Hold synth (or drum) + left: step back through the last five randomisations. The history records randomisations
  only, not manual edits.
- u10 (2026-11-08, the owner: "randomise should affect the currently focused fx slot"): on the effect page with FX 2
  in focus the draw goes to FX 2's four knobs (a plugin in FX 2 is refused: no range table); on the LFO page with
  LFO 2 in focus to the shown sound's LFO 2 parameters. Both refuse while the slot is off, as the stock page does.
  Back steps restore the slot's values while it still holds that type.

## 11. Tape markers — built in u4 (2026-11-07), device test pending
- On the tape page, shift + encoder click places that colour's marker at the playhead; if the marker is already at
  the playhead, it's removed; if it's elsewhere, it moves to the playhead. Click jumps to the marker.
- Four markers, saved with the tape, drawn as little flags (a pole and a ">" pennant, the owner after u5) above the tape.

## Tooling
- **Playground** (`tools/playground/coop-playground.html`): mock-ups of every plugin kind (synth, FX, drum,
  sequencer, utility) and every feature above, with the four encoders, shift, Help (fn), T-keys and the 320 x 160
  screen; it copies out the plugin's contract (parameter table + action table).
- **`/port` skill** (`.claude/skills/port/SKILL.md` at the repository root, so every agent in the repo has it): ports a kennel or OPUS plugin into `plugins/<id>/`, runs the
  pack and the op1emu gate, and insists on the optimisation pass the stereo rule demands.

## 12. The tracker — built t1..t7 (2026-11-07), parked 2026-11-08 (`attic/tracker/`, docs/tracker.md)
The owner's brief is `docs/tracker-handover.md`; the first pass and its guesses are in `docs/tracker.md`. Tracker
mode replaces the tape view (Shift + tape, or settings → UI → tape view); six mono channels on the stock voices,
patterns + order list, effect words, overview / zoom / song / setup views, landscape and two portrait orientations.

## 13. Engine controls beyond the four turns — built in e4 (2026-11-10), device test pending

The owner's decisions (2026-11-10, from the rings port, `docs/request-engine-controls-l1.md`):
- On a plugin engine's page (T1) every encoder has eight actions: turn and shift + turn stay the stock's (the four
  knob words; shift is fine adjustment, iter's range table [C op1emu]), and **click, shift + click, fn + turn,
  fn + click, shift + fn + turn, shift + fn + click** go to the engine's declared controls. fn is Help held (a tap still
  opens help). fn + turn and fn + click are new layers beyond the manifesto's six (guidelines §4).
- Clicks act on release and only when shift and fn are as they were on the press; an undeclared layer does nothing.
- **The core shows the change**: the control's name and value top left, in the encoder's colour, for about a second,
  for every engine (with or without its own screen).
- **Randomise** draws a declared steps control within its steps (a continuous one over 0..32767), unless the plugin
  marks it `"random": false`.
- Storage: the slot's free knob words 5, 6, 7 (saved with the sound). Velocity and MIDI CCs for these layers: not yet
  (they would need the v2 parameter store's CC map and a velocity source per control).
- Collisions checked: fn + an encoder had no meaning before (bend and drum pan use shift on T2, LFO 2 and FX 2 use
  lift / drop and their page keys, the settings menu takes its own keys while open).
- e4c (the owner, 2026-11-10): Help with Shift held never reaches the stock (Shift + Help + Help up opened the date and
  time screen); a plain Help tap still opens help.

## 14. Effect controls beyond the four turns — built in e6 (2026-11-11), device test pending

The owner's decisions (2026-11-11, for the eleven kennel effect ports: bitcrusher, drive, ensemble, freq_shift,
tape_drive, tape_echo, wavefolder, cs_hall, cs_plate, cs_clouds, cs_clouds_delay):
- Effects get the engines' layers (§13) on the effect page, at **every** effect position: FX 1 of the synth, the drum
  and the master, and FX 2 of the instrument and of the master. Turn and shift + turn stay the stock's (the four
  knobs; shift is the stock fine adjustment, delta / 16 [C, te-op1-cfw HANDOVER: the synth FX knob case]).
- **fn + turn** on an encoder edits that encoder's second control (kennel's push-page word): continuous, coarse steps,
  1 % of the range a detent (328 in Q15).
- **shift + fn + turn** on the same encoder moves **the same control** in fine steps: 1/16 of the coarse step, as the
  stock shift does (20.5 in Q15 a detent, given as 20 and 21; 1,600 detents across the range). The plugin declares it
  with `"fine": true` on the fn_turn control; that encoder then has nothing else on shift + fn + turn. New with e6: for
  engines shift + fn + turn is a separate slot unless the engine declares `"fine"` too (allowed, not used yet).
- **click** (on release) carries switches and latches: `steps` controls that wrap and are saved with the sound, and
  `latch` controls (freeze, hold): momentary, **not saved**, off again whenever the effect starts at that position.
- The colour follows the encoder (blue, green, white, orange); the core shows the name and value top left in that
  colour for about a second, over FX 1's page and over coop's FX 2 page.
- A plugin effect with no declared controls behaves exactly as before (fn + turn is then a plain turn, as it was).
- Not decided, left as built: randomise does not touch an effect's declared controls (e4's engine rule draws them; for
  an input gain a random draw is a level jump). Say if effects should follow the engine rule.
- Found on the way (op1emu): the stock starts the instrument effect again when you switch between synth and drum, so
  a latch at the synth or drum position is off after a visit to the other instrument (the effect's own state starts
  again there too; this is not new).
