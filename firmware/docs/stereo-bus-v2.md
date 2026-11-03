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

## What the stock structure allows (measured 2026-11-03)

Per block the stock calls mix loop A and then mix loop B once each (117,222 hook calls for 58,611 blocks, the site
marker alternating 0xffa00f30 / 0xffa00fb2), both on the same summed mono bus. So the stock order is: per
instrument, voices -> sum -> decimate -> that instrument's FX -> added into one mono bus -> DC -> loops A and B into
the master pair. The instrument FX render (the hijacked vtable entry of P2) is therefore the one place where an
instrument's signal exists on its own, after decimation and before the sum, and P2b already hands it that
instrument's side pair there. The v2 path is realised at that point:

- the pair is formed before any effect: `L = mono + dL`, `R = mono + dR` (the mono bus is the dry mid, the side
  pair the per-voice placement), and every effect, stock or plugin, runs on the pair;
- the effect's output goes back as the new mono and the new side, so the stock sum, DC block and mix loops carry
  the processed stereo signal unchanged in form;
- a stock effect selected at an instrument position runs as the stock *stereo* object of that type (the master's
  dual-mono implementation), instantiated by coop for that position; the stock mono render of every type becomes
  a no-op, so the mono bus is never processed on its own again.

Keeping the carrier as mono + difference instead of two plain buses is an implementation choice: it is the same
stereo signal, it keeps spread 0 bit for bit with stock when no effect runs, and it changes no stock code on the
carrier. What the owner asked for holds: effects see the whole stereo signal, nothing bypasses them.

## S1 as built (2026-11-03)

- Every stock mono FX vtable's render (+0x14) and init (+0x1c) entry points at `coop_fx_render_mono` /
  `coop_fx_init_mono` (`tools/l1_stereo_insert.py`, `FX_MONO_CLASSES`), so no stock mono effect runs on the mono
  bus any more. [C in bytes, I on device]
- The stock keeps **one** instrument FX object for the synth and drum positions (placement-constructed at
  0xffb00600, pointer 0x1aaad60) and renders it once per block for the active instrument; its init carries the
  position. coop keeps one stereo twin to match: on init it constructs the selected type's *stereo* class (the
  master's) in its own heap object (`st_make`, `plugin_host.cc`), and on render it runs that object on
  `L = sat(mono + dL)`, `R = sat(mono + dR)` and writes back mono `(l + r) / 2` and the side `l - m`, `r - m`.
  [C in op1emu]
- Buffers. The delay and cwo stereo classes take their line from the table at 0x01AB8B94 (`[+0x14]` read per
  render by delay, `[+8]` cached at construction by cwo); the twin gets its own line (0x2B14C bytes) and the table
  entry is swapped around the call. punch, phone, filter and nitro are dual-mono: their stereo render calls the
  stock mono render function directly (not through the vtable), so they need nothing. grid and spring keep a tank at
  a fixed address in their stereo code (grid 0x01D8C70C, spring 0x01DE1F50 / 0x01DE6248; the mono classes use
  0x01D0868C and their own), so **the same one of those two types at an instrument and on the master at once shares
  one tank**: bounded, audible as interference. Planned fix: clones of those two renders retargeted at the mono
  tanks, which nothing uses now. [I from the disassembly]
- The stock delay is mono inside: its stereo render sums `g·(L + R)` into one scratch buffer (L1 0xFF801B50)
  before the line, so its echoes are centred wherever it runs, the master included. Echoes that follow the
  placement need a stereo delay (the coop `stereo_delay` plugin does it). [I from the disassembly, matches the
  emulator: the side energy of the S1 delay tail is 16, 2, 0 per 200 ms]

Emulator gates (op1emu, 2026-11-03, sequential runs):
- **Nothing bypasses the effect.** The chord with stock phone and with stock spring at the synth position: the
  side signal of the loudest second correlates 0.994 between the two effects on P2b (the side is the dry
  placement whatever the effect) and 0.185 on S1 (each effect processes it). Spring on S1: mid 859, side 607
  (P2b: mid 1803, side 250 dry); the reverb is wide now.
- No faults with delay, phone, spring at the synth position (`det_fx_stock_delay.txt`, `det_fx_stock_phone.txt`).

## Steps

1. **RE** (in progress): the stock master FX objects per type (vtable, ctor, size, render/init entries), the master
   FX factory, the mono vtables' render entries, the position dispatch tables.
2. **S1** (built, see above; realised at the insert point instead of new buses): per-instrument L/R buses in `stereo_bus.cc` (replacing the pairs of P2b), the mix kernels take L/R
   directly, the record tap takes L/R; the mono FX dispatch of all stock types made a no-op that records the
   selected type per position; coop instantiates the stock stereo object of that type for the position and runs
   it on the L/R buses (dual-mono inside, as the master does); coop plugins run on the same buses. Gate: v1 full
   image vs v2 with no FX selected, within rounding; a stock filter at the synth position filters everything.
3. **S2**: the second FX slot per position and on the master; the saved-sound JSON member for it.
4. **S3**: patch out the stock mono stages for the CPU.

## Display slots (2026-11-03, the owner on the first S1 build: nitro's dials did not move)

The stock effects write their page state (the levels and dials) into a table in L1 at 0xFF801774, 16 bytes per
slot: a mono object into slot = its position (the mono init stores it, nitro at object +0x18), the stereo classes
into slots 2 and 3 (the master's). The S1 twin wrote the master's slots and the instrument page read a slot nobody
updated. Fix: the twin runs with slots 2-3 swapped for its own copy and its left slot is copied into the
instrument's slot after each render (`st_render`). op1emu: nitro's dials follow the encoders as on P2b, the audio
sample-identical to before the fix (`tests/sim/det_fx_stock_nitro.txt`). [C in op1emu, I on device]
