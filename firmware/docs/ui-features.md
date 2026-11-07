# UI features: the owner's decisions (2026-11-07)

How each coop feature is reached and how it behaves, as the owner decided in conversation on 2026-11-07. Nothing here
is built: every feature is mocked in the playground (`tools/playground/coop-playground.html`) before it's coded, and
nothing is done without a device result in `docs/device-tests.md`. [I] marks a detail still to verify; everything
else is the owner's word.

Process rule (the owner): **mock up everything before building it.**

## 1. Settings menu — built in u1 (2026-11-07), device test pending
- Open: hold synth + drum + tape + mixer. This replaces the lander game, which coop removed.
- Navigate: blue turn moves between items; right arrow enters a submenu; left arrow goes back one level. At the top
  level, left arrow or any mode key leaves.
- Edit: green turn changes the selected item's value; the value is drawn in green.
- Submenus:
  - **Voices**: spread amount 0-100 (synth only; drums have per-voice pan instead); drum ADSR (stock / transient
    designer, TBD); master EQ (stock / advanced, TBD); master compression (stock / advanced, TBD). The TBD
    alternatives are shown greyed until specified.
  - **UI**: tape position (bottom, stock / top, as the op1repacker mod); date and time (for file timestamps on save;
    24 h; `yyyy/mm/dd`).
  - **Plugins**: one row per file with kind, version and measured cost; encoder click toggles enabled / disabled,
    applied at the next boot.

## 2. Pitch bend (per synth preset) — built in u5 (2026-11-07), device test pending (the semitone scale of the stock bend word is inferred: calibrate on the device)
- Bend stays stock: shift + left / right.
- Amount and rate are on the **shifted T2 screen** (the stock page with play mode and portamento). Engines keep all
  four shifted encoders on T1 for their own second rows.
- Amount: -12 to +12 semitones in steps of 1 (0 = no bend). Time: `OFF`, 1-127, `MAX`, the stock portamento scale,
  mapped to 0 ms – 2 s (exponential): how long the pitch takes to reach the new bend. (Decided 2026-11-07, after the
  first mock-up.)
- Encoders: white = amount, orange = rate [I: to verify that stock leaves both free on that page; blue = play mode
  and green = portamento in stock].

## 3. Drum per-voice pan (per drum preset) — storage built in u5; the control is not placed: the stock takes all four shifted encoders on the drum T1 page (reverse, start fine, end fine, volume), owner to choose another home
- Shifted T1 on the drum page: blue = pan of the **last played voice**, shown `L50 … C … R50`.
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
- Shift + T3 on synth, drum or master opens the FX list with **two columns**: blue column = FX 1 (as stock), green
  column = FX 2. The green column defaults to **none**, so an untouched sound stays stock.
- On the effect screen, **lift** focuses FX 1 and **drop** focuses FX 2. The focused slot's page is shown; T3 shows
  the slot last touched. The choice persists with the sound.
- The stock `FX OFF` becomes `FX1 OFF` / `FX2 OFF`.
- This replaces the P3a Shift + Help page (`docs/screens.md`).

## 8. Two LFO slots — the green column on the list built in u3 (2026-11-07); one LFO 2 for both instruments until P3b
- Shift + T4: blue column = LFO 1, green column = LFO 2; lift / drop focus them on the LFO screen, as for effects.
  Same destinations, additive (`docs/lfo2.md`).

## 9. Mute and solo — solo built in u4 (2026-11-07), device test pending
- Shift + T1-T4: mute the track, as stock.
- Shift + Help + T1-T4: **solo** the track, exclusive (soloing another un-solos the first). Leaving the tape page
  keeps the state.
- Help is the fn key for every shift + fn combination (the manifesto's deep layer).

## 10. Randomise — built in u4 (2026-11-07): synth T1-T4, drum T2-T4 (the drum T1 page is per-voice sample data: left alone); device test pending
- Hold synth (or drum) + right: randomise the **current page's** parameters (engine knobs on T1, ADSR on T2, FX on
  T3, LFO on T4).
- Hold synth (or drum) + left: step back through the last five randomisations. The history records randomisations
  only, not manual edits.

## 11. Tape markers — built in u4 (2026-11-07), device test pending
- On the tape page, shift + encoder click places that colour's marker at the playhead; if the marker is already at
  the playhead, it's removed; if it's elsewhere, it moves to the playhead. Click jumps to the marker.
- Four markers, saved with the tape, drawn as coloured ticks on the tape timeline.

## Tooling
- **Playground** (`tools/playground/coop-playground.html`): mock-ups of every plugin kind (synth, FX, drum,
  sequencer, utility) and every feature above, with the four encoders, shift, Help (fn), T-keys and the 320 x 160
  screen; it copies out the plugin's contract (parameter table + action table).
- **`/port` skill** (`.claude/skills/port/SKILL.md`): ports a kennel or OPUS plugin into `plugins/<id>/`, runs the
  pack and the op1emu gate, and insists on the optimisation pass the stereo rule demands.
