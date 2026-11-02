# Second LFO slot (Phase 3, "LFO x2") — design from the traced stock LFO path

Status: design + call-site map, 2026-11-02 [C where marked]. Not implemented yet. Builds on
`../te-op1-cfw/docs/lfo-interface.md` (types, objects, vtables, parameters, ranges, presets) and on
the decrypted L1, which that document could not read.

## What stock does (per block, per sounding voice) [C]

One LFO object per voice (6 synth / 12 drum, L1 scratchpad `0xffb00a00`, type-dependent size) plus one
"fx LFO" per instrument. The voice loop calls each voice's LFO `process(this, &lfo_params[slot], stage)`
(vtable `+0x14`) through three L1 sites, guarded by `lfo != 0` and the slot's `lfo_active` byte:

| site | stage (R2) | what the LFO may modulate | registers at the CALL |
|---|---|---|---|
| synth `0xffa00c8e..0xffa00ca0` | 0 (R2 zeroed at `0xffa00c66`) | engine knobs (scratch `0xff801724`), pitch array `0xff801950`, gain `0xff801d50` — before the render | `P2 = *(vt+0x14)`, R0 = obj + `W[vt+0x10]`, R1 = `[SP+0x3c]` (`0xff8014c4`) + slot*16, R2 = 0; R7 = voice |
| synth `0xffa00af2..0xffa00b04` | 1 | envelope (ADSR scratch) | same shape, R2 = 1 |
| drum `0xffa00802..0xffa0080a` | 3 | drum engine | `P0 = *(vt+0x14)`, R1 = `[SP+0x2c]` (`0xff801644`) + inst*16, R2 = 3; R7 = 12 - voice |

The fx-LFO stages (2 synth FX, 4 drum envelope, 5 drum FX) are dispatched from the FX driver (R2 = 2
at `0xffa02984`, 4 at `0xffa022e0`, 5 at `0xffa0255e`; exact `CALL` sites still to pin). Trigger
(`+0x1c`) is `Core::LfoTrigger(voice)` (SDRAM `0x019673dc`), called by VoiceCtl right before
`Core::NoteOn` (`0x01967854`) — the function the stereo bus already hooks (`coop_core_noteon_thunk`).

Objects are placement-constructed by per-type getters (`lfo-interface.md` §2): tremolo `ctor
0x019cbd88(this)` on 28 B; value `[this] = 0x0191097c; 0x019f15f8(this+4)` on 12 B; random
`[this] = 0x0191090c; W[this+4] = 0; [this+8] = 0; 0x01aa3862(this+0xc, 8, 2, ..)` twice, zero
`+0xc..+0x28` on 44 B. Element, midi, bend and crank are 4-byte objects whose state lives in globals.

## coop design

A **second LFO per instrument** with its own type, parameters and active flag, processed right after
the stock one at every stage, so both modulate the same destinations (ApplyRange adds and clamps, so
two LFOs compose additively):

```
at each L1 process site (synth 0, synth 1, drum 3):      stock LFO1 process (unchanged)
                                                           coop:  if lfo2[voice]: lfo2->process(lfo2 + off, &lfo2_params, stage)
at Core::NoteOn (already hooked):                         coop:  lfo2[voice]->trigger(lfo2 + off, &lfo2_params)
```

- **Objects:** coop-owned instances of the stock LFO classes in region A (6 synth + 12 drum + 1 fx),
  built by calling the stock constructors with coop memory as `this` (the three per-object-state types:
  tremolo, value, random; the vptr-only types share global state with LFO1 and are not offered on slot 2).
  Rebuilt when the slot-2 type changes (mirrors `lfo_factory.cpp`'s resets).
- **Parameters:** `g_coop_lfo2_type` (0 tremolo / 1 value / 2 random / 7 off), `g_coop_lfo2_params[8]`
  (int16, validated against the stock Ranges table `0x01de5740` so nothing the stock code reads is out
  of range), `g_coop_lfo2_active`; image-time words first (`inject_app --set-word`), UI later with the
  plugin runtime. Saved with the patch via co-op (the stock patch JSON stays untouched).
- **Hooks:** the three L1 sites are rewritten exactly like the render calls (`P2 = [P0+0x14]` moves
  into a trampoline, the site gets `CALL tramp`; the `R2 = 3` of the drum site moves too). Thunks run
  the stock process, then the coop LFO2 process with the same stage and the voice from R7, and return
  the stock result (the caller tests R0). The trigger rides on `coop_core_noteon_thunk`.
- **Rate/cost:** one extra process call per sounding voice per block (689 Hz); the stock processes are
  small SDRAM functions (random: 8 LCG draws + smoothing). Budget impact well under 1% at 6 voices;
  measured when implemented.
- **Gate:** with slot 2 off, the image is stock bit for bit (same method as the stereo bus:
  `tests/sim/det_chord.txt`, `tools/wav_compare.py`). With slot 2 = tremolo on a static chord, the
  output shows the extra amplitude modulation (period from params) and nothing else changes.

## Open points

- Pin the fx-LFO `CALL` sites (stages 2/4/5) and decide whether slot 2 also runs on the fx LFO; the
  coop stereo FX chain (C3) may supersede the stock FX hosting first.
- Whether idle voices' LFOs are processed (affects phase continuity of a value LFO across notes).
- Free L1: 40 of 76 trampoline bytes are used by the stereo bus; three more 12-byte trampolines fit
  exactly (36 B). Beyond that, use the region-C SDRAM trampoline pattern from `l1_stereo_insert.py`
  for SDRAM sites, or claim more L1 (kennel's L1 map).
