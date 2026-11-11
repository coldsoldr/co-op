# cs_hall: coldsoldr's hall reverb as a coop effect

An 8-line feedback delay network hall reverb (Jot's FDN) in fixed point, from te-op1-cfw: the DSP
`ports/fx/src/hall` (design and measurements in te-op1-cfw `docs/hall-port.md`), the kennel build
`ports/knl/cs_hall` 0.1 (never built as a `.knl` or published before this port; it compiles and packs: 36,460 bytes,
`knl_pack.py` self-check ok, 2026-11-11). GPL-3.0-or-later with the OP-1 firmware permission; the half-rate path
uses Clouds' halfband filters and Rings' soft-limit table (Emilie Gillet, MIT). The list shows it as "cs hall".

Signal path: the stereo input decimated 2:1 (22.05 kHz), summed to mono into a pre-delay (0-200 ms), an input gain,
an early-reflections line (5 + 5 taps), three diffusing allpasses, eight modulated, damped delay lines with a
Householder feedback, even lines left and odd right, the early taps, a width matrix; interpolated back to 44.1 kHz
and mixed equal-power with the dry signal through a soft limit. Freeze gates the input and makes the loop lossless.

**A §2.2 exception, accepted (the owner, 2026-11-11):** the tank's input is the sum L + R. That is the reverb's
design, not a mono collapse: the wet is stereo from the tank's taps (even lines left, odd right, then the width
matrix), and the dry stays the full stereo input, so a voice's placement survives in the dry and the reverb is wide.

## Files

| File | From | Changes |
|---|---|---|
| `cs_hall.cc` | `ports/knl/cs_hall/hall_knl.cc`, `ports/fx/op1/half_rate.h` | the coop glue (below) |
| `hall.cc`, `hall.h`, `hall_layout.h` | `ports/fx/src/hall/` | compile-time `double` maths replaced by the integer literals it evaluated to (printed by the upstream build); C run kernels host only; `InitKeepBuffers` / `ClearBase` / `ClearWords` |
| `hall_bfin.S` | `ports/fx/src/hall/hall_bfin.S` | include path; kennel's mono kernels not built (`HALL_MONO_KERNELS`) |
| `halfband.h`, `halfband.cc` | `ports/clouds/src/core/halfband.{h,cc}`, `dsp_fixed.h`, `rings/src/fixed.h` | only what the hall uses; the soft-limit and crossfade tables as constant data (upstream built the soft-limit table at run time) |
| `halfband_bfin.S` | `ports/clouds/src/core/halfband_bfin.S` | none |

Forbidden constructs found and fixed (guidelines §2.1, §2.3, §7.1): `constexpr double` table generation in `hall.cc`
(Exp, Cos, SizeScale: 6 tables, 16 size rows) → integer literals; kennel's `knl::Fine` (reads the shift byte at
0x01DD7F78) → the core's `"fine"`; `op1_fx_param_q15` (the range table at 0x01DE5C50) → the SDK's constant ranges;
`op1_fx_page` (the freeze latch) → `params[8]` bit 0; `op1_fx_scratch` (kennel's per-position buffer) → `.bss`;
`op1_fx_knobs_live` / `op1_fx_screen_pos` / the screen node calls → `view->knobs`, `view->slot`, the e9 screen calls ("Screen", below); the 84 KB tank clear in
`Init` → spread over blocks in render (below). No libc, no float, no static constructors; imports `coop_fx_scratch` and the e9 screen calls only.
`tools/asm_lint.py plugins/cs_hall/*.S`: 0 packets flagged.

## The contract

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | size | decay | damp | mix |
| Click | — | — | — | freeze (latch) |
| Shift + turn | size fine | decay fine | damp fine | mix fine |
| Shift + click | — | — | — | — |
| fn + turn | predelay | early | width | — |
| fn + click | — | — | — | — |
| Shift + fn + turn | predelay fine | early fine | width fine | — |
| Shift + fn + click | — | — | — | — |

Shift + turn is the stock fine adjustment of the turn (no `shift_turn` is declared). The fn + turn controls are
continuous, saved with the sound in words 5-7, coarse 328 Q15 a detent (1 %), fine 1/16 of that with `"fine": true`.

| id | name | action | word / bit | kind | default | what |
|---|---|---|---|---|---|---|
| (turn) | size | turn blue | `params[0]` | range 64..14528 → Q15 | 19661 | room → hall, 16 steps (line scale 0.2-1.0); a change crossfades over 256 samples |
| (turn) | decay | turn green | `params[1]` | 0..24576 → Q15 | 16384 | RT60 0.3-20 s (log); the top (Q15 ≥ 32512) is infinite |
| (turn) | damp | turn white | `params[2]` | 0..22528 → Q15 | 9830 | HF decay ratio 1-10 (log) |
| (turn) | mix | turn orange | `params[3]` | 8192..32767 → Q15 | 11469 | dry → wet, equal power |
| predelay | predelay | fn + blue, fine on shift + fn + blue | word 5 | continuous | 3277 (20 ms) | 0-200 ms, linear |
| early | early | fn + green, fine | word 6 | continuous | 16384 | early taps 0 → full; late part full to the middle, then down to 0 |
| width | width | fn + white, fine | word 7 | continuous | 32767 | stereo width, 0 mono → full |
| freeze | freeze | orange click | bit 0 of `params[8]` | latch | off | holds the tail (no input, no loss), not saved, never randomised |

Fixed: motion (the lines' modulation) at 9830, as kennel (the firmware build's fourth page-2 knob).

**The upstream mapping and what moved** (kennel `hall_knl.cc`, `FX_hall`): the four turns are kennel's; kennel's
push pages (blue → word 5 pre-delay, green → word 6 early, white → word 7 width, each a per-encoder "page 2"
toggle with kennel's quarter-click fine on shift) became fn + turn on the same encoder, with shift + fn + turn as
the fine step (the owner, 2026-11-11; guidelines §4, §4.1, held layers instead of the page toggle); kennel's orange
`OP1_PRESS_LATCH` freeze is the orange click (guidelines §4.3). Mix stays on the orange turn (§4.3).
- **Width at every position.** Kennel ran the mono render at synth and drum (mono in, mono out; width shown at the
  master only). coop runs the stereo render everywhere, so width acts at every position, default full width
  (accepted by the owner, 2026-11-11).
- **Latches clear when the stock switches synth ↔ drum** (the stock starts the instrument effect again: freeze is
  off after a visit to the other instrument; `docs/ui-features.md` §14, accepted by the owner). A sound change and a
  re-selection clear it too.
- **Units.** Guidelines §3.3 would show predelay in `MS` (0-200 linear: ms = Q15 × 200 / 32767); `plugin.json`
  "controls" has no `format` field yet (`tools/plugin/pack.py`), so the core shows its default readout. **To do when
  the schema has a `format` field** (the owner, 2026-11-11: show predelay in ms): add `"format": "ms"` with range
  0-200.
- **Randomise**: the three words are declared randomisable (the default), so e6's randomise (synth / drum key + right
  arrow on the effect page) draws predelay, early and width; freeze never is.
- **`"stereo": true` is not carried over**: in a kennel file that flag means kennel's `op1_fx_stereo_t` layout, and
  coop's loader would refuse a `coop_fx_t` with it ("plugin flags not supported yet"). A `coop_fx_t` is stereo by ABI.

## The glue (cs_hall.cc)

- One instance in the slot's position buffer (e8; before e8 in `.bss`, 86,740 bytes, one per relocation): each
  position has its own tail.
- params read once a block: `params[0..3]` mapped from the filter type's range units to Q15 with kennel's formula
  (`(v - lo) × 32767 / (hi - lo)`, four divides a block, outside the sample loops), `params[5..7]` clamped to Q15,
  freeze from `params[8]` bit 0.
- **The tank clear (guidelines §2.3).** Kennel zeroed the instance (84 KB) in `Init` and on a restart; coop's init
  runs in the audio interrupt at the stock positions, so `init` only resets the reverb's control state and marks the
  buffers. `render` then zeroes 4 KB a block (1024 word stores) and passes the input through dry until done: 21 blocks,
  ~30 ms of dry signal after every selection, sound change or re-selection (accepted as it is by the owner,
  2026-11-11: listen for it on the device). Before e8 a tank the reverb hadn't written since it was last zero (the
  loader zeroed `.bss`) was not cleared; e8's shared position buffer is always cleared, so a first selection is dry
  for the same ~30 ms.
- **The restart after a pause** is kennel's (`half_rate.h`): more than 5 ms between two renders (the effect bypassed
  with T3, or not rendered) starts the reverb again, cleared the same way. The pause is measured with the core clock's
  CYCLES register (not a firmware address; rings does the same).
- No mono render, no `knl::Fine`. The screen is `cs_hall_screen.cc` (below); the render publishes nothing for it.

## Screen (e9, 0.2)

Kennel's screen (`te-op1-cfw/ports/knl/cs_hall`: `hall.svg`, the firmware build's hall screen with the category dots
removed, and `hall_knl.cc` ScreenUpdate) on the e9 effect-screen path (`docs/plugins.md` "e9"): `cs_hall.svg` (from
`gen_cs_hall_svg.py`) and `cs_hall_screen.cc`. Kennel has no generator for `hall.svg`: the coop generator holds
kennel's coordinates as data (every point copied from `hall.svg`) and writes them in coop's colours and rules. It
shows wherever cs_hall is the effect on the page: FX 1 of the synth, the drum or the master, FX 2 in focus.

| Part | Encoder | What moves it |
|---|---|---|
| the hall outline and the reflection path inside it | blue (size) | scaled 0.4..1.0 about the floor's centre |
| the decay envelope (+ the infinity sign at infinite) | green (decay) | length by decay, from the pre-delay's end; full length and the sign while infinite (decay at the top, or frozen) |
| the damped envelope | white (damp) | shortened from the decay envelope's length by damp |
| the mix bar (top right) | orange (mix) | scaled from its left end |
| the freeze star (top) | orange click (freeze) | body tone, orange while the latch is on (the decay goes full length with the sign) |
| fn layer: the pre-delay gap (left) | blue with fn (predelay) | the gap's length, and every envelope starts after it (up to 50 units) |
| fn layer: the early taps and the late tail | green with fn (early) | the taps rise, the tail falls as early goes up; the tail's length follows decay |
| fn layer: the width triangles (top right) | white with fn (width) | apart by up to 14 units each way |
| fn layer: the motion marks | (pencil) | still: motion is fixed at 9830 in this build |

While fn is held the fn layer replaces the plain turns' parts (the hall stays, dimmed to its body tone; the mix bar
gives way to the width marks), as kennel's page 2 did.

What changed from kennel's screen:
- **Inputs**: the four turns from `view->knobs` (kennel's `op1_fx_knobs_live`); predelay / early / width from
  `view->words[0..2]` (kennel's `ScreenWord(5..7)`); freeze from `view->latches` bit 0 (kennel's `Pressed(pos, 3)`).
  Kennel's screen read nothing from the DSP (only the constant `kHallInfinite`), so the render publishes nothing and
  the state is unchanged (no `Display` words, scratch unchanged).
- **Kennel's page 2 is fn held** (`view->mods` bit 1; kennel: `AnyPage2(pos, 0x7)`, any of the three page knobs
  pressed). Kennel's page indicators (`page1ind`, `page2ind`: two squares top left) are gone: fn is held, not a page,
  and the layer is its own mark.
- **Width moves at every position** (kennel moved its marks at the master only: coop's stereo render has width
  everywhere).
- **Colours**: coop's brights (blue 698EFF, green 00ED95, white FFFFFF, orange FF3A5D). Colour is the knob: damp is
  white (kennel: its pencil DED7FF), width is white (fn + white; kennel: orange), the freeze star lights orange
  (orange's click; kennel lit it in decay's green). One body tone 38354D (kennel's second dim 514959, used for the
  wall mark, the reflection path, the dimmed hall and the unlit star, is folded into it); the motion marks pencil
  DFD9FF (no encoder owns motion). A hidden palette line (`palorange`) carries the star's lit colour.
- **The motion marks are still art**: kennel scaled them by the motion word every frame; motion is fixed (9830), so
  the generator draws them at that scale (0.3 about their centre lines) with no ids.
- **The width triangles are paths** (`M L L Z`, filled), kennel's polygons not being in R246's renderer subset
  (guidelines §5.3). The infinity sign stays kennel's path (`M C Z`, one contour).
- No numeric readouts to drop (kennel's hall had none): the core's popup names each control and its value.
- SVG 3,927 bytes (kennel's 4,047), checked by `tools/plugin/pack.py` (R246's parser rules) and by the generator.

Not done (open for the owner): the screen brief's "names and values listed" on the held layer (the e9 path has no
text calls; the core's popup shows the one being turned); freeze is shown by the star and by the decay going infinite,
not by the envelope "held flat".

## Memory, L1, cost
- **e9 (2026-11-11, 0.2, the screen):** file 23,184 bytes (SVG 3,927), `.text` 12,268, `.rodata` 5,248, `.bss` 100
  (the five buffer pointers and the screen's node table and colours); load 17,616 once, +1,396 over e8 (16,220 with
  e9's new `coop_fx_t` field). The state and `"scratch"` are unchanged (nothing published). Audio bit-identical, host
  and simulator (host test below).
- **e8 (2026-11-11):** the hall and the half-rate path (at most the declared 86,752 bytes, static_assert) are in the slot's position buffer (plugin.json `"scratch": 86752`, `coop_fx_scratch`), as kennel's `op1_fx_scratch` had them; the file is relocated once and its `.bss` is the five buffer pointers (20 bytes); init resets the state there at every selection. The figures below are from before e8 (state in `.bss`, five relocations). The tank is cleared on every (re)start now, a fresh selection included (~30 ms dry). File 17,060 bytes, load 16,216 once.

- File 17,712 bytes; `.text` 11,168, `.rodata` 5,024, `.data` 0, `.bss` 86,740: load size 102,932 bytes, so five
  relocations take ~515 KB of heap 0 (op1emu reports ~8-9 MB free after the scan, `docs/engines.md`) [I until the gate].
- L1: none (effects have no L1 lease yet, guidelines §2.3 [proposed]). The tank lives in SDRAM: per sample 8 lines ×
  2 reads, 10 early taps, 3 allpass tails and 12 writes (24 while mirrored), so the device will run above the
  simulator.
- Cost, upstream: the hall alone in `bfin-elf-run`, per 32-sample call (one per coop block): 8,845 avg / 9,315 peak
  stereo, 16,731 while a size change crossfades (8 blocks); kennel declared no `cost` and never ran it on a device.
- Cost, this port, the whole render in `bfin-elf-run` (`make -C tests/host/cs_hall sim`): 18.2-22.0 k avg, 26.2 k max
  a block (the max is a size crossfade); the glue (decimate, interpolate and mix both sides, the conversions,
  control) is about half. The simulator models no cache or SDRAM stalls: expect the device 10-35 % above op1emu
  (coop's figure) and up to 1.5-4× the simulator from cold caches (upstream's warning for the plate and hall) [I].
- **Running at every position**: each position that selects it runs its own full stereo render (kennel ran the mono
  kernels at synth and drum, ~9 % cheaper in the hall core). Synth FX 1 + master FX 1 + both FX 2 is four times the
  figure above.
- Device: pending (`docs/device-tests.md` "cs_hall"). Budget: under 30 k a block, over 60 k does not ship.
- Optimisation plan, in order, if the device figure needs it (each re-checked bit for bit against this build):
  1. measure the split hall / glue in the simulator; 2. fuse the int32 ↔ int16 conversions into the decimators' input
  copy and the mix's output store; 3. one `MixSoftLimit` pass for both sides (the products in the dual MAC);
  4. decimator / interpolator kernels that keep their history in place instead of copying it each block.

## Host test

`make -C tests/host/cs_hall` (host C) and `make -C tests/host/cs_hall sim` (the Blackfin build with its assembly in
`bfin-elf-run`): this plugin against te-op1-cfw's hall played through kennel's glue (transcribed: knob formula,
latch, restart), 6,880 blocks of noise, saw, impulses, full-scale noise, mono input, every control moving, size
crossfades, infinite decay, freeze, a sound change (the 21-block clear) and a 5 ms pause. **Bit-identical, host and
simulator** (2026-11-11). Mutating the reference's clear length or the motion constant breaks the match (checked).
In a worktree pass `TE=/mnt/i/GitHub/coldsoldr/te-op1-cfw/ports`.

## Gate plan (op1emu; run 2026-11-11, results in `docs/device-tests.md` "cs_hall": "cs hall" is the first row of the alphabetical FX list, picked with `ccw 96`)

Build (WSL, `cd /mnt/i/GitHub/coldsoldr/co-op/firmware`): `make -C core/sdram combined` (the e6 core) and
`make -C plugins` (`build/plugins/cs_hall.coop`). Then, one run at a time:
1. NAND with cs_hall alone:
   `mkdir -p build/nand_cs_hall/plugins && cp build/plugins/cs_hall.coop build/nand_cs_hall/plugins/` and
   `python3 -I tools/make_nand.py --provision ~/op1emu-work/provision_nand.py --firmware ~/Downloads/op1_246.op1
   --mkyaffs2 ~/op1emu-work/yaffs2utils/mkyaffs2 --overlay build/nand_cs_hall --output ~/op1emu-work/cs_hall_nand.bin`.
2. `NAND_KEEP=~/op1emu-work/cs_hall_nand.bin tests/sim/hear.sh <e6 LDR> tests/sim/det_fx_cs_hall.txt
   ~/op1emu-work/cs_hall_controls.wav`: the five positions (synth, drum, master FX 1; instrument and master FX 2),
   each control, the chord. Expect (stats `fxctl`, the scenario's header): `p0 ...:1985/16384/324xx/0`,
   `p3 ...:3933/16384/32767/0`, `p2 ...:2949/16384/32767/1`, `p4 ...:3277/16384/32111/0`, `p1 ...:3605/16712/32767/0`;
   `plugins seen 1 loaded 1 failed 0`; no fault; a reverb tail in the WAV after the chord (the master's held).
3. Same NAND, `tests/sim/det_fx_reload.txt` (a power cycle): the words above kept, every latch 0 (its header's
   figures are test_fx's: compare with step 2's instead).
4. `tests/sim/det_fx_select.txt` on a fresh cs_hall NAND (the synth position, a chord) and on the pristine NAND
   (`det_chord.txt`, unchanged against e6).
5. Read the stats `cost` line (`fx1_plugin=`, `s2_instr=`, `s2_master=`) and `plugins` (`heap_largest`): emulator
   figures (the device is 10-35 % higher).
