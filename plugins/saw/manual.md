# saw

The reference synth engine for the coop firmware: the smallest engine that uses everything a coop engine can do. Two oscillators a little apart (saw, square or triangle), a square one or two octaves below them and a one-pole low-pass. Every key plays in tune across the keyboard, six voices at once; the stock envelope, LFO and pitch bend work as on any engine.

Needs coop **e4b** or later (version 0.2 calls the firmware's L1 memory lease when it loads; on older builds it is refused or, on e4, crashes at boot).

On Opus 2 it needs core 2.31 or later: see On opus, below.

## Install

Choose **coop** in co-op's Settings, then on the Plugins tab use **Install on OP-1** with the OP-1 plugged in by USB normally (not in disk mode). Power-cycle the OP-1. On the synth page, Shift + T1 opens the engine list: **saw** sits between sampler and string.

## Controls

Help works as **fn**: hold it with another control.

| Encoder | Turn | fn + turn | Shift + fn + click | On the screen |
|---|---|---|---|---|
| Blue | Brightness: the low-pass, dark to open | **Shape**: saw, square, triangle (two detents a step) | — | The oscillators' height and shape; the shape icons top right (dim blue while fn is held) |
| Green | Detune: the second oscillator up to about 27 cents above the first | — | — | The green oscillator slides right |
| White | Sub: the level of the square below | — | — | The white square grows |
| Orange | Drift: a slow wobble of the detune, a different speed on each voice | — | **Sub octave**: -1 or -2 | The orange dot swings; one or two dots bottom right for the octave (bright while Shift + fn is held) |

Shift + turn is fine adjustment, as on any engine. When the shape or the octave changes, its name and value show top left for a moment. Randomise (hold the synth key, press the right arrow) also picks a shape; it leaves the octave alone.

## On opus

`saw.opmod` is the coop build ported to Opus 2 by coldsoldr's
[op1-plugins](https://github.com/coldsoldr/op1-plugins): the same sound,
screen and controls. It needs Opus 2 with core 2.31 or later.

- Download `saw.opmod` from op1-plugins'
  [releases](https://github.com/coldsoldr/op1-plugins/releases), copy it into
  the `mods` folder of the OP-1's disk (disk mode: shift + COM, then T3),
  eject and restart.
- Shift + T1 on the synth page opens the engine list.
- **fn** is Opus's FN key: FN + blue turns the shape, and shift + FN + an
  orange click switches the sub octave.

## Notes

- The oscillators are not band-limited: the top of the keyboard aliases a little, and the low-pass takes the edge off.
- If the file is removed, sounds saved on saw load as the stock iter engine and keep their settings; they play as saw again once the file is back.
- Source: `firmware/plugins/saw` in the co-op repository (GPL-3.0-or-later).
