# Screens (P3)

coop's own pages on the OP-1. P3a is the first: the second effect slot's page. Its mechanism is the one later pages
build on; the screen-object route (kennel's) is described for the pages that need SVG art.

## How kennel does it (hundekraft 3.1.4, RE of its source 2026-11-04) [C in its source, I here]

- R246 has a fixed table of 0x44 screen objects at `0x01DF4E54`; the screen shown is `0x018B65C8`. kennel never adds
  an id: it builds extra objects of the stock iter screen class (`ctor 0x01A3BAB0(obj, id, svg)`, vtable copied from
  `0x019103FC`, its update/init entries replaced) and swaps one into an existing slot while its page is up.
  Hooks: the boot screen-construction CALL at `0x01A3BD58`, the load-all CALL at `0x019F07D2`, `gfx_render_frame`'s
  CALL screens_update at `0x019B2690`, and three ScreenId jump-table entries (`for_fx` case 5 at `0x018F7FF8`,
  `for_synth` case 14 at `0x018F8074`, `for_lfo` case 4 at `0x018F80F8`).
- Drawing: the stock SVG renderer (`0x01A503D8` load, `0x01A3B580` find node, `0x019EAD58` transform,
  `0x01A3B4C8` draw) and the raster line `0x01A24A4C` on `0x01A3B490`'s raster; text as a stroke font of raster lines
  (`screens/stroke_font.h`). 320 x 160 units, Q10. Gfx thread, once per frame.
- Overlays: after the stock `screens_update`, kennel calls each feature's `overlay(raster, screen id)`
  (`kn_core.c kn_screens_update`).
- Memory: parsed SVGs live in R246's TE heap (TLSF, `0x840000` bytes at `0x0106B7CC`). 3.1.4 adds 1.5 MB of heap 0
  to it when it runs low ("te_new.cpp:31 out of memory" when switching screens), and resets the drawing caches of
  documents that left the table.
- Keys `0x019C2D3E` (CALL key dispatch `0x019B6C74`), encoders `0x019C2E94` (CALL step `0x019B6C0C(knob, &delta)`,
  1024 per detent). Codes: 0x00 help, 0x02-0x05 mode keys, 0x06-0x09 T1-T4, 0x26 shift, 0x3B-0x3E encoder pushes.
  Shift is the byte `0x01DD7F78`.
- Per-sound data: inside the stock sound JSON, through the stock setters and hooks in `parse_patch_json`
  (`0x01A037A8`); global settings in `op1.db` (`op1rsettings`).
- The stock effect pages: `for_fx(type)` (`0x01A3BEC8`, table `0x018F7FE4`) gives screen 8 delay, 9 grid, 10 punch,
  11 filter, 12 nitro, 13 cwo, 14 phone, 15 spring. The effect position byte `0x01DF4E3C`: 0 synth, 1 drum, 3 master.

## P3a: the slot 2 page (2026-11-04) [I until the device]

An overlay, not a screen object: nothing in the screen table changes, no SVG is loaded, and the TE heap is not
touched. `coop_ui.cc`:
- the frame hook (CALL screens_update at `0x019B2690` -> region-C trampoline +144 -> `coop_ui_frame`) runs the stock
  update, then, while the page is up and an effect page is shown, fills the screen and draws the page with kennel's
  stroke font;
- the encoder hook (CALL `0x019B6C0C` at `0x019C2E94` -> +160 -> `coop_ui_knob`) takes every turn while the page is
  up and passes the rest to the stock step;
- keys through `coop_key_event` (lander_off.cc) -> `coop_ui_key`.

Controls (defaults; easy to change):
- On any effect page (instrument or master), **Shift + Help** opens FX 2 of that position: instrument slot 2 for
  the synth and drum pages, master slot 2 for the master page. Shift + Help again or Help closes it; a mode key or
  T1-T4 closes it and then acts as usual; the keyboard plays through (the first build closed the page on any key,
  so a chord shut it).
- The four encoders turn the slot's four knobs, 1 % a detent, applied at once. A detent is taken as a delta of 1024
  (kennel, tested on hardware); op1emu's `turn ENC cw 2` gives that. [I until the device]
- **Shift + blue** steps through none, the eight stock effects and the loaded plugins; the name turns blue with
  "PUSH BLUE TO SET" until a **blue push** applies it (so turning past grid does not construct every type on the way).
- The slot is saved to `/yaffs2/user/coop_slot2.bin` when the page closes; it is still global (the same for every
  sound): per-sound storage is P3b.

op1emu (2026-11-04, `tests/sim/det_ui_slot2.txt`): phone at the synth position, Shift + Help opens INSTRUMENT FX 2,
green raises knob 2 to 10, Shift + blue then a blue push applies DELAY, a chord plays with the page up; the page
draws cleanly over the phone page (thin overlapping strokes: one wide fill left rows uncovered). No faults.

Next: P3b, slot 2 per sound (a JSON member of the saved sound, kennel's `state_key` route); then the screen-object
route for pages with SVG art (engines, effects), with kennel's TE heap extension.

## u1 / u2: FX 2 by drop / lift, the settings menu, the stock font (2026-11-07) [I until the device]

The owner's decisions (`docs/ui-features.md` §1, §7) replace P3a's Shift + Help:
- On an effect page **drop** focuses FX 2 (the overlay opens) and **lift** focuses FX 1 (it closes). The focus is kept
  per position (`s_focus2[]`, coop_ui.cc), so the frame hook reopens the page when the effect page comes back and FX 2
  was the slot last touched. While FX 2 is focused the page's own key (T3 on the synth and drum, T2 on the mixer)
  toggles FX 2 off and on: `coop_slot2_bypass` (plugin_host.cc) keeps the effect chosen and built but skips its
  render; the page says `FX2 OFF`, and the corner mark on the stock page reads `FX2 OFF` in grey. The slot file
  `coop_slot2.bin` is version 2 (two off bytes; version 1 still loads). Lift and drop on an effect page no longer
  copy / paste the sound (they still do on the other pages).
- **The settings menu** (`core/sdram/coop_settings.cc`): the four mode keys held together, where the Lander game
  was (lander_off.cc now calls `coop_settings_open` as it swallows the fourth key). Blue moves, right enters, left
  backs out and leaves at the top, a mode key leaves and acts; green edits the value (green on screen); a push on a
  Plugins row toggles the file, applied at the next boot. VOICES: spread (live: `g_coop_spread`), drum ADSR / master
  EQ / master compression shown STOCK with the alternative "NOT YET"; UI: tape position BOTTOM, date and time NOT
  YET; PLUGINS: every `.coop` file the boot scan saw, ON / OFF. File `/yaffs2/user/coop_settings.bin` ("CST" v1:
  spread %, four choice bytes, the disabled names), read in `coop_plugins_boot` before the scan, so a file present
  overrides the image's spread word and the disabled files are skipped (`plugins ... disabled N` in the stats).
- The two-column FX / LFO lists (shift + T3 / T4, green column) are the next package: they need the stock list
  screen's cursor and loader mapped first.
- **u2, the stock font**: `core/sdram/coop_text.h` binds the firmware's Text object as kennel's `op1_synth.h` does
  (ctor 0x01A61904 with place, size and colour; set 0x01A61AAC, which reallocates in the TE heap, so GUI thread and
  only on change; draw 0x01A6259C with an identity Q10 matrix; size 12 = the battery page's text, 24 for a name).
  One object per place + size + colour, kept in `.bss`; strings lower case (capitals are icons). The stroke font
  stays for the bars and the background fill. Colour code as the manifesto: blue = the cursor (blue moves it),
  green = the value (green changes it), lavender titles, `625D77` hints and "not yet".

## u3: the green column on the stock FX and LFO lists (2026-11-07) [I until the device]

The stock browser, mapped in the bytes (research 2026-11-07; `OP1_vdk.dis`): one class for every list
(PresetBrowserController, mode 8, vtable `0x191129C`; screen id **0x24**, `presetbrowser.svg`, object `0x01DE4FF8`).
Which list: `[0x01DE511C]` = the list object, FX `0x01BAB748` (built once from `fx_types`), LFO `0x01BAB738`; whose:
`[0x01DE5124]` = the owner controller, its first word the mode (0 synth, 1 drum, 3 mixer). The blue knob object
`0x01DE5128` holds the row (`[0x01DE5138] / 4`: four detents a row); on a change the owner's `vt+0x4c` loads the type
at once, so **the stock column loads on the turn**; the page key (T3; T4 for the LFO list; the mixer's master FX page
is page 2 = T3 as well) is forwarded to the owner and closes the browser; green, white and orange go to a child knob
whose range is 0 on these lists (nothing happens). The screen's right half is its "preset" column, empty on these
lists, green by stock's own design (text `00ED95`, cursor `00475B`, cursor rect x 185..300, rows 23 apart, text x
190.7, six rows).

coop (coop_ui.cc): while screen 0x24 shows the FX or LFO list, the frame hook draws a second column there: "none"
and the list's rows (FX: cwo, delay, grid, nitro, phone, punch, spring, then the plugins; LFO: tremolo, value,
random, the three types slot 2 offers), the cursor set from the slot's current choice when the browser comes up; the
encoder hook takes green (four detents a row, as blue) and passes blue to the stock; on the page key's press (T3 /
T4) the column, if it was turned, is applied: FX 2 of the owner's position (`coop_slot2_set`, FX 2 then in focus,
so the page that follows is the FX 2 page) or LFO 2 (`coop_lfo2_set_type`: one LFO 2 for both instruments, its
parameters set to the middle of the stock Ranges), saved to `coop_slot2.bin` (version 3 carries LFO 2). Any other
closing key discards the green choice. Open question, in the bytes: the forwarded T3 toggles FX 1 off when the
browser was opened from the FX page (key 8 on page 2): `tests/sim/det_ui_fxlist_t3.txt` looks at the phone page
after it.

## u4: randomise (2026-11-07) [I until the device]

`core/sdram/coop_random.cc`: with the synth key held, the right arrow randomises the page shown and the left arrow
steps back through the last five randomisations (docs/ui-features.md §10). T1 draws every engine knob inside its
stock range (`0x01DE5890` rows; shifted knobs only when the type keeps them as separate parameters, mode 3; the
sampler is left alone), T4 every LFO parameter with a non-zero scale (`0x01DE5740`); both written as the stock
encoder handler writes them, the live words then `Patch::SetKnobs` / `Patch::SetLfoParams`, so the sound changes at
once and saves as edited. The arrows are swallowed while synth is held (stock: octave). T2 (envelope), T3 (effect)
and the drum pages follow once their setters and ranges are mapped (research in progress). op1emu
(`tests/sim/det_ui_random.txt`, spread 0): the chord's level 316 -> 224 after the randomise, 316 after the step
back, an LFO wobble after the T4 randomise; no fault.

## u4: Help as fn, solo, markers, the mode keys as modifiers (2026-11-07) [I until the device]

`core/sdram/coop_tape_ui.cc`, from the tape research of 2026-11-07 (OP1_vdk.dis; kennel's markers.c for the
geometry):
- **Help is fn.** Stock enters help on the press, so coop swallows the press and sends the stock a press + release on
  the release only if nothing used Help while it was down: a tap still opens help, a hold is the modifier
  (`coop_fn_held`). The same deferral applies to the **synth and drum keys on their own mode** (lander_off.cc): stock
  resets the page to T1 on that press, so a hold (randomise) keeps the page and a tap still goes to T1.
- **Solo**: Shift + Help + T1..T4 on the tape (mode 2) mutes the other three through the stock's own flags
  (`u8[4]` at 0xFF8010A0, applied by Core::update_mutes 0x01967ED0, the same path as the stock Shift + Tn mute; the
  mixer page follows), remembers the flags before the first solo and restores them when the soloed key is pressed
  again. "solo n" top right. Mutes are volatile across boots in stock, so is the solo.
- **Markers**: on the tape page the encoder pushes are free in stock. Shift + a push places that colour's marker at
  the playhead (`[0xFF802974]` in 1/64 samples), removes it when the playhead is on it (3 px at the current zoom),
  moves it otherwise; a plain push locates there (the stock tape_locate 0x01A55024, which suspends the loop as the
  stock's own jumps do). Drawn kennel's way: the tape scrolls under the fixed centre line (x 160.13), the zoom is the
  stock float px/s at 0x018C183C read as bits, a grey line through the tracks and a caret in the encoder colour
  above. Saved at once to `/yaffs2/user/coop_markers.bin` ("CMK" 1, four int32 samples, -1 none), read after the
  tape mounts, cleared by an erase.
- The randomiser (`coop_random.cc`) now covers synth T1-T4 and drum T2-T4 with the live range tables the boot check
  uses (knobs 0x01DE5890, adsr 0x01DE5710, dyna_env 0x01DE56C8, fx 0x01DE5C50, lfo 0x01DE5740) and the stock setters
  (SetKnobs 0x01A00D14, SetAdsr 0x01A00C98, SetFxParams 0x019FF664, SetLfoParams 0x019FF734, dyna 0x01A0005C); the
  drum T1 page is the last voice's sample record and is left alone; effect and LFO pages are skipped while off.
- Undo groundwork (for the tape undo package): the clip pool at 0x01AA8D70 (512 x 12-byte records), edits through
  tape_clips_add / split / join / remove, the record end adding one clip in tape_controller_set_mode; coop already
  owns lift / drop / erase and every cache write-back (`flush_part`), where the old bytes can be kept. Kennel keeps 7
  steps by spilling 64 KiB chunks to flash; a 10 s stereo take is ~1.8 MB, so RAM holds about one level.

## u5: the sound's "coop" member, pitch bend amount and time (2026-11-07) [I until the device]

- **Per-sound coop data** (`core/sdram/coop_sound.cc`): the stock's parse_patch_json (0x01A037A8) rebuilds a sound's
  JSON from the keys it knows and assigns the fresh Value over the slot's (CALL operator= 0x019BDDC0 at 0x01A044A8),
  dropping anything else. coop retargets that CALL (region-C trampoline +176) at `coop_keep`, which copies the
  source's `"coop"` object into the fresh Value first (kennel's dp_keep_op1r route, hook_dpkeep) and then reads it
  into a per-slot cache (synth slots: the Value is the Patch object 0x01DDD7E8 + slot * 0x38, + 4; drums
  0x01DDD9A8). Saves serialise the whole Value, so the member travels with slots, user presets and lift / drop; a
  sound without it gets defaults. Writes go through operator[] / Value(int) / operator= and a stock setter call
  (SetAdsr with the live words) that marks the patch dirty for the background writer. Fields: `bend` (-12..12,
  default 12), `bend_t` (0..128, default 0), `pan[24]` (-50..50, default 0; the drum pan, control not placed yet).
- **Bend** (`core/sdram/coop_bend.cc`): stock's Shift + left / right writes a global target W[0xFF8016DC] = -32 / +64
  (a linear frequency delta in 1/64: an octave each way [I], slewed by the Core with a fixed short glide). coop takes
  those keys first and writes its own target, 64 * (2^(st/12) - 1) for the sound's amount (so +12 = the stock +64),
  and ramps toward it every audio block over the bend time (OFF: at once, the stock's own slew stays; 1..127 / MAX:
  5 ms .. 2 s on the stock portamento's scale). The shifted T2 page (screen 40, playmode.svg) hides two rows the
  stock drew ("G-BEND" white, "DETUNE" red) whose encoders (shifted white = adsr[6], orange = adsr[7]) write words
  nothing reads: coop takes those two encoders for bend amount and time and draws the rows in the stock font.
  The stock state bytes the stock's handlers write around the target (0x018C2188/9 held, 0xFF80104E MIDI echo,
  0xFF801052/3) are written the same way. MIDI bend in is untouched.
- **Drum per-voice pan**: the stock drum T1 shift layer is fully taken (shifted blue = reverse, green / white =
  start / end fine, orange = volume), so the owner's "blue on the drum shift layer" needs another home; the storage
  (`pan[24]` keyed by the drum key 0..23, applied at the voice's start instead of the random spread draw) is ready.

- **Overwrite save** (`core/sdram/coop_save.cc`): the stock clock pass (0x019AD090 from the main loop) names the
  new preset with a date stamp (snprintf 0x01A9CE04 at 0x019AD220 synth / 0x019AD5A6 drum), makes the snapshot
  marker, INSERTs the row (0x01995938 at 0x019AD3DC / 0x01995B80 at 0x019AD762) and reloads. With the mode key held
  through the clock, coop's stamp hook writes the sound's own name (its origin found with fw_synth_slot_preset
  0x01A02030 / 0x01A01DC8: a user preset of the same type and name) and the insert hook runs an UPDATE of that user
  row (folder and "name" match in the styled JSON) instead; a factory preset or no match saves as stock.
  "overwriting <name>" over the save clock. Trampolines +192..+240. Only presets in the snapshot folder (the ones
  the device made) qualify, and the snapshot marker is unlinked first (0x01A9A3FC), because the stock appends `_n`
  to the NAME when the file exists. The stock's db exec (0x0196F950) formats the SQL through printf again: no `%`
  in a statement (GLOB, not LIKE); the rows are compact JSON. Verified in op1emu with a store watch on
  `g_over_trace` and op1.db read back from the kept NAND.
- **Rename** (`core/sdram/coop_rename.cc`): in the synth preset browser (screen 0x24 on the list 0x01DDD688) the
  orange push on a user preset (array 0x01DDDB68, factory flag +0x30, index from the selection accessor
  0x01A17560) starts naming on the encoders; orange push saves: `UPDATE synth_user SET patch = replace(patch,
  '"name" : "old"', '"name" : "new"')` for the row, then the preset array is reloaded (0x01A0567C) and the list
  refilled (0x01A02790). Sampler presets are refused (their .raw is named after them). Synth only in u5.

## u6: drum per-voice pan (2026-11-07) [I until the device]

`core/sdram/coop_drumpan.cc`: on the drum's T2 page with Shift held, green pans the last played key ([0xFF801044],
0..23) between L50 and R50, kept in the kit's "coop" member (`pan[24]`, coop_sound.cc; the kit is marked dirty
through the stock's dyna_env setter 0x01A0005C). A readout "pan <key> <l12>" is drawn in green. When a drum voice
starts, the stereo stage (stereo_bus.cc) takes that key's pan instead of a random spread draw: spread stays the
synth's. The owner chose the T2 page because the stock takes all four shifted encoders on the drum's T1 page
(reverse, start fine, end fine, volume); shifted blue on T2 is the dynamic envelope's fifth word, green / white /
orange write nothing there.

## u7: the owner's corrections after the first device run of u5 (2026-11-07) [I until the device]

- **The crash** (SEQSTAT 0x2a, RETX 0x2c011ba6, leaving the settings menu after a plugin toggle): coop wrote files
  from the KeyHandler thread (the settings file, the slot file, the markers; the rename's db exec too). All coop file
  and database work now runs on the main loop: the KeyHandler thread sets a pending flag, `coop_ui_service`
  (called from `coop_fx_service` every pass) does the write (coop_settings_service, coop_markers_service,
  coop_rename_service, the slot file through coop_slot2_request_save). The overwrite hooks already ran in the main
  loop (the stock's save path).
- **Settings**: a blue push enters a submenu as the right arrow does; no hints on screen.
- **The mode-key deferral** (randomise) is skipped while Shift is held: Shift + synth is a stock combo on the press.
- **Solo** is a set: Shift + Help + several T keys solo several tracks; works on the tape and the mixer; nothing drawn.
- **Markers** are flags: a pole from y 110 to 122 and a ">" pennant in the encoder colour.
- **FX 1 in focus** shows the stock page alone (no corner mark). FX 2 is to look exactly like the stock effect page
  of its type: the stock effect screen's data sources are being mapped for that.
- **Bend**: the device showed the stock word W[0xFF8016DC] is a RATE toward a fixed octave (a small value bent an
  octave slowly, the time did nothing). u7 ramps the live bend word W[0xFF80104C] (the one the MIDI pitch-bend
  handler 0x01A84EFA writes directly) every block toward -16384 * amount / 12 (down) or 32764 * amount / 12 (up), over
  the bend time, and back to 0 on the release, with the MIDI handler's state bytes; the keyboard word stays 0.
  Amount is 0..12, the same both ways. [I: full MIDI scale = an octave, from the 1:2 asymmetry shared with the
  keyboard word; the device test listens for it.]
- **Overwrite** applies to user presets in any folder; the snapshot marker is unlinked only when it is the preset's
  own (snapshot folder) or an empty marker of the same name (size 0: yaffs lseek 0x01A9A0E4), so another preset's
  audio of that name is never touched.
- **Rename** counts the pushes seen in the browser (`rename_pushes`); the device's `rename_refused 4` with
  `rename_starts 0` means the pushes arrived and were refused: factory or sampler presets (a sampler's audio file is
  named after it; its rename needs the stock's file rename, not mapped yet).
- **Drum pan**: with Shift held on the drum's T2 page coop draws its own PAN page (black; "pan" and the value in
  blue, the key in grey), and the **blue** encoder pans the last played key; the stock's shifted blue there (the
  dynamic envelope's fifth word) is taken over.
- **FX 2 on the stock page** (`core/sdram/coop_fx2view.cc`, from the stock screens mapped 2026-11-07): every effect
  screen's Update reads its dials through read_fx_param (0x01A28DA0(slot, pos) -> a static std::vector<float>, the
  position's knob words normalised with the range table) and its animation through the display getter
  (0x01A28B34(pos, n, a) -> the display slots 0xFF801774 as floats), one CALL of each per Update at sixteen sites
  (cwo 0x019427B8 / C6, delay 0x01984F60 / 6C, nitro 0x019A74D4 / E2, grid 0x019B05F8 / 0604, punch 0x019E6A48 / 56,
  filter 0x019E832C / 38, phone 0x01A1CFD2 / DE, spring 0x01A33FB2 / 0x01A33D1C). coop retargets them (trampolines
  +256 / +272) at wrappers that call the stock, then, while FX 2 of that position is in focus with a stock type,
  overwrite the vector's floats with slot 2's knobs (q15 / 32767 through the stock's own soft-float 0x01AA7050 and
  0x01AA54C8) and the slot-2 twin's display words. The screen id is posted like a key event (0x019D7C94(id, slot,
  pos)) on focus changes, and the pop CALL in screens_update (0x01A3BB40 -> 0x019D7D70, trampoline +288) is wrapped
  to rewrite an effect-page id to FX 2's type while in focus, since every handled key reposts FX 1's. A plugin or
  nothing in FX 2 keeps coop's own page. The knob-touched bytes (0x01DD7F74 + k) are set when coop takes a turn so the
  stock highlights the dial.
