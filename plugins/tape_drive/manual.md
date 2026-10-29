# tape drive

Tape saturation, wow and flutter, and worn-tape darkening.

![tape drive screen](screenshot.png)

## What it is

A **tape machine in a box**: gentle saturation like recording hot to tape, a
low head bump, high-frequency loss, **wow and flutter** (the pitch wobble of
an uneven transport) and some hiss. Run a synth or the whole mix through it
and it sounds like it came off a worn cassette or a reel-to-reel.

## Before you start

- **An original OP-1** (the 2011 model). It does not run on the OP-1 field
  or the OP-Z.
- **hundekraft 3.0.3** on it, from the
  [hundekraft workshop](https://hundekraft.github.io/op1-workshop/).
- Install it from co-op's Plugins tab over USB, or drop `tape_drive.knl`
  onto **Your plugins** in the hundekraft workshop. Either way the OP-1 asks
  you to press T1 to install, then restarts.
- A plugin runs as firmware code: back up your tape and sounds first. If the
  OP-1 ever fails to start after you add a plugin, switch it off and on
  again: it starts once without plugin files, and you can remove the file
  from the workshop.

## Controls

| Encoder | Main page | Press, then turn |
|---|---|---|
| blue | **saturate**: how hot the tape is driven, clean to compressed and warm | **input gain** −6 to +6 dB (Shift: tenths) |
| green | **wobble**: the amount of pitch wobble, slow deep wow at the bottom, fast flutter at the top | |
| white | **age**: clockwise = a newer tape; anticlockwise adds high-frequency loss and hiss | |
| orange | **mix** | |

## Second pages and Shift

- Pick it as any effect: hold Shift and press T3 on a synth or drum sound,
  or on the master in the mixer, and choose **tape drive**.
- Pressing **blue** turns the blue knob into the **input gain** (−6 dB to
  +6 dB, 0 dB at the centre) while it is pressed, and the screen shows a
  bracket and a dB readout. Release the knob and the page closes.
- On the second page, holding Shift makes each click a tenth of a dB. On the
  main page the OP-1's own Shift fine-adjust applies as it does to the stock
  engines.

## On screen

Two reels and the tape path. The wave on the tape between them grows and
flattens as saturate rises, the reels shake at the wobble rate (slow for
wow, fast for flutter, with the marker on the scale below showing where you
are), and at the right the low-pass glyph closes and the specks of hiss
count up as the tape ages. The gain and mix bars sit at the edges as on
every effect.

## Tips

- It is stereo on the master and mono on a synth or drum sound.
- Its saturation is the same as tape echo's.

## What is saved

The four knobs and the input gain are stored with the sound, as the stock
knobs are.

## Credits and licence

- The DSP, the OP-1 glue and the screen are coldsoldr's own work, built on
  hundekraft's kennel plugin SDK.
- Licence: GPL-3.0-or-later, with the OP-1 firmware permission of the SDK.
  Source: https://github.com/coldsoldr/te-op1-cfw
