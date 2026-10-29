# rings

Mutable Instruments Rings as a synth engine: a physical-modelling resonator,
six models, up to six voices.

![rings screen](screenshot.png)

## What it is

**Mutable Instruments Rings**, the Eurorack resonator, as a synth engine. A
key strikes or bows a resonator and the resonator sings. There are six
models, chosen on the blue knob's second page:

1. **modal**: a bank of band-pass resonators struck by the key, like a bar
   or a plate. Structure spaces them from harmonic to inharmonic.
2. **sympathetic strings**: Karplus-Strong strings that resonate with each
   other, two per voice.
3. **inharmonic string**: one string with dispersion and modulation.
4. **FM voice**: two-operator FM with feedback.
5. **quantized sympathetic strings**: the strings tuned to a chord table.
6. **string + reverb**: strings into a shared reverb.

## Before you start

- **An original OP-1** (the 2011 model). It does not run on the OP-1 field
  or the OP-Z.
- **hundekraft 3.0.3** on it, from the
  [hundekraft workshop](https://hundekraft.github.io/op1-workshop/).
- Install it from co-op's Plugins tab over USB, or drop `rings.knl` onto
  **Your plugins** in the hundekraft workshop. Either way the OP-1 asks you
  to press T1 to install, then restarts.
- A plugin runs as firmware code: back up your tape and sounds first. If the
  OP-1 ever fails to start after you add a plugin, switch it off and on
  again: it starts once without plugin files, and you can remove the file
  from the workshop.

## Controls

| Encoder | Main page | Press, then turn |
|---|---|---|
| blue | **structure**: the spacing of the modes or the coupling of the strings | **model**, one of the six per click |
| green | **brightness**: how bright the excitation is and how many modes sound | **polyphony**: all (six voices), 1, 2 or 4 |
| white | **damping**: decay time, clockwise = longer | |
| orange | **position**: where the string or plate is struck | |

## Second pages and Shift

- Pick it as any synth: hold Shift and press T1, turn blue to **rings**,
  press to load.
- Pressing **blue** and turning picks the **model**, one per click. Pressing
  **green** and turning sets the **polyphony**. Release the knob and the
  page closes.
- On the main page the OP-1's own Shift fine-adjust applies as it does to
  the stock engines.

## On screen

The rings light up from the centre with structure, the rays round them with
brightness. The pad at the lower right lifts off the rings as damping
rises, and the red dot is the strike position. Each voice that sounds sends
a ripple out from the core, and the core glows with the total level. The
MODEL and POLY rows on the right show the two settings; while a knob is on
its second page the row is bracketed and shown in white.

## Tips

- Fewer voices buy more detail: at polyphony 1 the single voice gets every
  mode or string the engine can afford. "All" is six voices.
- The stock T2 envelope shapes the sound after the resonator as it does for
  every engine, so set a release there if the rings cut off too early.
- **CPU.** The sound is heavy on the CPU at "all" next to big effects on the
  master; polyphony 4 leaves room. Use 4 voices, or lighter effects, when
  it crackles.

## What is saved

The four knobs, the model and the polyphony are stored with the sound, as
the stock knobs are.

## Credits and licence

- Ported from Mutable Instruments Rings by Émilie Gillet (MIT), bit-exact
  where the hardware allowed and credited in each source file.
- The OP-1 glue and the screen are coldsoldr's own work, built on
  hundekraft's kennel plugin SDK.
- Licence: GPL-3.0-or-later, with the OP-1 firmware permission of the SDK.
  Source: https://github.com/coldsoldr/te-op1-cfw
