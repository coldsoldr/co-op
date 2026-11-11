# wavefolder

A sine wavefolder with first-order antiderivative anti-aliasing (ADAA), in stereo at every effect position. Ported
from the hundekraft kennel plugin `wavefolder` 0.2 (te-op1-cfw `ports/knl/wavefolder/fold_knl.cc`); the DSP is
te-op1-cfw's FOLD (`ports/fx/src/fold` at 5aa314b, the owner's own code, here under this folder's GPL-3.0-or-later),
documented in te-op1-cfw `docs/fold-port.md` (mappings, fixed-point formats, aliasing, DC, the kernels' tests).

Version 0.1 (2026-11-11). Device: **pending** (`docs/device-tests.md` "wavefolder 0.1").

## Files

| File | What |
|---|---|
| `wavefolder.cc` | the `coop_fx_t`: the range words to 0..32767, the gain word, the bus conversion, the L == R shortcut |
| `src/fold.h`, `src/fold.cc` | FOLD: control, smoothing, ramps; the C reference kernels (host builds only) |
| `src/fold_bfin.S` | the Blackfin kernels (shape, tone mono, tone stereo), as upstream |
| `src/bus_bfin.S` | coop's bus <-> int16 conversions, two samples a pass (new) |
| `src/fold_layout.h`, `src/fold_tables.h`, `src/gen_fold_tables.py` | as upstream (the tables regenerate byte-identical) |

What changed against upstream: `Fold::Process` returns whether it wrote the output (false at a settled mix 0),
`ChannelsEqual` / `MirrorChannels` for the shortcut, the test hook `set_use_asm` is gone, and the Blackfin build has
no C kernels (the assembly takes every even n; coop passes 64). The DSP arithmetic is untouched.

## Controls

The four turns are kennel's four knobs (`params[0..3]`, in the filter type's range units: `{64..14528}`,
`{0..24576}`, `{0..22528}`, `{8192..32767}`, `sdk/coop_plugin.h`), normalised to 0..32767 once a block as kennel's
`op1_fx_param_q15` did (only when a word moved). The input gain is the one declared control (the owner's layout,
2026-11-11; `docs/ui-features.md` §14).

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | fold | symmetry | tone | mix |
| Click | — | — | — | — |
| Shift + turn | fold fine (stock) | symmetry fine (stock) | tone fine (stock) | mix fine (stock) |
| Shift + click | — | — | — | — |
| fn + turn | gain (-6..+6 dB, 0 dB at the centre, 0.5 dB a detent; saved, word 5) | — | — | — |
| fn + click | — | — | — | — |
| Shift + fn + turn | **gain fine** (the same control, 0.1 dB a detent) | — | — | — |
| Shift + fn + click | — | — | — | — |

`plugin.json` "controls":

```json
{"id": "gain", "name": "gain", "action": "fn_turn", "encoder": "blue", "word": 5, "kind": "continuous",
 "default": 16384, "fine": true, "step": 1365, "fine_step": 273, "random": false}
```

- **Blue's shift + fn layer is gain fine** (the owner's decision, `docs/ui-features.md` §14): nothing else may sit
  there. Kennel's steps (the owner, 2026-11-11): coarse `"step": 1365` Q15 a detent (0.5 dB), fine `"fine_step": 273`
  (0.1 dB).
- Gain is not randomised (`"random": false`: a random draw would be a level jump). There is no other saved control.
- What the gain does (upstream, unchanged): it scales the folder's gain only, kept inside the fold knob's own range
  (0.25..4 turns), so at either end of the fold knob it adds nothing; the level make-up follows the fold knob alone,
  and the dry side of the mix never sees it, so at mix 0 nothing changes.

**Upstream mapping (kennel):** turns fold, symmetry, tone, mix (unchanged, mix already on orange, §4.3). Blue push
(`OP1_PRESS_PAGE`) → page 2, where blue set the gain (word 5) and shift + blue moved it a tenth of a dB
(`knl::Fine`, rewriting the stored word after reading the shift byte at 0x01DD7F78). Now: fn + blue and shift + fn
+ blue, the core's fine (no firmware address, no word rewriting). The page-2 toggle is gone (§4: every layer is
held). No other page word, press or latch upstream, so nothing else to map.

## Stereo and cost

The upstream is stereo (kennel `op1_fx_stereo_t`, `render_stereo` at the master; the mono render at the instrument
positions). coop runs the stereo path at every position (synth, drum, master, both FX 2 slots): the instruments now
hear their own stereo pair folded per side (two channel states, shared controls), as the master did on kennel.
`plugin.json` carries no `"stereo"` flag: a `coop_fx_t` is stereo by ABI, and coop's loader reads kennel's flag as
the `op1_fx_stereo_t` layout and would refuse the file.

Not oversampled: FOLD is ADAA at 1x by design (te-op1-cfw `docs/fold-port.md` "Choice: ADAA at 1x"; 2x would have
doubled the cost for 8-20 dB less aliasing at the top folds only).

| Simulator cycles a 64-sample block (`make -C tests/host/wavefolder sim`, compute only) | avg | max |
|---|---|---|
| upstream `Fold::Process`, stereo (no range mapping, int16 buffers) | 4,807 | 4,980 |
| coop render, stereo | 5,716 | 6,962 |
| coop render, L == R input (the shortcut when the channel states are equal) | 5,286 | 6,957 |
| coop render, mix 0 (bus untouched; the kernels run into a sink to stay warm) | 4,830 | 6,679 |

- Upstream device cost: never measured on kennel (te-op1-cfw device test 58 judged by ear); upstream estimates the
  device at 1.5-2.5x the simulator (cold caches: the sine table 1 KB and the low part of the sinc table, 4.2 KB in
  all, are SDRAM `.rodata`). So **about 9-15 k cycles a block on the device [I]**, under the 30 k budget at every
  position; with the plugin at all five positions at once about 45-75 k in all [I].
- What stereo at every position means: on kennel the instruments ran the mono path (3,059 sim); now they run the
  stereo one (4,807 + coop's overhead). The overhead (about 900 sim) is the bus conversion (int32 <-> int16, in
  `src/bus_bfin.S`, about 6 cycles a stereo sample in and out after its rewrite from C, which took about 14) and the range
  mapping (divisions, only in blocks where a turn moved: the max column).
- Optimisation done: the conversions in assembly (-400 a block); the range / gain mapping cached (no division in a
  settled block); the L == R shortcut (mono kernels once, copied, bit-identical to the stereo run; taken only while
  the two channel states are equal, i.e. from init on a mono source). Next, if the device says so: fold the
  conversions into the kernels (the shape kernel reading the int32 high halves, the tone kernel writing int32).
- Memory: `.text` 3,064, `.rodata` 6,008 (the tables), `.bss` 936 (one `Fold` of 636 bytes, two 128-byte int16
  blocks, the parameter cache): load size 10,008 bytes a relocation, **50,040 bytes for the five**. L1: none
  (effects can't lease L1 yet, guidelines §2.3). Init: resets about 40 words, no tables built.
- Cost on the device: **pending** (`fx1_plugin=`, `s2_instr=`, `s2_master=` in `/yaffs2/user/coop_stats.txt`).

## Screen

No plugin screen: effects show the stock filter page (`docs/plugins.md` "Effect controls", Display), and the core
draws "GAIN nn" top left in blue while fn + blue moves it. The upstream SVG (`fold.svg`, the canvas "Fold screen"
design A "Wave, aligned") is not carried across. Brief for when effect screens exist (manifesto, guidelines §5): the
folded output cycle large across the width between two dim fold lines, blue choosing one of eight poses by fold; the
green offset line moving -24..+24 px about the centre; the white low-pass glyph at the right, its corner walking left
as tone darkens; the orange mix gauge filling from its foot; the gain as a blue bar about the centre at the far left,
shown with its value only while fn is held. No words at rest.

## Tests

- **Host test** (`tests/host/wavefolder`, `make TE=<te-op1-cfw>/ports all sim`): the plugin's `render` on the int32
  bus against te-op1-cfw's `Fold::Process` driven as `fold_knl.cc` drove it, over a seeded scenario (knob jumps
  with edges, LFO-rate walks of all five words, mix 0 stretches, noise, sines, silence, full scale, L == R stretches,
  re-inits): **bit-identical** natively (C kernels, 20,000 blocks) and in the GNU simulator (assembly, 6,000 blocks),
  and the simulator run equals the native one. The bus contract holds: blocks it wrote have the low 16 bits 0; mix-0
  blocks are untouched word for word. A broken shortcut (MirrorChannels emptied) fails the compare.
- `tools/asm_lint.py plugins/wavefolder/src/*.S`: 0 packets flagged.

## op1emu gate (ready to run, not run: gates go one at a time)

```sh
make -C core/sdram combined && make -C plugins
mkdir -p build/nand_wavefolder/plugins && cp build/plugins/wavefolder.coop build/nand_wavefolder/plugins/
python3 -I tools/make_nand.py --provision ~/op1emu-work/provision_nand.py --firmware ~/Downloads/op1_246.op1 \
    --mkyaffs2 ~/op1emu-work/yaffs2utils/mkyaffs2 --overlay build/nand_wavefolder --output ~/op1emu-work/nand_wavefolder.bin
```

wavefolder alone on the NAND, so it is the list's last row as test_fx was; the e6 scenarios work unchanged.
1. `tests/sim/det_fx_select.txt`: the synth position, a chord through it at the defaults (fold 30 %, tone 70 %, full
   wet). Expect `plugins seen 1 loaded 1 failed 0`, no fault, audible folding in the dump, the cost line's
   `fx1_plugin=` (emulator figure; the device is 10-35 % higher).
2. `tests/sim/det_fx_controls.txt`: every position (synth FX 1, instrument FX 2, master FX 1, master FX 2, drum FX 1)
   and a chord plus drum keys at the end. Expect `fxctl fx 1`, words 5 per position as test_fx's: p0 11197, p3
   19114, p2 15019, p4 16384, p1 17749; words 6 / 7 and the latch 0 everywhere; the green / orange clicks and fn +
   white `ignored` (undeclared); `master_saves 1`; the cost line's `fx1_plugin=`, `s2_instr=`, `s2_master=`.
3. `tests/sim/det_fx_reload.txt` (power cycle on the same NAND): the five words kept.
4. The pristine NAND with the same image: `det_chord.txt` unchanged.
