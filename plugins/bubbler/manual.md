# Bubbler

*A granular delay, from SoundHack's +bubbler.*

A granular delay: the input is chopped into overlapping grains that repeat with their own timing, size, pitch and direction. Density sets how many grains overlap, and a filter with resonance colours the feedback. A port of Tom Erbe's +bubbler from SoundHack's Delay Trio.

## Controls

| Page | Blue | Green | White | Orange |
|---|---|---|---|---|
| Main | Time | Feedback | Density | Mix |
| Shift, page 2 | Time Var | Start Var | Size | Size Var |
| Shift, page 3 | Pitch | Pitch Var | Filter (press for Reso) | Reverse |

Hold shift and use left and right to move between pages 2 and 3; letting go of shift goes back to Main, and the last page is remembered. Pitch steps through −12, −7, −5, −3, off, key (follows MIDI notes), +3, +5, +7 and +12. Pitch Var is the chance a grain jumps by an octave. The longest base delay is one second.

## Who made it

Bubbler is by ESC_CODE (Volker Junginger) ([VolkerJunginger/OP-1](https://github.com/VolkerJunginger/OP-1), GPL-3.0-or-later). It ports Tom Erbe's +bubbler from SoundHack's Delay Trio, whose original code keeps its BSD-style licence. Built with LizardMan's Opus plugin kit.

## Get it

Bubbler 1.5.0 is one of the three ESC Delay Trio effects for Opus core 2.31 or later on the original OP-1 (firmware 246). Download its `.opmod`, or all three with the illustrated manual, from the [Delay Trio folder](https://github.com/VolkerJunginger/OP-1/tree/main/Delay-Trio), copy it to the `mods` folder on the OP-1's disk and restart. It is an instrument effect: on the master it passes audio through dry. co-op lists it for reference and hosts no copy.

## Links

- [Delay Trio: downloads, install and controls](https://github.com/VolkerJunginger/OP-1/tree/main/Delay-Trio)
- [Illustrated manual (PDF)](https://github.com/VolkerJunginger/OP-1/blob/main/Delay-Trio/Delay%20Trio%20Illustrated%20Manual.pdf)
- [Source](https://github.com/VolkerJunginger/OP-1/tree/main/Delay-Trio/Source) and [credits](https://github.com/VolkerJunginger/OP-1/blob/main/Delay-Trio/Credits.md)
- [SoundHack](https://www.soundhack.com/)
