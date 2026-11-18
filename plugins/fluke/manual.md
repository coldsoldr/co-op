# Fluke

*A generative sequencer for melodies and six-lane drum grooves.*

A generative sequencer that plays the current synth, or up to six drum lanes. In Synth it writes a melody in one of six scales and ten styles; in Drum it builds a groove across the lanes in one of 14 styles, from Straight and Clave to Funk and Shuffle. Shape it live with Length, Density or Euclidean Pulses, Rotate, Rate and Swing, and Lift a pattern to Drop back to it.

## Melody

Choose FLUKE as the sequencer in Synth. The keyboard sets the root note without restarting.

| Encoder | Turn | Press | With shift held |
|---|---|---|---|
| Blue | Length, 1–16 steps | New melody | Range, or Random |
| Green | Density, or Pulses | Density / Pulses | Style |
| White | Rotate | Next scale | Direction |
| Orange | Rate | Hold on / off | Swing |

The scales are chromatic, major, minor, major pentatonic, Dorian and Mixolydian. Pulses spreads an exact number of hits evenly, Euclidean style.

## Drums

Choose FLUKE as the sequencer in Drum. It starts with three lanes, on F3, G3 and C#4, in ALL. Hold shift and press a key to add it as a lane and edit it, up to six. Shift and the edited key again goes back to ALL. To remove a lane, edit it, then hold Cut and press its key.

| View | Blue | Green | White | Orange |
|---|---|---|---|---|
| ALL, turn | Length | Density | Rotate | Global Swing |
| ALL, press | New for every lane | | | Hold |
| Edit, turn | Length | Density / Pulses | Rotate | Rate |
| Edit, press | New for this lane | Density / Pulses | | Hold |
| Edit, shift + turn | Random | Style | Direction | Lane Swing |

In ALL, turns move every lane from where it is; in Edit, only that lane. Each lane has its own length, rate (1/32 to 1/4), swing and direction, and plays a role in one shared groove: foundation, backbeat, hat, offbeat, percussion or fill. The styles are Straight, Backbeat, Offbeat, Tresillo, Clave, Break, Fill, Random, Rock, Funk, Disco, Reggae, Latin and Shuffle. New tries eight patterns and keeps the one that fits the groove best. At most four notes play at once.

## Both modes

Lift saves the melody or the whole drum pattern, which survives a restart; Drop brings it back on the next step; Cut jumps back to step one, in time. In the plugin's Opus settings, **Autostart** holds and starts the sequence when the tape records (the default), plays and records, or never; **Drum keys** makes plain keys in ALL transpose or just preview the drums; and **Hits overlay** shows the lanes on HITS's screen. Turning an encoder faster moves its value further, so long sweeps take fewer turns.

## Who made it

Fluke is by ESC_CODE (Volker Junginger) ([VolkerJunginger/OP-1](https://github.com/VolkerJunginger/OP-1), GPL-3.0). Its note generation and quantising adapt Mutable Instruments Marbles, and its grooves draw on Grids, both by Emilie Gillet (MIT). Built with LizardMan's Opus plugin kit.

## Get it

FLUKE, HITS and FLUX come as a set from the [device page](https://volkerjunginger.github.io/OP-1/), one `.opmod` each or together in a zip, for Opus core 2.57 or later on the original OP-1 (firmware 246). Copy the files to the `mods` folder on the OP-1's disk, eject and restart; replace older copies by file name and remove an old `hitsfluke.opmod`. co-op lists it for reference and hosts no copy. In Drum, HITS and FLUX follow the lane you edit when they are installed.

## Links

- [Device page](https://volkerjunginger.github.io/OP-1/#fluke): downloads and every device
- [Install, credits and source](https://github.com/VolkerJunginger/OP-1/tree/main/Fluke)
- [Illustrated manual (PDF)](https://github.com/VolkerJunginger/OP-1/blob/main/docs/manuals/FLUKE%20v.2.0.0%20Manual.pdf)
