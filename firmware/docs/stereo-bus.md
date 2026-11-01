# Stereo instrument bus (Phase 2 milestone C, item 2)

Goal: true stereo synths — per-voice panning into a stereo bus — and the voice-spread control
(random per-voice pan scaled by an amount; see the `synth-voice-spread-feature` memory).

## The stock pipeline (op1emu write-traced, 2026-11-01) [C]

Dynamically confirmed with the op1emu write tracer (`docs/emulator.md`, `tests/sim/watch_writes.sh`)
on the stock R246 image, tracing every store into the voice bus `0xff800bb4+0x100` and the master pair
`0xff800d34+0x100` through a synth chord and a drum hit, then reading the disassembly at the PCs that
fired. Per audio block, in execution order (the `--watch-log` sequence):

1. **Clear the master pair.** `LSETUP(0xffa06a48, 0xffa06a4a) LC0=32`: 32 int32 stores each to
   d34 (`0xff800d34`, L) and db4 (`0xff800db4`, R).
2. **Clear the voice bus.** `0xffa01d1e` (`LSETUP` x64 int32): `0xff800bb4..0xff800cb3`, 128 x int16.
3. **Per-voice render + mono accumulate.** Two different loops, one per engine:
   - **Synth** (3 renders/block under a 3-note chord): render via `CALL (P2)` at `0xffa00d28`
     (`P2 = *(vtable+0x14)`), then `LSETUP(0xffa00d4a, 0xffa00d62) LC0=127` plus an epilogue store:
     `bus[i] = sat(bus[i] + render[i] * ramp[i] * gain)`, ramp = the 128-entry int16 array `P5`
     (`[SP+0x38]`), gain = `W[P4]`; stores at **`0xffa00d5a`** (body, `W[I0++] = R1.H`) and
     **`0xffa00d70`** (sample 127). Then the voice meter `R0 = MAX(R0,R1)(V)` at `0xffa00d7c` into
     `[P0+0x40]`. 128 samples per voice [C]: the pre-decimation bus is 128 @ 88.2 kHz.
   - **Drums**: render via `CALL (P0)` at `0xffa00904` (`render = *(vtable+0x24)`, param
     `W[vtable+0x20]`), then `LSETUP(0xffa00926, 0xffa0093e) LC0=128`:
     `bus[i] = sat(bus[i] + sat(2 * render[i] * gain))`, gain = `W[P5]`; store at **`0xffa0093e`**.
     The drum engine's SDRAM sample path also stores single samples into the bus from
     `0x01a66b80` (`W[I0++] = R3.H`, one store per call). The synth loop never ran in drum mode and
     the drum loop never ran under the chord. (Earlier static reading placed the synth sum here; it is
     the drum sum.)
4. **In-place decimation, 128 -> 64 (SDRAM).** The function ending at `0x01967612` reads the 128
   samples through `I0` and writes the 64 decimated samples back over `0xff800bb4..0xff800c32`
   (stores `0x01967586`, `0x0196759a`, `0x019675d2`, `0x0196760e`; unrolled x2, `LSETUP` x31). A
   32x16 fractional IIR-style filter (`A1 = R0.L*R6.L (FU) ... (A0 += A1)`). Its nonzero store count
   equals the mix loop's exactly, so this is what the mix reads.
5. **Mix into the master pair**, two 64-iteration loops back to back in the master stage:
   - **Loop A** `LSETUP(0xffa00f30, 0xffa00f86)`: reads the 64 decimated samples, applies an L gain
     `R0.H` and an R gain `R2.H` (built from an SDRAM word near `0x01b4ef6e`) times `R6.L`, computes
     peak meters (`ABS`/`MAX`), accumulates into d34 (store `0xffa00f76`) / db4 (`0xffa00f7e`).
     **It wrote zero in every block of both scenarios**: its gain is zero in normal play. This is the
     loop the earlier static pass called "the global pan".
   - **Loop B** `LSETUP(0xffa00fb2, 0xffa00fda)`: reads the same 64 samples, multiplies by gain
     `R0.L` and a per-sample ramped gain `W[P1]` (stepped by `R2` each sample), and adds **the same
     value to L and to R**: d34 at **`0xffa00fca`** (`W[I2++] = R1.L`), db4 at **`0xffa00fd2`**
     (`W[I0++] = R0.H`). This is the active path; it carried the chord and the drum hit. No pan.
6. **SDRAM post-mix** `LSETUP(0x019de5ac, 0x019de5e8)`: adds one mono term (`R3.H`, from an
   `A0/A1` accumulator generator) to both channels in place (stores `0x019de5cc` R, `0x019de5d4` L).
   Its nonzero count equals loop B's, so the term was zero here: a silent source (metronome/click?) [I].
7. **A master-bus pass** writes L back at `0xffa033c0` then R at `0xffa03590` (64 stores each; fewer
   nonzero than loop B, so it zeroes small values). It still runs in the milestone-B image where
   `0xffa04280` was replaced, so it is NOT that stage's body; its identity is open [I].
8. **Coop master stage (`coop_master_apply`)** at the getter site `0xffa017ac`, then the **stock master
   level/comp `0xffa04280`**: reads d34/db4 and writes the int32 output pair **e34/f34**
   (`0xff800e34`/`0xff800f34`, stores `0xffa044e0`/`0xffa046f0`), which the SPORT TX conversion loop
   (`0xffa0171c..0xffa0178e`) turns into the codec DMA ring `0xff800770`. See `docs/audio-graph.md`
   (second correction): replacing `0xffa04280` silences the codec.

So the stock synth and drum output reaching the master is **mono: identical L and R** (loop B), and the
only pan machinery (loop A) is silent. True per-voice stereo must intercept at step 3 (the per-voice
accumulate) and carry two channels through steps 4-5; the stereo result then flows through d34/db4 and
the stock level stage into e34/f34 unchanged in shape (both are already stereo pairs).

## coop design

Replace the per-voice **mono** accumulate with a per-voice **stereo** accumulate, and decimate the
stereo bus to the master:

```
for each sounding voice:
    render the voice (stock vtable render, unchanged)
    panL, panR = equal-power pan gains for this voice's pan (pan = random(voice) * spread, set at note-on)
    busL[i] += voice[i] * gain * panL        (coop stereo bus, int32 @ 88.2 kHz)
    busR[i] += voice[i] * gain * panR
decimate busL/busR 88.2 -> 44.1 into the master pair d34/db4 (replacing the global pan-mix)
```

- **Per-voice pan + spread** (`coop_voice.h`): each voice gets a pan in [-1,1] assigned at note-on
  from a deterministic RNG, scaled by the spread amount (0 = center, 1 = hard L..R). Equal-power pan
  law (constant perceived loudness across the field). Fixed-point, no float.
- **Stereo bus**: two int32 accumulators of 128 samples (88.2 kHz) in L1 data B / region A.
- **Centre-pan == mono**: with spread 0 (all voices centred), the stereo bus L and R are equal and the
  result must match stock mono within rounding — the regression check for the takeover.

## Staged plan

- **C1 — infrastructure + centre-pan takeover (no audible change).** Build the stereo bus and the
  per-voice panned accumulate, take over the mono-accumulate + pan-mix with all pans centred, and
  verify in op1emu that the output matches stock (by ear and by probing d34/db4). Proves the takeover
  is correct before anything moves.
- **C2 — per-voice spread.** Add the spread control and per-voice random pan at note-on; verify voices
  spread across the field (probe/measure L!=R, spread 0 collapses to centre).
- **C3 — stereo-by-design engines/FX.** Later: engines that render true stereo, stereo insert FX.

## C1 takeover mechanism (to decide; options, 2026-11-01) [I]

The per-voice accumulate is inline loop code, not a `CALL` to a function, so `l1_insert.py`'s
CALL retarget does not apply directly. Options, in order of preference:

1. **Hook the render and redirect the stock sum.** Patch the bus immediates in the stock accumulate
   (`I1.L/I1.H = 0xff800bb4` at `0xffa00d2e`/`0xffa00d32` for the synth, `P1.L/P1.H` at
   `0xffa00912`/`0xffa00916` for drums) to a scratch buffer, so the stock loop still runs (meters,
   ramp/gain consumption unchanged) but into a bin. Wrap each engine's render through its vtable slot
   (`+0x14` synth, `+0x24` drums; the instance table is SDRAM, plaintext) so the wrapper runs the stock
   render and then does the stereo accumulate from the render buffer with the same ramp/gain math.
   Then take over step 5 by retargeting the mix: the decimator (SDRAM, plaintext, patchable freely)
   runs twice (L and R) and loop B's output goes per channel. Smallest L1 footprint: 4 imm16 patches.
2. **Overwrite the loop region** `0xffa00d2a..0xffa00d70` (0x46 bytes) with a `CALL` to a trampoline
   plus padding, passing the render buffer, ramp and gain in the registers the loop uses. More L1
   bytes changed; register/stack contract must be exact.

Either way the regression gate is: centered pan, L == R == stock loop B output, verified with the
write tracer (`nonzero` and values at `0xffa00fca`/`0xffa00fd2` vs the coop writer) and a rendered WAV
A/B against the stock image.

## Status

- [C] Pipeline traced dynamically (above; write tracer, chord + drum scenarios). [C] `coop_voice.h`
  DSP (pan law, spread, stereo mix) + host tests.
- [ ] C1 L1 takeover of the voice accumulate + mix (the risky step; mechanism above; verify in op1emu
  with the write tracer and a rendered WAV A/B, like milestone B).
- [ ] C2 spread control + UI. [ ] C3 stereo engines/FX.

## Drums are pannable too (owner decision, 2026-11-01)

Per-voice panning applies to the **drum kit** as well: each drum voice (up to 12) gets its own pan +
spread, the same model as synth voices. `coop_voice.h` already sizes `COOP_MAX_VOICES = 12` for this.
The drum voice path (drum FX position 1, drum voice table `0xff801754`) mirrors the synth takeover:
intercept the per-drum-voice accumulate, pan each drum voice into the stereo bus. Scheduled after the
synth stereo bus is proven (C1/C2 on synth first, then the same on drums).

## Stereo through the FX chain

The whole insert/master chain is stereo in / stereo out, with insert FX placed after the per-voice pan,
so per-voice placement and spread survive the effects. Mono effects are hosted dual-mono. See
docs/fx-abi.md for the full contract and the 3rd-party porting rules.
