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
