# Stereo instrument bus (Phase 2 milestone C, item 2)

Goal: true stereo synths — per-voice panning into a stereo bus — and the voice-spread control
(random per-voice pan scaled by an amount; see the `synth-voice-spread-feature` memory).

## The stock pipeline (op1emu-traced, 2026-11-01)

All in the L1 master/voice path. Confirmed by disassembly of the decrypted L1 and buffer probing.

1. **Per-voice render + mono accumulate** (around `0xffa008f2`..`0xffa0093e`). For each sounding voice:
   the stock code dispatches the voice's render through its vtable (`render = *(vtable+0x24)`,
   param `W[vtable+0x20]`), then a `LSETUP` loop (`0xffa00912`) multiplies the rendered samples by the
   voice gain and **sums them into the mono voice bus `0xff800bb4`** (128 x int16 @ 88.2 kHz). Per-voice
   gains come from `0xff801d50` (int16). So all voices are summed to **one mono bus**; per-voice
   position is lost here.
2. **Decimate + global pan-mix** (`0xffa00ef0`..`0xffa00f86`). A 64-iteration `LSETUP` reads the mono
   bus `0xff800bb4` (`W[I1++]`), multiplies by **one global left gain `R0.H` and one global right gain
   `R2.H`**, and accumulates into the master bus **d34 (`0xff800d34`, L) / db4 (`0xff800db4`, R)**,
   int16. This is the only "stereo" the stock firmware does for the synth: a single pan for the whole
   bus, applied while downmixing 88.2 -> 44.1 kHz into the master.

So stock "stereo" = mono sum + one global pan. True per-voice stereo must intercept **before** the
mono sum, at step 1.

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

## Status

- [C] Pipeline traced (above). [C] `coop_voice.h` DSP (pan law, spread, stereo mix) + host tests.
- [ ] C1 L1 takeover of the voice accumulate + pan-mix (the risky step; needs careful op1emu
  verification, like milestone B — confirm buffers carry signal before trusting the insert).
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
