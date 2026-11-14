# Fluke

*A generative sequencer you play like an instrument.*

A generative sequencer that plays the current synth or sampler. It writes a melody in one of six scales and ten styles, which you shape live with Length, Density or Euclidean Pulses, Rotate, Rate, Direction and Swing. Lift saves a pattern so you can wander off and Drop back to it.

## Controls

| Encoder | Turn | Press | With shift held |
|---|---|---|---|
| Blue | Length, 1–16 steps | New melody | Range, −3 to +3 |
| Green | Density, or Pulses | Density / Pulses | Style |
| White | Rotate | Next scale | Direction |
| Orange | Rate, 1/32 to a whole note | Hold on / off | Swing, 0–100 |

The keyboard sets the root note without restarting. Density adds or removes notes from the generated rhythm; Pulses spreads an exact number of hits evenly, Euclidean style. With Hold off, the sequence stops when you let go of the last key. The scales are chromatic, major, minor, major pentatonic, Dorian and Mixolydian. The styles (Scale, Chord, Fifth, Root, Triad, Walk, Leap, Motif, Reply, Anchor) reshape the current melody rather than writing a new one. Direction plays forward, reverse, back and forth, or odd steps then even.

Lift saves the pattern, which survives a restart; Drop brings it back on the next step; Cut / Split jumps back to step one on the next step and stays in time.

## Who made it

Fluke is by ESC_CODE (Volker Junginger) ([VolkerJunginger/OP-1](https://github.com/VolkerJunginger/OP-1), GPL-3.0). Its generator includes an integer adaptation of Mutable Instruments Marbles by Emilie Gillet (MIT). Built with LizardMan's Opus plugin kit.

## Get it

Fluke 1.0.0 is marked experimental and needs Opus 2.8 or later on the original OP-1 (firmware 246). Download `fluke.opmod` with its PDF manual from the [Fluke folder](https://github.com/VolkerJunginger/OP-1/tree/main/Fluke) or its [release](https://github.com/VolkerJunginger/OP-1/releases/tag/fluke-v1.0.0), copy it to the `mods` folder on the OP-1's disk, restart, and pick fluke in the synth sequencer list. co-op lists it for reference and hosts no copy.

## Links

- [Fluke: download, install and every control](https://github.com/VolkerJunginger/OP-1/tree/main/Fluke)
- [Manual (PDF)](https://github.com/VolkerJunginger/OP-1/blob/main/Fluke/Fluke%20Manual.pdf)
- [Source](https://github.com/VolkerJunginger/OP-1/tree/main/Fluke/Source)
