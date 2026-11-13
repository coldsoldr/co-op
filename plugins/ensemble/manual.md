# ensemble

Chorus to string-machine ensemble.

![ensemble screen](screenshot.png)

## What it is

A **chorus / string ensemble**. Copies of the signal are delayed by a few
milliseconds and the delays are swept by slow LFOs, so the copies drift in
pitch against each other. With one copy per side it is a classic chorus,
wide and shimmering. With three phase-spread copies per side it becomes the
ensemble sound of 1970s string machines like the Solina: lush, thick and
slowly swirling. The stock OP-1 has no chorus.

## Before you start

- **On opus:** Opus 2 with core 2.31 or later; see On opus, below.
- **Three builds, one per firmware:** `ensemble.knl` for hundekraft,
  `ensemble.coop` for coop and `ensemble.opmod` for opus. The Plugins tab
  offers the one for the firmware chosen in Settings. On coop you need **coop
  e10** or later, and the controls differ a little (see On coop, below); the
  rest of this section is about hundekraft.
- **An original OP-1** (the 2011 model). It does not run on the OP-1 field
  or the OP-Z.
- **hundekraft 3.0.3** on it, from the
  [hundekraft workshop](https://hundekraft.github.io/op1-workshop/).
- Install it from co-op's Plugins tab over USB, or drop `ensemble.knl` onto
  **Your plugins** in the hundekraft workshop. Either way the OP-1 asks you
  to press T1 to install, then restarts.
- A plugin runs as firmware code: back up your tape and sounds first. If the
  OP-1 ever fails to start after you add a plugin, switch it off and on
  again: it starts once without plugin files, and you can remove the file
  from the workshop.

## Controls

| Encoder | Main page | Press, then turn |
|---|---|---|
| blue | **rate**: LFO speed, 0.05 to 8 Hz | **input gain** −6 to +6 dB (Shift: tenths) |
| green | **depth**: how far the delays sweep | |
| white | **voices**: one sweeping copy per side (chorus) to three (ensemble) | |
| orange | **mix** | |

## Second pages and Shift

- Pick it as any effect: hold Shift and press T3 on a synth or drum sound,
  or on the master in the mixer, and choose **ensemble**.
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

Install `ensemble.coop` from co-op's Plugins tab over USB with coop chosen
in Settings. It loads at the next power cycle.

## On opus

`ensemble.opmod` is the coop build ported to Opus 2 by coldsoldr's
[op1-plugins](https://github.com/coldsoldr/op1-plugins): the same sound,
screen and controls. It needs Opus 2 with core 2.31 or later.

- Download `ensemble.opmod` from op1-plugins'
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

Two lanes, left and right (the second appears on the master). Each voice is
a green mark that sways about its home on the lane with the LFO, further
with depth and faster with rate; more marks appear as voices goes up. The
dot on the dial at the right circles at the LFO rate, and the blue bar under
it is the rate. The orange bar at the right edge is the mix.

## Tips

- It is stereo on the master and mono on a synth or drum sound. The second
  lane only acts on the master.
- Ensemble runs its core at half the audio rate behind filters, to fit the
  processor; it restarts cleanly after a pause.

## What is saved

The four knobs and the input gain are stored with the sound, as the stock
knobs are.

## Credits and licence

- The DSP, the OP-1 glue and the screen are coldsoldr's own work, built on
  hundekraft's kennel plugin SDK.
- Licence: GPL-3.0-or-later, with the OP-1 firmware permission of the SDK.
  Source: https://github.com/coldsoldr/te-op1-cfw (hundekraft) and
  https://github.com/coldsoldr/co-op/tree/main/firmware/plugins/ensemble (coop)
