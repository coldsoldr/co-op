# tape_drive: tape saturation, wobble and age (coop effect)

A port of te-op1-cfw's kennel effect **tape_drive 0.3** (coldsoldr; `ports/knl/tape_drive/tape_knl.cc`, DSP
`ports/fx/src/tape`, design and measurements in te-op1-cfw `docs/tape-port.md`). On kennel it was tested on the device
and called "good" (te-op1-cfw `docs/device-tests.md` Test 57, then renamed in Test 61) [C kennel]. In coop it is
built and host-tested; the op1emu gate and the device run are still to do.

| File | What |
|---|---|
| `tape_drive.cc` | coop's glue: the `coop_fx_t`, the knob mapping, the int32 to int16 bus and back |
| `src/tape.cc`, `src/tape.h`, `src/tape_layout.h`, `src/tape_tables.h` | te-op1-cfw's DSP. Only the include paths and one initialiser changed (below) |
| `src/tape_bfin.S` | te-op1-cfw's three kernels (stereo delay, mono delay, lane). Only the include path changed. `tools/asm_lint.py`: 0 flagged |
| `src/gen_tape_tables.py` | the offline table generator (plain Python, not built) |
| `tape_drive_screen.cc` | the effect screen (e9): kennel's ScreenInit / ScreenUpdate on the coop screen calls |
| `gen_tape_drive_svg.py`, `tape_drive.svg` | the screen's art (generated: `python3 gen_tape_drive_svg.py tape_drive.svg`) |
| `op1/support.cc` | `memcpy` / `memset` / `memmove` for GCC's struct copies (the same file as `plugins/rings/op1/support.cc`) |
| `LICENSE-MIT` | Beads' WowAndFlutter terms (the wobble's design) |

Licence: the DSP and the glue are coldsoldr's (GPL-3.0-or-later with the OP-1 firmware permission). The wobble is
Mutable Instruments Beads' `WowAndFlutter` (MIT), rewritten in fixed point (`LICENSE-MIT`). The halfband uses hiir's
coefficient design (Laurent de Soras, WTFPL).

## Contract

### Action table

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | saturate | wobble | age (clockwise = newer) | mix |
| Click | — | — | — | — |
| Shift + turn | saturate fine (stock) | wobble fine (stock) | age fine (stock) | mix fine (stock) |
| Shift + click | — | — | — | — |
| fn + turn | gain, -6..+6 dB, 0.5 dB a detent (continuous, saved) | — | — | — |
| fn + click | — | — | — | — |
| Shift + fn + turn | **gain fine** (the same control, 0.1 dB a detent: `"fine": true`) | — | — | — |
| Shift + fn + click | — | — | — | — |

Blue's shift + fn layer is the gain's fine adjustment. This is the owner's decision (`docs/ui-features.md` §14) and
the e6 `"fine"` pairing (`guide/plugin-guidelines.md` §4.1), so nothing else can sit there. The steps are kennel's
(the owner, 2026-11-11): `"step": 1365` (0.5 dB a detent: the word is (w - 16384) x 4 Q16 octaves, +-6 dB over the
range) and `"fine_step": 273` (0.1 dB).

### Controls (`plugin.json`)

```json
"controls": [
  {"id": "gain", "name": "gain", "action": "fn_turn", "encoder": "blue", "word": 5, "kind": "continuous",
   "default": 16384, "fine": true, "step": 1365, "fine_step": 273, "random": false}
]
```

- **The turns** are `params[0..3]`. They arrive in the filter type's range units
  ({64..14528}, {0..24576}, {0..22528}, {8192..32767}, `sdk/coop_plugin.h`).
  - The render maps them to 0..32767 with kennel's `op1_fx_param_q15` formula (`(v - lo) * 32767 / (hi - lo)`,
    clamped), so the DSP gets the same numbers it got on kennel.
  - Defaults are Q15 `[13107, 8192, 22937, 32767]`, as upstream: saturate 0.4, wobble 0.25, age 0.3 worn
    (word 0.7), mix full.
  - Age is mirrored as upstream: turning clockwise gives a newer, brighter tape (the owner, 2026-10-27).
- **The gain** is word 5: `gain_oct = (clamp(params[5]) - 16384) * 4`. That is Q16 octaves added to the saturation
  drive's exponent only (about -6..+6 dB, 16384 = 0 dB).
  - The level make-up and the dry side of the mix never see it, so at mix 0 the gain changes nothing.
  - It is not randomised (`"random": false`, the owner: a random draw would be a level jump).
- **Words 6, 7 and the latch word** are unused: upstream has nothing else.

### Upstream mapping, and what moved

| kennel (tape_knl.cc) | coop |
|---|---|
| knobs blue saturate, green wobble, white age (mirrored), orange mix | the same four turns |
| blue press `OP1_PRESS_PAGE`, page 2 blue = word 5 gain | `fn_turn` blue, word 5 (same word, same default 16384, same formula) |
| shift + blue on page 2: a tenth of a dB a click (`knl::Fine`, 273 words, written back from the render through the shift byte at `0x01dd7f78`) | shift + fn + blue, the core's fine: 273 Q15 a detent (0.1 dB, kennel's click); coarse 1365 a detent (0.5 dB) |
| green / white / orange presses | none (`OP1_PRESS_NONE`) |
| `"stereo": true` (`op1_fx_stereo_t`: mono render at synth and drum, stereo at master) | no flag. A `coop_fx_t` is stereo by ABI, and coop's loader would read a kennel stereo flag as the `op1_fx_stereo_t` layout and refuse the file. The stereo render runs at every position |
| `page2_defaults [16384, 0, 0]` | taken from the controls by the packer (never written next to `controls`) |
| `knl::Instance` (the position's scratch buffer) | one `TapeSat` in `.bss`: each of the five relocations has its own |
| `op1_fx_param_q15` (the stock range table at `0x01DE5C50`) | the SDK's constant ranges, once a block |
| the screen (`SCREEN_tape`, `tape_drive.svg`) | `SCREEN_tape_drive` on e9's effect-screen path (see "Screen") |

The page became a held layer (guidelines §4). The mix was already on orange (§4.3), so nothing moved.

## Stereo and cost

- **Stereo path.** The render calls upstream's `TapeSat::Process(l, r)` at every position:
  - one shared wobble delay (a ring of L | R pairs);
  - per channel, a lane: 2x halfband up, quintic soft clip, halfband down, head bump, HF loss, mix.
  - The channels are never summed. On kennel the synth and drum positions ran this mono, so in coop they now hear
    the effect in stereo.
- **The bus.**
  - coop hands int32 (the int16 bus widened by 16 bits) and the DSP is int16. Each block's high halves go through the
    DSP, and the output keeps the input's low halves: `out = wet16 << 16 | (in & 0xffff)`.
  - So the high half is bit-identical to upstream, and at mix 0 the whole int32 sample is the input (host test).
- **Upstream cost** (te-op1-cfw `docs/tape-port.md`, bfin-elf-run, per 64-sample call):
  - stereo 7,026 cycles, mono 4,128;
  - the stereo delay kernel 744, each lane 2,836, control about 200-400.
- **coop, simulator** (`tests/host/tape_drive`, bfin-elf-run, no cache or SDRAM model): **avg 9,061, peak 9,286 a
  block**, the glue included.
  - The glue is the int32 / int16 conversion (2 x 64 samples each way) and four divisions for the knob mapping
    (`__divsi3`).
  - So stereo at the synth and drum positions costs about 2.2x what the mono kennel render did there, and about 1.3x
    at the master.
  - Five instances (every position) would be about 46 k together.
- **Against the budget** (under 30 k a block, over 60 k does not ship):
  - Expected on the device: about 10-13 k (op1emu-style figures plus 10-35 %) [I].
  - Worst case with every SDRAM line it touches cold: about 60 lines x ~107 cycles, so up to about 16 k [I].
  - No optimisation pass is needed to ship. If the device figure comes in high, in this order:
    1. Fold the int16 conversion into the kernels: the delay kernel reads the high halves in place with a 4-byte
       stride, and the mix writes back the same way. That saves about 1 k.
    2. Replace the four divisions with exact reciprocal multiplies (keeping kennel's rounding). That saves about
       0.4 k.
    3. Run the mono half once when L == R. Not taken now: when the lanes' states differ, the output would differ from
       upstream for the length of a tail.
- **Memory.**
- **e9 (2026-11-11, 0.5, the screen):** file 22,384 bytes (SVG 7,815), `.text` 11,104, `.rodata` 1,584, `.bss` 176
  (the five buffer pointers and the screen's node tables); load 12,864 bytes once (+2,232 over e8's 10,632 with the
  new `coop_fx_t` field). The state is unchanged (the render publishes nothing: `"scratch": 3392` as before).
- **e8 (2026-11-11):** the TapeSat and the block copies are in the slot's position buffer (plugin.json `"scratch": 3392`, `coop_fx_scratch`), as kennel's `knl::Instance` had them; the file is relocated once and its `.bss` is the five buffer pointers (20 bytes); init resets the state there at every selection. The figures below are from before e8 (state in `.bss`, five relocations). File 11,084 bytes, load 10,628 once.
  - `.text` 9,416, `.rodata` 1,124, `.data` 0, `.bss` 3,372.
  - **Load size 13,912 bytes, x 5 relocations = 69,560 bytes** of heap 0.
  - The packed file is 11,064 bytes.
- **L1:** none. L1 for effects is still [proposed]; everything sits in `.bss` (SDRAM).
- **init:** `TapeSat::Init()`, which clears the 2 KB ring and sets about 40 fields: about 1.5 k cycles, cheap
  enough for the audio thread [I].
- **Device cost:** pending (the `cost` line `fx1_plugin=` / `s2_instr=` / `s2_master=`).

## Changes to the upstream DSP

- `src/tape.h`: `TapeParams::gain_oct` lost its `= 0` default member initialiser. It gave `TapeParams`, and so
  `TapeSat`, a non-trivial constructor, so a static instance could have needed `.ctors`, which coop refuses
  (guidelines §2.1). Every caller sets it.
- The `#include` paths: `src/tape/...` became `src/...`.
- Nothing else. The C reference kernels and the mono path stay: the host test uses them, and they are cold code on
  the device.

Forbidden constructs found in the upstream plugin (`tape_knl.cc`, `common/knl_fx.h`), and how each was replaced:

| Construct | Replaced by |
|---|---|
| `knl::ShiftHeld()` reading `0x01dd7f78`, and `knl::Fine` writing word 5 back through `op1_fx_knobs(pos)` | the core's `"fine"` |
| `op1_fx_param_q15` (`0x01DE5C50`) | constant ranges |
| `op1_fx_scratch` | `.bss` |
| `op1_fx_page`, `op1_fx_screen_pos`, `op1_fx_knobs_live`, `op1_screen_draw` | the e9 view (`view->mods`, `view->slot`, `view->knobs`) and `coop_scr_draw` |

The DSP itself had no float, no libc call, no address and no per-sample division. The only `float`, `sin` and `exp`
are in comments and in the offline generator script.

## Screen (e9, 0.5)

Kennel's screen (`te-op1-cfw/ports/knl/tape_drive`: `tape_drive.svg` from `gen_tape_drive_knl_svg.py`, the canvas
"Tape screen" design A "Reels, aligned", 2026-10-27, `tape_knl.cc` ScreenUpdate) on the e9 effect-screen path
(`docs/plugins.md` "e9"): `tape_drive.svg` (from `gen_tape_drive_svg.py`) and `tape_drive_screen.cc`. It shows
wherever tape_drive is the effect on the page: FX 1 of the synth, the drum or the master, FX 2 in focus.

| Part | Encoder | What moves it |
|---|---|---|
| the wave on the tape between the reels | blue (saturate) | eight variants, soft to square by saturate; dims to the body tone while fn is held |
| the reels' hubs | green (wobble) | jittered at the wobble's rate, 0.5 Hz (wow) to 8 Hz (flutter), up to 2 units; the right hub a quarter turn behind |
| the marker on the wow -> flutter scale | green (wobble) | 0..104 units along the scale |
| the low-pass glyph | white (age) | its corner walks left as the tape wears (clockwise = newer: the corner goes right), dimming toward a worn white |
| the hiss specks above it | white (age) | a count set, 0..6 as the tape wears |
| the gain gauge (left) | blue with fn (gain) | the bar about the centre, -6..+6 dB; a bracket while fn is held |
| the mix gauge (right) | orange (mix) | scaled about its foot |
| the reels, tape path, head, scale, gauge rails | (none) | still, in the body tone |

What changed from kennel's screen:
- **Inputs**: the four turns from `view->knobs` (kennel's `knl::ScreenPage1`, the stock pages' values with the LFO);
  the gain from `view->words[0]` (word 5). Age is mirrored as in the render. Kennel's screen read nothing from the
  DSP, so the render publishes no display data: the state, the audio and `"scratch"` are unchanged.
- **Kennel's blue page 2 is fn held** (`view->mods` bit 1): fn + blue turns the gain, so while fn is down the gain's
  bracket shows and the saturation wave (blue's plain turn) dims to the body tone, as bitcrusher's staircase does.
- **The dB readout is gone** (kennel's page-2 sign, digit, dot and tenth glyph sets, 4.3 KB): the core's popup names
  the control and its value when fn + blue turns it (in percent: a `format` field in the controls schema would let it
  say dB; not built).
- **Leaner art**: the wave variants have 27 points (2 units apart over the 52-unit gap, one decimal; kennel's had 53,
  two decimals); lines go without `stroke-linejoin` (a line has no joins); the palette keeps only the four colours the
  code reads. Kennel's two polygons (the hubs' triangles, the head) are closed polylines, the head without its black
  fill (it sat on black). Nothing is drawn with raster lines: no large repeated set was left once the readout went.
- **Colours** are coop's brights (blue 698EFF, green 00ED95, white FFFFFF, orange FF3A5D). Kennel drew the reels, the
  tape path and the head in a lighter grey (514959) and the rails in the body tone; here all still structure is the
  one body tone 38354D (the manifesto's "one dim body tone"). The age glyph wears from white toward 8E86B0
  (kennel: toward 514959).
- SVG 7,815 bytes (kennel's 15,111), checked by `tools/plugin/pack.py` (R246's parser rules) and by the generator.
- The animation state (the frame clock, the hubs' phase) is shared by the positions, as kennel's was.

The render is unchanged but for the `coop_fx_t` (`COOP_FX_F_SCREEN`, `&SCREEN_tape_drive`); the audio is
bit-identical (host test, host and simulator).

## Gate plan (op1emu, step 9: not run here; one run at a time)

1. Build: `make -C plugins` gives `build/plugins/tape_drive.coop`. Build the NAND with `tools/make_nand.py`, with
   the `.coop` alone in `/yaffs2/plugins`.
2. `tests/sim/det_fx_select.txt`-style runs:
   - Select `tape drive` in the FX list (alphabetical) at FX 1 of the synth, the drum and the master, and at FX 2 of
     the instrument and of the master.
   - Play a chord through each.
   - Expect: no fault; the stats `plugins` line shows `seen 1 loaded 1 failed 0` and tape_drive rendering at each
     position; the `cost` line shows `fx1_plugin=` / `s2_instr=` / `s2_master=` near 9 k (emulator).
3. `tests/sim/det_fx_controls.txt` (the e6 walk), unchanged, with tape_drive alone on the NAND. It picks the
   stock list's last row and, for FX 2, the same list turns as for test_fx: with one plugin on the NAND it is the
   same row [I]. Expect:
   - `fxctl fx 1`;
   - word 5 exactly as for test_fx: `p0 ...:11197/0/0/0`, `p3 ...:19114/0/0/0`, `p2 ...:15019/0/0/0`,
     `p4 ...:16384/0/0/0`, `p1 ...:17749/0/0/0`. tape_drive declares nothing on words 6 and 7 or the latch word, so
     they read 0, and test_fx's green and orange clicks and fn + white land in `ignored` instead;
   - `fine 1`, `master_saves 1`, `plugins seen 1 loaded 1 failed 0`, no fault.
4. `tests/sim/det_fx_reload.txt`: power-cycle the NAND and expect word 5 kept at every position.
5. A pristine-NAND run (`det_chord.txt`) must be unchanged against the same core.
6. Read the emulator cost figures as emulator figures: the device is 10-35 % higher.

No new scenario file is needed: the e6 scenarios pick the lone plugin by its list position, not by name. If FX 2's
list puts `tape drive` on a different row from test_fx [I], adjust the list turns in a copy named
`tests/sim/det_fx_tape_drive.txt`.
