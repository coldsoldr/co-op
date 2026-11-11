# cs_plate

coldsoldr's plate reverb: Dattorro's figure-of-eight plate ("Effect Design Part 1", JAES 1997) in fixed point, as
a coop effect. The DSP is te-op1-cfw's `ports/fx/src/plate` (9ced415, coldsoldr's own, GPL-3.0-or-later with the
OP-1 firmware permission); the half-rate path around it is te-op1-cfw's Clouds port (MIT, `LICENSE-MIT`). It was
built for hundekraft as a kennel effect (`ports/knl/cs_plate`), never released; this port is from that build.
Wet only, mixed with the dry at the full rate: the plate runs at 22.05 kHz between Clouds' halfband filters.

## Controls (guidelines §3.2)

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | decay | damp | pre-delay | mix |
| Click | — | — | — | freeze (latch) |
| Shift + turn | decay fine | damp fine | pre-delay fine | mix fine |
| Shift + click | — | — | — | — |
| fn + turn | size (8 steps) | diffusion | motion | — |
| fn + click | — | — | — | — |
| Shift + fn + turn | — | diffusion fine | motion fine | — |
| Shift + fn + click | — | — | — | — |

- **The turns** are kennel's four knobs (`params[0..3]`). They arrive in the filter type's range units
  (`sdk/coop_plugin.h`) and are mapped to Q15 once a block with kennel's own formula (`op1_fx_param_q15`), using the
  SDK's constant ranges instead of the firmware table kennel read. decay 0..32767 -> tank gain 0.2..0.98; damp
  bright -> dark (26.7 kHz .. 600 Hz); pre-delay 0..200 ms; mix dry -> wet (equal power). Defaults are kennel's:
  16384, 9830, 3277, 11469.
- **size** (fn + blue, word 5): `steps` "1".."8", the plate's eight tank lengths (0.6 .. 1.0 of Dattorro's,
  geometric). The DSP takes the step index (`params[5]`, clamped 0..7). Kennel quantised a continuous word 5
  (`(w * 7 + 16383) / 32767`); guidelines §3.1 make that a stepped control (the owner, 2026-11-11). The default is
  step index 4, shown as "5" (kennel's default word 18725 lands on it). **4 detents a step**: every size change mutes
  the wet for about 20 blocks (~29 ms: a fade out, a 256-sample crossfade of the tank reads, a fade in), so a step
  should take a deliberate turn. 8 steps x 4 = 32 detents across, about what kennel's page knob took (~3 clicks of
  1536 a zone). **No wrap**: 8 -> 1 would be the biggest jump in tank length. No fine (a steps control has none), so
  shift + fn + blue is empty. Drawn by randomise.
- **diffusion** (fn + green, word 6, default 22938): the input allpasses' gains, 0 .. Dattorro's. Continuous, with
  its fine on shift + fn + green; saved; drawn by randomise. The screen name is `diffuse`, because names have at most
  8 characters.
- **motion** (fn + white, word 7, default 9830): the tank's modulation depth, 0 .. 2x Dattorro's (16384 = his).
  Continuous, with its fine; saved; drawn.
- **freeze** (orange click, latch bit 0 of `params[8]`): decay 1, damping off, the input gated off, so the tail
  holds. It is not saved and never randomised. **A latch clears whenever the effect starts again at its position**:
  when it is selected, when the sound changes, and when the stock switches between synth and drum (the stock restarts
  the instrument effect). A freeze held at the synth or drum position is therefore off after a visit to the other
  instrument. The owner accepted this on 2026-11-11 (`docs/ui-features.md` §14).
- There is no input gain (there was none upstream). The low cut stays fixed at kennel's 3277 (about 49 Hz): kennel
  could not put it on a page knob, and it has no place in the owner's layout.
- Common meanings (§4.3): mix on the orange turn and freeze on the orange click follow the table. Size sits on
  fn + blue, not on the blue turn as §4.3 suggests: the owner kept kennel's four turns (2026-11-11).

**Upstream mapping.**

| kennel cs_plate | coop cs_plate |
|---|---|
| page 1: blue decay, green damp, white pre-delay, orange mix | the four turns, unchanged (`params[0..3]`) |
| blue press -> page 2: size (word 5, continuous, quantised to 0..7 in the glue) | fn + blue, 8 steps, word 5 = the step index |
| green press -> page 2: diffusion (word 6) | fn + green, continuous, fine on shift + fn + green |
| white press -> page 2: motion (word 7) | fn + white, continuous, fine on shift + fn + white |
| shift + a page-2 knob: quarter clicks (`knl::Fine`, the shift byte) | the core's `"fine": true` (1/16 of a detent) |
| orange `OP1_PRESS_LATCH`: freeze (`knl::Pressed(pos, 3)`) | orange click, `"kind": "latch"`, bit 0 |
| low cut fixed at 3277 | the same |

A sound saved by kennel holds a Q15 size word; coop reads it as a step index clamped to 0..7, so it plays at size 8
unless the word was under 7. No kennel sound was ever saved with this plugin (it was never released).

## Stereo (guidelines §2.2)

The plate is true stereo, as upstream: one tank, both inputs, two sets of Dattorro's output taps. It runs this path at
every position (synth, drum, master, both second slots), with one state per position (each relocation has its own
`.bss`). The plugin.json does **not** carry kennel's `"stereo": true`: coop's loader would read that flag as kennel's
`op1_fx_stereo_t` layout. A `coop_fx_t` is stereo by its ABI.

Dattorro's plate sums its input to mono at the pre-delay (`(l + r) >> 1`). The stereo picture comes from the tank's
two output tap sets, and the dry path keeps the full stereo input. This is the reverb's design, not a dual-mono wrap,
so it is kept as it is. It is an open question for the owner against §2.2's "never sum L and R" rule.

## What coop forbids, found and fixed

| upstream | why | coop |
|---|---|---|
| `constexpr double` maths for the size, damping and low-cut tables (`plate.cc`) | no `float` / `double` anywhere (§2.1) | integer literal tables (`plate.cc`), generated from the upstream build and checked entry by entry by the host test |
| `knl::Fine` reads the shift byte at 0x01DD7F78 | no firmware addresses (§2.1) | the core's `"fine": true` |
| `op1_fx_param_q15` reads the range table at 0x01DE5C50 | no firmware addresses | the SDK's constant ranges, once a block (`cs_plate.cc Turn`) |
| `knl::Pressed(pos, 3)`, `knl::Instance` (kennel's scratch buffer per position) | kennel host API | `params[8]` bit 0; the state in this relocation's `.bss` |
| `HalfRate::Begin` restarts after a 5 ms pause, timed by `op1::Cycles()` | kennel had no init on resume | gone: coop calls `init` whenever a position starts the effect again |
| the soft-limit table built at the first `Begin` (`static inline` member, from Rings' `kSoftLimitQ30`) | work on the audio thread | a literal table (`halfband.cc`), checked against the upstream's builder |
| `PlateReverb::Init` clears 76 KB at once (kennel's init) | slow init on the audio thread (§2.3) | `init` only arms a clear; renders clear 8 KB a block (below) |
| the screen (`op1_find_node`, `op1_fx_knobs_live`, page-2 state) | firmware screen nodes and globals | not ported: no effect screen hook yet (below) |

No libc, no static constructors (the state objects have trivial constructors and live in `.bss`), no division in a
per-sample loop (four divides a block for the turns). The C reference kernels are compiled for the host only. The
Blackfin always runs the assembly (`plate_bfin.S`, `halfband_bfin.S`, unchanged from upstream but for an include path;
`tools/asm_lint.py`: 0 packets flagged). `PlateTankRunMonoAsm` stays in `plate_bfin.S` unused.

## Init and memory (guidelines §2.3)

- `init(slot)` only sets a counter (a few cycles). The renders that follow zero the state 2048 words (8 KB) a block,
  for 10 blocks (~14.5 ms), leaving the block untouched: the dry passes, as kennel's restart block did. Then the plate
  is reset, and the next block is the first processed one. The halfband filters' and the mix smoother's start state
  is all zero, so there is nothing else to build. A clear block costs 2,152 cycles at most in the GNU simulator, which
  models no cache or SDRAM. On the device each cleared line is an SDRAM write [I].
- File 11,460 bytes; `.text` 7,600, `.rodata` 2,282, `.bss` 77,652 (the plate's pre-delay 8.8 KB, diffusers 2.7 KB,
  tank 64.2 KB, the half-rate buffers). Load size ~87.5 KB a relocation, so **x 5 = ~438 KB of heap 0** for the five
  relocations. Read `heap_largest` in the stats `plugins` line on the device with the other plugins installed.
- L1: none. Effect leases are [proposed]. The tank is 64 KB (SDRAM by necessity). Upstream measured about 70
  cache lines touched per call, which is the device's main excess over the simulator (`docs/plate-port.md` there).

## Cost

| | cycles a block (64 samples) |
|---|---|
| upstream, bfin-elf-run, stereo, 32-sample call at 22.05 kHz (`docs/plate-port.md`) | 7,994 avg, 8,123 max (plate only) |
| this port, GNU simulator, the whole render, still knobs | **17,456**: decimate 2,054, plate 7,956, interpolate + mix 5,456, the two int16 / int32 passes ~780, the turns and the rest ~1,200 |
| this port, GNU simulator, 11,352 random blocks (moving controls, size changes, freezes) | 17,912 avg, 19,828 max; the first block after a clear 18,178; a clear block 2,152 |
| op1emu (`fx1_plugin=`) | pending (gate) |
| device | pending |

- Upstream's figure is the plate alone. The kennel build ran the same half-rate path around it, so its whole cost
  was about the same 17.5 k [I] (kennel never measured it). The half-rate path costs as much as the plate.
- The simulator figure is under the 30 k budget. The device adds stalls: 10-35 % for typical code, possibly more
  here (about 70 cache lines a call, `docs/plate-port.md`), so about 19-24 k [I]. That is within budget but not by
  much: read the device cost line.
- Every position at once: each running instance pays its own cost, with no shared state. A plate on all five
  positions is about 5 x 17.5 k = 88 k in the simulator (more on the device), against the 580,499 block.
- Running at every position is stereo by design. On kennel the same stereo render ran only at the master; the
  synth and drum positions now get it too, so they also pay the stereo cost (there was no mono path to save).
- The optimisation pass, if the device figure comes close to 30 k (none is needed for the simulator figure):
  - fold the int32 / int16 conversions into the decimator's input copy and the mix's output (~0.8 k);
  - with L == R (a mono synth with no spread), decimate once, since the plate sums its input anyway (~1 k);
  - the interpolate + mix stage (5.5 k for 2 x 64 samples) is the largest after the tank: one fused dual-MAC
    kernel for both channels (`docs/plugin-performance.md` rule 7);
  - prefetch the tank's read streams (upstream never tried it).
  Each needs a bit-exactness check against this version.

## Screen (guidelines §5)

The stock filter page shows for now: coop has no effect screen hook, so the core shows each control's name and value
top left for ~1.2 s when it moves. Upstream's `plate.svg` (te-op1-cfw `ports/fx/op1/plate.svg`) is not carried
across. Its kennel build hid category dots and toggled a page-2 view on knob presses, which coop's held layers don't
have. Brief for when effect screens exist (to mock in `tools/playground/coop-playground.html` first):
- a plate seen side-on whose rings grow with decay (1-4 rings; all four and the outline in the damp colour while
  frozen);
- a pre-delay gap before the plate that scales with pre-delay;
- a dark band over the tail that scales with damp;
- the mix as the stock's mix bar;
- while fn is held: the size as the plate's scale (8 steps, the step number), diffusion as 1-8 dots at the input,
  motion as two small bars at the plate's edges.
- State tells the truth (§5.4): freeze shows only while the latch is on. A latch cleared by a synth <-> drum switch
  must show as off.

## Tests

- **Host** (`make -C tests/host/cs_plate`; from a worktree add `TE=/mnt/i/GitHub/coldsoldr/te-op1-cfw/ports`).
  `cs_plate_compare.cc` links this plugin and te-op1-cfw's plate + Clouds half-rate path side by side.
  - Tables: identical (size 8 x 22 entries; the control's coefficients at every Q15 input for damping and low cut;
    the crossfade tables; the 513-entry soft-limit table).
  - The chain: `FX_cs_plate.render` against upstream `PlateReverb` + `HalfRate` driven as `plate_knl.cc` does, 60
    random segments (each from an init) with noise, saws, bursts and silence (L != R), random turns in range units
    (some out of range), size steps, diffusion, motion and freeze toggles. **Bit-identical**: 102,088 blocks compared, 600 clear blocks,
    509 size changes, 138 freeze toggles.
- **GNU simulator** (`make -C tests/host/cs_plate sim`): the same test built for the Blackfin, the plugin's assembly
  against upstream's C. **Bit-identical**: 6 segments, 11,358 blocks, 59 size changes, 22 freeze toggles. It prints
  the cycle figures above.
- **op1emu gate**: not run in this port (gate runs go one at a time). The plan:
  1. `make -C plugins`, then a NAND with `build/plugins/cs_plate.coop` alone in `/yaffs2/plugins`
     (`tools/make_nand.py ... --overlay`, `docs/plugins.md` "Building and testing"). Alone means it is the last row
     of the stock FX list, as the scenarios assume.
  2. `tests/sim/det_fx_select.txt`: synth position, a chord. Expect `plugins seen 1 loaded 1 failed 0`, no fault,
     and the plugin's blocks in the stats.
  3. `tests/sim/det_fx_cs_plate.txt` (new): synth FX 1 on, chord tails, fn + blue / green / white, the fine, a
     freeze and a size change under the frozen tail. Expected words and counts are in its header ([I] until run):
     `fxctl ... p0=...:3/23594/10178/0`. Listen to the dump: the tail rings, holds while frozen, carries on after
     the size change with no click, and decays after the unfreeze. Read the cost line's `fx1_plugin=`.
  4. `tests/sim/det_fx_controls.txt` with cs_plate alone (it walks synth FX 1, instrument FX 2, master FX 1, master
     FX 2, drum FX 1, and randomises twice). It was written for test_fx, so its expected words differ here. At p0:
     size 4 -> 3 (fn + blue 4 detents down), shift + fn + blue and the green click are ignored, motion 9830 + 328 +
     20, freeze on then cleared by the drum visit; randomise then draws all three words. The check is no fault, every
     position rendering (`pN=` blocks and inits) and the latch behaviour. Then `det_fx_reload.txt` (power cycle,
     same NAND): every word kept, every latch 0.
  5. A pristine-NAND `det_chord.txt` against the image run without the plugin: unchanged.
  6. Note in the record that op1emu counts instructions, not stalls: the device is 10-35 % higher, and this tank
     touches more SDRAM lines than most.

## Files

`cs_plate.cc` (the glue), `plate.h` / `plate.cc` / `plate_layout.h` / `plate_bfin.S` (the plate), `halfband.h` /
`halfband.cc` / `halfband_bfin.S` / `half_rate.h` (the half-rate path), `LICENSE-MIT` (the Clouds-derived parts).
