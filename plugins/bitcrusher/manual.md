# bitcrusher

A bit crusher: lower sample rate and bit depth, with a jittery clock.

![bitcrusher screen](screenshot.png)

## What it is

A **bit crusher**. It lowers the sample rate (each sample is held for
longer, which turns the sound gritty and adds metallic aliasing) and the bit
depth (fewer levels, so it gets noisy and steppy). **Jitter** makes the
sample clock unsteady, like a dying sampler. From a subtle 12-bit grit to
2-bit destruction.

## Before you start

- **An original OP-1** (the 2011 model). It does not run on the OP-1 field
  or the OP-Z.
- **hundekraft 3.0.3** on it, from the
  [hundekraft workshop](https://hundekraft.github.io/op1-workshop/).
- Install it from co-op's Plugins tab over USB, or drop `bitcrusher.knl`
  onto **Your plugins** in the hundekraft workshop. Either way the OP-1 asks
  you to press T1 to install, then restarts.
- A plugin runs as firmware code: back up your tape and sounds first. If the
  OP-1 ever fails to start after you add a plugin, switch it off and on
  again: it starts once without plugin files, and you can remove the file
  from the workshop.

## Controls

| Encoder | Main page | Press, then turn |
|---|---|---|
| blue | **rate**: the sample rate, clockwise = higher, cleaner; anticlockwise down to 500 Hz | **input gain** −6 to +6 dB (Shift: tenths) |
| green | **bits**: the bit depth, clockwise = more bits; anticlockwise down to 2 | |
| white | **jitter**: randomness of the sample clock | |
| orange | **mix** | |

## Second pages and Shift

- Pick it as any effect: hold Shift and press T3 on a synth or drum sound,
  or on the master in the mixer, and choose **bitcrusher**.
- Pressing **blue** turns the blue knob into the **input gain** (−6 dB to
  +6 dB, 0 dB at the centre) while it is pressed, and the screen shows a
  bracket and a dB readout. Release the knob and the page closes.
- On the second page, holding Shift makes each click a tenth of a dB. On the
  main page the OP-1's own Shift fine-adjust applies as it does to the stock
  engines.

## On screen

A smooth wave and its stepped copy. The steps get wider as the rate drops,
the green ladder on the left gains rungs as the bits go (the levels the
sound is rounded to), and the row of clock ticks along the top shakes with
jitter. The dot runs along the stairs at the sample clock's speed. The blue
bar at the left edge is the input gain; it warms towards white as the input
gets loud. The orange bar at the right edge is the mix.

## Tips

- It is stereo on the master and mono on a synth or drum sound.

## What is saved

The four knobs and the input gain are stored with the sound, as the stock
knobs are.

## Credits and licence

- The DSP, the OP-1 glue and the screen are coldsoldr's own work, built on
  hundekraft's kennel plugin SDK.
- Licence: GPL-3.0-or-later, with the OP-1 firmware permission of the SDK.
  Source: https://github.com/coldsoldr/te-op1-cfw
