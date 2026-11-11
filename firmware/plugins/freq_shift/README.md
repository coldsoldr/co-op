# freq_shift

A frequency shifter with barber-pole feedback and stereo spread, and a ring modulator, after Mutable Instruments
Warps (Emilie Gillet, MIT), as a coop effect. The DSP is te-op1-cfw's fixed-point port (`ports/fx/src/shift`, commit
5aa314b: a Bode shifter on Niemitalo's polyphase IIR Hilbert pair as Warps' `quadrature_transform.h`, 44.1 kHz,
two Blackfin kernels), the build hundekraft's kennel plugin `freq_shift` 0.2 used (`ports/knl/freq_shift`). Licence:
the DSP in `src/` is MIT (`LICENSE-MIT`); `freq_shift.cc` is coop's (GPL-3.0-or-later with the OP-1 firmware
permission) and the plugin file is conveyed under the GPL.

## Controls (guidelines §3.2)

| | Blue | Green | White | Orange |
|---|---|---|---|---|
| Turn | shift (-2 kHz .. 0 .. +2 kHz) | feedback | spread | mix |
| Click | — | mode: shift / ring (steps, saved, wraps) | — | — |
| Shift + turn | shift fine (stock) | feedback fine (stock) | spread fine (stock) | mix fine (stock) |
| Shift + click | — | — | — | — |
| fn + turn | gain (-6 .. 0 .. +6 dB on the wet path, 0.5 dB a detent) | — | — | — |
| fn + click | — | — | — | — |
| Shift + fn + turn | gain fine (0.1 dB a detent) | — | — | — |
| Shift + fn + click | — | — | — | — |

- **Blue's shift + fn layer is the gain's fine** (the owner's decision, `docs/ui-features.md` §14, `"fine": true`):
  nothing else may sit there. Kennel's steps (the owner, 2026-11-11): `"step": 1365` (0.5 dB a detent),
  `"fine_step": 273` (0.1 dB).
- The four turns are kennel's four knobs (`params[0..3]`, in the filter type's range units; `freq_shift.cc`
  normalises them to 0..32767 as kennel's `op1_fx_param_q15` did, from the ranges `sdk/coop_plugin.h` names).
  Shift: `2000 Hz x (2 (knob - 0.5))^3` (fine near 0; 16384 = 0 Hz). Feedback: loop gain 0..0.93. Spread: L at shift
  + spread / 3 of the knob range, R at shift - spread / 3 (full spread at shift 0: L +74 Hz, R -74 Hz). Mix: linear
  dry / wet; mix 0 returns the input bit for bit.
- Ring mode: Q crossfades to 0 over 256 samples, so the wet is I x cos (both sidebands at -6 dB, no carrier) at
  |shift ± spread|; the feedback works there too.
- Gain: word 5, Q15, default 16384 (0 dB), `"random": false` (a random draw would be a level jump). It scales the
  analytic signal after the Hilbert (the wet path); the dry side of the mix doesn't see it. -6 dB at 0, +6 dB at the
  top, one table entry a dB (kennel's `knl::GainQ14`). The core shows `GAIN` and its value as it does any continuous
  control [I: not in dB; kennel's screen read the word as dB in tenths, (word x 120 + 16383) / 32767 - 60].
- Mode: word 6, step 0 `shift`, 1 `ring`, default `shift`, randomised (the default). The core shows `MODE SHIFT` /
  `MODE RING`.

```json
"controls": [
  {"id": "gain", "name": "gain", "action": "fn_turn", "encoder": "blue", "word": 5, "kind": "continuous",
   "default": 16384, "fine": true, "step": 1365, "fine_step": 273, "random": false},
  {"id": "mode", "name": "mode", "action": "click", "encoder": "green", "word": 6, "kind": "steps",
   "steps": ["shift", "ring"], "default": 0, "wrap": true}
]
```

**Upstream mapping.** Kennel `freq_shift` 0.2 (`shift_knl.cc`): the four turns as here; a blue press flipped blue to
page 2, where it turned the input gain (word 5, `knl::GainQ14`; shift + blue a tenth of a dB a click through
`knl::Fine`, which rewrote the stored word from the firmware's shift byte); a green press flipped green to page 2,
where one click flipped the mode (word 6, `knl::ClickMode(word, 2)`, 1536 a click). coop retires the page-2 toggle
(guidelines §4, every layer is held): the gain is on fn + blue with its fine on shift + fn + blue, the mode on the
green click as a step index. Word 7 and the white and orange presses were unused upstream and stay empty. A sound
saved by kennel holds the mode as a click count (0..32767): it reads as `ring` only when the word is 1, so such a
sound loads in shift mode [I: kennel sounds don't load in coop as they are anyway].

No `"stereo"` in `plugin.json`: kennel's flag means its `op1_fx_stereo_t` layout, which coop's loader checks at
+0 / +4 / +12 of the entry object (a `coop_fx_t` has magic and api there, so the file would be refused). A coop
effect is stereo by its ABI.

## Stereo

Every position runs the stereo path (guidelines §2.2). Upstream's stereo path (the kennel master) summed L and R into
**one** Hilbert pair and shifted that two ways (L at shift + spread, R at shift - spread, each with its own
barber-pole loop and its own dry), to stay under its 5 MHz target. That is the "sum to mono and spread again" the
guidelines forbid: at the instrument positions coop's bus carries per-voice placement, which the wet would lose. So
the port gives **each channel its own Hilbert pair** (x = that channel's input), keeping upstream's shifters, loops
and mixes per channel (approved by the owner, 2026-11-11: no L + R sum):
- L is upstream's mono call on L at shift + spread, and R upstream's mono call on R at shift - spread, bit for bit
  (the host test).
- **L == R** (a mono source, or a centred voice at an instrument position): the block runs one pair and both shifters
  read its I/Q, once the right pair is known to equal the left one (from init, or after 16 blocks in a row in which
  the two pairs gave the same I/Q from L == R input; the right pair then takes the left one's state when it next
  runs). Bit-identical to upstream's stereo path in that case (the host test), so a mono source sounds exactly as on
  the kennel master. The 32-bit states never met exactly in the host scenario (the allpasses' rounding keeps a small
  remainder), so the 16-block rule is the one that fires; its output stayed bit-identical to two independent pairs
  there (2 syncs) [I: not a proof for every input].
- Spread 0 still runs two shifters (their states could be shared only after the same kind of check on 2 KB of loop
  lines; not worth it at this cost).
- The bus is Q31 (the int16 bus widened); the DSP sees the high halves. A channel the DSP doesn't write (mix 0 at both
  ends of the block) is left exactly as it came, low bits included.

## Screen

Not carried. Kennel's screen (`shift.svg`, the canvas "Freq shift screen" design B "Dial": needles at shift ± spread,
the spread arc, input ripples, feedback arcs, a gain gauge with a dB readout, a mix bar, and a bracket round the mode
marks while green was on page 2) needs an effect screen hook coop doesn't have yet (`docs/plugins.md` "Effect
controls", Display). The stock filter page shows; the core's popup shows `GAIN` and `MODE` top left in blue and
green for about a second.

Brief for when effect screens exist (guidelines §5): the Dial, one needle per channel in blue (they part with the
white spread, one needle when the spread is 0), the feedback arcs in green, the spread arc in white, the mix bar in
orange. The mode marks stay (green, the lit one the current step); drop the page-2 bracket (no page 2 now: the popup
names the mode on the click). Gain becomes a held-layer readout: while fn is held, the gain gauge and its dB value
in blue, the drawing dimmed to the body tone; nothing at rest. Ripples from the input level need a level the screen
reads from the core, not from the render (§5.3).

## Memory, L1, cost
- **e8 (2026-11-11):** the Hilbert pairs, shifters, block buffers and param cache are in the slot's position buffer (plugin.json `"scratch": 4640`, `coop_fx_scratch`), as kennel's `op1_fx_scratch` had them; the file is relocated once and its `.bss` is the five buffer pointers (20 bytes); init resets the state there at every selection. The figures below are from before e8 (state in `.bss`, five relocations). File 10,152 bytes, load 9,664 once.

- File 10,300 bytes; `.text` 5,124, `.rodata` 4,428 (the 4 KB sin / cos table and its steps, the Hilbert
  coefficients), `.data` 0, `.bss` 4,616 (two Hilbert pairs, two shifters with their 256-sample complex loop lines,
  the kernels' scratch, two int16 block buffers). Load size 14,168 bytes; five relocations: 70,840 bytes of heap 0.
- L1: none (effects have no lease yet, guidelines §2.3). The hot data is the 4.6 KB of `.bss` (each loop line is
  streamed linearly) and the 4 KB table, read near-sequentially at the shift rate.
- `init` clears 4.6 KB word by word (about 1,200 stores) on the audio thread: inside the "few thousand cycles".
- Per block outside the sample loops: the four turns' and the gain's mapping (a `__divsi3` each, only when the word
  changed), the phase steps (only when a position changed), the ramps' steps.
- Cost, cycles a block (the budget is 580,499; an effect SHOULD stay under 30 k, over 60 k doesn't ship):

  | | L == R (one pair) | L != R (two pairs) |
  |---|---|---|
  | upstream, GNU simulator (`docs/shift-port.md`): stereo 6,585 steady / 7,187 peak; mono 4,954 / 5,550 | | |
  | this port, GNU simulator (`make -C tests/host/freq_shift sim`), avg / max | 9,762 / 11,013 | 14,383 / 15,868 |
  | (the C kernels, for scale) | 96,748 / 98,655 | 163,948 / 166,313 |
  | op1emu (stats `fx1_plugin=`) | pending (gate) | pending (gate) |
  | device (stats `fx1_plugin=`, `s2_instr=`, `s2_master=`) | pending | pending |

  The simulator counts instructions (no cache misses, no SDRAM waits); op1emu likewise, and the device is typically
  10-35 % higher for code with memory traffic (upstream expected some of Clouds' 2-4x SDRAM factor with the object
  outside L1) [I]. Against upstream's stereo call (one pair, 6,585) the one-pair block pays ~3.2 k for the glue:
  the Q31 <-> int16 conversion and the L == R compare (one pass in, one out a channel), and the gain loop in C
  (upstream's too, but its figure was at 0 dB, where the loop is skipped). The second pair adds ~4.6 k. If the device
  figure needs it: the gain folded into the Hilbert kernel's int16 conversion, and the conversions in assembly
  (dual 16-bit halves), perhaps -2 k. Stereo at every position means this cost at each position the plugin is selected in, up to five
  at once (about five times the figure, still well inside the block).

## Host test

`make -C tests/host/freq_shift` (WSL; `TE=` the te-op1-cfw `ports` folder when the checkout isn't the main one):
12,000 blocks of a seeded scenario (knob walks and jumps, gain moves, mode switches, mix 0 stretches, the top
feedback, L == R, L != R, L == R again, silence), through the plugin's `render` with the params as the core hands
them, against te-op1-cfw's DSP and kennel's mappings:
- against two upstream mono instances (L at shift + spread, R at shift - spread): **bit-identical**;
- with L == R throughout, against upstream's stereo path (the kennel master): **bit-identical**;
- mix 0, any other settings, random low bits: both channels back bit for bit.

`make -C tests/host/freq_shift sim`: the same scenario in the GNU simulator through the assembly (the plugin's path)
and through the C kernels, each compared with the host's output; and the cycles of every render.

## Gate plan (op1emu, step 9: not run in this port; one gate at a time)

1. Build: `make -C plugins` (`build/plugins/freq_shift.coop`), the e6 core (`make -C core/sdram combined` or the e6
   image already built).
2. NAND: `freq_shift.coop` alone in `build/nand_overlay/plugins/`, `tools/make_nand.py` as in `docs/plugins.md`
   "Building and testing" (plugin NAND), and a pristine NAND.
3. `tests/sim/det_fx_select.txt` on the plugin NAND: the stock FX list is alphabetical with the plugins sorted
   in, so "freq shift" is row 2 (`ccw 96` then `cw 16`; run 2026-11-11, results in `docs/device-tests.md`); the chord plays through it at the synth position. Expect `plugins seen 1 loaded 1 failed 0`,
   `plugin_blocks` > 0, no fault; the cost line's `fx1_plugin=`.
4. `tests/sim/det_fx_controls.txt` on the plugin NAND (written for test_fx, whose controls sit where freq_shift's
   do on blue and green): it selects the effect at p0 synth, p3 instrument FX 2, p2 master, p4 master FX 2, p1 drum and
   plays a chord and drum keys. Read `fxctl`: p0's w5 = 11197 (fn + blue 4 down, 1 fine up), w6 = 1 (green click:
   ring); fn + white and the orange click land on undeclared slots here (`ignored` counts them; test_fx's depth and
   mute are not freq_shift's). Expected `pN=...:w5/w6/0/0`: p0 11197/drawn/0/0, p3 19114/drawn/0/0, p2 15019/0/0/0,
   p4 16384/0/0/0, p1 17749/1/0/0 (the scenario randomises p0 and p3 after their clicks: the mode is drawn there, the
   gain kept) [I: the counts of `ignored` and `latches` differ from test_fx's run].
5. `tests/sim/det_fx_reload.txt` on the NAND step 4 leaves: the words kept at every position (mode and gain saved).
6. `tests/sim/det_fx_freq_shift.txt` (this port's): the plugin at the synth position, a chord held while the shift
   sweeps through 0 Hz, feedback up, spread up, mix to the top, the green click to ring and back, fn + blue gain with
   its fine, then mix 0; no fault, `fxctl` `p0=...:8740/0/0/0` (16384 - 6 x 1365 + 2 x 273), the chord audible in the dump (`tools/wav_levels.py`:
   level and width move with the turns, dry after the mix goes to 0), the cost line's `fx1_plugin=`. Run it at the
   master too (mixer T3 instead of synth T3) for `p2` and the stereo spread on the master bus.
7. Pristine NAND `det_chord.txt`: unchanged against e6.

## Device test

Pending: `docs/device-tests.md` "freq_shift".
