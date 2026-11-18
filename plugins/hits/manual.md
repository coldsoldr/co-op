# HITS

*A 24-pad drum synth with 17 models.*

A synthesized drum kit: 24 pads, each playing one of 17 models adapted from Mutable Instruments Braids, from kicks and metal to plucks and noise. Shape each pad's pitch, timbre and length, make pads choke each other, and add grit with Decimate.

## Controls

Choose HITS as the drum engine with shift + T1. Hold shift and press a key to pick the pad you edit.

| Encoder | Turn | Press | With shift held |
|---|---|---|---|
| Blue | Pitch | Next model | Color |
| Green | Timbre | Next choke group | Pad level |
| White | The model's own parameter | Next decimate depth | Low cut |
| Orange | Length | Fluke hold | High cut |

Pads in one choke group cut each other off. Decimate (12, 8 or 4 bit) works per pad, or for the whole kit with the **Decimate scope** and **Global decimate depth** settings. With FLUKE as the drum sequencer, HITS shows its lanes on the keyboard and orange toggles FLUKE's hold. Turning an encoder faster moves its value further, so long sweeps take fewer turns.

## Who made it

HITS is by ESC_CODE (Volker Junginger) ([VolkerJunginger/OP-1](https://github.com/VolkerJunginger/OP-1), GPL-3.0). Its models adapt Mutable Instruments Braids by Emilie Gillet (MIT), and SoundHack's +decimate inspired its bit reduction. Built with LizardMan's Opus plugin kit.

## Get it

FLUKE, HITS and FLUX come as a set from the [device page](https://volkerjunginger.github.io/OP-1/), one `.opmod` each or together in a zip, for Opus core 2.57 or later on the original OP-1 (firmware 246). Copy the files to the `mods` folder on the OP-1's disk, eject and restart; replace older copies by file name and remove an old `hitsfluke.opmod`. co-op lists it for reference and hosts no copy.

## Links

- [Device page](https://volkerjunginger.github.io/OP-1/#hits): downloads and every device
- [Install, credits and source](https://github.com/VolkerJunginger/OP-1/tree/main/Fluke)
- [Illustrated manual (PDF)](https://github.com/VolkerJunginger/OP-1/blob/main/docs/manuals/FLUKE%20v.2.0.0%20Manual.pdf)
