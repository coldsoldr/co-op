# saw

The reference synth engine for the coop firmware: the smallest engine that uses everything a coop engine can do. Two saws a little apart, a square an octave below them and a one-pole low-pass. Every key plays in tune across the keyboard, six voices at once; the stock envelope, LFO and pitch bend work as on any engine.

## Install

Choose **coop** in co-op's Settings, then on the Plugins tab use **Install on OP-1** with the OP-1 plugged in by USB normally (not in disk mode). Power-cycle the OP-1. On the synth page, Shift + T1 opens the engine list: **saw** sits between sampler and string.

## Controls

| Encoder | Turn | On the screen |
|---|---|---|
| Blue | Brightness: the low-pass, dark to open | The saws' height |
| Green | Detune: the second saw up to about 27 cents above the first | The green saw slides right |
| White | Sub: the level of the square an octave down | The white square grows |
| Orange | Drift: a slow wobble of the detune, a different speed on each voice | The orange dot swings wider and faster |

## Notes

- The saws are not band-limited: the top of the keyboard aliases a little, and the low-pass takes the edge off.
- If the file is removed, sounds saved on saw load as the stock iter engine and keep their settings; they play as saw again once the file is back.
- Source: `firmware/plugins/saw` in the co-op repository (GPL-3.0-or-later).
