# Stereo bus v2: a native stereo instrument path (decision, 2026-11-03)

## Why

The difference-bus design (docs/stereo-bus.md) keeps the stock mono path untouched and carries the per-voice
placement beside it as `dL, dR`, added back at the mix. That gave bit-exactness with stock at spread 0 and a small
cost. It breaks the moment a stock effect sits on the mono bus: **the effect sees the mono sum only, the stereo
difference bypasses it and is added back unprocessed.** The owner heard exactly that on the device with P2b: a filter
on the synth lets the panned part of the signal through unfiltered. No plugin-side fix exists for a stock mono
insert, and every further stage on the instrument path would have to be special-cased the same way (two FX per
instrument, sends, the tape monitor). The instrument stage was becoming the convoluted part of the diagram.

The owner's recommendation, adopted: **the instrument path is stereo from the pan hook on.** Voices render as in
stock (mono sources); the coop voice hook places each voice into a stereo pair per instrument; every FX slot is
stereo in / stereo out; the stock mono buses are no longer used (left running until patched out). The stock
*master* effects are already stereo (dual-mono wrappers of the same cores the instrument positions use), so the
same objects serve every position: one FX implementation per effect for synth, drum and master, with a chain of
two per instrument and two on the master as the goal.

## The path

```
voices (stock, mono, 88.2 kHz)
  -> coop voice hook: L += v·gL, R += v·gR         (per instrument: synth pair, drum pair)
  -> coop decimator pair + DC block (88.2 -> 44.1 kHz)       per instrument
  -> FX slot 1 -> FX slot 2 (stereo objects: stock master-type instances or coop plugins)
  -> coop mix (gain, ramp) into the master pair d34/db4;  tape record tap takes L/R
  -> master FX 1 -> master FX 2 (stereo)  -> coop master stand-in -> stock level stage -> output
```

The stock mono sum, decimator, DC block and mono FX dispatch keep executing on their own bus until they are
patched out; coop's mix reads none of it. Spread 0 therefore stops being stock bit for bit; the gate becomes the
coop reference (the v1 image's full-spread output, which v2 must reproduce within rounding where no FX runs).

## Costs (device, per block of 580,499 cycles)

- Voice hook: two MACs per sample for every voice (the difference design skipped centred voices): worst 18 voices
  ~1.2k each = ~22k, against ~15k today.
- Decimation and DC: 2 channels per instrument instead of 1 stock + 2 difference: no change until the stock mono
  stages are patched out, then a saving.
- The stock mono stages run for nothing until patched out: ~10% of the block (their cost today). Patching them
  out (the decimator call, the DC block, the mono FX dispatch, in L1) is the second step.

## Steps

1. **RE** (in progress): the stock master FX objects per type (vtable, ctor, size, render/init entries), the master
   FX factory, the mono vtables' render entries, the position dispatch tables.
2. **S1**: per-instrument L/R buses in `stereo_bus.cc` (replacing the pairs of P2b), the mix kernels take L/R
   directly, the record tap takes L/R; the mono FX dispatch of all stock types made a no-op that records the
   selected type per position; coop instantiates the stock stereo object of that type for the position and runs
   it on the L/R buses (dual-mono inside, as the master does); coop plugins run on the same buses. Gate: v1 full
   image vs v2 with no FX selected, within rounding; a stock filter at the synth position filters everything.
3. **S2**: the second FX slot per position and on the master; the saved-sound JSON member for it.
4. **S3**: patch out the stock mono stages for the CPU.
