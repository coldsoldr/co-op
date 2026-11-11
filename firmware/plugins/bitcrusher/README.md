# bitcrusher

Sample-rate and bit-depth reduction, in stereo, at every effect position. The DSP is te-op1-cfw's crush core
(`ports/fx/src/crush/crush.h`, coldsoldr), the one the kennel plugin `ports/knl/bitcrusher` (5aa314b, hundekraft
3.1) wrapped; GPL-3.0-or-later as coop's own plugins (the owner's code: there is no upstream licence file to carry).
No anti-aliasing: the aliasing is the sound.

- **Rate**: a 32-bit phase accumulator; when it wraps, the input is sampled and held. 44.1 kHz down to 500 Hz,
  exponentially (6.46 octaves).
- **Bits**: the held sample rounded to 16 down to 2 bits.
- **Jitter**: each new hold restarts the phase at a random fraction of a period, so holds come early by up to
  jitter x one period.
- **Gain**: into the crusher only, -6..+6 dB (0 dB at the centre); the crushed signal clips at full scale. The dry
  side of the mix is the signal as it came.
- **Mix**: dry / wet, linear; the top is exactly wet.

## Controls (guidelines §3.2)

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | rate | bits | jitter | mix |
| Click | — | — | — | — |
| Shift + turn | rate fine | bits fine | jitter fine | mix fine |
| Shift + click | — | — | — | — |
| fn + turn | gain (-6..+6 dB, 0.5 dB a detent) | — | — | — |
| fn + click | — | — | — | — |
| Shift + fn + turn | gain fine (0.1 dB a detent) | — | — | — |
| Shift + fn + click | — | — | — | — |

- The four turns are the stock's (`params[0..3]`, in the filter type's range units, normalised in the render as
  kennel's `op1_fx_param_q15` did). Blue and green turn the stock way, clockwise = more: a higher rate, more bits
  (the owner, 2026-10-27); the core's rate and bits are the amount of crushing, so both are the knob mirrored. Shift
  + turn is the stock fine adjustment.
- **Blue's shift + fn layer is the gain's fine step** (the owner's decision, `docs/ui-features.md` §14): fn + blue
  moves the gain 0.5 dB a detent (`"step": 1365` Q15, 2730.7 a dB over the +-6 dB range), shift + fn + blue 0.1 dB
  (`"fine_step": 273`), kennel's steps (the owner, 2026-11-11). Declared `"fine": true`, so nothing else sits on
  blue's shift + fn + turn.
- The gain is saved with the sound (word 5, default 16384 = 0 dB) and is not randomised (`"random": false`: a random
  draw would be a level jump).
- No latch, so nothing is cleared when the stock switches synth ↔ drum (the crusher's own state, its phase and held
  samples, starts again there, as on kennel).
- Mix is already on orange and rate on blue (guidelines §4.3): nothing moved.

**Upstream mapping.** Kennel: the same four turns; a blue press flipped blue to page 2, where it turned the input
gain (word 5, ~1536 a click, saved), and shift + blue there moved it a tenth of a dB a click (`knl::Fine`, which read
the stock shift byte at 0x01DD7F78 and rewrote the stored word). The other three presses were unused. coop retires
the page toggle (guidelines §4: every layer is held): the gain is a declared `fn_turn` control on word 5, its fine on
shift + fn + blue, both dispatched by the core. A sound saved under kennel keeps its gain word: same word, same scale.

`plugin.json` "controls":

```json
{"id":"gain","name":"gain","action":"fn_turn","encoder":"blue","word":5,"kind":"continuous","default":16384,"fine":true,"step":1365,"fine_step":273,"random":false}
```

## What changed from upstream

- **Stereo at every position** (guidelines §2.2): upstream is a stereo core (one clock, a held sample per channel),
  flagged stereo, but kennel ran its stereo render only at the master and the mono one at synth and drum. coop runs
  the stereo path everywhere, so the instrument positions now crush each side of the stereo pair (with no width,
  L = R, the two sides come out equal, as mono did). Never summed to mono.
- **coop's buffers**: int32 planar (the int16 bus widened by 16 bits). The crusher reads the top 16 bits, as
  upstream read the int16 sample; the dry side keeps whatever low bits came in (dry + the int16 mix delta << 16). With
  the host's widened int16 the output is upstream's sample << 16, bit for bit (host test below).
- **Removed**: kennel's per-position instance array (each coop relocation has its own `.bss`), the shift-byte fine
  hack and the page state (`op1_fx_page`), the firmware range-table read in `op1_fx_param_q15` (0x01DE5C50: the
  ranges are the SDK's constants now), the input meter pass and the whole kennel screen (both back with e9: "Screen"). `CrushParams`' default
  member initialiser and the class's private members became plain data (no constructor in `.bss`).
- **Not set**: plugin.json `"stereo"`. That flag means kennel's `op1_fx_stereo_t` layout (render_stereo at +12) to
  the loader, which checks three callback pointers at offsets 0, 4 and 12; a `coop_fx_t` has its magic and api there,
  so the file would be refused. A coop effect is always stereo.

## Screen (e9, 0.2)

Kennel's screen (`te-op1-cfw/ports/knl/bitcrusher`: `crush.svg` from `gen_crush_knl_svg.py`, the canvas "Bitcrusher
screen" option C, `crush_knl.cc` ScreenUpdate) on the e9 effect-screen path (`docs/plugins.md` "e9"):
`bitcrusher.svg` (from `gen_bitcrusher_svg.py`) and `bitcrusher_screen.cc`. It shows wherever bitcrusher is the
effect on the page: FX 1 of the synth, the drum or the master, FX 2 in focus.

| Part | Encoder | What moves it |
|---|---|---|
| the staircase (eight step widths) and the clock row | blue (rate) | variant by rate; dims while fn is held |
| the bit ladder | green (bits) | 8 - round(7 x bits) rungs |
| the clock row's two halves shaken apart | white (jitter) | up to 4 units |
| the sampling dot | (pencil) | runs the steps, faster at higher rates |
| the gain gauge (left) | blue with fn (gain) | the bar about the centre, -6..+6 dB; its blue brightens with the input level; a bracket while fn is held |
| the mix gauge (right) | orange (mix) | scaled about its foot |

What changed from kennel's screen:
- **Inputs**: the four turns from `view->knobs` (kennel's `op1_fx_knobs_live`, the stock pages' values with the LFO);
  the gain from `view->words[0]`; the input level from the state the render publishes (`state.h` `Display.level`,
  kennel's `in_level[]` in `.bss`): one word written once a block after the DSP, read while `view->live`.
- **Kennel's blue page 2 is fn held** (`view->mods` bit 1): fn + blue turns the gain, so while fn is down the gain's
  bracket shows and the staircase dims (kennel did both on page 2).
- **The dB readout is gone** (kennel's page-2 "+1.5" glyphs, 4.2 KB): the core's popup names the control and its value
  when fn + blue turns it (in percent: a `format` field in the controls schema would let it say dB; not built).
- **The clock row is drawn by the code** with raster lines (`coop_scr_line`, white, the step width of the rate) instead
  of 8 variants x 2 halves of SVG ticks (17 KB of kennel's 29 KB file). Their position matches the SVG's (op1emu
  screenshots, `docs/device-tests.md` "e9").
- **Colours** are coop's brights (blue 698EFF, green 00ED95, white FFFFFF, orange FF3A5D); the gain gauge is blue (it is
  blue's control under fn; kennel drew it white), the dot pencil DFD9FF.
- SVG 6,995 bytes (kennel's 29,389), checked by `tools/plugin/pack.py` (R246's parser rules) and by the generator.

The render's only addition is the meter (a peak over the block's 128 samples before the crusher, ~0.3 k cycles a
block [I]); the audio is bit-identical (host test below).

## Memory, L1, cost
- **e9 (2026-11-11, 0.2, the screen):** file 12,292 bytes (SVG 6,995), `.text` 2,728, `.rodata` 744, `.bss` 168 (the
  screen's node tables); load 3,640 bytes once (+2,164 over e8); the state 20 bytes (the crusher's 16 and the level
  word: `"scratch": 32` unchanged). The SVG parses to 15,744 bytes of the stock TE heap (op1emu `fscr_te`), not heap 0.
- **e8 (2026-11-11):** the state is in the slot's position buffer (plugin.json `"scratch": 32`, `coop_fx_scratch`); the file is relocated once and its `.bss` is the five buffer pointers (20 bytes); init resets the state there at every selection. The figures below are from before e8 (state in `.bss`, five relocations). File 1,860 bytes, load 1,476 once.

- File 1,672 bytes; `.text` 1,200, `.rodata` 140, `.data` 0, `.bss` 16 (the phase, two held samples, the jitter
  generator). Load size 1,356 bytes; five relocations (FX 1 at synth, drum, master; FX 2 instrument, master): 6,780
  bytes of heap 0. No imports.
- L1: none. The per-sample loop touches only the two buffers (on the interrupt stack) and registers; the state is
  loaded once a block and written back once.
- Cost plan (the budget: under 30 k cycles a block, over 60 k doesn't ship):
  - Upstream: te-op1-cfw's simulator gave 2.5 MHz stereo / 2.4 MHz mono for this core (Test 39), about 3.6 k / 3.5 k
    cycles a block at 689 blocks a second [I: a simulator figure of the old fx host].
  - Stereo at every position doubles nothing here: the core was stereo already, and the second channel is a few
    instructions a sample on the shared clock (upstream's mono / stereo figures above differ by 3 %). Each slot that
    selects it pays its own block (figure below); all five at once would be about 5 x that [I].
  - Measured: `make -C tests/host/bitcrusher profile` (GNU simulator, compute only, 4,000 random blocks, the scenario
    below): **5,259 cycles a block on average, 6,733 at most** [C simulator]. The four range divisions, the gain's two
    and one 64-bit multiply run once a block; the loop is a hardware loop (LSETUP) with no calls, the hold (rare at
    low rates) out of line.
  - Optimisation: none needed at this cost. If the device says otherwise: run the hold's gain as a 16 x 16 multiply,
    replace the per-block divisions by reciprocal multiplies, and fuse the two channels' mix in the dual MAC
    (rule 7). A mono path for L == R is not worth its code at ~2.6 k a channel.
  - Device: pending (`fx1_plugin=`, `s2_instr=`, `s2_master=` in the stats `cost` line).

## Host test

`tests/host/bitcrusher` (`make -C tests/host/bitcrusher`, in WSL; from a worktree pass
`TE=/mnt/i/GitHub/coldsoldr/te-op1-cfw/ports`): the coop render against te-op1-cfw's own `op1fx::Crush::Process`
(stereo path) with crush_knl.cc's parameter mapping and `knl::GainQ14`, 40,000 seeded blocks in one carried state
(noise, quiet, silence, a saw, edge values, L == R a quarter of the time; knobs held, walked, jumped, at and past both
range ends; the gain word anywhere). **0 mismatching samples** with the widened int16 input, and 0 with random low 16
bits (top halves equal to upstream's, low halves the input's) [C host, 2026-11-11].

## op1emu gate (not run yet: gates go one at a time)

A NAND with `build/plugins/bitcrusher.coop` alone in `/plugins` (`tools/make_nand.py --overlay`, as `docs/plugins.md`
"Building and testing"; or `PLUGINS=build/plugins/bitcrusher.coop tests/sim/run_emu.sh ...`), the e6 image. The stock FX
list is alphabetical with the plugins sorted in, so bitcrusher is its first row: the FX 1 picks are `turn enc1 ccw 96`
instead of `cw 96` (FX 2's list is in plugin order: unchanged). Run 2026-11-11, results in `docs/device-tests.md`:
1. `tests/sim/det_fx_select.txt`: bitcrusher at the synth position (FX 1), a chord through it. Expect `plugins seen
   1 loaded 1 failed 0`, `plugin_blocks` counting, no fault; the dump audibly crushed (defaults: ~15 kHz, 13 bits,
   fully wet).
2. `tests/sim/det_fx_controls.txt`: all five positions (p0 synth FX 1, p3 instrument FX 2, p2 master FX 1, p4 master
   FX 2, p1 drum FX 1) with a chord and drum keys. Expect in the stats `fxctl` line `fx 1`, words 5 per position as
   test_fx's run: p0 11197 (four coarse down, one fine up: 16384 - 4 x 1365 + 273), p3 19114, p2 15019
   (`master_saves 1`), p4 16384, p1 17749; words 6 / 7 and the latch word 0 everywhere; green / orange clicks and the white fn turns
   counted as `ignored` (undeclared) [I: the counts other than the words are not predicted].
3. `tests/sim/det_fx_reload.txt` on the NAND step 2 left: the same words 5 after the power cycle.
4. A pristine-NAND `det_chord.txt` run: unchanged against the e6 reference.
Read the `cost` line (`fx1_plugin=`, `s2_instr=`, `s2_master=`): an emulator figure, the device is 10-35 % higher.
