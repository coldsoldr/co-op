# Delayed

*A modulated delay with a resonant filter, from SoundHack's +delay.*

A modulated delay with a resonant filter in the feedback path, feedback up to 200% and a Freeze that holds what is in the buffer. An LFO with six shapes moves the delay time, with an optional Smooth setting that rounds off its edges. A port of Tom Erbe's +delay from SoundHack's Delay Trio.

## Controls

| Page | Blue | Green | White | Orange |
|---|---|---|---|---|
| Main | Time | Feedback | Filter (press for Reso) | Mix (press for Freeze) |
| Shift | LFO Freq | LFO Depth | LFO Shape | LFO Phase |

The LFO shapes are sine, triangle, square, up, down and random. Smooth LFO, in the plugin's Opus settings, softens the jumps, including both edges of the square. The longest base delay is one second.

Turning an encoder faster moves its value further, so long sweeps take fewer turns.

## Who made it

Delayed is by ESC_CODE (Volker Junginger) ([VolkerJunginger/OP-1](https://github.com/VolkerJunginger/OP-1), GPL-3.0-or-later). It ports Tom Erbe's +delay from SoundHack's Delay Trio, whose original code keeps its BSD-style licence. Built with LizardMan's Opus plugin kit.

## Get it

Delayed 1.0.1 is one of the three ESC Delay Trio effects for Opus core 2.31 or later on the original OP-1 (firmware 246). Download its `.opmod`, or all three with the illustrated manual, from the [Delay Trio folder](https://github.com/VolkerJunginger/OP-1/tree/main/Delay-Trio), copy it to the `mods` folder on the OP-1's disk and restart. It is an instrument effect: on the master it passes audio through dry. co-op lists it for reference and hosts no copy.

## Links

- [Delay Trio: downloads, install and controls](https://github.com/VolkerJunginger/OP-1/tree/main/Delay-Trio)
- [Illustrated manual (PDF)](https://github.com/VolkerJunginger/OP-1/blob/main/Delay-Trio/Delay%20Trio%20Illustrated%20Manual.pdf)
- [Source](https://github.com/VolkerJunginger/OP-1/tree/main/Delay-Trio/Source) and [credits](https://github.com/VolkerJunginger/OP-1/blob/main/Delay-Trio/Credits.md)
- [SoundHack](https://www.soundhack.com/)
