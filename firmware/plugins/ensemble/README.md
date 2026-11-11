# ensemble

A stereo chorus that morphs into a string ensemble: te-op1-cfw's kennel effect (`ports/knl/ensemble`, version 0.2, by
coldsoldr), its fixed-point core after Mutable Instruments Rings' and Plaits' ensemble, with Clouds' half-rate path
(Emilie Gillet, MIT). Per side, one to three copies of the input read from a delay line swept by a slow LFO (the
rate, 0.05..8 Hz) and, with the third voice, a 6.57 Hz vibrato; the right side's sweeps are the left's inverted, and
each side's third copy reads the other side's input. The core runs at 22.05 kHz (2:1 halfband decimation in, the wet
interpolated back and mixed with the full-rate dry through an equal-power crossfade and a soft limit). Design and
upstream measurements: te-op1-cfw `docs/ensemble-port.md`.

## Controls (guidelines §3.2)

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | rate | depth | voices | mix |
| Click | — | — | — | — |
| Shift + turn | rate fine | depth fine | voices fine | mix fine |
| Shift + click | — | — | — | — |
| fn + turn | gain (input, -6..+6 dB) | — | — | — |
| fn + click | — | — | — | — |
| Shift + fn + turn | gain fine | — | — | — |
| Shift + fn + click | — | — | — | — |

- **The four turns** are kennel's four knobs (`params[0..3]`), unchanged: blue rate (0.05..8 Hz, log), green depth
  (the sweep's reach, up to 3.6 ms, less at fast rates so it stays a chorus), white voices (one copy a side, the
  chorus, through two to three, the ensemble with the vibrato), orange mix (dry to wet). The core hands them in the
  filter type's range units ({64..14528}, {0..24576}, {0..22528}, {8192..32767}, `sdk/coop_plugin.h`); the plugin
  normalises them to 0..32767 exactly as kennel's `op1_fx_param_q15` did, with the ranges as constants (no range
  table read), and only in a block where a word moved (the divisions stay out of a steady block). Shift + turn is the
  stock fine adjustment.
- **gain** (`fn` + blue; the owner's layout, 2026-11-11): kennel's blue page-2 word 5 (`knl::GainQ14`): -6 dB at 0,
  0 dB at 16384 (the default), +6 dB at the top, one table entry a dB, interpolated. It scales the wet path's input
  only (the decimated signal before the core), so at mix 0 it changes nothing. Saved with the sound (word 5), not
  randomised (`"random": false`: a random draw would be a level jump). **Blue's shift + fn layer is gain fine**
  (`"fine": true`, the owner's decision, `docs/ui-features.md` §14): 1/16 of the coarse step (328 Q15 a detent
  coarse, 20.5 fine), so nothing else sits there.
- No latch, no steps control: the upstream has none, so nothing clears on the stock's synth / drum switch.

```json
"controls": [
  {"id": "gain", "name": "gain", "action": "fn_turn", "encoder": "blue", "word": 5, "kind": "continuous",
   "default": 16384, "fine": true, "random": false}
]
```

**Upstream mapping.** Kennel ensemble: the four turns as here; a blue press flipped blue to page 2, where blue turned
the input gain (word 5, about 1536 a click) and shift + blue gave a tenth of a dB a click through `knl::Fine` (which
read the stock shift byte at a firmware address, 0x01DD7F78, and rewrote the stored word). coop retires the page-2
toggle (guidelines §4: every layer is held): the gain is fn + blue, and its fine step is the core's (shift + fn +
blue), so `knl::Fine` and the shift-byte read are gone. Nothing else moved; mix was already on orange (§4.3). A sound
saved by kennel keeps its gain: the word is the same Q15 value.

**Kennel's `"stereo": true` is not carried over.** coop's loader reads that flag as kennel's `op1_fx_stereo_t`
layout and would refuse a `coop_fx_t`; a coop effect is stereo by its ABI (`render(slot, l, r, params, n)`).

## Stereo (guidelines §2.2)

The upstream is true stereo (each side's third copy reads the other side's input; the right side's sweeps are the
left's inverted), and it is ported as such: the stereo path runs at every position, synth, drum and master FX 1 and
both FX 2 slots. On kennel the synth and drum positions ran the mono render (the left side only, one line); on coop
they run the full stereo path, so the instrument positions now hear the ensemble wide (a mono input comes out wide:
L/R correlation 0.31 for chorus at depth .5 on a saw, upstream figure) and the per-voice placement coop gives the
instrument bus goes through it unchanged (L and R stay separate, never summed).

One addition, exact: when a block's L and R are equal and the two decimators hold the same history (a mono
instrument), the right decimator's output and history are copied from the left instead of computed. Everything else
must run per side (the two sides' sweeps differ), so this saves one decimator a block (~1 k cycles [I]).

## Screen

The stock filter page shows (effect screens aren't built: `docs/plugins.md` "Effect controls", Display); the core
draws `GAIN` and its value top left in blue while fn + blue moves it. The upstream's own screen (`ensemble.svg`,
"Drift": each voice a string swaying about its home mark by depth, the R lane opposite, a dot circling the dial at
the rate, the gain gauge and readout on page 2, the mix gauge) is not carried across. Brief for when effect screens
exist (guidelines §5): keep the Drift drawing; voices, homes and the rate dot in their encoders' colours (blue rate
dot and bar, green depth ticks, white voice strings, orange mix gauge); the R lane always shown (coop is stereo at
every position); the gain gauge dim at rest and lit in blue while fn is held (§5.2: no words at rest, the held layer
lists its name and value). The SVG needs checking against the manifesto (stroke widths, no glow) before it ships.

## Memory, L1, cost

- File 10,820 bytes (0.3); `.text` 5,236, `.rodata` 3,764 (the 1025-point sine, the rate and cap curves, the
  soft-limit and crossfade tables), `.data` 0, `.bss` 10,380 (two int16 delay lines of 2,288 words, the half-rate
  filters and buffers, the control state). Load size 19,380 bytes (image 9,000 + `.bss`) x 5 relocations = 96,900
  bytes of heap 0.
- L1: none (effects have no lease yet). The lines are in `.bss` (SDRAM); a block touches about 15 cache lines of each
  (upstream estimate) plus the tables.
- init does no work beyond a flag: the first render after it re-initialises the filters and clears the lines (2 x
  4,576 bytes, a word at a time) and passes that block through dry, as kennel's restart did; so does the first block
  after a pause of over 5 ms (the CYCLES register, as rings reads it; no firmware address).
- Cost (cycles a 64-sample block; budget under 30 k, 580,499 for everything):

  | | mean | max |
  |---|---|---|
  | upstream, bfin-elf-run: core alone, stereo, 3 voices | 3,759 | 3,879 |
  | upstream, half-rate path (decimate, interpolate, mix, stereo) | ~7,700 [I, from the plate's 5.3 MHz] | |
  | **coop render, bfin-elf-run** (`make -C tests/host/ensemble sim`: all scenarios, stereo every block) | **12,801** | **14,823** |
  | op1emu (`fx1_plugin=`) | pending (gate) | |
  | device | pending | |

  The simulator has no cache or SDRAM model; the device typically runs 10-35 % higher, so ~14-20 k expected [I].
  Stereo at every position doubles what kennel's instrument positions cost (mono there: ~1.5 MHz core + the mono
  half-rate path, about 6 k a block [I]); at the master it is what kennel ran.

## Sources (copied, stripped of kennel-only code)

| coop | te-op1-cfw | changes |
|---|---|---|
| `ensemble.cc` | `ports/knl/ensemble/ensemble_knl.cc`, `ports/knl/common/knl_fx.h` | the coop ABI; ranges as constants; no `knl::Fine`, no scratch buffer, no screen |
| `src/ensemble_core.cc`, `src/ensemble.h`, `src/ensemble_layout.h` | `ports/fx/src/ensemble/` | namespace `coop_ensemble`; tables from `src/ensemble_tables.cc`; C side kernel host-only |
| `src/ensemble_bfin.S` | `ports/fx/src/ensemble/ensemble_bfin.S` | symbols `EnsSide3Asm` / `EnsSide1Asm` |
| `src/halfband.h`, `src/halfband.cc`, `src/halfband_bfin.S` | `ports/clouds/src/core/halfband.*` | cut to the ensemble's use; symbols prefixed `Ens`; C host-only |
| `src/half_rate.h` | `ports/fx/op1/half_rate.h` | stereo only; const soft-limit table; the shared decimator pass |
| `src/ensemble_tables.cc` (generated) | constexpr double tables in `ensemble.cc`, Rings' `kSoftLimitQ30`, Clouds' crossfades | `gen_tables.py`: integers, no float |
| `op1/support.cc` | `ports/rings/op1/support.cc` (as `plugins/rings`) | none |

Forbidden constructs found and fixed (guidelines §2.1): the upstream's `constexpr double` table maths (14 float lines)
→ integer tables from `gen_tables.py`, checked value for value; the shift byte (0x01DD7F78, `knl::Fine`) and the
range table (0x01DE5C50, `op1_fx_param_q15`) → the core's fine step and constant ranges; `op1_fx_scratch` /
`op1_fx_knobs` / `op1_fx_page` / `op1_fx_screen_pos` / `op1_fx_knobs_live` → `.bss` and the params block; the soft-limit
table built at the first call → const. No libc call (support.cc is the runtime GCC may call for struct copies), no
static constructor, no division per sample (the knob and gain divisions run only in a block where a word moved).
`tools/asm_lint.py`: 0 packets flagged on both `.S` (they ran on the device in te-op1-cfw's kennel builds).

## Tests

- `make -C tests/host/ensemble` (host, C kernels) and `make -C tests/host/ensemble sim` (bfin-elf-run, coop's
  assembly): the plugin's `render` against te-op1-cfw's kennel glue on its own DSP (C kernels), block for block, the
  state carried: 40,000 host / 4,000 simulator blocks of stereo noise, L == R stretches, a stereo saw pair and
  full-scale edges, the knobs and gain jumping (out-of-range words too) and sweeping, re-inits, and (host) pauses over
  5 ms. **Bit-identical** on both. Also every value of the rate, cap and gain curves, the vibrato increment and the
  soft-limit and crossfade tables. A one-sample fault planted in the shared decimator pass is caught at once.
  From a worktree, pass `TE=/mnt/i/GitHub/coldsoldr/te-op1-cfw/ports`.

## op1emu gate plan (not run yet: gate runs go one at a time)

With `build/plugins/ensemble.coop` **alone** on the NAND overlay (`PLUGINS=build/plugins/ensemble.coop`; it is then
the last row of the FX list, where the scenarios' `turn enc1 cw 96` lands):

1. `tests/sim/det_fx_select.txt`: synth FX 1, a chord through it. Expect no fault, `plugins seen 1 loaded 1 failed
   0`, the plugin's blocks counting, `fx1_plugin=` in the cost line (compare with 12.8 k).
2. `tests/sim/det_fx_ensemble.txt` (new): synth FX 1, all four turns moved, white to the top (3-voice kernel) and to
   the bottom (1-voice chorus kernel), fn + blue 3 detents up: `fxctl` `p0=...:17368/0/0/0`; a held chord each way.
   The WAV: wide, no clicks at the kernel switch.
3. `tests/sim/det_fx_controls.txt`: the plugin at every position (p0 synth, p3 instrument FX 2, p2 master, p4 master
   FX 2, p1 drum) with a chord and drum keys. Expected `fxctl` words 5/6/7/latch: p0 15092/0/0/0, p3 17040/0/0/0,
   p2 16056/0/0/0, p4 16384/0/0/0, p1 16712/0/0/0 (the scenario's green and orange clicks, fn + white and shift + fn
   + white are undeclared here: counted as `ignored`, `latches 0`, words 6 and 7 stay 0) [I].
4. `tests/sim/det_fx_reload.txt` after 3 (power cycle, same NAND): the gain words kept at every position.
5. A pristine-NAND `tests/sim/det_chord.txt` against the same image: unchanged.

Read the stats `cost` line (`fx1_plugin=`, `s2_instr=`, `s2_master=`) and say it is an emulator figure.

## Licence

`ensemble.cc`, `gen_tables.py`: GPL-3.0-or-later with the OP-1 firmware permission (`../../LICENSE`, `../../NOTICE`).
The DSP in `src/` follows Mutable Instruments' Rings, Plaits and Clouds (MIT, `LICENSE-MIT`); the plugin file is
conveyed under the GPL, which the MIT terms allow.
