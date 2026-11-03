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

## S3: the stereo pair from the voice on (2026-11-03, the owner: "why not get rid of the mono buses")

**The stock structure, traced (op1emu write tracer, stock image).** There is one voice bus, not one per instrument.
Per block the stock clears 0xff800bb4 (128 x int16 at 88.2 kHz), renders each voice and adds it to the bus (synth
and drums alike), runs **in drum mode** a drum bus stage on the summed bus (0x01a66922, called from L1 0xffa02364:
128 samples in place, two state words in the object it is handed plus the global 0x01dfc80c, a rectifier inside: a
level / dynamics stage; it halves a single hit), decimates (0xffa005d4) and DC-blocks (0x019674f8) back into the
first 64 samples, runs **the active instrument's effect** on that buffer in place (the phone render writes it at
0x01a1bf34 once per block), and mixes it (loop B; loop A on the tape page). One effect object serves both
instruments because one effect runs on everything: the selected instrument's. So S1's two side pairs were never
the stock's shape (the drums' side bypassed the synth's effect); S3 has one pair. [C in op1emu]

The decimator + DC pair is called from three L1 sites (0xffa01eb4, 0xffa02456 in the drum path, 0xffa02888), each
`CALL 0xffa005d4; P1 = 0x019674f8; CALL (P1)` (14 bytes). [C in bytes]

**What coop does now.**
- The voice hooks add each voice's whole contribution into one 88.2 kHz pair: `L += v + v*(gL-1)`,
  `R += v + v*(gR-1)` (`coop_pair_accum_synth/drum`, asm verified bit-exact against C in the GNU simulator), with
  v exactly the stock per-voice value (synth `(out*env)*gain`, drums `sat(2*(out*gain))`). The drum hook no
  longer skips a voice whose render returned 0: the stock only sets a flag there and sums it anyway.
- Drum mode: the drum bus stage call is retargeted (`coop_drumbus_thunk` -> `coop_drum_bus`) and the stock stage
  runs on L and on R, the right side with its own copy of the three state words. Per side, unlinked.
- The three decimator + DC sites become an inline far call (`[--SP] = RETS; P0 = coop_instr_thunk; CALL (P0);
  RETS = [SP++]`, 14 bytes; the free L1 trampoline range is full in the tape build) -> `coop_instr_stage`: the
  pair decimator with the stock ramp word (written back as the stock decimator does), the DC block on both sides,
  then mid `(L + R) >> 1` into the stock bus buffer and the side `L - mid`, `R - mid` into coop's buffers. The
  thunk saves the DAG, loop and accumulator registers (one site keeps M1 across the pair) and runs the stock pair
  when `g_coop_stereo_enable` is 0.
- The effect dispatch, the mix loops and the record tap are unchanged: they take mid + side, which is the pair
  exactly (`sat(mid + side) = L`). The mid in the stock buffer is what the stock meters read.
- Still running and unused: the stock per-voice sum into the mono bus (its result is overwritten by the mid).
  Patching it out is a small saving left for later (about 0.4k cycles per sounding voice [I]).

**Debug aid.** `--set-word g_coop_dbg_cmp=1` (op1emu, spread 0): the thunk also runs the stock decimator and DC on
the stock bus, the drum stage runs on a third state copy, and the stage compares coop with stock at the 88.2 kHz
bus, after the decimator and after the DC block (`g_dbg_acc_bad / dec_bad / dc_bad`, first mismatch in
`g_dbg_*_first`). It found the drum bus stage (coop's drums came out exactly twice the stock's).

**Emulator gates (2026-11-03, sequential, `--align 200`).** 
- Debug comparison at spread 0 (`g_coop_dbg_cmp=1`): coop's 88.2 kHz bus, decimator output and DC output equal
  the stock's in every block of the chord (50,018 blocks) and the drum scenario (38,127 blocks, 18,380 drum bus
  stage calls).
- Spread 0 against T4a's spread 0 (the established coop reference, itself within 1 LSB of stock): chord and drums
  within 1 LSB (different images shift by a fraction of a block); drums: the same energy and peak as stock
  (RMS 60.0, peak 2597 in both; before the drum bus stage was found, coop's drums were exactly twice as loud).
- Full spread against S1 in the same job: chord and tape record within 1 LSB, the coop plugin within 2 LSB, stock
  phone at the synth position the same levels and width (its details differ).
- No faults in any scenario (chord, drums, tape record and play, plugin, phone, nitro).

## Steps

1. **RE** (in progress): the stock master FX objects per type (vtable, ctor, size, render/init entries), the master
   FX factory, the mono vtables' render entries, the position dispatch tables.
2. **S1** (built, see above; realised at the insert point instead of new buses): per-instrument L/R buses in `stereo_bus.cc` (replacing the pairs of P2b), the mix kernels take L/R
   directly, the record tap takes L/R; the mono FX dispatch of all stock types made a no-op that records the
   selected type per position; coop instantiates the stock stereo object of that type for the position and runs
   it on the L/R buses (dual-mono inside, as the master does); coop plugins run on the same buses. Gate: v1 full
   image vs v2 with no FX selected, within rounding; a stock filter at the synth position filters everything.
3. **S2**: the second FX slot per position and on the master; the saved-sound JSON member for it.
4. **S3** (built, see above): the stereo pair from the voice hook, coop's decimator and DC block in place of the
   stock mono ones, the drum bus stage per side; the stock per-voice sum still runs, unused.

## Display slots (2026-11-03, the owner on the first S1 build: nitro's dials did not move)

The stock effects write their page state (the levels and dials) into a table in L1 at 0xFF801774, 16 bytes per
slot: a mono object into slot = its position (the mono init stores it, nitro at object +0x18), the stereo classes
into slots 2 and 3 (the master's). The S1 twin wrote the master's slots and the instrument page read a slot nobody
updated. Fix: the twin runs with slots 2-3 swapped for its own copy and its left slot is copied into the
instrument's slot after each render (`st_render`). op1emu: nitro's dials follow the encoders as on P2b, the audio
sample-identical to before the fix (`tests/sim/det_fx_stock_nitro.txt`). [C in op1emu, I on device]
