# wavefolder

West Coast wavefolding.

![wavefolder screen](screenshot.png)

## What it is

A **wavefolder**, the West Coast synth trick. Instead of clipping the peaks
off it folds them back down, so a sine wave that is pushed harder grows extra
wiggles in its tops and bottoms. Bright, metallic, vocal and very synthy, and
it changes dramatically with the level you feed it. Best on a synth.
Oversampled and antialiased.

## Before you start

- **On opus:** Opus 2 with core 2.31 or later; see On opus, below.
- **Three builds, one per firmware:** `wavefolder.knl` for hundekraft,
  `wavefolder.coop` for coop and `wavefolder.opmod` for opus. The Plugins tab
  offers the one for the firmware chosen in Settings. On coop you need **coop
  e10** or later, and the controls differ a little (see On coop, below); the
  rest of this section is about hundekraft.
- **An original OP-1** (the 2011 model). It does not run on the OP-1 field
  or the OP-Z.
- **hundekraft 3.0.3** on it, from the
  [hundekraft workshop](https://hundekraft.github.io/op1-workshop/).
- Install it from co-op's Plugins tab over USB, or drop `wavefolder.knl`
  onto **Your plugins** in the hundekraft workshop. Either way the OP-1 asks
  you to press T1 to install, then restarts.
- A plugin runs as firmware code: back up your tape and sounds first. If the
  OP-1 ever fails to start after you add a plugin, switch it off and on
  again: it starts once without plugin files, and you can remove the file
  from the workshop.

## Controls

| Encoder | Main page | Press, then turn |
|---|---|---|
| blue | **fold**: the gain into the folder, the number of folds | **input gain** −6 to +6 dB (Shift: tenths) |
| green | **symmetry**: a DC offset before the folder; off-centre it folds only the top or only the bottom | |
| white | **tone**: a low-pass after the folder, dark to bright, to tame the fizz | |
| orange | **mix** | |

## Second pages and Shift

- Pick it as any effect: hold Shift and press T3 on a synth or drum sound,
  or on the master in the mixer, and choose **wavefolder**.
- Pressing **blue** turns the blue knob into the **input gain** (−6 dB to
  +6 dB, 0 dB at the centre) while it is pressed, and the screen shows a
  bracket and a dB readout. Release the knob and the page closes.
- On the second page, holding Shift makes each click a tenth of a dB. On the
  main page the OP-1's own Shift fine-adjust applies as it does to the stock
  engines.

## On coop

The four knobs on the main page are the same as on hundekraft. Pick it from
the effect list (shift + T3) at any position, FX 1 or FX 2. It runs in
stereo everywhere. The extra controls are on fn (Help held) instead of a
second page:

| Encoder | fn + turn | Also |
|---|---|---|
| blue | **input gain**, −6 to +6 dB, 0.5 dB a click | shift + fn + blue: the same in 0.1 dB clicks |

Install `wavefolder.coop` from co-op's Plugins tab over USB with coop chosen
in Settings. It loads at the next power cycle.

## On opus

`wavefolder.opmod` is the coop build ported to Opus 2 by coldsoldr's
[op1-plugins](https://github.com/coldsoldr/op1-plugins): the same sound,
screen and controls. It needs Opus 2 with core 2.31 or later.

- Download `wavefolder.opmod` from op1-plugins'
  [releases](https://github.com/coldsoldr/op1-plugins/releases), copy it into
  the `mods` folder of the OP-1's disk (disk mode: shift + COM, then T3),
  eject and restart.
- op1-plugins also has `cs_fx.opmod`, all nine coldsoldr effects in one
  file. Use either it or the single files, not both: the second copy of an
  effect is refused. Switching between them later turns the effects already
  on sounds back into the filter until you choose them again.
- Pick it from the effect list (shift + T3) on a synth or drum sound, or on
  the master in the mixer. Opus has no FX 2 slot for it.
- The extra controls are those of On coop, above, with **fn** being
  Opus's FN key.
- At the synth and the drum it runs in mono (Opus's instrument sound is
  mono); its stereo is heard on the master.

## On screen

One cycle of the output wave gains more and more folds as fold rises. The
green centre line moves up or down with symmetry, and the low-pass slope at
the right opens with tone. The orange bar at the right edge is the mix.

## Tips

- Best on a synth. It changes dramatically with the level you feed it, and
  the input gain on blue's second page sets that level.
- It is stereo on the master and mono on a synth or drum sound.

## What is saved

The four knobs and the input gain are stored with the sound, as the stock
knobs are.

## Credits and licence

- The DSP, the OP-1 glue and the screen are coldsoldr's own work, built on
  hundekraft's kennel plugin SDK.
- Licence: GPL-3.0-or-later, with the OP-1 firmware permission of the SDK.
  Source: https://github.com/coldsoldr/te-op1-cfw (hundekraft) and
  https://github.com/coldsoldr/co-op/tree/main/firmware/plugins/wavefolder (coop)
